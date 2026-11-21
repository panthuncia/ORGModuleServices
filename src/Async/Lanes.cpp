// Lanes: serial passes and job queues on an Executor.
#include <ORGModuleServices/Async/Lanes.h>

namespace org::async {

Lane::Lane(Executor& a_executor, LaneDesc a_desc, std::function<void()> a_drain) :
    executor(a_executor), desc(std::move(a_desc)), scope(executor.CreateScope(desc.name)) {
    pump.Configure(
        [this](SerializedTaskPump::Task a_task) {
            return executor.Dispatch(scope, desc.taskClass, desc.dispatch, desc.name,
                [task = std::move(a_task)](const TaskContext&) { task(); });
        },
        std::move(a_drain),
        [this] {
            if (stopped.load(std::memory_order_acquire))
                return;
            if (desc.refused)
                desc.refused();
        });
}

Lane::~Lane() {
    Stop();
}

void Lane::Stop() {
    if (stopped.exchange(true, std::memory_order_acq_rel))
        return;
    pump.Stop();
    scope->CancelAndWait();
}

SerializedTaskPump& MakeLane(Executor& a_executor, LaneDesc a_desc, std::function<void()> a_drain) {
    // Never destroyed: a completion may still wake it while the process tears down.
    auto* lane = new Lane(a_executor, std::move(a_desc), std::move(a_drain));
    return lane->Pump();
}

} // namespace org::async
