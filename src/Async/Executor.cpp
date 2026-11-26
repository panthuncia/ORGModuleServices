// Executor: per-domain job queues, worker threads, timers, scopes and ParallelFor.
#include <ORGModuleServices/Async/Executor.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <exception>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_unordered_map.h>

namespace org::async {

namespace {

thread_local const void* currentExecutor = nullptr;
// A worker's current priority, read once after ThreadStart and kept up to date by boosts, so nothing queries per chunk.
thread_local int threadPriority = 0;
// The worker runs a frame-critical job, so its ParallelFor's helpers are frame-critical too.
thread_local bool inFrameCritical = false;
// The worker's entry in its executor's boost list (State::Worker), or null.
thread_local void* currentWorker = nullptr;

template <class T>
void RaiseTo(std::atomic<T>& a_max, T a_value) {
    for (T seen = a_max.load(std::memory_order_relaxed);
         a_value > seen && !a_max.compare_exchange_weak(seen, a_value, std::memory_order_relaxed);) {}
}

} // namespace

struct Executor::State {
    struct Job {
        std::chrono::steady_clock::time_point ready;
        std::shared_ptr<ExecutorScope> scope;
        Task task;
        bool frameCritical = false;  // DispatchFrameCritical's
    };

    // Unbounded and lock-free for producers: any thread pushes, every worker of the domain pops.
    struct Queue {
        tbb::concurrent_queue<Job> jobs;
        std::atomic<std::size_t> size{0};  // pushed and not yet popped, for statistics
        void Push(Job&& a_job) {
            size.fetch_add(1, std::memory_order_relaxed);
            jobs.push(std::move(a_job));
        }
        bool Pop(Job& a_job) {
            if (!jobs.try_pop(a_job))
                return false;
            size.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
        std::size_t Size() const { return size.load(std::memory_order_relaxed); }
    };

    struct Domain {
        Queue ring;
        Queue frameCritical;  // taken before the ring's
        std::counting_semaphore<> available{0};  // one release per pushed job and per wake-up
        unsigned workers = 0;
        std::atomic<std::size_t> active{0}, highWater{0};
    };

    // One per ParallelFor name. Never removed, so a helper outliving its caller still counts into it.
    struct ParallelForEntry {
        std::string name;
        std::atomic<std::uint64_t> calls{0}, chunks{0}, helperChunks{0}, blocked{0}, blockedNanoseconds{0},
            maxBlockedNanoseconds{0}, boosts{0}, idleHelpers{0};
    };
    struct NameHash {
        using transparent_key_equal = std::equal_to<>;  // lookup by string_view, so no string per call
        std::size_t operator()(std::string_view a_name) const noexcept { return std::hash<std::string_view>{}(a_name); }
    };

    // A worker whose priority a waiting thread may raise (WaitBoost). own: what the worker set for itself (threadPriority).
    // applied: what it runs at, at least the waiters' boost. Both change under the mutex.
    struct Worker {
        std::mutex mutex;
        std::uintptr_t thread = 0;
        int own = 0, applied = 0;
    };

    std::atomic_bool stopping{false};
    std::vector<std::unique_ptr<Domain>> domains;
    std::uint32_t parallelDomain = 0;
    std::atomic<std::uint64_t> accepted{0}, completed{0}, cancelled{0}, rejected{0}, failed{0};
    ThreadPriority priority;  // both set, or ParallelFor leaves priorities alone
    // The frame-critical threads, which take only the frame-critical queue, one token per job.
    unsigned frameCriticalWorkers = 0;
    std::counting_semaphore<> frameCriticalAvailable{0};
    // Lock-free lookup and insertion; nodes never move.
    tbb::concurrent_unordered_map<std::string, std::unique_ptr<ParallelForEntry>, NameHash, std::equal_to<>> parallelFor;
    // Waits that raise the workers: the waiting threads' priorities, and the highest of them (INT_MIN with none). Workers
    // join the list at start. The boost mutex is taken before a worker's.
    std::mutex boostMutex;
    std::vector<int> waiters;
    std::atomic<int> boost{INT_MIN};
    std::vector<std::unique_ptr<Worker>> boostable;
    std::atomic<std::uint64_t> waitBoosts{0};

    ParallelForEntry& Entry(std::string_view a_name) {
        if (const auto found = parallelFor.find(a_name); found != parallelFor.end())
            return *found->second;
        auto entry = std::make_unique<ParallelForEntry>();
        entry->name = a_name;
        return *parallelFor.emplace(std::string(a_name), std::move(entry)).first->second;  // a racing insert of the name wins
    }
    bool Boosts() const { return priority.get && priority.set; }
    bool BoostsForWaits() const { return Boosts() && priority.thread && priority.setThread; }

    // A worker starting: it reads its priority and joins the boost list.
    void StartWorker() {
        if (!Boosts())
            return;
        threadPriority = priority.get();  // after ThreadStart set it
        if (!BoostsForWaits())
            return;
        auto worker = std::make_unique<Worker>();
        worker->thread = priority.thread();
        worker->own = worker->applied = threadPriority;
        currentWorker = worker.get();
        std::lock_guard boostLock(boostMutex);
        std::lock_guard lock(worker->mutex);
        if (const int applied = (std::max)(worker->own, boost.load(std::memory_order_relaxed)); applied != worker->applied) {
            priority.set(applied);
            worker->applied = applied;
        }
        boostable.push_back(std::move(worker));
    }
    // The calling worker sets its own priority. It runs at least at a waiter's boost meanwhile.
    void SetOwn(int a_priority) {
        threadPriority = a_priority;
        auto* worker = static_cast<Worker*>(currentWorker);
        if (!worker) {
            priority.set(a_priority);
            return;
        }
        std::lock_guard lock(worker->mutex);
        worker->own = a_priority;
        if (const int applied = (std::max)(a_priority, boost.load(std::memory_order_relaxed)); applied != worker->applied) {
            priority.set(applied);
            worker->applied = applied;
        }
    }
    // With the boost mutex: the boost follows the waiters, and every worker runs at its own priority or the boost.
    void ApplyBoostLocked() {
        const int raised = waiters.empty() ? INT_MIN : *std::max_element(waiters.begin(), waiters.end());
        boost.store(raised, std::memory_order_relaxed);
        for (const auto& worker : boostable) {
            std::lock_guard lock(worker->mutex);
            if (const int applied = (std::max)(worker->own, raised); applied != worker->applied) {
                priority.setThread(worker->thread, applied);
                worker->applied = applied;
            }
        }
    }
    TaskClass ParallelClass() const { return {0, parallelDomain}; }

    explicit State(const std::vector<unsigned>& a_domainWorkers, std::uint32_t a_parallelDomain, unsigned a_frameCriticalWorkers,
        ThreadPriority a_priority) :
        parallelDomain(a_parallelDomain), priority(std::move(a_priority)), frameCriticalWorkers(a_frameCriticalWorkers) {
        if (a_domainWorkers.empty() || a_domainWorkers.size() > 256)
            throw std::invalid_argument("executor needs between 1 and 256 domains");
        if (a_parallelDomain >= a_domainWorkers.size())
            throw std::invalid_argument("executor's parallel domain does not exist");
        for (const unsigned workers : a_domainWorkers)
            if (!workers)
                throw std::invalid_argument("executor domain needs a worker");
        domains.reserve(a_domainWorkers.size());
        for (const unsigned workers : a_domainWorkers) {
            domains.push_back(std::make_unique<Domain>());
            domains.back()->workers = workers;
        }
    }
    // Every worker of every domain looks again, for a cancelled scope's timers or shutdown.
    void WakeAll() {
        for (auto& domain : domains)
            domain->available.release(domain->workers);
        frameCriticalAvailable.release(frameCriticalWorkers);
    }
    void Run(unsigned domain);
    void RunFrameCritical();
    void Execute(unsigned domain, Job& job);
};

Executor::WaitBoost::~WaitBoost() {
    if (!state)
        return;
    std::lock_guard lock(state->boostMutex);
    if (const auto found = std::find(state->waiters.begin(), state->waiters.end(), priority); found != state->waiters.end())
        state->waiters.erase(found);
    state->ApplyBoostLocked();
}

Executor::WaitBoost::WaitBoost(WaitBoost&& a_other) noexcept : state(std::move(a_other.state)), priority(a_other.priority) {}

Executor::WaitBoost Executor::Boost(const std::shared_ptr<State>& a_state) {
    WaitBoost boost;
    if (currentExecutor == a_state.get() || !a_state->BoostsForWaits())
        return boost;
    boost.priority = a_state->priority.get();
    boost.state = a_state;
    a_state->waitBoosts.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(a_state->boostMutex);
    a_state->waiters.push_back(boost.priority);
    a_state->ApplyBoostLocked();
    return boost;
}

Executor::WaitBoost Executor::BoostWorkersForWait() const { return Boost(state); }

struct Executor::ExecutorScope final : TaskScope {
    std::shared_ptr<State> owner;
    std::atomic_bool cancelled{false};
    std::atomic<std::size_t> pending{0};  // includes executing work and timers
    // The first failure. Written when a task throws and read by Wait, both off the hot path.
    mutable std::mutex failureMutex;
    std::exception_ptr failure;
    explicit ExecutorScope(std::shared_ptr<State> value) : owner(std::move(value)) {}
    bool StopRequested() const noexcept override { return cancelled.load() || owner->stopping.load(); }
    void Cancel() noexcept override {
        cancelled.store(true);
        owner->WakeAll();  // reclaims its timers now, not at their deadline
    }
    void Wait() const override {
        if (currentExecutor == owner.get()) throw std::logic_error("executor workers cannot wait on their executor's scopes");
        if (pending.load() != 0) {
            const auto boost = Boost(owner);
            for (auto value = pending.load(); value != 0; value = pending.load())
                pending.wait(value);
        }
        std::exception_ptr error;
        {
            std::lock_guard lock(failureMutex);
            error = failure;
        }
        if (error) std::rethrow_exception(error);
    }
};

struct Executor::Threads {
    std::mutex shutdownMutex;
    std::vector<std::thread> workers;
};

void Executor::State::Execute(unsigned domain, Job& job) {
    auto& d = *domains[domain];
    d.active.fetch_add(1, std::memory_order_relaxed);
    auto scope = std::move(job.scope);
    std::exception_ptr error;
    const bool wasCancelled = scope->StopRequested();
    if (!wasCancelled) {
        // Hold the worker at the raised priority for a frame-critical job, since workers may sit below other threads. Restored after.
        const int previous = threadPriority;
        const bool raise = job.frameCritical && Boosts() && priority.frameCritical > previous;
        if (raise)
            SetOwn(priority.frameCritical);
        const bool wasFrameCritical = std::exchange(inFrameCritical, inFrameCritical || job.frameCritical);
        try { job.task({[scope] { return scope->StopRequested(); }}); }
        catch (...) { error = std::current_exception(); }
        inFrameCritical = wasFrameCritical;
        if (raise)
            SetOwn(previous);
    }
    // Release captured resources before signalling completion, even when the task never started. Cancel never destroys queued work.
    job.task = {};
    d.active.fetch_sub(1, std::memory_order_relaxed);
    if (error) {
        failed.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(scope->failureMutex);
        if (!scope->failure) scope->failure = error;
    } else if (wasCancelled) {
        cancelled.fetch_add(1, std::memory_order_relaxed);
    } else {
        completed.fetch_add(1, std::memory_order_relaxed);
    }
    if (scope->pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
        scope->pending.notify_all();
}

void Executor::State::Run(unsigned domain) {
    currentExecutor = this;
    StartWorker();
    auto& d = *domains[domain];
    // Delayed jobs this worker took from the ring early, earliest first.
    std::vector<Job> timers;
    auto later = [](const Job& a, const Job& b) { return a.ready > b.ready; };
    for (;;) {
        // Due and cancelled timers first. A cancelled scope's timers are reclaimed now, not at their deadline.
        const auto now = std::chrono::steady_clock::now();
        if (std::any_of(timers.begin(), timers.end(), [&](const Job& a_job) { return a_job.ready <= now || a_job.scope->StopRequested(); })) {
            std::vector<Job> kept;
            for (auto& timer : timers) {
                if (timer.ready <= now || timer.scope->StopRequested())
                    Execute(domain, timer);
                else
                    kept.push_back(std::move(timer));
            }
            timers = std::move(kept);
            std::make_heap(timers.begin(), timers.end(), later);
        }
        if (stopping.load() && timers.empty() && d.ring.Size() == 0 && d.frameCritical.Size() == 0) {
            currentExecutor = nullptr;
            return;
        }
        // One token per pushed job and per wake-up, acquired before the pop, so tokens never pile up.
        if (stopping.load())
            (void)d.available.try_acquire_for(std::chrono::milliseconds(1));  // draining, so never block on a spent token
        else if (timers.empty())
            d.available.acquire();
        else if (!d.available.try_acquire_until(timers.front().ready))
            continue;  // the earliest timer is due
        Job job;
        if (!d.frameCritical.Pop(job) && !d.ring.Pop(job))
            continue;  // a wake-up, or a job another worker took with its token
        if (job.ready > std::chrono::steady_clock::now() && !job.scope->StopRequested()) {
            timers.push_back(std::move(job));
            std::push_heap(timers.begin(), timers.end(), later);
        } else {
            Execute(domain, job);
        }
    }
}

void Executor::State::RunFrameCritical() {
    currentExecutor = this;
    StartWorker();
    auto& d = *domains[parallelDomain];
    for (;;) {
        if (stopping.load() && d.frameCritical.Size() == 0) {
            currentExecutor = nullptr;
            return;
        }
        if (stopping.load())
            (void)frameCriticalAvailable.try_acquire_for(std::chrono::milliseconds(1));
        else
            frameCriticalAvailable.acquire();
        Job job;
        if (d.frameCritical.Pop(job))  // otherwise a parallel worker took it, or it was a wake-up
            Execute(parallelDomain, job);
    }
}

Executor::Desc Executor::DefaultDesc(unsigned a_preparationWorkers, unsigned a_streamingWorkers) {
    Desc desc;
    desc.domainWorkers = {1, a_preparationWorkers, 1, a_streamingWorkers};
    desc.parallelDomain = Preparation.domain;
    return desc;
}

Executor::Executor(unsigned preparationWorkers, ThreadStart onThreadStart, ThreadPriority priority, unsigned frameCriticalWorkers) :
    Executor([&] {
        auto desc = DefaultDesc(preparationWorkers);
        desc.onThreadStart = std::move(onThreadStart);
        desc.priority = std::move(priority);
        desc.frameCriticalWorkers = frameCriticalWorkers;
        return desc;
    }()) {}

Executor::Executor(Desc a_desc) :
    state(std::make_shared<State>(a_desc.domainWorkers, a_desc.parallelDomain, a_desc.frameCriticalWorkers, std::move(a_desc.priority))),
    threads(std::make_unique<Threads>()) {
    const auto onThreadStart = std::move(a_desc.onThreadStart);
    std::size_t total = a_desc.frameCriticalWorkers;
    for (const unsigned workers : a_desc.domainWorkers)
        total += workers;
    threads->workers.reserve(total);
    try {
        // Domain by domain, in index order, then the frame-critical threads.
        for (std::uint32_t domain = 0; domain < a_desc.domainWorkers.size(); ++domain)
            for (unsigned i = 0; i < a_desc.domainWorkers[domain]; ++i)
                threads->workers.emplace_back([shared = state, onThreadStart, domain, i] {
                    if (onThreadStart)
                        onThreadStart(TaskClass{0, domain}, i);
                    shared->Run(domain);
                });
        const TaskClass frameCriticalThread{0, static_cast<std::uint32_t>(a_desc.domainWorkers.size())};
        for (unsigned i = 0; i < a_desc.frameCriticalWorkers; ++i)
            threads->workers.emplace_back([shared = state, onThreadStart, frameCriticalThread, i] {
                if (onThreadStart)
                    onThreadStart(frameCriticalThread, i);
                shared->RunFrameCritical();
            });
    } catch (...) {
        Shutdown();
        throw;
    }
    parallelScope = CreateScope("parallel for");
}

Executor::~Executor() { Shutdown(); }

Scope Executor::CreateScope(std::string_view) {
    return std::make_shared<ExecutorScope>(state);
}

bool Executor::Dispatch(const Scope& scope, TaskClass cls, TaskDispatch, std::string_view name, Task task, TaskTraceMetadata) {
    return DispatchAfter(scope, {}, cls, name, std::move(task));
}

bool Executor::DispatchAfter(const Scope& scope, std::chrono::steady_clock::duration delay, TaskClass cls, std::string_view, Task task) {
    const auto own = std::dynamic_pointer_cast<ExecutorScope>(scope);
    const auto now = std::chrono::steady_clock::now();
    if (!own || own->owner != state || own->StopRequested() || !task || cls.lane != 0 || cls.domain >= state->domains.size() ||
        delay > std::chrono::steady_clock::time_point::max() - now) {
        state->rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto& domain = *state->domains[cls.domain];
    // Count before the push, because a worker may finish the job before this returns.
    own->pending.fetch_add(1, std::memory_order_acq_rel);
    domain.ring.Push({now + (std::max)(delay, std::chrono::steady_clock::duration::zero()), own, std::move(task)});
    state->accepted.fetch_add(1, std::memory_order_relaxed);
    const std::size_t queued = domain.ring.Size();
    for (std::size_t high = domain.highWater.load(std::memory_order_relaxed);
         queued > high && !domain.highWater.compare_exchange_weak(high, queued, std::memory_order_relaxed);) {}
    domain.available.release();
    return true;
}

bool Executor::DispatchFrameCritical(const Scope& a_scope, std::string_view, Task a_task) {
    const auto own = std::dynamic_pointer_cast<ExecutorScope>(a_scope);
    if (!own || own->owner != state || own->StopRequested() || !a_task) {
        state->rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto& domain = *state->domains[state->parallelDomain];
    own->pending.fetch_add(1, std::memory_order_acq_rel);
    domain.frameCritical.Push({std::chrono::steady_clock::now(), own, std::move(a_task), true});
    if (state->frameCriticalWorkers)
        state->frameCriticalAvailable.release();
    state->accepted.fetch_add(1, std::memory_order_relaxed);
    const std::size_t queued = domain.ring.Size() + domain.frameCritical.Size();
    for (std::size_t high = domain.highWater.load(std::memory_order_relaxed);
         queued > high && !domain.highWater.compare_exchange_weak(high, queued, std::memory_order_relaxed);) {}
    domain.available.release();
    return true;
}

void Executor::ParallelFor(std::string_view a_name, std::size_t a_count, std::size_t a_grain,
    const std::function<void(std::size_t, std::size_t)>& a_body) {
    if (!a_count)
        return;
    auto& entry = state->Entry(a_name);
    entry.calls.fetch_add(1, std::memory_order_relaxed);
    const std::size_t grain = (std::max)(a_grain, std::size_t(1));
    const std::size_t chunks = (a_count + grain - 1) / grain;
    entry.chunks.fetch_add(chunks, std::memory_order_relaxed);
    if (chunks == 1) {
        a_body(0, a_count);
        return;
    }
    // Shared with helpers, which may start after the caller returned. They only touch the body for a chunk they claimed,
    // and the caller returns only once every claimed chunk is done. The state outlives every worker.
    struct Shared {
        std::atomic<std::size_t> next{0}, done{0};
        std::size_t count = 0, grain = 0, chunks = 0;
        const std::function<void(std::size_t, std::size_t)>* body = nullptr;
        State* boosts = nullptr;  // null leaves priorities alone
        int callerPriority = 0;
        State::ParallelForEntry* entry = nullptr;
        std::mutex errorMutex;
        std::exception_ptr error;
        // The chunks this thread ran.
        std::size_t Work() {
            std::size_t ran = 0;
            for (std::size_t chunk = next.fetch_add(1, std::memory_order_relaxed); chunk < chunks;
                 chunk = next.fetch_add(1, std::memory_order_relaxed)) {
                try {
                    (*body)(chunk * grain, (std::min)(count, (chunk + 1) * grain));
                } catch (...) {
                    std::lock_guard lock(errorMutex);
                    if (!error)
                        error = std::current_exception();
                }
                ++ran;
                if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == chunks)
                    done.notify_all();
            }
            return ran;
        }
        // A parallel worker's share. Workers may sit below their callers' priority, so a helper holding a chunk the caller
        // waits on is raised to the caller's priority, never lowered, and restored after.
        void Help() {
            const int previous = threadPriority;
            const bool raise = boosts && callerPriority > previous && next.load(std::memory_order_relaxed) < chunks;
            if (raise) {
                boosts->SetOwn(callerPriority);  // a nested ParallelFor's helpers inherit it
                entry->boosts.fetch_add(1, std::memory_order_relaxed);
            }
            const std::size_t ran = Work();
            if (raise)
                boosts->SetOwn(previous);
            if (ran)
                entry->helperChunks.fetch_add(ran, std::memory_order_relaxed);
            else
                entry->idleHelpers.fetch_add(1, std::memory_order_relaxed);
        }
    };
    auto shared = std::make_shared<Shared>();
    shared->count = a_count;
    shared->grain = grain;
    shared->chunks = chunks;
    shared->body = &a_body;
    shared->entry = &entry;
    if (state->Boosts()) {
        shared->boosts = state.get();
        // A worker's priority is cached, and a boosted helper counts as its caller's. Any other thread is asked.
        shared->callerPriority = currentExecutor == state.get() ? threadPriority : state->priority.get();
    }
    const std::size_t helpers = (std::min)(chunks - 1, std::size_t(state->domains[state->parallelDomain]->workers));
    // A frame-critical caller's helpers are frame-critical, so no queued parallel job stands in front of its chunks.
    const bool critical = currentExecutor == state.get() && inFrameCritical;
    const TaskClass parallelClass = state->ParallelClass();
    for (std::size_t i = 0; i < helpers; ++i) {
        auto help = [shared](const TaskContext&) { shared->Help(); };
        if (!(critical ? DispatchFrameCritical(parallelScope, a_name, std::move(help))
                       : Dispatch(parallelScope, parallelClass, TaskDispatch::Cpu, a_name, std::move(help))))
            break;  // the caller takes whatever no helper does
    }
    shared->Work();
    if (std::size_t done = shared->done.load(std::memory_order_acquire); done != chunks) {
        // Only chunks a helper claimed and still runs count as blocked time.
        const auto blockedSince = std::chrono::steady_clock::now();
        for (; done != chunks; done = shared->done.load(std::memory_order_acquire))
            shared->done.wait(done, std::memory_order_acquire);
        const auto blocked = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - blockedSince).count());
        entry.blocked.fetch_add(1, std::memory_order_relaxed);
        entry.blockedNanoseconds.fetch_add(blocked, std::memory_order_relaxed);
        RaiseTo(entry.maxBlockedNanoseconds, blocked);
    }
    if (shared->error)
        std::rethrow_exception(shared->error);
}

Executor::Statistics Executor::GetStatistics() const {
    Statistics stats;
    stats.accepted = state->accepted.load();
    stats.completed = state->completed.load();
    stats.cancelled = state->cancelled.load();
    stats.rejected = state->rejected.load();
    stats.failed = state->failed.load();
    stats.waitBoosts = state->waitBoosts.load(std::memory_order_relaxed);
    const std::size_t count = state->domains.size();
    stats.queued.resize(count);
    stats.active.resize(count);
    stats.highWater.resize(count);
    for (std::size_t d = 0; d < count; ++d) {
        stats.queued[d] = state->domains[d]->ring.Size() + state->domains[d]->frameCritical.Size();
        stats.active[d] = state->domains[d]->active.load();
        stats.highWater[d] = state->domains[d]->highWater.load();
    }
    return stats;
}

std::vector<Executor::ParallelForStatistics> Executor::GetParallelForStatistics(bool a_takePeaks) const {
    std::vector<ParallelForStatistics> result;
    for (const auto& item : state->parallelFor) {  // safe beside concurrent inserts
        const auto& entry = item.second;
        auto& out = result.emplace_back();
        out.name = entry->name;
        out.calls = entry->calls.load(std::memory_order_relaxed);
        out.chunks = entry->chunks.load(std::memory_order_relaxed);
        out.helperChunks = entry->helperChunks.load(std::memory_order_relaxed);
        out.blocked = entry->blocked.load(std::memory_order_relaxed);
        out.blockedNanoseconds = entry->blockedNanoseconds.load(std::memory_order_relaxed);
        out.maxBlockedNanoseconds = a_takePeaks ? entry->maxBlockedNanoseconds.exchange(0, std::memory_order_relaxed)
                                                : entry->maxBlockedNanoseconds.load(std::memory_order_relaxed);
        out.boosts = entry->boosts.load(std::memory_order_relaxed);
        out.idleHelpers = entry->idleHelpers.load(std::memory_order_relaxed);
    }
    return result;
}

GraphSchedulingLayout Executor::SchedulingLayout() const noexcept {
    const TaskClass control{0, 0};
    return {1, DomainCount(), control, control};
}

std::uint32_t Executor::DomainCount() const noexcept { return static_cast<std::uint32_t>(state->domains.size()); }

TaskClass Executor::ParallelClass() const noexcept { return state->ParallelClass(); }

unsigned Executor::Workers(std::uint32_t a_domain) const noexcept {
    return a_domain < state->domains.size() ? state->domains[a_domain]->workers : 0u;
}

unsigned Executor::FrameCriticalWorkers() const noexcept { return state->frameCriticalWorkers; }

void Executor::Shutdown() {
    if (currentExecutor == state.get()) throw std::logic_error("executor shutdown must run on its owner");
    std::lock_guard shutdownLock(threads->shutdownMutex);
    state->stopping.store(true);
    state->WakeAll();
    for (auto& worker : threads->workers) if (worker.joinable()) worker.join();
    // A dispatch that passed its check before stopping but pushed after its workers left runs as cancelled here, so its scope still reaches zero.
    for (unsigned domain = 0; domain < state->domains.size(); ++domain)
        for (State::Job job; state->domains[domain]->frameCritical.Pop(job) || state->domains[domain]->ring.Pop(job);)
            state->Execute(domain, job);
}

} // namespace org::async
