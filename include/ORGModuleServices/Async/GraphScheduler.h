#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace org::async {

// Host-defined classification IDs. The dependency graph does not own the host's
// domain inventory, worker pool, or priority policy.
struct TaskClass { std::uint32_t lane = 0, domain = 0; };
enum class TaskDispatch { Controlled, Cpu };
enum class TaskTraceEventID : std::uint8_t {
    Queued, Admitted, Started, Completed, Cancelled, Rejected, Resubmitted
};
struct TaskTraceMetadata {
    std::uint64_t taskKind = 0, correlationID = 0, admissionKey = 0;
    std::uint8_t workClass = 0, schedulingReason = 0, admissionGroup = 0;
};
struct TaskTraceEvent {
    TaskTraceEventID event = TaskTraceEventID::Queued;
    TaskTraceMetadata metadata;
    std::uint64_t taskID = 0, queueWaitMicros = 0, executionMicros = 0, queuedDepth = 0;
    std::uint32_t activeCount = 0, domain = 0, lane = 0;
    std::uint8_t outcome = 0;
};
using TaskTraceCallback = void (*)(void*, const TaskTraceEvent&) noexcept;

// Owning cancellation query: graph producers may copy it into a continuation.
// An empty context is not cancelled. Host adapters must not capture borrowed
// scheduler callback arguments in this function.
struct TaskContext {
    std::function<bool()> stopRequested;
    [[nodiscard]] bool StopRequested() const { return stopRequested && stopRequested(); }
};

class TaskScope {
public:
    virtual ~TaskScope() = default;
    [[nodiscard]] virtual bool StopRequested() const noexcept = 0;
    virtual void Cancel() noexcept = 0;
    virtual void Wait() const = 0;
    void CancelAndWait() { Cancel(); Wait(); }
};
using Scope = std::shared_ptr<TaskScope>;

class GraphScheduler {
public:
    using Task = std::function<void(const TaskContext&)>;
    virtual ~GraphScheduler() = default;
    [[nodiscard]] virtual Scope CreateScope(std::string_view name) = 0;

    // Never run a submitted task inline. False means no execution was accepted.
    // Implementations must reject foreign scopes and retain accepted tasks and
    // their scopes until completion/cancellation. Wait includes delayed work.
    virtual bool Dispatch(const Scope&, TaskClass, TaskDispatch, std::string_view,
        Task, TaskTraceMetadata = {}) = 0;
    virtual bool DispatchAfter(const Scope&, std::chrono::steady_clock::duration,
        TaskClass, std::string_view, Task) = 0;

    // Optional scheduler tracing. Removal must quiesce callbacks before returning.
    virtual bool InstallTaskTraceSink(void*, TaskTraceCallback) noexcept { return false; }
    virtual void RemoveTaskTraceSink(void*) noexcept {}

    template <class Lane, class Domain>
    bool Submit(const Scope& scope, Lane lane, Domain domain, std::string_view name,
        Task task, TaskTraceMetadata trace = {}) {
        return Dispatch(scope, {static_cast<std::uint32_t>(lane), static_cast<std::uint32_t>(domain)},
            TaskDispatch::Controlled, name, std::move(task), trace);
    }
    template <class Lane, class Domain>
    bool SubmitCpu(const Scope& scope, Lane lane, Domain domain, std::string_view name,
        Task task, TaskTraceMetadata trace = {}) {
        return Dispatch(scope, {static_cast<std::uint32_t>(lane), static_cast<std::uint32_t>(domain)},
            TaskDispatch::Cpu, name, std::move(task), trace);
    }
    template <class Lane, class Domain>
    bool ScheduleAfter(const Scope& scope, std::chrono::steady_clock::duration delay,
        Lane lane, Domain domain, std::string_view name, Task task) {
        return DispatchAfter(scope, delay,
            {static_cast<std::uint32_t>(lane), static_cast<std::uint32_t>(domain)}, name, std::move(task));
    }
};

} // namespace org::async
