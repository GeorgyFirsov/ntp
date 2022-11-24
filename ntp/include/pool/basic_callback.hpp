/**
 * @file basic_callback.hpp
 * @brief Interfaces and base classes for callback wrappers
 */

#pragma once

#include <Windows.h>

#include <functional>

#include "config.hpp"


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
	 * @param parameter Optional user defined pointer-sized parameter
	 */
    virtual void Call(void* parameter) = 0;
};


/**
 * @brief Base class for all callbacks. Does not implement 
 *        Call method of ntp::details::ICallback, hence
 *        it remains abstract
 * 
 * All callbacks are inherited from this class, that is used
 * to store callable and its arguments.
 */
template<typename Functor, typename... Args>
class alignas(NTP_ALLOCATION_ALIGNMENT) BasicCallback final
    : public ICallback
{
    BasicCallback(const BasicCallback&)            = delete;
    BasicCallback& operator=(const BasicCallback&) = delete;

private:
    // Type of packed arguments
    using tuple_t = std::tuple<std::decay_t<Args>...>;

public:
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

protected:
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

}  // namespace ntp::details
