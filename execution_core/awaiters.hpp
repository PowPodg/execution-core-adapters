#pragma once

#include "scheduler.hpp"

namespace execution_core {

    struct YieldAwaitable {
        Scheduler& scheduler;

        bool await_ready() const noexcept {
            return false;
        }

        template<typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
            auto& binding = handle.promise().execution_binding;

            Continuation continuation{
                handle,
                binding.task_id,
                binding.generation
            };

            return scheduler.schedule_from_running(continuation);
        }

        void await_resume() const noexcept {}
    };

    inline YieldAwaitable yield(Scheduler& scheduler) noexcept {
        return YieldAwaitable{ scheduler };
    }
//----------------------------------------------
    struct WaitSetAwaitable {
        IContinuationWaitSet& wait_set;

        bool await_ready() const noexcept {
            return false;
        }

        template<typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
            auto& binding = handle.promise().execution_binding;

            if (!binding.registry) {
                return false;
            }

            Continuation continuation{
                handle,
                binding.task_id,
                binding.generation
            };

            return binding.registry->suspend_to_waiting(continuation, wait_set);
        }

        void await_resume() const noexcept {}
    };
//-------------------------------------
    inline WaitSetAwaitable wait_on(IContinuationWaitSet& wait_set) noexcept {
        return WaitSetAwaitable{ wait_set };
    }

} // namespace execution_core
