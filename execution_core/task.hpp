#pragma once


#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>
#include <cassert>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace execution_core {
    class TaskRegistry;
    struct TaskId {
        std::uint64_t value = 0;
    };
    enum class CoroutineState {
        Suspended,
        Ready,
        Running,
        Waiting,
        FinalSuspended,
        Cancelled,
        Cleaning
    };
    struct Continuation {
        std::coroutine_handle<> handle{};
        TaskId task_id{};
        std::uint64_t generation = 0;
        std::uint64_t wait_epoch = 0;
    };
    struct IContinuationWaitSet;

    struct WaitSetRegistration {
        IContinuationWaitSet* wait_set = nullptr;
        std::uint64_t wait_epoch = 0;
    };

    struct ResumeToken {
        std::coroutine_handle<> handle{};
        TaskId task_id{};
        std::uint64_t generation = 0;
    };
    struct StartToken {
        std::coroutine_handle<> handle{};
        TaskId task_id{};
        std::uint64_t generation = 0;
    };
    struct ExecutionBinding {
        TaskRegistry* registry = nullptr;
        TaskId task_id{};
        std::uint64_t generation = 0;
    };
    struct CoroutineControlBlock {
        CoroutineState state = CoroutineState::Suspended;
        std::uint64_t generation = 0;
        std::uint64_t wait_epoch = 0;
        std::optional<WaitSetRegistration> wait_registration;
        bool starting = false;
    };
    struct StartImmediately {};
    struct StartSuspended {};

    template<typename T = void, typename StartPolicy = StartSuspended>
    class Task {

        static_assert(
            std::is_same_v<StartPolicy, StartImmediately> ||
            std::is_same_v<StartPolicy, StartSuspended>
            );

        static_assert(
            std::is_void_v<T> ||
            (
                std::is_object_v<T> &&
                !std::is_array_v<T> &&
                !std::is_same_v<std::remove_cv_t<T>, std::nullopt_t> &&
                !std::is_same_v<std::remove_cv_t<T>, std::in_place_t> &&
                std::is_destructible_v<T>
                ),
            "execution_core::Task<T>: T must be void or compatible with std::optional<T>"
            );

    private:
        template<typename Promise>

        struct base_promise {
            ExecutionBinding execution_binding{};
            std::exception_ptr exception;

            Task get_return_object() noexcept {
                return Task{
                    std::coroutine_handle<Promise>::from_promise(
                        static_cast<Promise&>(*this)
                    )
                };
            }

            auto initial_suspend() noexcept {
                if constexpr (std::is_same_v<StartPolicy, StartImmediately>) {
                    return std::suspend_never{};
                }
                else {
                    return std::suspend_always{};
                }
            }

            std::suspend_always final_suspend() noexcept {
                return {};
            }

            void unhandled_exception() {
                exception = std::current_exception();
            }
        };

        struct void_promise final : base_promise<void_promise> {
            void return_void() noexcept {}
        };

        template<typename U>
        struct value_promise final : base_promise<value_promise<U>> {
            std::optional<U> result;

            template<typename V>
            void return_value(V&& value) {
                result.emplace(std::forward<V>(value));
            }
        };

    public:
        using promise_type = std::conditional_t<
            std::is_void_v<T>,
            void_promise,
            value_promise<T>
        >;

        using handle_type = std::coroutine_handle<promise_type>;

    public:
        Task() noexcept = default;

        explicit Task(handle_type handle) noexcept
            : handle_(handle)
        {
            if constexpr (std::is_same_v<StartPolicy, StartImmediately>) {
                started_ = true;
            }
        }

        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;

        Task(Task&& other) noexcept
            : handle_(std::exchange(other.handle_, {})),
              started_(std::exchange(other.started_, false))
        {
        }

        Task& operator=(Task&& other) noexcept {
            if (this != &other) {
                destroy();
                handle_ = std::exchange(other.handle_, {});
                started_ = std::exchange(other.started_, false);
            }
            return *this;
        }

        ~Task() {
            destroy();
        }

        void start() {
            if constexpr (std::is_same_v<StartPolicy, StartSuspended>) {
                assert(handle_);
                assert(!started_);

                started_ = true;
                handle_.resume();
            }
        }


        void rethrow_if_exception() {
            if (handle_ && handle_.promise().exception) {
                std::rethrow_exception(handle_.promise().exception);
            }
        }


        explicit operator bool() const noexcept {
            return static_cast<bool>(handle_);
        }

        template<typename U = T>
            requires (!std::is_void_v<U>)
        U& result()& {
            assert(handle_);
            assert(handle_.done());

            rethrow_if_exception();

            assert(handle_.promise().result.has_value());
            return *handle_.promise().result;
        }

        template<typename U = T>
            requires (!std::is_void_v<U>)
        const U& result() const& {
            assert(handle_);
            assert(handle_.done());

            if (handle_.promise().exception) {
                std::rethrow_exception(handle_.promise().exception);
            }

            assert(handle_.promise().result.has_value());
            return *handle_.promise().result;
        }

        template<typename U = T>
            requires (!std::is_void_v<U>)
        U&& result()&& {
            assert(handle_);
            assert(handle_.done());

            rethrow_if_exception();

            assert(handle_.promise().result.has_value());
            return std::move(*handle_.promise().result);
        }

    private:
        friend class TaskRegistry;

        template<typename> friend struct TaskRecord;

        handle_type internal_handle() const noexcept {
            return handle_;
        }

        bool internal_started() const noexcept {
            return started_;
        }

        bool internal_done() const noexcept {
            return handle_ && handle_.done();
        }

        void destroy() noexcept {
            if (handle_) {
                handle_.destroy();
                handle_ = {};  
                started_ = false; 
            }
        }

    private:
        bool started_ = false;
        handle_type handle_{};
    };
    struct ITaskRecord {
        virtual ~ITaskRecord() = default;

        virtual void start() = 0;
        virtual bool done() const noexcept = 0;
        virtual void rethrow_if_exception() = 0;
        virtual void destroy_task() noexcept = 0;

        virtual bool matches_handle(std::coroutine_handle<> handle) const noexcept = 0;
    };
    template<typename T>
    struct TaskRecord final : ITaskRecord {
        Task<T, StartSuspended> task;

        explicit TaskRecord(Task<T, StartSuspended>&& task_) noexcept
            : task(std::move(task_))
        {
        }

        void start() override {
            task.start();
        }

        bool done() const noexcept override {
            return task.internal_done();
        }

        void rethrow_if_exception() override {
            task.rethrow_if_exception();
        }

        void destroy_task() noexcept override {
            task = {};
        }

        bool matches_handle(std::coroutine_handle<> handle) const noexcept override {
            return task.internal_handle().address() == handle.address();
        }

#ifdef EXECUTION_CORE_TEST_HOOKS
        std::coroutine_handle<> test_handle() const noexcept {
            return task.internal_handle();
        }

        void test_set_execution_binding(ExecutionBinding binding) noexcept {
            task.internal_handle().promise().execution_binding = binding;
        }
#endif
    };
    struct TaskEntry {
        CoroutineControlBlock control;
        std::unique_ptr<ITaskRecord> record;
        std::exception_ptr start_exception{};
        std::exception_ptr resume_exception{};
    };
    struct IContinuationWaitSet {
        virtual ~IContinuationWaitSet() = default;

        virtual bool publish(Continuation continuation) noexcept = 0;

        virtual std::optional<Continuation> extract_ready() noexcept = 0;

        virtual void remove(TaskId task_id, std::uint64_t generation) noexcept = 0;
    };
    class TaskRegistry {
    public:
        TaskRegistry() = default;
        TaskRegistry(const TaskRegistry&) = delete;
        TaskRegistry& operator=(const TaskRegistry&) = delete;
        template<typename T>
        TaskId spawn(Task<T, StartSuspended>&& task);
        std::optional<ResumeToken> ready_to_running(Continuation continuation) noexcept;
        void post_resume_reconcile(ResumeToken token) noexcept;
        bool schedule_ready(Continuation continuation) noexcept;
        void record_resume_exception(ResumeToken token, std::exception_ptr ex) noexcept;
        bool suspend_to_ready(Continuation continuation) noexcept;
        bool suspend_to_waiting(Continuation continuation,IContinuationWaitSet& wait_set) noexcept;
        bool cancel_waiting(Continuation continuation) noexcept;
        std::size_t cancel_all_waiting() noexcept;
        std::size_t shutdown() noexcept;
        std::size_t cleanup_completed() noexcept;
        std::vector<std::exception_ptr> take_completed_exceptions();
#ifdef EXECUTION_CORE_TEST_HOOKS
        void test_post_start_reconcile(StartToken token) noexcept;

        void test_record_start_exception(
            StartToken token,
            std::exception_ptr ex
        ) noexcept;

        std::unordered_map<std::uint64_t, TaskEntry>& test_entries() noexcept;
#endif
    private:
        std::mutex mutex_;
        void post_start_reconcile(StartToken token) noexcept;
        void record_start_exception(StartToken token, std::exception_ptr ex) noexcept;
        void store_completed_exception(std::exception_ptr ex) noexcept;
        std::vector<std::exception_ptr> completed_exceptions_;
        bool shutdown_requested_ = false;
        std::uint64_t next_task_id_ = 1;
        std::unordered_map<std::uint64_t, TaskEntry> entries_;
    };

#ifdef EXECUTION_CORE_TEST_HOOKS
    inline void TaskRegistry::test_post_start_reconcile(StartToken token) noexcept {
        post_start_reconcile(token);
    }

    inline void TaskRegistry::test_record_start_exception(
        StartToken token,
        std::exception_ptr ex
    ) noexcept {
        record_start_exception(token, std::move(ex));
    }

    inline std::unordered_map<std::uint64_t, TaskEntry>&
        TaskRegistry::test_entries() noexcept {
        return entries_;
    }
#endif
    template<typename T>
    TaskId TaskRegistry::spawn(Task<T, StartSuspended>&& task) {
        {
            std::lock_guard lock(mutex_);

            if (shutdown_requested_) {
                return {};
            }
        }

        auto record = std::make_unique<TaskRecord<T>>(std::move(task));

        assert(record->task.internal_handle());
        assert(!record->task.internal_started());

        auto handle = record->task.internal_handle();
        auto* typed_record = record.get();

        TaskId id{};
        auto generation = std::uint64_t{ 1 };
        StartToken token{};

        {
            std::lock_guard lock(mutex_);
            if (shutdown_requested_) {
                return {};
            }

            id = TaskId{ next_task_id_++ };

            handle.promise().execution_binding = ExecutionBinding{
                this,
                id,
                generation
            };

            TaskEntry entry;
            entry.control.state = CoroutineState::Running;
            entry.control.generation = generation;
            entry.control.starting = true;
            entry.record = std::move(record);

            token = StartToken{
                handle,
                id,
                generation
            };

            entries_.emplace(id.value, std::move(entry));
        }

        try {
            typed_record->start();
        }
        catch (...) {
            record_start_exception(token, std::current_exception());
        }

        post_start_reconcile(token);

        return id;
    }

} // namespace execution_core
