#pragma once

#include "execution_core.hpp"

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace execution_adapters {

class IExecutor {
public:
    virtual ~IExecutor() = default;
    virtual void post(std::function<void()> fn) = 0;
};

struct WorkerContext {
    std::size_t worker_index{};
    std::shared_ptr<void> user_context;
    std::exception_ptr initialization_exception;
};

using WorkerContextFactory = std::function<std::shared_ptr<void>(std::size_t worker_index)>;

class IContextExecutor {
public:
    virtual ~IContextExecutor() = default;
    virtual void post(std::function<void(WorkerContext&)> fn) = 0;
    [[nodiscard]] virtual std::size_t thread_count() const noexcept = 0;
};

class IDestructionSubscription {
public:
    virtual ~IDestructionSubscription() = default;
};

class ILifetime {
public:
    virtual ~ILifetime() = default;
    [[nodiscard]] virtual bool alive() const noexcept = 0;
    virtual std::shared_ptr<IDestructionSubscription>
        subscribe(std::function<void()> callback) = 0;
};

struct PostContext {
    std::shared_ptr<IExecutor> executor;
    std::shared_ptr<ILifetime> lifetime;

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(executor);
    }

    [[nodiscard]] bool alive() const noexcept {
        return !lifetime || lifetime->alive();
    }
};

class SingleContinuationWaitSet : public execution_core::IContinuationWaitSet {
public:
    bool publish(execution_core::Continuation continuation) noexcept override;
    std::optional<execution_core::Continuation> extract_ready() noexcept override;
    void remove(execution_core::TaskId task_id, std::uint64_t generation) noexcept override;

private:
    std::mutex mutex_;
    std::optional<execution_core::Continuation> continuation_;
};

void schedule_wait_set_if_alive(
    execution_core::ExecutionCoreRuntimeWeakPtr runtime,
    const std::shared_ptr<SingleContinuationWaitSet>& wait_set) noexcept;

void post_if_alive(PostContext context, std::function<void()> fn);

namespace detail {

template <typename Result>
struct BackgroundJobResult {
    std::optional<Result> value;
    std::exception_ptr exception;
};

template <>
struct BackgroundJobResult<void> {
    std::exception_ptr exception;
};

template <typename Result>
struct BackgroundSharedState final : SingleContinuationWaitSet {
    template <typename Value>
    void set_value(Value&& value) {
        std::lock_guard lock(result_mutex_);
        result_.value.emplace(std::forward<Value>(value));
    }

    void set_exception(std::exception_ptr exception) noexcept {
        try {
            std::lock_guard lock(result_mutex_);
            result_.exception = std::move(exception);
        } catch (...) {
            std::terminate();
        }
    }

    BackgroundJobResult<Result> take_result() {
        std::lock_guard lock(result_mutex_);
        return std::move(result_);
    }

private:
    std::mutex result_mutex_;
    BackgroundJobResult<Result> result_;
};

template <>
struct BackgroundSharedState<void> final : SingleContinuationWaitSet {
    void set_exception(std::exception_ptr exception) noexcept {
        try {
            std::lock_guard lock(result_mutex_);
            result_.exception = std::move(exception);
        } catch (...) {
            std::terminate();
        }
    }

    BackgroundJobResult<void> take_result() {
        std::lock_guard lock(result_mutex_);
        return std::move(result_);
    }

private:
    std::mutex result_mutex_;
    BackgroundJobResult<void> result_;
};

template <typename Result>
Result consume_result(BackgroundJobResult<Result>&& result) {
    if (result.exception) {
        std::rethrow_exception(result.exception);
    }

    if (!result.value.has_value()) {
        throw std::logic_error("background job resumed without a result");
    }

    return std::move(*result.value);
}

inline void consume_result(BackgroundJobResult<void>&& result) {
    if (result.exception) {
        std::rethrow_exception(result.exception);
    }
}

template <typename Promise>
bool suspend_current_to_wait_set(
    std::coroutine_handle<Promise> handle,
    SingleContinuationWaitSet& wait_set) noexcept {
    auto& binding = handle.promise().execution_binding;

    if (binding.registry == nullptr) {
        return false;
    }

    execution_core::Continuation continuation{
        handle,
        binding.task_id,
        binding.generation,
        0
    };

    return binding.registry->suspend_to_waiting(continuation, wait_set);
}

} // namespace detail

template <typename Function>
class BackgroundAwaitable {
public:
    using FunctionType = std::decay_t<Function>;
    using Result = std::invoke_result_t<FunctionType&>;
    using SharedState = detail::BackgroundSharedState<Result>;

    static_assert(!std::is_reference_v<Result>,
        "run_in_background() does not support reference return types; "
        "return a value, pointer, or std::reference_wrapper explicitly.");

    BackgroundAwaitable(execution_core::ExecutionCoreRuntimeWeakPtr runtime,
                        std::shared_ptr<IExecutor> background_executor,
                        Function&& function)
        : runtime_(std::move(runtime))
        , background_executor_(std::move(background_executor))
        , function_(std::forward<Function>(function))
        , state_(std::make_shared<SharedState>()) {
    }

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        auto runtime = runtime_.lock();
        if (!runtime || !runtime->alive()) {
            state_->set_exception(std::make_exception_ptr(
                std::runtime_error("execution_core runtime is not alive")));
            return false;
        }

        if (!background_executor_) {
            state_->set_exception(std::make_exception_ptr(
                std::runtime_error("background executor is not set")));
            return false;
        }

        if (!detail::suspend_current_to_wait_set(handle, *state_)) {
            state_->set_exception(std::make_exception_ptr(
                std::runtime_error("failed to suspend coroutine into execution_core wait-set")));
            return false;
        }

        try {
            auto state = state_;
            auto runtime_weak = runtime_;

            background_executor_->post(
                [function = std::move(function_), state, runtime_weak]() mutable {
                    try {
                        if constexpr (std::is_void_v<Result>) {
                            std::invoke(function);
                        } else {
                            state->set_value(std::invoke(function));
                        }
                    } catch (...) {
                        state->set_exception(std::current_exception());
                    }

                    schedule_wait_set_if_alive(std::move(runtime_weak), state);
                });
        } catch (...) {
            state_->set_exception(std::current_exception());
            schedule_wait_set_if_alive(runtime_, state_);
        }

        return true;
    }

    decltype(auto) await_resume() {
        return detail::consume_result(state_->take_result());
    }

private:
    execution_core::ExecutionCoreRuntimeWeakPtr runtime_;
    std::shared_ptr<IExecutor> background_executor_;
    FunctionType function_;
    std::shared_ptr<SharedState> state_;
};

template <typename Function>
class ContextBackgroundAwaitable {
public:
    using FunctionType = std::decay_t<Function>;
    using Result = std::invoke_result_t<FunctionType&, WorkerContext&>;
    using SharedState = detail::BackgroundSharedState<Result>;

    static_assert(!std::is_reference_v<Result>,
        "run_in_context_background() does not support reference return types; "
        "return a value, pointer, or std::reference_wrapper explicitly.");

    ContextBackgroundAwaitable(execution_core::ExecutionCoreRuntimeWeakPtr runtime,
                               std::shared_ptr<IContextExecutor> background_executor,
                               Function&& function)
        : runtime_(std::move(runtime))
        , background_executor_(std::move(background_executor))
        , function_(std::forward<Function>(function))
        , state_(std::make_shared<SharedState>()) {
    }

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        auto runtime = runtime_.lock();
        if (!runtime || !runtime->alive()) {
            state_->set_exception(std::make_exception_ptr(
                std::runtime_error("execution_core runtime is not alive")));
            return false;
        }

        if (!background_executor_) {
            state_->set_exception(std::make_exception_ptr(
                std::runtime_error("context background executor is not set")));
            return false;
        }

        if (!detail::suspend_current_to_wait_set(handle, *state_)) {
            state_->set_exception(std::make_exception_ptr(
                std::runtime_error("failed to suspend coroutine into execution_core wait-set")));
            return false;
        }

        try {
            auto state = state_;
            auto runtime_weak = runtime_;

            background_executor_->post(
                [function = std::move(function_), state, runtime_weak](WorkerContext& context) mutable {
                    try {
                        if (context.initialization_exception) {
                            std::rethrow_exception(context.initialization_exception);
                        }

                        if constexpr (std::is_void_v<Result>) {
                            std::invoke(function, context);
                        } else {
                            state->set_value(std::invoke(function, context));
                        }
                    } catch (...) {
                        state->set_exception(std::current_exception());
                    }

                    schedule_wait_set_if_alive(std::move(runtime_weak), state);
                });
        } catch (...) {
            state_->set_exception(std::current_exception());
            schedule_wait_set_if_alive(runtime_, state_);
        }

        return true;
    }

    decltype(auto) await_resume() {
        return detail::consume_result(state_->take_result());
    }

private:
    execution_core::ExecutionCoreRuntimeWeakPtr runtime_;
    std::shared_ptr<IContextExecutor> background_executor_;
    FunctionType function_;
    std::shared_ptr<SharedState> state_;
};

template <typename Function>
auto run_in_background(execution_core::ExecutionCoreRuntimeWeakPtr runtime,
                       std::shared_ptr<IExecutor> background_executor,
                       Function&& function) {
    return BackgroundAwaitable<Function>(
        std::move(runtime),
        std::move(background_executor),
        std::forward<Function>(function));
}

template <typename Function>
auto run_in_context_background(execution_core::ExecutionCoreRuntimeWeakPtr runtime,
                               std::shared_ptr<IContextExecutor> background_executor,
                               Function&& function) {
    return ContextBackgroundAwaitable<Function>(
        std::move(runtime),
        std::move(background_executor),
        std::forward<Function>(function));
}

std::shared_ptr<IExecutor> make_thread_pool_executor(std::size_t thread_count = 0);

std::shared_ptr<IContextExecutor> make_context_thread_pool_executor(
    std::size_t thread_count = 0,
    WorkerContextFactory context_factory = {});

} // namespace execution_adapters
