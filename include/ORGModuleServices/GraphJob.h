#pragma once

#include <ORGModuleServices/Async/ArtifactResources.h>
#include <OpenRenderGraph/GraphDesc.h>

#include <exception>
#include <memory>
#include <span>
#include <string>

namespace org::services {

/**
 * @brief The GPU submission set of one compiled-graph execution, for a StateGraph artifact.
 *
 * Submitted once the execution is on its queues. Complete once the GPU finished it: polling it retires the graph's
 * finished work without blocking (CompiledGraph::TryRetire). Failed when the execution was poisoned. Each signal of
 * the execution is one submission, owned by the graph.
 */
std::shared_ptr<const async::GpuSubmissionSet> MakeGpuSubmissionSet(std::shared_ptr<CompiledGraph> graph,
    ExecutionHandle execution);

/**
 * @brief A producer's GPU step: executes `graph` with `bindings` and returns Ready(payload) with the execution's
 * submission set, or a failure.
 *
 * The execution is ordered against every graph execution admitted before it on the same runtime, so a producer
 * whose input came from another graph job needs no CPU wait: the GPU waits. Dependents that require only
 * UploadSubmitted may start, and execute their own graphs, while the GPU still runs this one.
 */
template <class Result>
Result RunGraphJob(const std::shared_ptr<CompiledGraph>& graph, std::span<const CompiledGraph::Binding> bindings,
    async::ArtifactPayload payload, const IHostExecutionData* hostData = nullptr) {
    ExecutionHandle execution;
    try {
        execution = graph->Execute(bindings, hostData);
    } catch (const std::exception& error) {
        return Result::Failure("graph '" + graph->Name() + "': " + error.what());
    }
    if (execution.Poisoned()) return Result::Failure("graph '" + graph->Name() + "': its execution was abandoned");
    return Result::Ready(std::move(payload), MakeGpuSubmissionSet(graph, std::move(execution)));
}

} // namespace org::services
