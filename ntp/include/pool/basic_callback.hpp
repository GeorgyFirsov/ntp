/**
 * @file basic_callback.hpp
 * @brief Interfaces and base classes for callback wrappers
 */

#pragma once

#include <Windows.h>

#include <map>
#include <mutex>
#include <memory>
#include <functional>
#include <type_traits>
#include <shared_mutex>

#include "config.hpp"
#include "details/utils.hpp"


namespace ntp::details {

/**
 * @brief Cancellation test timeout, while waiting for callbacks.
 *        Set to 500 msec.
 */
NTP_INLINE constexpr auto kTestCancelTimeout = 500ul;


/**
 * @brief Cancellation checker for threadpool
 * 
 * @returns true if cancellation requested, false -- otherwise
 */
using test_cancel_t = std::function<bool()>;


/**
 * @brief Interface for callback wrapper
 */
struct alignas(NTP_ALLOCATION_ALIGNMENT) ICallback
    : public ntp::details::NativeSlistEntry
{
    /**
	 * @brief Virtual destructor (generated by compiler)
	 */
    virtual ~ICallback() = default;

    /**
	 * @brief Method to invoke callback with an optional argument
	 * 
     * @param instance Structure that defines the callback instance
	 * @param parameter Optional user defined pointer-sized parameter
	 */
    virtual void Call(PTP_CALLBACK_INSTANCE instance, void* parameter) = 0;
};


/**
 * @brief Smart pointer to ICallback implementation object
 */
using callback_t = std::unique_ptr<ICallback>;


/**
 * @brief Base class for all callbacks.
 * 
 * All callbacks are inherited from this class, that is used
 * to store callable and its arguments.
 * 
 * @tparam Derived Derived from this class implementation for specific callback type (CRTP)
 * @tparam Functor Arbitrary callable to wrap
 * @tparam Args... Variadic pack of arguments of callable
 */
template<typename Derived, typename Functor, typename... Args>
class alignas(NTP_ALLOCATION_ALIGNMENT) BasicCallback
    : public ICallback
{
    BasicCallback(const BasicCallback&)            = delete;
    BasicCallback& operator=(const BasicCallback&) = delete;

private:
    // Type of packed arguments
    using tuple_t = std::tuple<std::decay_t<Args>...>;

protected:
    /**
     * @brief Constructor from callable and its arguments
     *
     * @param functor Callable to invoke
     * @param args Arguments to pass into callable (they will be copied into wrapper)
     */
    template<typename... CArgs>
    explicit BasicCallback(Functor functor, CArgs&&... args)
        : args_(std::forward<CArgs>(args)...)
        , functor_(std::move(functor))
    { }

    /**
	 * @brief Get stored callable
	 *
	 * @returns Reference to callable
     */
    Functor& Callable() noexcept { return functor_; }

    /**
     * @brief Get stored pack of arguments
     * 
     * @returns Reference to packed arguments tuple
     */
    tuple_t& Arguments() noexcept { return args_; }

private:
    void Call(PTP_CALLBACK_INSTANCE instance, void* parameter) final
    {
        const auto converted_parameter = AsDerived()->ConvertParameter(parameter);
        return AsDerived()->CallImpl(instance, converted_parameter);
    }

    Derived* AsDerived() noexcept { return static_cast<Derived*>(this); }

private:
    // Packed arguments
    tuple_t args_;

    // Callable
    Functor functor_;
};


/**
 * @brief Base class for all callbacks' managers
 */
class BasicManager
{
public:
    /**
	 * @brief Constructor, that saves an environment associated with a threadpool
	 */
    explicit BasicManager(PTP_CALLBACK_ENVIRON environment) noexcept
        : environment_(environment)
    { }

protected:
    /**
	 * @brief Get an environment associated with owning threadpool
	 * 
	 * @returns Pointer to the environment
	 */
    PTP_CALLBACK_ENVIRON Environment() const noexcept { return environment_; }

private:
    // Non-owning pointer to environment associated with a threadpool
    PTP_CALLBACK_ENVIRON environment_;
};


/**
 * @brief Extended version of ntp::details::BasicManager with container of callbacks.
 * 
 * This extended version is used for waits, because each such object must have separate native handle.
 * 
 * Derived class must implement the following methods:
 * - static Close(native_handle_t native_handle)
 * - void SubmitInternal(native_handle_t native_handle, object_context_t& user_context)
 * 
 * @tparam NativeHandle Type of native object handle (e.g. PTP_WAIT)
 * @tparam ObjectContext Type of context specific for threadpool object kind
 * @tparam Derived Derived from this class implementation for specific callback type (CRTP)
 */
template<typename NativeHandle, typename ObjectContext, typename Derived>
class BasicManagerEx
    : public BasicManager
{
    // Forward declaration of context structure
    struct Context;

protected:
    /**
     * @brief Smart pointer to specific context.
     */
    using context_t = std::unique_ptr<Context>;

    /**
     * @brief Raw pointer to specific context. Is never invalidated, while pointee lives.
     *        E.g. after any modification of the container, this pointer still remains
     *        valid, if corresponding object is not erased from container.
     */
    using context_pointer_t = typename context_t::pointer;

    /**
     * @brief Type of native threadpool object handle.
     */
    using native_handle_t = NativeHandle;

    /**
     * @brief Implementation's context.
     */
    using object_context_t = ObjectContext;

    /**
     * @brief Container with callbacks represented by their handles.
     */
    using callbacks_t = std::map<native_handle_t, context_t>;

    /**
     * @brief Lock primitive.
     */
    using lock_t = ntp::details::RtlResource;

private:
    /**
     * @brief Meta information about context.
     */
    struct MetaContext
    {
        BasicManagerEx* manager; /**< Pointer to parent wait manager */

        native_handle_t native_handle; /**< Native handle of corresponding threadpool object */
    };

    /**
     * @brief Callback context structure
     */
    struct Context final
    {
        object_context_t object_context; /**< Context specific to callback kind */

        MetaContext meta_context; /**< Meta information about context */

        ntp::details::callback_t callback; /**< Pointer to callback wrapper */
    };

protected:
    /**
	 * @brief Constructor that initializes all necessary objects.
	 *
	 * @param environment Owning threadpool environment
     */
    BasicManagerEx(PTP_CALLBACK_ENVIRON environment)
        : BasicManager(environment)
        , callbacks_()
        , lock_()
    { }

public:
    /**
     * @brief Cancels and removes an object.
     *
     * If no such object is present, does nothing.
     *
     * @param object Handle for an object
     */
    void Cancel(HANDLE object) noexcept
    {
        const auto native_handle = static_cast<native_handle_t>(object);
        CloseAndRemove(native_handle);
    }

    /**
     * @brief Cancel all pending callbacks.
     */
    void CancelAll() noexcept
    {
        std::unique_lock lock { lock_ };

        for (auto& [native_handle, _] : callbacks_)
        {
            Derived::Close(native_handle);
        }

        callbacks_.clear();
    }

protected:
    /**
     * @brief Put context into container and then submit associated callback.
     * 
     * @param native_handle Native handle of threadpool object to submit
     * @param context Filled context of callback
     */
    void SubmitContext(native_handle_t native_handle, context_t&& context)
    {
        std::unique_lock lock { lock_ };

        const auto [iter, _]                     = callbacks_.emplace(native_handle, std::move(context));
        iter->second->meta_context.manager       = this;
        iter->second->meta_context.native_handle = native_handle;

        AsDerived()->SubmitInternal(native_handle, iter->second->object_context);
    }

    /**
     * @brief Look for specific threadpool object by handle.
     * 
     * @param native_handle Handle to threadpool object to look for
     * @returns Corresponding context or nullptr, if such object does not exist
     */
    context_pointer_t Lookup(native_handle_t native_handle) noexcept
    {
        std::shared_lock lock { lock_ };

        if (const auto callback = callbacks_.find(native_handle); callback != callbacks_.end())
        {
            return callback->second.get();
        }

        return nullptr;
    }

protected:
    /**
     * @brief Create new empty context.
     * 
     * @returns Smart pointer to empty context
     */
    static context_t CreateContext()
    {
        return std::make_unique<Context>();
    }

    /**
     * @brief A right way to delete object from its callback.
     * 
     * Removes callback and object association and then removes context
     * by its value. Removing by value is required, because we can
     * collide with Cancel/CancelAll functions. After container's lock
     * is released, no already closed objects will remain in container.
     * 
     * It may be confusing, why object is removed by value. This is the
     * key to atomicity of removal. If Cancel/CancelAll closes and 
     * removes object first, then there will be no object anymore,
     * and this function will do nothing.
     * 
     * @param instance Running callback instance (it is extremely important
     *                 to remove association between object and callback)
     * @param context Context to remove from container
     */
    static void CleanupContext(PTP_CALLBACK_INSTANCE instance, context_pointer_t context)
    {
        DisassociateCurrentThreadFromCallback(instance);

        auto native_handle = context->meta_context.native_handle;
        auto manager       = context->meta_context.manager;

        return manager->CloseAndRemove(native_handle);
    }

private:
    Derived* AsDerived() noexcept
    {
        return static_cast<Derived*>(this);
    }

    void CloseAndRemove(native_handle_t native_handle)
    {
        std::unique_lock lock { lock_ };

        if (const auto callback = callbacks_.find(native_handle); callback != callbacks_.end())
        {
            Derived::Close(native_handle);
            callbacks_.erase(callback);
        }
    }

private:
    // Container with callbacks
    callbacks_t callbacks_;

    // Syncronization primitive for callbacks container
    mutable lock_t lock_;
};

}  // namespace ntp::details
