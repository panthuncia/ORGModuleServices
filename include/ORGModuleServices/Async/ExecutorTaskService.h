#pragma once

#include <ORGModuleServices/Async/Executor.h>

#include <Render/Runtime/ITaskService.h>

#include <functional>
#include <memory>
#include <string_view>

namespace org::async {

// OpenRenderGraph's task service (org::runtime::ITaskService) over an Executor, so the graph starts no threads of its own.
//
// Frame-critical work without a delay goes to the executor's frame-critical queue. Everything else, streaming and
// background work and delayed frame-critical work, goes to the executor's parallel domain. ParallelFor runs on the
// executor's ParallelFor, with the caller taking part.
//
// Its scopes count their tasks, queued or running. Cancel drops those not yet started, and Wait returns once every one
// has left. A task leaves when the executor drops it, run or not.
struct ExecutorTaskServiceDesc {
    // Called on the worker when a task throws, with "<task name> threw: <what>". The task then counts as finished.
    // Empty: the exception goes to the executor, which counts the task as failed.
    std::function<void(std::string_view)> onTaskError;
};

// The service and the executor must both outlive every task the service took. A host that never shuts its executor
// down keeps the service for the process's life too.
[[nodiscard]] std::shared_ptr<runtime::ITaskService> CreateExecutorTaskService(Executor& a_executor,
    ExecutorTaskServiceDesc a_desc = {});

} // namespace org::async
