#pragma once

#include <ORGModuleServices/Async/GraphScheduler.h>
#include <ORGModuleServices/Async/GraphSchedulingLayout.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace org::async {

// A thread pool that implements GraphScheduler. Its threads are grouped in domains (TaskClass::domain), each with its own
// workers and its own unbounded MPMC queue. One lane per domain (TaskClass::lane is always 0).
//
// Dispatch never waits and never runs work inline. A job goes into its domain's queue and a semaphore wakes a worker.
// Delayed jobs are held by the worker that took them, earliest first. Scope waits and Shutdown are for teardown and
// tests, not for steady-state work.
//
// One domain is the parallel domain. ParallelFor's helpers run there, and so do frame-critical jobs
// (DispatchFrameCritical): they have their own queue in that domain, taken first, plus optional dedicated threads at a
// raised priority. Those threads let a frame-critical job start while the parallel workers are starved by other threads.
//
// The default layout has four domains: a coordinator thread, a parallel preparation pool, a serial thread and a small
// blocking pool. A host may pass its own domains in Desc.
class Executor final : public GraphScheduler {
    struct State;

public:
    // The default layout's classes. A host with its own domains uses its own classes.
    static constexpr TaskClass Coordinator{0, 0};
    static constexpr TaskClass Preparation{0, 1};  // the default parallel domain
    static constexpr TaskClass Scene{0, 2};        // one serial thread
    // Blocking work, such as file reads and program builds, kept off the parallel pool.
    static constexpr TaskClass Streaming{0, 3};
    // A frame-critical thread's class in the default layout, used only in ThreadStart. Its domain is the domain count.
    // Jobs reach these threads through DispatchFrameCritical.
    static constexpr TaskClass FrameCriticalThread{0, 4};
    static constexpr std::size_t kDefaultDomains = 4;
    static constexpr unsigned kDefaultStreamingWorkers = 2;
    // The default layout's GraphSchedulingLayout. SchedulingLayout() gives an executor's own.
    static constexpr GraphSchedulingLayout Layout{1, kDefaultDomains, Coordinator, Coordinator};

    struct Statistics {
        std::uint64_t accepted = 0, completed = 0, cancelled = 0, rejected = 0, failed = 0;
        std::uint64_t waitBoosts = 0;  // waits that raised the workers (WaitBoost)
        // Per domain.
        std::vector<std::size_t> queued, active, highWater;
    };

    // Cumulative statistics per ParallelFor name. A reporter takes deltas.
    // blockedNanoseconds is time callers spent waiting on chunks a helper had claimed, which ParallelFor exists to keep small.
    struct ParallelForStatistics {
        std::string name;
        std::uint64_t calls = 0;                     // calls with a_count > 0
        std::uint64_t chunks = 0, helperChunks = 0;  // every chunk, and those a helper ran
        std::uint64_t blocked = 0;                   // calls whose caller waited for a helper's chunk
        std::uint64_t blockedNanoseconds = 0, maxBlockedNanoseconds = 0;  // max since start, or since the last take
        std::uint64_t boosts = 0;                    // helpers raised to their caller's priority
        std::uint64_t idleHelpers = 0;               // helpers that started after every chunk was taken
    };

    // Called on each worker thread before it takes work, with its class and its index within the class.
    // The frame-critical threads get {0, domain count}.
    using ThreadStart = std::function<void(TaskClass, unsigned)>;

    // Get and set the calling thread's priority, in host-defined values where larger is higher.
    // With both set, a helper running a ParallelFor caller's chunks runs at least at the caller's priority and is
    // restored afterwards. Each worker reads its own once after ThreadStart. Nothing else may change a worker's priority.
    struct ThreadPriority {
        std::function<int()> get;
        std::function<void(int)> set;
        // A frame-critical job runs at least at this priority, restored after.
        int frameCritical = 0;
        // Optional, with get and set: a handle to the calling thread that stays valid for the executor's life, and set for a
        // thread by that handle. With them, a thread waiting for the executor's work raises the workers (WaitBoost).
        std::function<std::uintptr_t()> thread;
        std::function<void(std::uintptr_t, int)> setThread;
    };

    // A thread waiting for this executor's work. While it lives, every worker runs at least at the waiting thread's priority,
    // then each goes back to its own. Workers may sit below other threads, and a waiter above them would otherwise wait until
    // the scheduler lends them time. Does nothing on a worker of this executor, or without ThreadPriority::thread and setThread.
    // Scope waits take one.
    class WaitBoost {
    public:
        WaitBoost() = default;
        ~WaitBoost();
        WaitBoost(WaitBoost&& a_other) noexcept;
        WaitBoost& operator=(WaitBoost&&) = delete;
        WaitBoost(const WaitBoost&) = delete;
        WaitBoost& operator=(const WaitBoost&) = delete;

    private:
        friend class Executor;
        std::shared_ptr<State> state;
        int priority = 0;
    };

    struct Desc {
        // Worker threads per domain, indexed by TaskClass::domain. Every domain needs at least one.
        std::vector<unsigned> domainWorkers{1, 2, 1, kDefaultStreamingWorkers};
        // The domain ParallelFor's helpers and frame-critical jobs run in.
        std::uint32_t parallelDomain = Preparation.domain;
        // Dedicated threads that take only frame-critical jobs.
        unsigned frameCriticalWorkers = 0;
        ThreadStart onThreadStart;
        ThreadPriority priority;
    };

    // The default layout with a_preparationWorkers parallel workers.
    static Desc DefaultDesc(unsigned a_preparationWorkers, unsigned a_streamingWorkers = kDefaultStreamingWorkers);

    // Throws std::invalid_argument when a domain has no worker or the parallel domain does not exist.
    explicit Executor(Desc a_desc);
    // The default layout.
    explicit Executor(unsigned preparationWorkers = 2, ThreadStart onThreadStart = {}, ThreadPriority priority = {},
        unsigned frameCriticalWorkers = 0);
    ~Executor() override;
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    [[nodiscard]] Scope CreateScope(std::string_view name) override;
    bool Dispatch(const Scope&, TaskClass, TaskDispatch, std::string_view, Task, TaskTraceMetadata = {}) override;
    bool DispatchAfter(const Scope&, std::chrono::steady_clock::duration, TaskClass, std::string_view, Task) override;

    // A parallel-domain job something waits on every frame. Queued ahead of every other job of the parallel domain and
    // run at least at ThreadPriority::frameCritical. A ParallelFor inside it gets frame-critical helpers. Never runs inline.
    bool DispatchFrameCritical(const Scope& a_scope, std::string_view a_name, Task a_task);

    // Runs a_body(begin, end) over [0, a_count) in chunks of a_grain, on the caller and the parallel workers, and returns
    // once all ran. The caller takes chunks itself and only waits for chunks a worker is running, never for queued work.
    // Chunks are exactly [k * grain, min(count, (k + 1) * grain)), so callers may index results by a_begin / grain.
    // A helper holding a chunk runs at least at the caller's priority. The first exception is rethrown once every
    // claimed chunk finished. It waits, so never call it on a thread that must not wait.
    void ParallelFor(std::string_view a_name, std::size_t a_count, std::size_t a_grain,
        const std::function<void(std::size_t, std::size_t)>& a_body);

    // Any thread about to wait for work of this executor (WaitBoost).
    [[nodiscard]] WaitBoost BoostWorkersForWait() const;

    [[nodiscard]] Statistics GetStatistics() const;
    // Every ParallelFor name seen so far. a_takePeaks resets each maxBlockedNanoseconds once read.
    [[nodiscard]] std::vector<ParallelForStatistics> GetParallelForStatistics(bool a_takePeaks = false) const;

    // This executor's layout: one lane, its domains.
    [[nodiscard]] GraphSchedulingLayout SchedulingLayout() const noexcept;
    [[nodiscard]] std::uint32_t DomainCount() const noexcept;
    // The parallel domain's class.
    [[nodiscard]] TaskClass ParallelClass() const noexcept;
    [[nodiscard]] unsigned Workers(std::uint32_t a_domain) const noexcept;
    [[nodiscard]] unsigned FrameCriticalWorkers() const noexcept;

    // The owner must outlive its callbacks. Calling Shutdown or Wait on its own workers is an error.
    void Shutdown();

private:
    static WaitBoost Boost(const std::shared_ptr<State>& a_state);
    struct ExecutorScope;
    struct Threads;
    std::shared_ptr<State> state;
    std::unique_ptr<Threads> threads;
    Scope parallelScope;  // ParallelFor's helpers
};

} // namespace org::async
