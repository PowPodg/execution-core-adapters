// execution_core.hpp
#pragma once

#include "task.hpp"
#include "scheduler.hpp"
#include "awaiters.hpp"

namespace execution_core {

    class IExecutionCoreRuntime {
    public:
        virtual ~IExecutionCoreRuntime() = default;
        [[nodiscard]] virtual bool alive() const noexcept = 0;
        [[nodiscard]] virtual Scheduler& scheduler() noexcept = 0;
        virtual void request_run_ready() noexcept = 0;
    };

    using ExecutionCoreRuntimeWeakPtr = std::weak_ptr<IExecutionCoreRuntime>;

} // namespace execution_core