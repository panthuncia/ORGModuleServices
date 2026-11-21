// The OpenRenderGraph task service over the executor: where each priority runs, scope counting, delays, ParallelFor and
// the report of a task that throws.
#include <ORGModuleServices/Async/ExecutorTaskService.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Executor = org::async::Executor;
using org::runtime::TaskPriority;
using namespace std::chrono_literals;

namespace {

// The domain of the executor thread this is, from its ThreadStart.
thread_local std::uint32_t threadDomain = ~0u;

void NoteDomain(org::async::TaskClass a_class, unsigned) { threadDomain = a_class.domain; }

template <class T>
T Ready(std::future<T>& future)
{
    assert(future.wait_for(5s) == std::future_status::ready);
    return future.get();
}

template <class Predicate>
bool Eventually(Predicate a_done)
{
    for (const auto deadline = std::chrono::steady_clock::now() + 5s; std::chrono::steady_clock::now() < deadline;) {
        if (a_done())
            return true;
        std::this_thread::yield();
    }
    return a_done();
}

class ForeignScope final : public org::runtime::ITaskScope
{
public:
    void Cancel() noexcept override {}
    void Wait() override {}
    void CancelAndWait() override {}
};

}

// A throwing task is reported through the callback, counts as finished, and leaves its scope.
void TestThrowReport()
{
    Executor executor(2);
    std::mutex errorsMutex;
    std::vector<std::string> errors;
    const auto service = org::async::CreateExecutorTaskService(executor, { [&](std::string_view a_error) {
        std::lock_guard lock(errorsMutex);
        errors.emplace_back(a_error);
    } });
    const auto scope = service->CreateScope("throws");
    assert(service->Submit(scope, TaskPriority::Streaming, "standard", [] { throw std::runtime_error("boom"); }));
    assert(service->Submit(scope, TaskPriority::FrameCritical, "other", [] { throw 7; }));
    scope->Wait();
    {
        std::lock_guard lock(errorsMutex);
        std::sort(errors.begin(), errors.end());
        assert((errors == std::vector<std::string>{ "other threw", "standard threw: boom" }));
    }
    // Reported, so the executor saw them finish.
    assert(Eventually([&] { return executor.GetStatistics().completed == 2; }) && executor.GetStatistics().failed == 0);
    // The service still runs work after a report.
    std::atomic_bool ran{ false };
    assert(service->Submit(scope, TaskPriority::Background, "after", [&] { ran = true; }));
    scope->Wait();
    assert(ran.load());
}

// Without a callback, the exception goes to the executor, which counts a failed task; the scope is still left.
void TestThrowUnreported()
{
    Executor executor(1);
    const auto service = org::async::CreateExecutorTaskService(executor);
    const auto scope = service->CreateScope("unreported");
    assert(service->Submit(scope, TaskPriority::Streaming, "throws", [] { throw std::runtime_error("boom"); }));
    scope->Wait();
    assert(Eventually([&] { return executor.GetStatistics().failed == 1; }));
}

// Frame-critical work takes the frame-critical queue, so a frame-critical thread runs it while the parallel pool is busy.
// Streaming and background work run on the parallel domain, never on the blocking domain.
void TestRouting()
{
    Executor executor(1, NoteDomain, {}, 1);
    const auto service = org::async::CreateExecutorTaskService(executor);
    const auto scope = service->CreateScope("routing");
    for (const auto priority : { TaskPriority::Streaming, TaskPriority::Background }) {
        std::promise<std::uint32_t> domain;
        auto ranOn = domain.get_future();
        assert(service->Submit(scope, priority, "parallel", [&] { domain.set_value(threadDomain); }));
        assert(Ready(ranOn) == Executor::Preparation.domain);
    }
    // Hold the one parallel worker.
    std::promise<void> release, held;
    const auto gate = release.get_future().share();
    auto holding = held.get_future();
    assert(service->Submit(scope, TaskPriority::Streaming, "holding", [&] {
        held.set_value();
        gate.wait();
    }));
    Ready(holding);
    std::promise<std::uint32_t> critical;
    auto criticalOn = critical.get_future();
    assert(service->Submit(scope, TaskPriority::FrameCritical, "critical", [&] { critical.set_value(threadDomain); }));
    assert(Ready(criticalOn) == Executor::FrameCriticalThread.domain);
    // A delayed frame-critical task goes to the parallel domain's ordinary queue, behind the held worker.
    std::atomic_bool delayedRan{ false };
    assert(service->ScheduleAfter(scope, 1ms, TaskPriority::FrameCritical, "delayed critical", [&] { delayedRan = true; }));
    std::this_thread::sleep_for(20ms);
    assert(!delayedRan.load());
    release.set_value();
    scope->Wait();
    assert(delayedRan.load());
}

// Scopes count queued and running tasks; a cancelled scope refuses new work and drops what has not started; foreign scopes
// and empty tasks are refused; a null scope is accepted.
void TestScopes()
{
    Executor executor(1);
    const auto service = org::async::CreateExecutorTaskService(executor);
    const auto scope = service->CreateScope("scope");
    std::promise<void> release, held;
    const auto gate = release.get_future().share();
    auto holding = held.get_future();
    assert(service->Submit(scope, TaskPriority::Streaming, "holding", [&] {
        held.set_value();
        gate.wait();
    }));
    Ready(holding);
    std::atomic_bool queuedRan{ false };
    assert(service->Submit(scope, TaskPriority::Streaming, "queued", [&] { queuedRan = true; }));
    auto waiting = std::async(std::launch::async, [&] { scope->Wait(); });
    assert(waiting.wait_for(10ms) == std::future_status::timeout);
    scope->Cancel();
    assert(!service->Submit(scope, TaskPriority::Streaming, "cancelled", [] {}));
    release.set_value();
    Ready(waiting);
    assert(!queuedRan.load());
    const auto foreign = std::make_shared<ForeignScope>();
    assert(!service->Submit(foreign, TaskPriority::Streaming, "foreign", [] {}));
    assert(!service->Submit(service->CreateScope("empty"), TaskPriority::Streaming, "empty", {}));
    std::promise<void> unscoped;
    auto unscopedRan = unscoped.get_future();
    assert(service->Submit(nullptr, TaskPriority::Background, "unscoped", [&] { unscoped.set_value(); }));
    Ready(unscopedRan);
    // A delay holds the task back.
    const auto delayed = service->CreateScope("delayed");
    const auto start = std::chrono::steady_clock::now();
    std::atomic<std::chrono::steady_clock::duration> after{};
    assert(service->ScheduleAfter(delayed, 20ms, TaskPriority::Streaming, "delayed", [&] { after = std::chrono::steady_clock::now() - start; }));
    delayed->Wait();
    assert(after.load() >= 20ms);
}

// Every index once; ParallelForLimited makes at most the given number of chunks.
void TestParallelFor()
{
    Executor executor(3);
    const auto service = org::async::CreateExecutorTaskService(executor);
    std::vector<std::atomic_uint> hits(1000);
    service->ParallelFor("all", hits.size(), [&](std::size_t a_index) { hits[a_index].fetch_add(1); });
    for (const auto& hit : hits)
        assert(hit.load() == 1);
    std::vector<std::atomic_uint> limited(1000);
    service->ParallelForLimited("limited", limited.size(), 3, [&](std::size_t a_index) { limited[a_index].fetch_add(1); });
    for (const auto& hit : limited)
        assert(hit.load() == 1);
    service->ParallelFor("empty", 0, [](std::size_t) { assert(false); });
    for (const auto& stats : executor.GetParallelForStatistics()) {
        if (stats.name == "limited")
            assert(stats.chunks == 3);
        if (stats.name == "all")
            assert(stats.chunks == 1000);
    }
}

int main()
{
    TestThrowReport();
    TestThrowUnreported();
    TestRouting();
    TestScopes();
    TestParallelFor();
}
