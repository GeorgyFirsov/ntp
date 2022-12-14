/**
 * @file io.hpp
 * @brief Implementation of threadpool IO callback
 */

#pragma once

#include <tuple>
#include <utility>

#include "ntp_config.hpp"
#include "details/time.hpp"
#include "details/utils.hpp"
#include "pool/basic_callback.hpp"


namespace ntp::io::details {

/**
 * @brief Specific context for threadpool IO objects.
 *        Empty, because no additional information is needed.
 */
struct IoContext
{ };


/**
 * @brief Packed parameters for IO callback.
 * 
 * This pack is created in ntp::io::details::IoManager::InvokeCallback
 * and unpacked in ntp::io::details::IoCallback::CallImpl.
 */
struct IoData
{
    PVOID overlapped; /**< A pointer to a variable that receives the address of the OVERLAPPED structure 
                           that was specified when the completed I/O operation was started. */

    ULONG result; /**< The result of the I/O operation. If the I/O is successful, this parameter is NO_ERROR. 
                       Otherwise, this parameter is one of the system error codes. */

    ULONG_PTR bytes_transferred; /**< The number of bytes transferred during the I/O operation that has completed. */
};


/**
 * @brief IO callback wrapper (PTP_IO).
 *
 * @tparam Functor Type of callable to invoke in threadpool
 * @tparam Args... Types of arguments
 */
template<typename Functor, typename... Args>
class alignas(NTP_ALLOCATION_ALIGNMENT) IoCallback final
    : public ntp::details::BasicCallback<IoCallback<Functor, Args...>, Functor, Args...>
{
public:
    /**
     * @brief Constructor from callable and its arguments
     *
     * @param functor Callable to invoke
     * @param args Arguments to pass into callable (they will be copied into wrapper)
     */
    template<typename CFunctor, typename... CArgs>
    explicit IoCallback(CFunctor&& functor, CArgs&&... args)
        : BasicCallback(std::forward<CFunctor>(functor), std::forward<CArgs>(args)...)
    { }

    /**
     * @brief Parameter conversion function. Just performs pointer type conversion.
     */
    IoData* ConvertParameter(void* parameter) { return static_cast<IoData*>(parameter); }

    /**
     * @brief Callback invocation function implementation. Supports invocation of
     *        callbacks with or without PTP_CALLBACK_INSTANCE parameter. 
     */
    template<typename = void> /* if constexpr works only for templates */
    void CallImpl(PTP_CALLBACK_INSTANCE instance, IoData* io_data)
    {
        if constexpr (std::is_invocable_v<std::decay_t<Functor>, PTP_CALLBACK_INSTANCE, LPVOID, ULONG, ULONG_PTR, std::decay_t<Args>...>)
        {
            const auto required_args = std::make_tuple(instance, io_data->overlapped, io_data->result, io_data->bytes_transferred);
            const auto args          = std::tuple_cat(required_args, Arguments());
            std::apply(Callable(), args);
        }
        else
        {
            const auto required_args = std::make_tuple(io_data->overlapped, io_data->result, io_data->bytes_transferred);
            const auto args          = std::tuple_cat(required_args, Arguments());
            std::apply(Callable(), args);
        }
    }
};


/**
 * @brief Manager for wait callbacks. Binds callbacks and threadpool implementation.
 */
class IoManager final
    : public ntp::details::BasicManager<PTP_IO, IoContext, IoManager>
{
    friend class ntp::details::BasicManager<PTP_IO, IoContext, IoManager>;

public:
    /**
     * @brief Constructor that initializes all necessary objects.
     *
     * @param environment Owning threadpool environment
     */
    explicit IoManager(PTP_CALLBACK_ENVIRON environment);

    /**
     * @brief Submits a threadpool IO object with a user-defined callback.
     *
     * Creates a new callback wrapper, new IO object, put
     * it into a callbacks container and then starts threadpool IO.
     *
     * @param io_handle Handle of and object, wchich asynchronous IO is performed on
     * @param functor Callable to invoke
     * @param args Arguments to pass into callable (they will be copied into wrapper)
     * @returns handle for created wait object
     */
    template<typename Functor, typename... Args>
    native_handle_t Submit(HANDLE io_handle, Functor&& functor, Args&&... args)
    {
        auto context      = CreateContext();
        context->callback = std::make_unique<IoCallback<Functor, Args...>>(std::forward<Functor>(functor), std::forward<Args>(args)...);

        const auto native_handle = CreateThreadpoolIo(io_handle, reinterpret_cast<PTP_WIN32_IO_CALLBACK>(InvokeCallback),
            context.get(), Environment());

        if (!native_handle)
        {
            throw exception::Win32Exception();
        }

        static_assert(noexcept(SubmitContext(native_handle, std::move(context))),
            "[ntp::io::details::WaitManager::Submit]: inspect ntp::details::BasicCallback::SubmitContext and "
            "ntp::io::details::WaitManager::SubmitInternal for noexcept property, because an exception thrown "
            "here can lead to handle and memory leaks. SubmitContext is noexcept if and only if SubmitInternal "
            "is noexcept.");

        SubmitContext(native_handle, std::move(context));

        return native_handle;
    }

private:
    void SubmitInternal(native_handle_t native_handle, object_context_t& user_context) noexcept;

private:
    static void NTAPI InvokeCallback(PTP_CALLBACK_INSTANCE instance, context_pointer_t context, PVOID overlapped,
        ULONG result, ULONG_PTR bytes_transferred, PTP_IO io) noexcept;

    static void CloseInternal(native_handle_t native_handle) noexcept;

    static void AbortInternal(native_handle_t native_handle) noexcept;
};

}  // namespace ntp::io::details
