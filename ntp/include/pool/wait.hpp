/**
 * @file wait.hpp
 * @brief Implementation of threadpool wait callback
 */

#pragma once

#include <map>
#include <mutex>
#include <tuple>
#include <atomic>
#include <utility>
#include <optional>

#include "config.hpp"
#include "details/time.hpp"
#include "details/utils.hpp"
#include "details/exception.hpp"
#include "pool/basic_callback.hpp"


namespace ntp::wait::details {

/**
 * @brief Wait callback wrapper (PTP_WAIT).
 *
 * @tparam Functor Type of callable to invoke in threadpool
 * @tparam Args... Types of arguments
 */
template<typename Functor, typename... Args>
class alignas(NTP_ALLOCATION_ALIGNMENT) Callback final
    : public ntp::details::BasicCallback<Functor, Args...>
{
public:
    /**
     * @brief Constructor from callable and its arguments
     *
     * @param functor Callable to invoke
     * @param args Arguments to pass into callable (they will be copied into wrapper)
     */
    template<typename CFunctor, typename... CArgs>
    explicit Callback(CFunctor&& functor, CArgs&&... args)
        : BasicCallback(std::forward<CFunctor>(functor), std::forward<CArgs>(args)...)
    { }

    /**
     * @brief Invocation of internal callback (interface's parameter is assumed to be TP_WAIT_RESULT)
     */
    void Call(PTP_CALLBACK_INSTANCE instance, void* parameter) override
    {
        return CallImpl(instance, reinterpret_cast<TP_WAIT_RESULT>(parameter));
    }

private:
    template<typename = void> /* if constexpr works only for templates */
    void CallImpl(PTP_CALLBACK_INSTANCE instance, TP_WAIT_RESULT wait_result)
    {
        if constexpr (std::is_invocable_v<std::decay_t<Functor>, PTP_CALLBACK_INSTANCE, TP_WAIT_RESULT, std::decay_t<Args>...>)
        {
            const auto args = std::tuple_cat(std::make_tuple(instance, wait_result), Arguments());
            std::apply(Callable(), args);
        }
        else
        {
            const auto args = std::tuple_cat(std::make_tuple(wait_result), Arguments());
            std::apply(Callable(), args);
        }
    }
};


/**
 * @brief Manager for wait callbacks. Binds callbacks and threadpool implementation.
 */
class Manager final
    : public ntp::details::BasicManager
{
    // If we have such number of waits, then we need to scan for marked for removal callbacks
    static constexpr auto kRemovalScanThreschold = 100;

private:
    struct Context;

    // Mapping from wait handles to callback contexts. Map is used instead of
    // unordered map, because it never invalidates iterators and references.
    //
    // Callback context -----------------------------+
    // Wait handle -----------------+                |
    //                              |                |
    //                              V                V
    using callbacks_t = std::map<HANDLE, std::unique_ptr<Context>>;

    // Lock primitive
    using lock_t = ntp::details::RtlResource;

private:
    /**
     * @brief Meta information about context.
     */
    struct MetaContext
    {
        Manager* manager; /**< Pointer to parent wait manager */

        callbacks_t::iterator iterator; /**< Iterator to current context */
    };

    /**
     * @brief Context of threadpool wait callback.
     *
     * Stores all necessary information about current callback instance.
     */
    struct Context
    {
        std::optional<FILETIME> wait_timeout; /**< Wait timeout (pftTimeout parameter of SetThreadpoolWait function) */

        PTP_WAIT native_handle; /**< Native threadpool wait object */

        ntp::details::callback_t callback; /**< Pointer to callback wrapper */

        MetaContext meta; /**< Meta information about context */
    };

    /**
     * @brief 
     */
    class RemovalPermission
    {
        RemovalPermission(const RemovalPermission&)            = delete;
        RemovalPermission& operator=(const RemovalPermission&) = delete;

    public:
        RemovalPermission()
            : can_remove_(true)
        { }
        ~RemovalPermission() = default;

        void lock() noexcept { can_remove_.store(false, std::memory_order_release); }
        void unlock() noexcept { can_remove_.store(true, std::memory_order_release); }

        operator bool() const noexcept { return can_remove_.load(std::memory_order_acquire); }

    private:
        std::atomic_bool can_remove_;
    };

public:
    /**
	 * @brief Constructor that initializes all necessary objects.
	 *
	 * @param environment Owning threadpool environment
     */
    explicit Manager(PTP_CALLBACK_ENVIRON environment);

    ~Manager();

    /**
     * @brief Submits or replaces a threadpool wait object with a user-defined callback.
     * 
     * Creates a new callback wrapper, new wait object (if not already present), put 
     * it into a callbacks container and then sets threadpool wait.
     * 
     * @param wait_handle Handle to wait for
     * @param timeout Timeout while wait object waits for the specified handle 
     *                (pass ntp::time::max_native_duration for infinite wait timeout)
	 * @param functor Callable to invoke
	 * @param args Arguments to pass into callable (they will be copied into wrapper)
     */
    template<typename Rep, typename Period, typename Functor, typename... Args>
    void Submit(HANDLE wait_handle, const std::chrono::duration<Rep, Period>& timeout, Functor&& functor, Args&&... args)
    {
        std::unique_lock lock { lock_ };

        //
        // If we already have callback for this handle, then replace it with the new one
        //

        if (auto callback = callbacks_.find(wait_handle); callback != callbacks_.end())
        {
            return ReplaceUnsafe(callback, std::forward<Functor>(functor), std::forward<Args>(args)...);
        }

        //
        // Create new context and submit a new wait object
        //

        const auto [iter, _] = callbacks_.emplace(wait_handle, std::make_unique<Context>());
        auto& context        = *iter->second;
        auto& meta           = iter->second->meta;

        context.callback.reset(new Callback<Functor, Args...>(
            std::forward<Functor>(functor), std::forward<Args>(args)...));

        const auto native_timeout = std::chrono::duration_cast<ntp::time::native_duration_t>(timeout);
        if (native_timeout != ntp::time::max_native_duration)
        {
            //
            // If timeout does not equal to ntp::time::max_native_duration, then
            // it will be converted to FILETIME, otherwise wait never expires
            //

            context.wait_timeout = ntp::time::AsFiletime(native_timeout);
        }

        context.native_handle = CreateThreadpoolWait(reinterpret_cast<PTP_WAIT_CALLBACK>(InvokeCallback),
            iter->second.get(), Environment());

        //
        // Set context meta information and then set wait
        //

        meta.iterator = iter;
        meta.manager  = this;

        SubmitInternal(*iter->second);
    }

    /**
	 * @brief Submits or replaces a threadpool wait object of default type with a 
     * user-defined callback. It never expires.
	 *
	 * Just calls generic version of ntp::wait::details::Manager::Submit with
	 * ntp::time::max_native_duration as timeout parameter and 
     * ntp::wait::details::Type::kDefault as type parameter.
	 *
	 * @param wait_handle Handle to wait for
	 * @param functor Callable to invoke
	 * @param args Arguments to pass into callable (they will be copied into wrapper)
     */
    template<typename Functor, typename... Args>
    auto Submit(HANDLE wait_handle, Functor&& functor, Args&&... args)
        -> std::enable_if_t<!ntp::time::details::is_duration_v<Functor>>
    {
        return Submit(wait_handle, ntp::time::max_native_duration, std::forward<Functor>(functor), std::forward<Args>(args)...);
    }

    /**
     * @brief Replaces an existing threadpool wait callback with a new one.
     * 
     * Cancels current pending callback, that corresponds to the specified 
     * wait handle, creates a new callback wrapper and then sets wait again
	 * with unchanged parameters.
	 *
	 * @param wait_handle Handle to wait for
	 * @param functor New callable to invoke
	 * @param args New arguments to pass into callable (they will be copied into wrapper)
     * @throws ntp::exception::Win32Exception if wait handle is not present
     */
    template<typename Functor, typename... Args>
    void Replace(HANDLE wait_handle, Functor&& functor, Args&&... args)
    {
        std::unique_lock lock { lock_ };

        if (auto callback = callbacks_.find(wait_handle); callback != callbacks_.end())
        {
            return ReplaceUnsafe(callback, std::forward<Functor>(functor), std::forward<Args>(args)...);
        }

        throw exception::Win32Exception(ERROR_NOT_FOUND);
    }

    /**
     * @brief Cancels and removes wait object, that corresponds to a specified wait handle.
     * 
     * If no wait for the handle is present, does nothing.
     * 
     * @param wait_handle Handle, that is (possibly) awaited in an owning threadpool
     */
    void Cancel(HANDLE wait_handle) noexcept;

    /**
     * @brief Cancel all pending callbacks.
     */
    void CancelAll() noexcept;

private:
    template<typename Functor, typename... Args>
    void ReplaceUnsafe(callbacks_t::iterator callback, Functor&& functor, Args&&... args)
    {
        auto& [wait_handle, context] = *callback;

        //
        // Firstly we need to cancel current pending callback and only
        // after that we are allowed to replace it with the new one
        //

        SetThreadpoolWait(context->native_handle, nullptr, nullptr);

        context->callback.reset(new Callback<Functor, Args...>(
            std::forward<Functor>(functor), std::forward<Args>(args)...));

        SubmitInternal(*context);
    }

    void Remove(callbacks_t::iterator callback) noexcept;

    void SubmitInternal(Context& context) noexcept;

private:
    static void NTAPI InvokeCallback(PTP_CALLBACK_INSTANCE instance, Context* context, PTP_WAIT wait, TP_WAIT_RESULT wait_result) noexcept;

    static void CloseWait(PTP_WAIT wait) noexcept;

    static void Wait(PTP_WAIT wait) noexcept;

private:
    // Container with callbacks
    callbacks_t callbacks_;

    // Syncronization primitive for callbacks container
    mutable lock_t lock_;

    // If true, callback will not release its resource after completion.
    // This flag is used in CancelAll function to prevent container
    // modification while iterating over it.
    mutable RemovalPermission removal_permission_;
};

}  // namespace ntp::wait::details