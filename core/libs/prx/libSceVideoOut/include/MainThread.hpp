#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_MAINTHREAD_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_MAINTHREAD_HPP

#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <pthread.h>
#endif

template <typename Function>
auto RunOnMainThread(Function&& function) {
#ifdef __APPLE__
    if (pthread_main_np() == 0) {
        using Result = std::invoke_result_t<Function&>;
        struct Call {
            Function* function;
            std::exception_ptr failure;
            std::conditional_t<std::is_void_v<Result>, bool, std::optional<Result>> result{};
        } call{&function, nullptr};
        dispatch_sync_f(dispatch_get_main_queue(), &call, [](void* context) {
            auto& pending = *static_cast<Call*>(context);
            try {
                if constexpr (std::is_void_v<Result>) {
                    (*pending.function)();
                } else {
                    pending.result.emplace((*pending.function)());
                }
            } catch (...) {
                pending.failure = std::current_exception();
            }
        });
        if (call.failure) std::rethrow_exception(call.failure);
        if constexpr (std::is_void_v<Result>) {
            return;
        } else {
            return Result(std::move(*call.result));
        }
    }
#endif
    return function();
}

#endif
