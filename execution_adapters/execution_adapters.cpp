#include "execution_adapters.hpp"

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace execution_adapters {
namespace {

std::size_t normalize_thread_count(std::size_t thread_count)
{
    if (thread_count != 0) {
        return thread_count;
    }

    thread_count = std::thread::hardware_concurrency();
    if (thread_count == 0) {
        thread_count = 1;
    }

    return thread_count;
}

class ThreadPoolExecutor final : public IExecutor {
public:
    explicit ThreadPoolExecutor(std::size_t thread_count)
    {
        thread_count = normalize_thread_count(thread_count);
        workers_.reserve(thread_count);

        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this]() { worker_loop(); });
        }
    }

    ~ThreadPoolExecutor() override
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }

        cv_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void post(std::function<void()> fn) override
    {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }

            queue_.push(std::move(fn));
        }

        cv_.notify_one();
    }

private:
    void worker_loop()
    {
        while (true) {
            std::function<void()> job;

            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });

                if (stopping_ && queue_.empty()) {
                    return;
                }

                job = std::move(queue_.front());
                queue_.pop();
            }

            job();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
};

class ContextThreadPoolExecutor final : public IContextExecutor {
public:
    ContextThreadPoolExecutor(std::size_t thread_count, WorkerContextFactory context_factory)
        : thread_count_(normalize_thread_count(thread_count))
        , context_factory_(std::move(context_factory))
    {
        workers_.reserve(thread_count_);

        for (std::size_t i = 0; i < thread_count_; ++i) {
            workers_.emplace_back([this, i]() { worker_loop(i); });
        }
    }

    ~ContextThreadPoolExecutor() override
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }

        cv_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void post(std::function<void(WorkerContext&)> fn) override
    {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }

            queue_.push(std::move(fn));
        }

        cv_.notify_one();
    }

    [[nodiscard]] std::size_t thread_count() const noexcept override
    {
        return thread_count_;
    }

private:
    void worker_loop(std::size_t worker_index)
    {
        WorkerContext context;
        context.worker_index = worker_index;

        if (context_factory_) {
            try {
                context.user_context = context_factory_(worker_index);
            } catch (...) {
                context.initialization_exception = std::current_exception();
            }
        }

        while (true) {
            std::function<void(WorkerContext&)> job;

            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });

                if (stopping_ && queue_.empty()) {
                    return;
                }

                job = std::move(queue_.front());
                queue_.pop();
            }

            job(context);
        }
    }

    std::size_t thread_count_{};
    WorkerContextFactory context_factory_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void(WorkerContext&)>> queue_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
};

} // namespace

bool SingleContinuationWaitSet::publish(execution_core::Continuation continuation) noexcept
{
    try {
        std::lock_guard lock(mutex_);
        if (continuation_.has_value()) {
            return false;
        }

        continuation_ = continuation;
        return true;
    } catch (...) {
        std::terminate();
    }
}

std::optional<execution_core::Continuation>
SingleContinuationWaitSet::extract_ready() noexcept
{
    try {
        std::lock_guard lock(mutex_);
        auto result = continuation_;
        continuation_.reset();
        return result;
    } catch (...) {
        std::terminate();
    }
}

void SingleContinuationWaitSet::remove(
    execution_core::TaskId task_id,
    std::uint64_t generation) noexcept
{
    try {
        std::lock_guard lock(mutex_);
        if (!continuation_.has_value()) {
            return;
        }

        if (continuation_->task_id.value == task_id.value &&
            continuation_->generation == generation) {
            continuation_.reset();
        }
    } catch (...) {
        std::terminate();
    }
}

void schedule_wait_set_if_alive(
    execution_core::ExecutionCoreRuntimeWeakPtr runtime,
    const std::shared_ptr<SingleContinuationWaitSet>& wait_set) noexcept
{
    if (!wait_set) {
        return;
    }

    auto runtime_shared = runtime.lock();
    if (!runtime_shared || !runtime_shared->alive()) {
        return;
    }

    auto continuation = wait_set->extract_ready();
    if (!continuation.has_value()) {
        return;
    }

    runtime_shared->scheduler().schedule(*continuation);
    runtime_shared->request_run_ready();
}

void post_if_alive(PostContext context, std::function<void()> fn)
{
    if (!context.valid()) {
        return;
    }

    context.executor->post([context = std::move(context), fn = std::move(fn)]() mutable {
        if (context.alive()) {
            fn();
        }
    });
}

std::shared_ptr<IExecutor> make_thread_pool_executor(std::size_t thread_count)
{
    return std::make_shared<ThreadPoolExecutor>(thread_count);
}

std::shared_ptr<IContextExecutor> make_context_thread_pool_executor(
    std::size_t thread_count,
    WorkerContextFactory context_factory)
{
    return std::make_shared<ContextThreadPoolExecutor>(
        thread_count,
        std::move(context_factory));
}

} // namespace execution_adapters
