#pragma once

#include <ORGModuleServices/Async/Executor.h>
#include <ORGModuleServices/Async/SerializedTaskPump.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace org::async {

// What a lane runs on.
struct LaneDesc {
    std::string name;
    TaskClass taskClass = Executor::Preparation;
    TaskDispatch dispatch = TaskDispatch::Cpu;
    // Runs when the lane stops by itself: its task was refused, or its drain threw. Empty: the lane stops silently.
    std::function<void()> refused;
};

// Where a pool of lanes runs when its jobs wait for other work on the same executor, such as builds that wait for their
// compiles. The dependency is stated, so the pool never takes every thread the work it waits for needs.
struct WaitingPoolPlan {
    TaskClass taskClass = Executor::Preparation;
    unsigned lanes = 1;
    // The jobs run in the domain of the work they wait for.
    bool shared = false;
};

// The jobs run on runOn and wait for work in waitsOnDomain. When that is runOn's domain and it has two or more workers, the
// pool takes at most half of them, so the work waited for always has a thread. With one worker there, a single lane runs on
// fallback, which must be another domain. When the domains differ, the pool takes every worker of runOn's domain.
WaitingPoolPlan PlanWaitingPool(const Executor& executor, TaskClass runOn, std::uint32_t waitsOnDomain, TaskClass fallback);

// A lane: a_drain runs one pass at a time on an executor. Wakes coalesce, and a wake during a pass runs one more.
// It has its own scope, so Stop drops its queued pass and waits for a running one.
class Lane {
public:
    // The executor must outlive the lane.
    Lane(Executor& a_executor, LaneDesc a_desc, std::function<void()> a_drain);
    // Stops the lane. Never destroy it on the lane itself.
    ~Lane();
    Lane(const Lane&) = delete;
    Lane& operator=(const Lane&) = delete;

    // Any thread. Wakes the lane. False once it stopped.
    bool Wake() { return pump.Notify(); }

    // The lane's pump, for callers that drain inline (TryRunInline) or read its state.
    SerializedTaskPump& Pump() { return pump; }
    const SerializedTaskPump& Pump() const { return pump; }

    const std::string& Name() const { return desc.name; }

    // Teardown only, never on the lane itself. Stops the lane, drops a queued pass and waits for a running one.
    // Later wakes return false.
    void Stop();

private:
    Executor& executor;
    LaneDesc desc;
    Scope scope;
    std::atomic<bool> stopped{false};
    SerializedTaskPump pump;
};

// A lane that lives as long as the process: it is never destroyed. Use it for work that a completion may still wake while
// the process tears down. The executor must never be destroyed either. Returns its pump.
SerializedTaskPump& MakeLane(Executor& a_executor, LaneDesc a_desc, std::function<void()> a_drain);

// A lane with a queue: each posted job runs once, in post order, one at a time.
// Stop drops the queued jobs and waits for a running one.
template <class Job>
class JobLane {
public:
    JobLane(Executor& a_executor, LaneDesc a_desc, std::function<void(Job&)> a_run) :
        run(std::move(a_run)), lane(a_executor, std::move(a_desc), [this] { Drain(); }) {}
    ~JobLane() { Stop(); }
    JobLane(const JobLane&) = delete;
    JobLane& operator=(const JobLane&) = delete;

    // Any thread. Queues a_job. False once stopped, and the job is dropped.
    bool Post(Job a_job) {
        {
            const std::lock_guard lock(mutex);
            if (stopping)
                return false;
            queue.push_back(std::move(a_job));
        }
        return lane.Wake();
    }

    // Teardown only, never on the lane. Drops the queued jobs and waits for a running one.
    void Stop() {
        std::deque<Job> dropped;
        {
            const std::lock_guard lock(mutex);
            stopping = true;
            dropped.swap(queue);
        }
        lane.Stop();
    }

    Lane& Underlying() { return lane; }

private:
    void Drain() {
        for (;;) {
            std::optional<Job> job;
            {
                const std::lock_guard lock(mutex);
                if (stopping || queue.empty())
                    return;
                job.emplace(std::move(queue.front()));
                queue.pop_front();
            }
            run(*job);
        }
    }

    std::mutex mutex;
    std::deque<Job> queue;
    bool stopping = false;
    std::function<void(Job&)> run;
    // Last, so it stops first and its running pass ends before the queue goes.
    Lane lane;
};

} // namespace org::async
