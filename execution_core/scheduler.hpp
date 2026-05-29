#pragma once

#include "task.hpp"

#include <queue>
#include <mutex>


namespace execution_core {

    class Scheduler {
    public:
        explicit Scheduler(TaskRegistry& registry) noexcept;
        void schedule(Continuation continuation);
        void run_ready();

        bool schedule_from_running(Continuation continuation);
        std::size_t shutdown() noexcept;

    private:
        TaskRegistry& registry_;
        std::mutex mutex_;
        std::queue<Continuation> ready_;
    };

} // namespace execution_core
