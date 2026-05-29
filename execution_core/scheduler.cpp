#include "scheduler.hpp"
#include <exception>

namespace execution_core {

    Scheduler::Scheduler(TaskRegistry& registry) noexcept
        : registry_(registry)
    {
    }

    void Scheduler::schedule(Continuation continuation) {
        if (!registry_.schedule_ready(continuation)) {
            return;
        }

        try {
            std::lock_guard lock(mutex_);
            ready_.push(continuation);
        }
        catch (...) {
            std::terminate();
        }
    }

    void Scheduler::run_ready() {
        for (;;) {
            Continuation continuation;

            {
                std::lock_guard lock(mutex_);

                if (ready_.empty()) {
                    break;
                }

                continuation = ready_.front();
                ready_.pop();
            }

            auto token = registry_.ready_to_running(continuation);
            if (!token) {
                continue;
            }

            try {
                token->handle.resume();
            }
            catch (...) {
                registry_.record_resume_exception(*token, std::current_exception());
            }

            registry_.post_resume_reconcile(*token);
        }
    }
//---------------------------------------
    bool Scheduler::schedule_from_running(Continuation continuation)
    {
        if (!registry_.suspend_to_ready(continuation)) {
            return false;
        }

        try {
            std::lock_guard lock(mutex_);
            ready_.push(continuation);
        }
        catch (...) {
            std::terminate();
        }

        return true;
    }
//-------------------------------------------------
    std::size_t Scheduler::shutdown() noexcept {
        const std::size_t destroyed = registry_.shutdown();

        std::lock_guard lock(mutex_);
        std::queue<Continuation> empty;
        ready_.swap(empty);

        return destroyed;
    }

} // namespace execution_core