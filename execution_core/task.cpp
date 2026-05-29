
#include "task.hpp"

namespace execution_core {
    std::optional<ResumeToken>
        TaskRegistry::ready_to_running(Continuation continuation) noexcept {
        std::lock_guard lock(mutex_);

        if (shutdown_requested_) {
            return std::nullopt;
        }

        auto it = entries_.find(continuation.task_id.value);
        if (it == entries_.end()) {
            return std::nullopt;
        }

        TaskEntry& entry = it->second;

        if (!entry.record) {
            return std::nullopt;
        }

        if (entry.control.generation != continuation.generation) {
            return std::nullopt;
        }

        if (entry.control.state != CoroutineState::Ready) {
            return std::nullopt;
        }

        if (entry.control.starting) {
            return std::nullopt;
        }

        if (!continuation.handle) {
            return std::nullopt;
        }

        if (!entry.record->matches_handle(continuation.handle)) {
            return std::nullopt;
        }

        if (continuation.handle.done()) {
            return std::nullopt;
        }

        entry.control.state = CoroutineState::Running;

        return ResumeToken{
            continuation.handle,
            continuation.task_id,
            continuation.generation
        };
    }

    void TaskRegistry::post_resume_reconcile(ResumeToken token) noexcept {
        std::lock_guard lock(mutex_);

        auto it = entries_.find(token.task_id.value);
        if (it == entries_.end()) {
            return;
        }

        TaskEntry& entry = it->second;

        if (!entry.record) {
            return;
        }

        if (entry.control.generation != token.generation) {
            return;
        }

        if (!token.handle) {
            return;
        }

        if (!entry.record->matches_handle(token.handle)) {
            return;
        }

        if (entry.control.state == CoroutineState::Running) {
            if (token.handle.done()) {
                entry.control.state = CoroutineState::FinalSuspended;
            }
            else {
                entry.control.state = CoroutineState::Suspended;
            }
        }
    }


    bool TaskRegistry::schedule_ready(Continuation continuation) noexcept {
        std::lock_guard lock(mutex_);

        if (shutdown_requested_) {
            return false;
        }

        auto it = entries_.find(continuation.task_id.value);
        if (it == entries_.end()) {
            return false;
        }

        TaskEntry& entry = it->second;

        if (!entry.record) {
            return false;
        }

        if (entry.control.generation != continuation.generation) {
            return false;
        }

        if (!continuation.handle) {
            return false;
        }

        if (!entry.record->matches_handle(continuation.handle)) {
            return false;
        }

        if (entry.control.state == CoroutineState::Waiting) {
            if (entry.control.wait_epoch != continuation.wait_epoch) {
                return false;
            }

            if (!entry.control.wait_registration.has_value()) {
                return false;
            }

            if (entry.control.wait_registration->wait_epoch != continuation.wait_epoch) {
                return false;
            }

            entry.control.wait_registration.reset();
            entry.control.state = CoroutineState::Ready;
            return true;
        }

        if (entry.control.state == CoroutineState::Suspended) {
            if (continuation.wait_epoch != 0) {
                return false;
            }

            entry.control.state = CoroutineState::Ready;
            return true;
        }

        return false;
    }


    void TaskRegistry::record_resume_exception(ResumeToken token,std::exception_ptr ex) noexcept {
        std::lock_guard lock(mutex_);

        auto it = entries_.find(token.task_id.value);
        if (it == entries_.end()) {
            return;
        }

        TaskEntry& entry = it->second;

        if (!entry.record) {
            return;
        }

        if (entry.control.generation != token.generation) {
            return;
        }

        if (!token.handle) {
            return;
        }

        if (!entry.record->matches_handle(token.handle)) {
            return;
        }

        entry.resume_exception = std::move(ex);
    }

    void TaskRegistry::store_completed_exception(std::exception_ptr ex) noexcept {
        if (!ex) {
            return;
        }

        try {
            std::lock_guard lock(mutex_);
            completed_exceptions_.push_back(std::move(ex));
        }
        catch (...) {
            std::terminate();
        }
    }

    std::vector<std::exception_ptr> TaskRegistry::take_completed_exceptions() {
        std::vector<std::exception_ptr> result;

        std::lock_guard lock(mutex_);
        result.swap(completed_exceptions_);

        return result;
    }

    bool TaskRegistry::suspend_to_ready(Continuation continuation) noexcept {
        std::lock_guard lock(mutex_);

        if (shutdown_requested_) {
            return false;
        }

        auto it = entries_.find(continuation.task_id.value);
        if (it == entries_.end()) {
            return false;
        }

        TaskEntry& entry = it->second;

        if (!entry.record) {
            return false;
        }

        if (entry.control.generation != continuation.generation) {
            return false;
        }

        if (entry.control.state != CoroutineState::Running) {
            return false;
        }

        if (!continuation.handle) {
            return false;
        }

        if (!entry.record->matches_handle(continuation.handle)) {
            return false;
        }

        if (continuation.handle.done()) {
            return false;
        }

        entry.control.state = CoroutineState::Ready;
        return true;
    }
//-----------------------------------------------------------------------------
    bool TaskRegistry::suspend_to_waiting(
        Continuation continuation,
        IContinuationWaitSet& wait_set
    ) noexcept {
        std::lock_guard lock(mutex_);

        if (shutdown_requested_) {
            return false;
        }

        auto it = entries_.find(continuation.task_id.value);
        if (it == entries_.end()) {
            return false;
        }

        TaskEntry& entry = it->second;

        if (!entry.record) {
            return false;
        }

        if (entry.control.generation != continuation.generation) {
            return false;
        }

        if (entry.control.state != CoroutineState::Running) {
            return false;
        }

        if (entry.control.wait_registration.has_value()) {
            return false;
        }

        if (!continuation.handle) {
            return false;
        }

        if (!entry.record->matches_handle(continuation.handle)) {
            return false;
        }

        if (continuation.handle.done()) {
            return false;
        }

        const std::uint64_t next_wait_epoch = entry.control.wait_epoch + 1;

        continuation.wait_epoch = next_wait_epoch;

        if (!wait_set.publish(continuation)) {
            return false;
        }

        entry.control.wait_epoch = next_wait_epoch;
        entry.control.wait_registration = WaitSetRegistration{
            &wait_set,
            next_wait_epoch
        };
        entry.control.state = CoroutineState::Waiting;

        return true;
    }
//----------------------------------------------------
    bool TaskRegistry::cancel_waiting(Continuation continuation) noexcept {
        std::unique_ptr<ITaskRecord> record;
        IContinuationWaitSet* wait_set = nullptr;
        TaskId task_id{};
        std::uint64_t generation = 0;

        {
            std::lock_guard lock(mutex_);

            auto it = entries_.find(continuation.task_id.value);
            if (it == entries_.end()) {
                return false;
            }

            TaskEntry& entry = it->second;

            if (!entry.record) {
                return false;
            }

            if (entry.control.generation != continuation.generation) {
                return false;
            }

            if (entry.control.state != CoroutineState::Waiting) {
                return false;
            }

            if (!continuation.handle) {
                return false;
            }

            if (!entry.record->matches_handle(continuation.handle)) {
                return false;
            }

            if (entry.control.wait_epoch != continuation.wait_epoch) {
                return false;
            }

            if (!entry.control.wait_registration.has_value()) {
                return false;
            }

            if (entry.control.wait_registration->wait_epoch != continuation.wait_epoch) {
                return false;
            }

            wait_set = entry.control.wait_registration->wait_set;
            if (!wait_set) {
                return false;
            }

            task_id = continuation.task_id;
            generation = continuation.generation;

            entry.control.state = CoroutineState::Cancelled;
            entry.control.wait_registration.reset();
            entry.control.state = CoroutineState::Cleaning;

            record = std::move(entry.record);
            entries_.erase(it);
        }

        wait_set->remove(task_id, generation);

        if (record) {
            record->destroy_task();
        }

        return true;
    }
//-----------------------------------------------------------------------
    std::size_t TaskRegistry::cancel_all_waiting() noexcept {
        std::size_t count = 0;

        for (;;) {
            std::unique_ptr<ITaskRecord> record;
            IContinuationWaitSet* wait_set = nullptr;
            TaskId task_id{};
            std::uint64_t generation = 0;

            {
                std::lock_guard lock(mutex_);

                auto it = entries_.end();

                for (auto current = entries_.begin(); current != entries_.end(); ++current) {
                    if (current->second.control.state == CoroutineState::Waiting) {
                        it = current;
                        break;
                    }
                }

                if (it == entries_.end()) {
                    break;
                }

                TaskEntry& entry = it->second;

                if (!entry.record) {
                    entries_.erase(it);
                    continue;
                }

                if (entry.control.wait_registration.has_value()) {
                    wait_set = entry.control.wait_registration->wait_set;
                }

                task_id = TaskId{ it->first };
                generation = entry.control.generation;

                entry.control.state = CoroutineState::Cancelled;
                entry.control.wait_registration.reset();
                entry.control.state = CoroutineState::Cleaning;

                record = std::move(entry.record);
                entries_.erase(it);
            }

            if (wait_set) {
                wait_set->remove(task_id, generation);
            }

            if (record) {
                record->destroy_task();
            }

            ++count;
        }

        return count;
    }
//------------------------------------------------------------
    std::size_t TaskRegistry::shutdown() noexcept {
        struct PendingWaitRemoval {
            IContinuationWaitSet* wait_set = nullptr;
            TaskId task_id{};
            std::uint64_t generation = 0;
        };

        struct PendingDestroy {
            std::unique_ptr<ITaskRecord> record;
            bool final_suspended = false;
            std::exception_ptr start_exception{};
            std::exception_ptr resume_exception{};
        };

        std::vector<PendingWaitRemoval> wait_removals;
        std::vector<PendingDestroy> destroys;

        {
            std::lock_guard lock(mutex_);

            shutdown_requested_ = true;

            for (auto it = entries_.begin(); it != entries_.end();) {
                TaskEntry& entry = it->second;

                if (entry.control.starting) {
                    if (entry.control.wait_registration.has_value()) {
                        if (entry.control.wait_registration->wait_set) {
                            wait_removals.push_back(PendingWaitRemoval{
                                entry.control.wait_registration->wait_set,
                                TaskId{ it->first },
                                entry.control.generation
                                });
                        }

                        entry.control.wait_registration.reset();
                    }

                    if (entry.control.state != CoroutineState::Running) {
                        entry.control.state = CoroutineState::Cancelled;
                    }

                    ++it;
                    continue;
                }

                if (entry.control.state == CoroutineState::Running) {
                    ++it;
                    continue;
                }

                if (entry.control.wait_registration.has_value()) {
                    if (entry.control.wait_registration->wait_set) {
                        wait_removals.push_back(PendingWaitRemoval{
                            entry.control.wait_registration->wait_set,
                            TaskId{ it->first },
                            entry.control.generation
                            });
                    }

                    entry.control.wait_registration.reset();
                }

                const bool final_suspended =
                    entry.control.state == CoroutineState::FinalSuspended;

                if (!final_suspended) {
                    entry.control.state = CoroutineState::Cancelled;
                }

                entry.control.state = CoroutineState::Cleaning;

                if (entry.record) {
                    destroys.push_back(PendingDestroy{
                        std::move(entry.record),
                        final_suspended,
                        std::move(entry.start_exception),
                        std::move(entry.resume_exception)
                        });
                }

                it = entries_.erase(it);
            }
        }

        for (auto& removal : wait_removals) {
            removal.wait_set->remove(removal.task_id, removal.generation);
        }

        for (auto& pending : destroys) {
            if (pending.final_suspended) {
                store_completed_exception(std::move(pending.start_exception));
                store_completed_exception(std::move(pending.resume_exception));
            }

            if (pending.record) {
                if (pending.final_suspended && pending.record->done()) {
                    try {
                        pending.record->rethrow_if_exception();
                    }
                    catch (...) {
                        store_completed_exception(std::current_exception());
                    }
                }

                pending.record->destroy_task();
            }
        }

        return destroys.size();
    }

    std::size_t TaskRegistry::cleanup_completed() noexcept {
        std::size_t count = 0;

        for (;;) {
            std::unique_ptr<ITaskRecord> record;
            std::exception_ptr start_exception;
            std::exception_ptr resume_exception;
            bool final_suspended = false;

            {
                std::lock_guard lock(mutex_);

                auto it = entries_.end();

                for (auto current = entries_.begin(); current != entries_.end(); ++current) {
                    const auto& control = current->second.control;
                    const bool cleanup_state =
                        control.state == CoroutineState::FinalSuspended ||
                        control.state == CoroutineState::Cancelled;

                    if (cleanup_state && !control.starting && !control.wait_registration.has_value()) {
                        it = current;
                        break;
                    }
                }

                if (it == entries_.end()) {
                    break;
                }

                final_suspended =
                    it->second.control.state == CoroutineState::FinalSuspended;

                it->second.control.state = CoroutineState::Cleaning;
                start_exception = std::move(it->second.start_exception);
                resume_exception = std::move(it->second.resume_exception);
                record = std::move(it->second.record);
                entries_.erase(it);
            }

            if (final_suspended) {
                store_completed_exception(std::move(start_exception));
                store_completed_exception(std::move(resume_exception));
            }

            if (record) {
                if (final_suspended && record->done()) {
                    try {
                        record->rethrow_if_exception();
                    }
                    catch (...) {
                        store_completed_exception(std::current_exception());
                    }
                }

                record->destroy_task();
            }

            ++count;
        }

        return count;
    }


    void TaskRegistry::post_start_reconcile(StartToken token) noexcept {
        std::lock_guard lock(mutex_);

        auto it = entries_.find(token.task_id.value);
        if (it == entries_.end()) {
            return;
        }

        TaskEntry& entry = it->second;

        if (!entry.record) {
            return;
        }

        if (entry.control.generation != token.generation) {
            return;
        }

        if (!token.handle) {
            return;
        }

        if (!entry.record->matches_handle(token.handle)) {
            return;
        }

        if (entry.control.state != CoroutineState::Running) {
            entry.control.starting = false;
            return;
        }

        entry.control.starting = false;

        if (token.handle.done()) {
            entry.control.state = CoroutineState::FinalSuspended;
        }
        else {
            entry.control.state = CoroutineState::Suspended;
        }
    }

    void TaskRegistry::record_start_exception(StartToken token, std::exception_ptr ex) noexcept
    {
        std::lock_guard lock(mutex_);

        auto it = entries_.find(token.task_id.value);
        if (it == entries_.end()) {
            return;
        }

        TaskEntry& entry = it->second;

        if (!entry.record) {
            return;
        }

        if (entry.control.generation != token.generation) {
            return;
        }

        if (!token.handle) {
            return;
        }

        if (!entry.record->matches_handle(token.handle)) {
            return;
        }

        entry.start_exception = std::move(ex);
    }


}