#include <ORGModuleServices/GraphJob.h>

namespace org::services {

std::shared_ptr<const async::GpuSubmissionSet> MakeGpuSubmissionSet(std::shared_ptr<JobGraph> graph,
    ExecutionHandle execution) {
    auto set = std::make_shared<async::GpuSubmissionSet>();
    for (const auto& signal : execution.Signals()) {
        async::GpuQueueSubmission submission;
        submission.timelineOwner = graph;
        submission.value = signal.value;
        set->submissions.push_back(std::move(submission));
    }
    set->isSubmitted = [execution] { return execution.Submitted(); };
    set->isComplete = [graph, execution] {
        if (!execution.Completed() && !execution.Poisoned()) graph->TryRetire();
        return execution.Completed();
    };
    set->isFailed = [execution] { return execution.Poisoned(); };
    set->failure = [name = graph->Name()] { return "graph '" + name + "': its execution was abandoned"; };
    set->describe = [name = graph->Name(), index = execution.Index()] {
        return "graph '" + name + "' execution " + std::to_string(index);
    };
    // The callback runs when the graph retires the execution: at its next execution or retire, or when a poll of
    // isComplete retires it. That is not every transition, so the StateGraph keeps polling as well.
    set->subscribe = [execution](std::function<void()> callback) {
        execution.OnCompleted([callback = std::move(callback)](bool) { callback(); });
    };
    set->completionNotificationsAreAuthoritative = false;
    return set;
}

} // namespace org::services
