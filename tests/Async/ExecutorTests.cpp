// The executor: domains, queues, timers, scopes, ParallelFor, priorities, frame-critical work, custom layouts and lanes.
#include <ORGModuleServices/Async/Executor.h>
#include <ORGModuleServices/Async/Lanes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cassert>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using Executor = org::async::Executor;
using namespace std::chrono_literals;

template <class T>
T Ready(std::future<T>& future)
{
    assert(future.wait_for(5s) == std::future_status::ready);
    return future.get();
}

void TestLanesAndQueues()
{
    Executor executor(2), foreign;
    const auto scope = executor.CreateScope("unbounded");
    const auto caller = std::this_thread::get_id();
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::array<std::promise<std::thread::id>, 2> started;
    std::array<std::future<std::thread::id>, 2> futures{ started[0].get_future(), started[1].get_future() };
    for (unsigned i = 0; i != 2; ++i)
        assert(executor.SubmitCpu(scope, 0, 1, "blocked preparation", [&, i](const auto&) {
            started[i].set_value(std::this_thread::get_id());
            gate.wait();
        }));
    const auto first = Ready(futures[0]), second = Ready(futures[1]);
    assert(first != second && first != caller && second != caller);
    std::promise<std::thread::id> coordinator;
    auto coordinatorResult = coordinator.get_future();
    assert(executor.Submit(scope, 0, 0, "coordinator", [&](const auto&) { coordinator.set_value(std::this_thread::get_id()); }));
    const auto control = Ready(coordinatorResult);
    assert(control != caller && control != first && control != second);
    std::atomic_uint unexpectedlyRun{ 0 };
    for (unsigned i = 0; i != 2; ++i)
        assert(executor.ScheduleAfter(scope, 1h, 0, 1, "timer", [&](const auto&) { ++unexpectedlyRun; }));
    // No bound: queued behind the two blocked workers and the two timers.
    assert(executor.SubmitCpu(scope, 0, 1, "queued", [&](const auto&) { ++unexpectedlyRun; }));
    assert(!foreign.Submit(scope, 0, 0, "foreign", [](const auto&) {}));
    assert(!executor.Submit(scope, 1, 0, "invalid class", [](const auto&) {}));
    auto waiting = std::async(std::launch::async, [&] { scope->Wait(); });
    assert(waiting.wait_for(10ms) == std::future_status::timeout);
    scope->Cancel();
    release.set_value();
    Ready(waiting);
    assert(unexpectedlyRun == 0);
    const auto stats = executor.GetStatistics();
    assert(stats.highWater[1] == 3 && stats.queued[1] == 0 && stats.active[1] == 0 && stats.cancelled == 3 && stats.rejected == 1);
    assert(!executor.Submit(scope, 0, 0, "cancelled", [](const auto&) {}));
}

void TestOrderingAndFailures()
{
    Executor executor;
    const auto scope = executor.CreateScope("ordering");
    std::vector<unsigned> order;  // coordinator exclusively writes
    for (unsigned i = 0; i != 100; ++i)
        assert(executor.Submit(scope, 0, 0, "ordered", [&, i](const auto&) { order.push_back(i); }));
    scope->Wait();
    assert(order.size() == 100);
    for (unsigned i = 0; i != 100; ++i) assert(order[i] == i);
    const auto start = std::chrono::steady_clock::now();
    assert(executor.ScheduleAfter(scope, 20ms, 0, 0, "delay", [&](const auto&) {
        assert(std::chrono::steady_clock::now() - start >= 20ms);
    }));
    scope->Wait();
    assert(executor.Submit(scope, 0, 0, "reject worker wait", [scope](const auto&) {
        bool rejected = false;
        try { scope->Wait(); } catch (const std::logic_error&) { rejected = true; }
        assert(rejected);
    }));
    scope->Wait();
    const auto broken = executor.CreateScope("failure");
    assert(executor.SubmitCpu(broken, 0, 1, "throws", [](const auto&) { throw std::runtime_error("expected"); }));
    bool failed = false;
    try { broken->Wait(); } catch (const std::runtime_error&) { failed = true; }
    assert(failed && executor.GetStatistics().failed == 1);
    assert(executor.Submit(scope, 0, 0, "survives", [](const auto&) {}));
    scope->Wait();
}

void TestLifetimeAndShutdown()
{
    struct Retained
    {
        std::promise<std::thread::id>* destroyed;
        ~Retained() { destroyed->set_value(std::this_thread::get_id()); }
    };
    std::function<bool()> savedCancellation;
    org::async::Scope retainedScope;
    std::promise<std::thread::id> destroyed;
    auto destruction = destroyed.get_future();
    const auto caller = std::this_thread::get_id();
    {
        Executor executor;
        retainedScope = executor.CreateScope("retained");
        assert(executor.Submit(retainedScope, 0, 0, "query", [&](const auto& context) { savedCancellation = context.stopRequested; }));
        retainedScope->Wait();
        assert(savedCancellation && !savedCancellation());
        auto owner = std::make_shared<Retained>();
        owner->destroyed = &destroyed;
        assert(executor.ScheduleAfter(retainedScope, 1h, 0, 0, "retained timer", [owner](const auto&) { assert(false); }));
        owner.reset();
        executor.Shutdown();
        assert(Ready(destruction) != caller);
        assert(savedCancellation());
        assert(!executor.Submit(retainedScope, 0, 0, "stopped", [](const auto&) {}));
        executor.Shutdown();
    }
    assert(savedCancellation());
    retainedScope->CancelAndWait();
}

void TestParallelFor()
{
    Executor executor(4);
    // Every index once, across several threads, from a coordinator task (the frame's producers call it there).
    const std::size_t count = 100000;
    std::vector<std::atomic_uint> hits(count);
    std::mutex threadsMutex;
    std::set<std::thread::id> threads;
    std::promise<void> finished;
    auto result = finished.get_future();
    const auto scope = executor.CreateScope("parallel for");
    assert(executor.Submit(scope, 0, 0, "coordinator", [&](const auto&) {
        executor.ParallelFor("test", count, 64, [&](std::size_t a_begin, std::size_t a_end) {
            for (std::size_t i = a_begin; i < a_end; ++i)
                hits[i].fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(20));
            std::lock_guard lock(threadsMutex);
            threads.insert(std::this_thread::get_id());
        });
        finished.set_value();
    }));
    Ready(result);
    for (const auto& hit : hits)
        assert(hit.load() == 1);
    assert(threads.size() > 1);
    // Nothing, one chunk, and a throwing chunk: the rest still run, and the first error reaches the caller.
    executor.ParallelFor("empty", 0, 16, [](std::size_t, std::size_t) { assert(false); });
    std::size_t single = 0;
    executor.ParallelFor("single", 10, 16, [&](std::size_t a_begin, std::size_t a_end) { single += a_end - a_begin; });
    assert(single == 10);
    std::atomic_uint ran{ 0 };
    bool threw = false;
    try {
        executor.ParallelFor("throws", 1000, 10, [&](std::size_t a_begin, std::size_t) {
            ran.fetch_add(1);
            if (a_begin == 500)
                throw std::runtime_error("chunk");
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw && ran.load() == 100);
    // Helpers that start after the caller returned do nothing with the (gone) body.
    for (unsigned round = 0; round != 200; ++round) {
        std::vector<int> local(8, 0);
        executor.ParallelFor("short", local.size(), 1, [&](std::size_t a_begin, std::size_t) { local[a_begin] = 1; });
        for (int value : local)
            assert(value == 1);
    }
}

// Every size and grain: every index exactly once, in chunks [k * grain, min(count, (k + 1) * grain)) (callers index per-chunk
// results by a_begin / grain).
void TestParallelForCoverage()
{
    Executor executor(3);
    for (const std::size_t count : { 1, 2, 3, 7, 63, 64, 65, 127, 1000, 4097 })
        for (const std::size_t grain : { 0, 1, 2, 3, 16, 64, 1000, 5000 }) {
            const std::size_t step = grain ? grain : 1;
            std::vector<std::atomic_uint> hits(count);
            std::atomic_bool aligned{ true };
            executor.ParallelFor("coverage", count, grain, [&](std::size_t a_begin, std::size_t a_end) {
                if (a_begin % step || a_end != (std::min)(count, a_begin + step))
                    aligned = false;
                for (std::size_t i = a_begin; i < a_end; ++i)
                    hits[i].fetch_add(1);
            });
            assert(aligned.load());
            for (const auto& hit : hits)
                assert(hit.load() == 1);
        }
    const auto stats = executor.GetParallelForStatistics();
    assert(stats.size() == 1 && stats[0].name == "coverage" && stats[0].calls == 10 * 8);
}

namespace
{
    // Stands in for the OS thread priority: the executor reads and sets only the calling thread's.
    thread_local int fakePriority = 0;
    Executor::ThreadPriority FakePriority(std::atomic_uint& a_sets)
    {
        return { [] { return fakePriority; }, [&a_sets](int a_priority) { a_sets.fetch_add(1); fakePriority = a_priority; } };
    }
    Executor::ThreadStart FakeStart()
    {
        // The coordinator and the serial thread above normal, the workers below.
        return [](org::async::TaskClass a_class, unsigned) { fakePriority = a_class.domain == Executor::Preparation.domain ? -1 : 1; };
    }
    const Executor::ParallelForStatistics& Find(const std::vector<Executor::ParallelForStatistics>& a_stats, std::string_view a_name)
    {
        for (const auto& stats : a_stats)
            if (stats.name == a_name)
                return stats;
        assert(false);
        return a_stats.front();
    }
    // Every worker of the pool reports its priority at once (each blocks until all have started, so each runs one).
    std::vector<int> WorkerPriorities(Executor& a_executor, unsigned a_workers)
    {
        const auto scope = a_executor.CreateScope("priorities");
        std::mutex seenMutex;
        std::vector<int> seen;
        std::atomic_uint started{ 0 };
        for (unsigned i = 0; i != a_workers; ++i)
            assert(a_executor.SubmitCpu(scope, 0, 1, "priority", [&](const auto&) {
                {
                    std::lock_guard lock(seenMutex);
                    seen.push_back(fakePriority);
                }
                started.fetch_add(1);
                for (const auto deadline = std::chrono::steady_clock::now() + 5s; started.load() != a_workers && std::chrono::steady_clock::now() < deadline;)
                    std::this_thread::yield();
            }));
        scope->Wait();
        assert(seen.size() == a_workers);
        return seen;
    }
}

// A helper runs an above-normal caller's chunks at the caller's priority and goes back to its own afterwards, also when a chunk
// throws; a caller below the helpers never lowers them.
void TestParallelForPriority()
{
    constexpr unsigned kWorkers = 3;
    std::atomic_uint sets{ 0 };
    Executor executor(kWorkers, FakeStart(), FakePriority(sets));
    for (const int priority : WorkerPriorities(executor, kWorkers))
        assert(priority == -1);
    const auto scope = executor.CreateScope("boost");
    std::mutex seenMutex;
    std::set<std::thread::id> helpers;
    std::atomic_bool wrongPriority{ false }, threw{ false };
    assert(executor.Submit(scope, 0, 0, "above normal caller", [&](const auto&) {
        const auto caller = std::this_thread::get_id();
        auto body = [&](std::size_t a_begin, std::size_t) {
            if (fakePriority != 1)
                wrongPriority = true;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            if (std::this_thread::get_id() != caller) {
                std::lock_guard lock(seenMutex);
                helpers.insert(std::this_thread::get_id());
            }
            if (a_begin == 37)
                throw std::runtime_error("chunk");
        };
        try { executor.ParallelFor("boosted", 64, 1, body); } catch (const std::runtime_error&) { threw = true; }
        assert(fakePriority == 1);
    }));
    scope->Wait();
    assert(threw.load() && !wrongPriority.load() && !helpers.empty());
    // Restored: every worker is back below normal.
    for (const int priority : WorkerPriorities(executor, kWorkers))
        assert(priority == -1);
    const auto boosted = Find(executor.GetParallelForStatistics(), "boosted");
    assert(boosted.boosts >= helpers.size() && sets.load() == 2 * boosted.boosts && boosted.chunks == 64);
    // A caller below the workers: they keep their own priority, and nothing is set.
    fakePriority = -2;
    const unsigned setsBefore = sets.load();
    std::atomic_bool lowered{ false };
    const auto self = std::this_thread::get_id();
    executor.ParallelFor("below", 64, 1, [&](std::size_t, std::size_t) {
        if (fakePriority != (std::this_thread::get_id() == self ? -2 : -1))
            lowered = true;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    });
    assert(!lowered.load() && sets.load() == setsBefore && Find(executor.GetParallelForStatistics(), "below").boosts == 0);
    for (const int priority : WorkerPriorities(executor, kWorkers))
        assert(priority == -1);
    fakePriority = 0;
}

// A frame-critical job is taken before the preparation jobs queued ahead of it, at ThreadPriority::frameCritical, and the worker goes
// back to its own priority after it.
void TestFrameCritical()
{
    std::atomic_uint sets{ 0 };
    auto priority = FakePriority(sets);
    priority.frameCritical = 2;
    Executor executor(1, FakeStart(), std::move(priority));
    const auto scope = executor.CreateScope("frame critical");
    std::promise<void> release, blocking;
    const auto gate = release.get_future().share();
    auto blocked = blocking.get_future();
    assert(executor.SubmitCpu(scope, 0, 1, "blocking", [&](const auto&) {
        blocking.set_value();
        gate.wait();
    }));
    Ready(blocked);
    std::mutex orderMutex;
    std::vector<std::pair<char, int>> order;  // (job, its priority)
    const auto note = [&](char a_job) {
        std::lock_guard lock(orderMutex);
        order.emplace_back(a_job, fakePriority);
    };
    assert(executor.SubmitCpu(scope, 0, 1, "queued first", [&](const auto&) { note('a'); }));
    assert(executor.DispatchFrameCritical(scope, "critical", [&](const auto&) { note('c'); }));
    assert(!executor.DispatchFrameCritical(scope, "no task", {}));
    release.set_value();
    scope->Wait();
    assert(order.size() == 2 && order[0] == std::make_pair('c', 2) && order[1] == std::make_pair('a', -1));
    assert(sets.load() == 2);
    for (const int seen : WorkerPriorities(executor, 1))
        assert(seen == -1);
    assert(executor.GetStatistics().rejected == 1);
    // A frame-critical thread takes it while every preparation worker is busy, and takes no other job.
    std::atomic_uint frameStarts{ 0 };
    Executor withThreads(1, [&](org::async::TaskClass a_class, unsigned) { frameStarts += a_class.domain == Executor::FrameCriticalThread.domain; }, {}, 1);
    const auto busy = withThreads.CreateScope("busy");
    std::promise<void> free, held;
    const auto hold = free.get_future().share();
    auto holding = held.get_future();
    assert(withThreads.SubmitCpu(busy, 0, 1, "holding the worker", [&](const auto&) {
        held.set_value();
        hold.wait();
    }));
    Ready(holding);
    std::promise<std::thread::id> ran;
    auto ranOn = ran.get_future();
    std::atomic_bool queuedRan{ false };
    assert(withThreads.SubmitCpu(busy, 0, 1, "behind the held worker", [&](const auto&) { queuedRan = true; }));
    assert(withThreads.DispatchFrameCritical(busy, "critical", [&](const auto&) { ran.set_value(std::this_thread::get_id()); }));
    const auto criticalThread = Ready(ranOn);
    assert(criticalThread != std::this_thread::get_id() && !queuedRan.load() && frameStarts.load() == 1);
    free.set_value();
    busy->Wait();
    assert(queuedRan.load());
}

namespace
{
    // The OS thread priority, settable from another thread by the address of the thread's own value (ThreadPriority::thread).
    thread_local std::atomic_int sharedPriority{ 0 };
    Executor::ThreadPriority SharedPriority()
    {
        Executor::ThreadPriority priority{ [] { return sharedPriority.load(); }, [](int a_priority) { sharedPriority = a_priority; } };
        priority.thread = [] { return reinterpret_cast<std::uintptr_t>(&sharedPriority); };
        priority.setThread = [](std::uintptr_t a_thread, int a_priority) { reinterpret_cast<std::atomic_int*>(a_thread)->store(a_priority); };
        return priority;
    }
}

// A thread waiting on a scope raises the workers to its own priority while it waits, a running task included, and they go back to
// their own afterwards. A worker's own raise (a frame-critical job) is kept under the boost and restored after it.
void TestWaitBoost()
{
    constexpr unsigned kWorkers = 2;
    auto priority = SharedPriority();
    priority.frameCritical = 2;
    Executor executor(Executor::Desc{ .domainWorkers = { 1, kWorkers, 1, 1 },
        .onThreadStart = [](org::async::TaskClass a_class, unsigned) { sharedPriority = a_class.domain == Executor::Preparation.domain ? -1 : 1; },
        .priority = std::move(priority) });
    const auto scope = executor.CreateScope("boosted wait");
    std::promise<void> started;
    auto running = started.get_future();
    std::atomic_int seen{ -100 };
    assert(executor.SubmitCpu(scope, 0, 1, "starved", [&](const auto&) {
        started.set_value();
        // Runs until a waiter raised it.
        for (const auto deadline = std::chrono::steady_clock::now() + 5s; sharedPriority.load() < 0 && std::chrono::steady_clock::now() < deadline;)
            std::this_thread::yield();
        seen = sharedPriority.load();
    }));
    Ready(running);
    sharedPriority = 0;  // the waiting thread: normal
    scope->Wait();
    assert(seen.load() == 0 && executor.GetStatistics().waitBoosts == 1);
    // Restored once the wait ended.
    const auto check = executor.CreateScope("after");
    std::mutex seenMutex;
    std::vector<int> after;
    std::atomic_uint arrived{ 0 };
    for (unsigned i = 0; i != kWorkers; ++i)
        assert(executor.SubmitCpu(check, 0, 1, "priority", [&](const auto&) {
            {
                std::lock_guard lock(seenMutex);
                after.push_back(sharedPriority.load());
            }
            arrived.fetch_add(1);
            for (const auto deadline = std::chrono::steady_clock::now() + 5s; arrived.load() != kWorkers && std::chrono::steady_clock::now() < deadline;)
                std::this_thread::yield();
        }));
    // Polled, not waited: a wait would raise them while they report.
    for (const auto deadline = std::chrono::steady_clock::now() + 5s; arrived.load() != kWorkers && std::chrono::steady_clock::now() < deadline;)
        std::this_thread::yield();
    check->Wait();
    assert(after.size() == kWorkers && std::all_of(after.begin(), after.end(), [](int a_priority) { return a_priority == -1; }));
    // A frame-critical job above the waiter keeps its own priority, and the worker goes back to -1 after both.
    std::promise<void> criticalStarted;
    auto criticalRunning = criticalStarted.get_future();
    std::atomic_bool release{ false };
    std::atomic_int during{ -100 };
    assert(executor.DispatchFrameCritical(check, "critical", [&](const auto&) {
        criticalStarted.set_value();
        while (!release.load())
            std::this_thread::yield();
        during = sharedPriority.load();
    }));
    Ready(criticalRunning);
    std::thread waiter([&] {
        sharedPriority = 0;
        check->Wait();
    });
    for (const auto deadline = std::chrono::steady_clock::now() + 5s; executor.GetStatistics().waitBoosts != 2 && std::chrono::steady_clock::now() < deadline;)
        std::this_thread::yield();
    release = true;
    waiter.join();
    assert(during.load() == 2);
    sharedPriority = 0;
}

// The caller waits only for a chunk a helper holds, and that wait is reported under its name; helpers that start late do nothing.
void TestParallelForWaitAndLateHelpers()
{
    Executor executor(2);
    // The caller holds chunk 0 until a helper has claimed chunk 1, which the helper then holds for a while: the caller blocks.
    const auto caller = std::this_thread::get_id();
    std::atomic_bool helperStarted{ false };
    executor.ParallelFor("blocks", 2, 1, [&](std::size_t, std::size_t) {
        if (std::this_thread::get_id() != caller) {
            helperStarted = true;
            std::this_thread::sleep_for(30ms);
            return;
        }
        for (const auto deadline = std::chrono::steady_clock::now() + 5s; !helperStarted.load() && std::chrono::steady_clock::now() < deadline;)
            std::this_thread::yield();
    });
    assert(helperStarted.load());
    // A helper counts its chunks once it finished them, possibly after the caller returned.
    for (const auto deadline = std::chrono::steady_clock::now() + 5s; Find(executor.GetParallelForStatistics(), "blocks").helperChunks != 1 && std::chrono::steady_clock::now() < deadline;)
        std::this_thread::yield();
    const auto blocks = Find(executor.GetParallelForStatistics(true), "blocks");
    assert(blocks.calls == 1 && blocks.chunks == 2 && blocks.blocked == 1 && blocks.blockedNanoseconds >= 20'000'000 &&
           blocks.maxBlockedNanoseconds == blocks.blockedNanoseconds && blocks.helperChunks == 1);
    assert(Find(executor.GetParallelForStatistics(), "blocks").maxBlockedNanoseconds == 0);  // taken
    // Every worker busy: the caller runs every chunk itself and never waits; the helpers find nothing once they start.
    const auto scope = executor.CreateScope("busy");
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::atomic_uint busy{ 0 };
    for (unsigned i = 0; i != 2; ++i)
        assert(executor.SubmitCpu(scope, 0, 1, "busy", [&](const auto&) {
            busy.fetch_add(1);
            gate.wait();
        }));
    for (const auto deadline = std::chrono::steady_clock::now() + 5s; busy.load() != 2 && std::chrono::steady_clock::now() < deadline;)
        std::this_thread::yield();
    assert(busy.load() == 2);
    std::vector<int> local(100, 0);
    executor.ParallelFor("late", local.size(), 10, [&](std::size_t a_begin, std::size_t a_end) {
        assert(std::this_thread::get_id() == caller);
        for (std::size_t i = a_begin; i < a_end; ++i)
            ++local[i];
    });
    for (int value : local)
        assert(value == 1);
    release.set_value();
    scope->Wait();
    for (const auto deadline = std::chrono::steady_clock::now() + 5s; Find(executor.GetParallelForStatistics(), "late").idleHelpers != 2 && std::chrono::steady_clock::now() < deadline;)
        std::this_thread::yield();
    const auto late = Find(executor.GetParallelForStatistics(), "late");
    assert(late.idleHelpers == 2 && late.helperChunks == 0 && late.blocked == 0 && late.chunks == 10);
}

// The default layout: four domains, the frame-critical threads after them, and the scheduling layout to match.
void TestDefaultLayout()
{
    const auto desc = Executor::DefaultDesc(3);
    assert((desc.domainWorkers == std::vector<unsigned>{ 1, 3, 1, Executor::kDefaultStreamingWorkers }) && desc.parallelDomain == Executor::Preparation.domain);
    std::mutex startsMutex;
    std::multiset<std::pair<std::uint32_t, unsigned>> starts;
    Executor executor(3, [&](org::async::TaskClass a_class, unsigned a_index) {
        assert(a_class.lane == 0);
        std::lock_guard lock(startsMutex);
        starts.emplace(a_class.domain, a_index);
    }, {}, 2);
    const std::multiset<std::pair<std::uint32_t, unsigned>> expected{ { 0, 0 }, { 1, 0 }, { 1, 1 }, { 1, 2 }, { 2, 0 }, { 3, 0 }, { 3, 1 },
        { Executor::FrameCriticalThread.domain, 0 }, { Executor::FrameCriticalThread.domain, 1 } };
    for (const auto deadline = std::chrono::steady_clock::now() + 5s; std::chrono::steady_clock::now() < deadline;) {
        {
            std::lock_guard lock(startsMutex);
            if (starts.size() == expected.size())
                break;
        }
        std::this_thread::yield();
    }
    {
        std::lock_guard lock(startsMutex);
        assert(starts == expected);
    }
    const auto layout = executor.SchedulingLayout();
    assert(layout.laneCount == Executor::Layout.laneCount && layout.domainCount == Executor::Layout.domainCount && layout.Valid());
    assert(executor.DomainCount() == Executor::kDefaultDomains && executor.Workers(Executor::Streaming.domain) == Executor::kDefaultStreamingWorkers);
    assert(executor.ParallelClass().domain == Executor::Preparation.domain && executor.FrameCriticalWorkers() == 2);
    assert(executor.GetStatistics().queued.size() == Executor::kDefaultDomains);
}

// A host's own domains: two of them, the parallel one second, and a frame-critical thread.
void TestCustomLayout()
{
    std::atomic_uint frameCriticalStarts{ 0 };
    Executor::Desc desc;
    desc.domainWorkers = { 1, 3 };
    desc.parallelDomain = 1;
    desc.frameCriticalWorkers = 1;
    desc.onThreadStart = [&](org::async::TaskClass a_class, unsigned) { frameCriticalStarts += a_class.domain == 2; };
    Executor executor(std::move(desc));
    const auto layout = executor.SchedulingLayout();
    assert(layout.laneCount == 1 && layout.domainCount == 2 && layout.Valid());
    assert(executor.Workers(0) == 1 && executor.Workers(1) == 3 && executor.Workers(2) == 0 && executor.FrameCriticalWorkers() == 1);
    assert(executor.ParallelClass().domain == 1 && executor.ParallelClass().lane == 0);
    const auto scope = executor.CreateScope("custom");
    // Classes outside the layout are refused: the default layout's serial domain does not exist here.
    assert(!executor.Submit(scope, 0, 2, "no such domain", [](const auto&) {}));
    assert(!executor.Submit(scope, 1, 0, "no such lane", [](const auto&) {}));
    std::atomic_uint ran{ 0 };
    std::mutex threadsMutex;
    std::set<std::thread::id> threads;
    assert(executor.Submit(scope, 0, 0, "serial", [&](const auto&) { ++ran; }));
    assert(executor.DispatchFrameCritical(scope, "critical", [&](const auto&) { ++ran; }));
    executor.ParallelFor("custom", 64, 1, [&](std::size_t, std::size_t) {
        ++ran;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        std::lock_guard lock(threadsMutex);
        threads.insert(std::this_thread::get_id());
    });
    scope->Wait();
    assert(ran.load() == 66 && threads.size() > 1);
    const auto stats = executor.GetStatistics();
    assert(stats.queued.size() == 2 && stats.rejected == 2);
    for (const auto deadline = std::chrono::steady_clock::now() + 5s; frameCriticalStarts.load() != 1 && std::chrono::steady_clock::now() < deadline;)
        std::this_thread::yield();
    assert(frameCriticalStarts.load() == 1);
}

void TestInvalidLayouts()
{
    const auto refused = [](std::vector<unsigned> a_workers, std::uint32_t a_parallel) {
        Executor::Desc desc;
        desc.domainWorkers = std::move(a_workers);
        desc.parallelDomain = a_parallel;
        try {
            Executor executor(std::move(desc));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    assert(refused({}, 0));
    assert(refused({ 1, 0 }, 0));
    assert(refused({ 1, 1 }, 2));
    assert(!refused({ 1 }, 0));
    bool threw = false;
    try {
        Executor executor(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
}

// Wakes during a pass coalesce into one more pass; a drain that throws stops the lane and reports once; Stop does not report.
void TestLanes()
{
    Executor executor(2);
    std::promise<void> release, inPass;
    const auto gate = release.get_future().share();
    auto entered = inPass.get_future();
    std::atomic_uint passes{ 0 }, refusals{ 0 };
    {
        org::async::Lane lane(executor, { "coalescing", Executor::Preparation, org::async::TaskDispatch::Cpu, [&] { ++refusals; } }, [&] {
            if (passes.fetch_add(1) == 0) {
                inPass.set_value();
                gate.wait();
            }
        });
        assert(lane.Name() == "coalescing");
        assert(lane.Wake());
        Ready(entered);
        for (unsigned i = 0; i != 5; ++i)
            assert(lane.Wake());
        release.set_value();
        for (const auto deadline = std::chrono::steady_clock::now() + 5s; passes.load() != 2 && std::chrono::steady_clock::now() < deadline;)
            std::this_thread::yield();
        std::this_thread::sleep_for(10ms);
        assert(passes.load() == 2);
        lane.Stop();
        assert(!lane.Wake() && refusals.load() == 0);
    }
    std::promise<void> reported;
    auto report = reported.get_future();
    org::async::Lane throwing(executor, { "throwing", Executor::Coordinator, org::async::TaskDispatch::Controlled, [&] { reported.set_value(); } },
        [] { throw std::runtime_error("drain"); });
    assert(throwing.Wake());
    Ready(report);
    assert(!throwing.Wake());
    // No refusal callback: the lane stops silently.
    std::atomic_uint silentPasses{ 0 };
    org::async::Lane silent(executor, { "silent" }, [&] {
        ++silentPasses;
        throw std::runtime_error("drain");
    });
    assert(silent.Wake());
    for (const auto deadline = std::chrono::steady_clock::now() + 5s; silentPasses.load() == 0 && std::chrono::steady_clock::now() < deadline;)
        std::this_thread::yield();
    for (const auto deadline = std::chrono::steady_clock::now() + 5s; silent.Wake() && std::chrono::steady_clock::now() < deadline;)
        std::this_thread::yield();
    assert(!silent.Wake());
    // A process-lifetime lane, on an executor that is never destroyed either.
    auto* forever = new Executor(1);
    std::promise<void> drained;
    auto drainedOnce = drained.get_future();
    std::atomic_bool once{ false };
    auto& pump = org::async::MakeLane(*forever, { "forever" }, [&] {
        if (!once.exchange(true))
            drained.set_value();
    });
    assert(pump.Notify());
    Ready(drainedOnce);
}

// Jobs run once each, in post order, one at a time; Stop drops the queued ones and waits for the running one.
void TestJobLane()
{
    Executor executor(3);
    {
        std::vector<unsigned> order;  // the lane writes alone
        std::atomic_uint running{ 0 };
        std::atomic_bool overlapped{ false };
        std::promise<void> last;
        auto finished = last.get_future();
        org::async::JobLane<unsigned> lane(executor, { "ordered" }, [&](unsigned& a_job) {
            if (running.fetch_add(1) != 0)
                overlapped = true;
            order.push_back(a_job);
            running.fetch_sub(1);
            if (a_job == 199)
                last.set_value();
        });
        for (unsigned i = 0; i != 200; ++i)
            assert(lane.Post(i));
        Ready(finished);
        assert(!overlapped.load() && order.size() == 200);
        for (unsigned i = 0; i != 200; ++i)
            assert(order[i] == i);
    }
    std::promise<void> release, started;
    const auto gate = release.get_future().share();
    auto running = started.get_future();
    std::atomic_uint ran{ 0 };
    org::async::JobLane<int> lane(executor, { "stopped" }, [&](int& a_job) {
        ++ran;
        if (a_job == 0) {
            started.set_value();
            gate.wait();
        }
    });
    assert(lane.Post(0));
    Ready(running);
    for (int i = 1; i != 10; ++i)
        assert(lane.Post(i));
    auto stopping = std::async(std::launch::async, [&] { lane.Stop(); });
    for (const auto deadline = std::chrono::steady_clock::now() + 5s; lane.Post(-1) && std::chrono::steady_clock::now() < deadline;)
        std::this_thread::yield();
    assert(stopping.wait_for(10ms) == std::future_status::timeout);  // waits for the running job
    release.set_value();
    Ready(stopping);
    assert(ran.load() == 1 && !lane.Post(11) && !lane.Underlying().Wake());
}

// A pool whose jobs wait for work in their own domain leaves that work a thread.
void TestWaitingPoolPlan()
{
    Executor four(4), one(1), five(5);
    const auto halved = org::async::PlanWaitingPool(four, Executor::Preparation, Executor::Preparation.domain, Executor::Streaming);
    assert(halved.lanes == 2 && halved.shared && halved.taskClass.domain == Executor::Preparation.domain);
    const auto odd = org::async::PlanWaitingPool(five, Executor::Preparation, Executor::Preparation.domain, Executor::Streaming);
    assert(odd.lanes == 2 && odd.shared);
    const auto alone = org::async::PlanWaitingPool(one, Executor::Preparation, Executor::Preparation.domain, Executor::Streaming);
    assert(alone.lanes == 1 && !alone.shared && alone.taskClass.domain == Executor::Streaming.domain);
    const auto apart = org::async::PlanWaitingPool(four, Executor::Streaming, Executor::Preparation.domain, Executor::Streaming);
    assert(apart.lanes == Executor::kDefaultStreamingWorkers && !apart.shared && apart.taskClass.domain == Executor::Streaming.domain);
}

int main()
{
    TestLanesAndQueues();
    TestOrderingAndFailures();
    TestLifetimeAndShutdown();
    TestParallelFor();
    TestParallelForCoverage();
    TestParallelForPriority();
    TestParallelForWaitAndLateHelpers();
    TestFrameCritical();
    TestWaitBoost();
    TestDefaultLayout();
    TestCustomLayout();
    TestInvalidLayouts();
    TestLanes();
    TestJobLane();
    TestWaitingPoolPlan();
}
