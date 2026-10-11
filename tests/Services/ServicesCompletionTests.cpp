// Completion callbacks and the executor hook of ShaderCompiler and PipelineService.
#include <ORGModuleServices/PipelineService.h>
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
#include <ORGModuleServices/ShaderCompiler.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<int> failures{ 0 };
#define CHECK(expression) do { if (!(expression)) { std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } } while (false)

using namespace std::chrono_literals;
using org::services::ServiceTask;

template<class T> bool IsReady(const std::shared_future<T>& future) { return future.wait_for(0s) == std::future_status::ready; }

// A queue the test drains on its own thread: work runs only when, and where, the test says.
class ManualExecutor {
public:
    org::services::ServiceSubmit Submitter() {
        return [this](ServiceTask task) { std::scoped_lock lock(mutex_); queue_.push_back(std::move(task)); ++submitted_; return true; };
    }
    size_t Pending() const { std::scoped_lock lock(mutex_); return queue_.size(); }
    size_t Submitted() const { std::scoped_lock lock(mutex_); return submitted_; }
    // Runs until empty, including tasks submitted while draining.
    size_t Drain() {
        size_t ran = 0;
        for (;;) {
            ServiceTask task;
            { std::scoped_lock lock(mutex_); if (queue_.empty()) return ran; task = std::move(queue_.front()); queue_.pop_front(); }
            task(); ++ran;
        }
    }
private:
    mutable std::mutex mutex_;
    std::deque<ServiceTask> queue_;
    size_t submitted_{};
};

// A fixed-size pool; records its thread ids.
class FixedPool {
public:
    explicit FixedPool(size_t count) {
        for (size_t i = 0; i < count; ++i) threads_.emplace_back([this] { Run(); });
        std::unique_lock lock(mutex_);
        started_.wait(lock, [&] { return ids_.size() == threads_.size(); });
    }
    ~FixedPool() {
        { std::scoped_lock lock(mutex_); stop_ = true; }
        condition_.notify_all();
        for (auto& thread : threads_) thread.join();
    }
    org::services::ServiceSubmit Submitter() {
        return [this](ServiceTask task) {
            { std::scoped_lock lock(mutex_); queue_.push_back(std::move(task)); }
            condition_.notify_one(); return true;
        };
    }
    bool Owns(std::thread::id id) const { std::scoped_lock lock(mutex_); return ids_.contains(id); }
private:
    void Run() {
        { std::scoped_lock lock(mutex_); ids_.insert(std::this_thread::get_id()); }
        started_.notify_all();
        for (;;) {
            ServiceTask task;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) return;
                task = std::move(queue_.front()); queue_.pop_front();
            }
            task();
        }
    }
    mutable std::mutex mutex_;
    std::condition_variable condition_, started_;
    std::deque<ServiceTask> queue_;
    std::set<std::thread::id> ids_;
    std::vector<std::thread> threads_;
    bool stop_{};
};

// Counts callbacks and lets a test wait for a total.
class Counter {
public:
    void Hit() { std::scoped_lock lock(mutex_); ++count_; condition_.notify_all(); }
    size_t Count() const { std::scoped_lock lock(mutex_); return count_; }
    bool WaitFor(size_t target, std::chrono::seconds timeout = 60s) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] { return count_ >= target; });
    }
private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    size_t count_{};
};

org::services::PipelineRecipe Recipe(std::string id, uint64_t shaderKey, std::atomic_uint32_t* builds = nullptr,
    std::vector<std::thread::id>* buildThreads = nullptr, std::mutex* buildThreadsMutex = nullptr) {
    return { std::move(id), 1, shaderKey, 3, 4, [=] {
        if (builds) ++*builds;
        if (buildThreads) { std::scoped_lock lock(*buildThreadsMutex); buildThreads->push_back(std::this_thread::get_id()); }
        return std::static_pointer_cast<void>(std::make_shared<uint64_t>(shaderKey));
    } };
}

void PipelineDefaultUnchanged() {
    org::services::PipelineService pipelines;
    std::atomic_uint32_t builds{};
    std::vector<std::thread::id> threads; std::mutex threadsMutex;
    auto recipe = Recipe("tests.default", 1, &builds, &threads, &threadsMutex);
    const auto key = pipelines.BuildKey(recipe);
    auto first = pipelines.Request(recipe);
    auto duplicate = pipelines.Request(recipe);
    CHECK(first.get()); CHECK(duplicate.get()); CHECK(builds == 1);
    CHECK(threads.size() == 1 && threads[0] != std::this_thread::get_id()); // still a thread of its own
    pipelines.PublishReady(5); CHECK(pipelines.Find(key)); CHECK(pipelines.InFlightCount() == 0);
    // With a callback, still default executor: once, on the building thread, after the future is ready.
    Counter calls; std::thread::id callbackThread;
    bool joinedReady = false;
    auto second = Recipe("tests.default", 2);
    auto future = pipelines.Request(second, [&](const org::services::PipelineArtifact& artifact) {
        callbackThread = std::this_thread::get_id();
        // Re-entering the service from the callback joins the now-done flight: its future is ready already.
        joinedReady = IsReady(pipelines.Request(second)) && artifact;
        calls.Hit();
    });
    CHECK(calls.WaitFor(1)); CHECK(future.get());
    CHECK(joinedReady); CHECK(calls.Count() == 1); CHECK(callbackThread != std::this_thread::get_id());
}

void PipelineCallbacksThroughManualQueue() {
    ManualExecutor executor;
    org::services::PipelineService pipelines;
    pipelines.Configure(executor.Submitter());
    std::atomic_uint32_t builds{};
    std::vector<std::thread::id> buildThreads; std::mutex buildThreadsMutex;
    auto recipe = Recipe("tests.manual", 7, &builds, &buildThreads, &buildThreadsMutex);
    const auto key = pipelines.BuildKey(recipe);
    int a = 0, b = 0, c = 0, d = 0, e = 0;
    std::vector<std::thread::id> callbackThreads;
    auto note = [&](int& count) { return [&](const org::services::PipelineArtifact& artifact) { CHECK(artifact); ++count; callbackThreads.push_back(std::this_thread::get_id()); }; };
    auto first = pipelines.Request(recipe, note(a));
    auto joined = pipelines.Request(recipe, note(b));    // joins before completion
    auto plainJoin = pipelines.Request(recipe);           // a future-only joiner
    CHECK(executor.Pending() == 1); CHECK(builds == 0);    // nothing ran anywhere: the library made no thread
    CHECK(!IsReady(first) && !IsReady(joined) && !IsReady(plainJoin));
    CHECK(a == 0 && b == 0);
    CHECK(executor.Drain() == 1);
    CHECK(builds == 1); CHECK(a == 1 && b == 1); CHECK(IsReady(first) && IsReady(joined) && IsReady(plainJoin));
    CHECK(buildThreads.size() == 1 && buildThreads[0] == std::this_thread::get_id());
    // Joins after completion (still in flight until PublishReady): at once, on this thread, no new work.
    auto late = pipelines.Request(recipe, note(c));
    CHECK(c == 1); CHECK(IsReady(late)); CHECK(executor.Pending() == 0);
    pipelines.PublishReady(1); CHECK(pipelines.Find(key));
    // Active hit: at once, on this thread.
    auto active = pipelines.Request(recipe, note(d));
    CHECK(d == 1); CHECK(IsReady(active)); CHECK(active.get().key == key); CHECK(executor.Pending() == 0);
    for (const auto id : callbackThreads) CHECK(id == std::this_thread::get_id());
    // A new key through the queue again.
    pipelines.Request(Recipe("tests.manual", 8, &builds), note(e));
    CHECK(e == 0); CHECK(executor.Drain() == 1); CHECK(e == 1); CHECK(builds == 2);
    // A null callback is allowed.
    pipelines.Request(Recipe("tests.manual", 9), {}); CHECK(executor.Drain() == 1);
    CHECK(a == 1 && b == 1 && c == 1 && d == 1 && e == 1);
}

void PipelineFailuresReachCallbacks() {
    ManualExecutor executor;
    org::services::PipelineService pipelines;
    pipelines.Configure(executor.Submitter());
    int nullCalls = 0, throwCalls = 0, noFactoryCalls = 0, joinCalls = 0;
    auto expectFailure = [](int& count) { return [&](const org::services::PipelineArtifact& artifact) { CHECK(!artifact); CHECK(!artifact.error.empty()); ++count; }; };
    org::services::PipelineRecipe returnsNull{ "tests.fail.null", 1, 1, 1, 1, [] { return org::services::PipelinePayload{}; } };
    org::services::PipelineRecipe throws{ "tests.fail.throw", 1, 1, 1, 1, []() -> org::services::PipelinePayload { throw std::runtime_error("boom"); } };
    org::services::PipelineRecipe noFactory{ "tests.fail.none", 1, 1, 1, 1, {} };
    auto f1 = pipelines.Request(returnsNull, expectFailure(nullCalls));
    pipelines.Request(returnsNull, expectFailure(joinCalls));
    auto f2 = pipelines.Request(throws, expectFailure(throwCalls));
    auto f3 = pipelines.Request(noFactory, expectFailure(noFactoryCalls));
    CHECK(executor.Drain() == 3);
    CHECK(nullCalls == 1 && joinCalls == 1 && throwCalls == 1 && noFactoryCalls == 1);
    CHECK(f2.get().error == "boom"); CHECK(!f1.get()); CHECK(!f3.get());
    // A refusing submitter: the request completes at once, with an error, on the caller.
    org::services::PipelineService refusing;
    refusing.Configure([](ServiceTask) { return false; });
    int refusedCalls = 0;
    auto refused = refusing.Request(Recipe("tests.refused", 1), expectFailure(refusedCalls));
    CHECK(refusedCalls == 1); CHECK(IsReady(refused)); CHECK(!refused.get());
    // A throwing submitter is a refusal too.
    org::services::PipelineService throwing;
    throwing.Configure([](ServiceTask) -> bool { throw std::runtime_error("queue closed"); });
    int thrownCalls = 0;
    auto thrown = throwing.Request(Recipe("tests.thrown", 1), expectFailure(thrownCalls));
    CHECK(thrownCalls == 1); CHECK(IsReady(thrown));
}

void PipelineCallbackReentersWithoutDeadlock() {
    for (const bool manual : { true, false }) {
        ManualExecutor executor;
        org::services::PipelineService pipelines;
        if (manual) pipelines.Configure(executor.Submitter());
        Counter calls;
        pipelines.Request(Recipe("tests.reenter", 1), [&](const org::services::PipelineArtifact&) {
            // Every service entry point from inside a callback.
            pipelines.Request(Recipe("tests.reenter", 2), [&](const org::services::PipelineArtifact&) { calls.Hit(); });
            pipelines.Request(Recipe("tests.reenter", 1), [&](const org::services::PipelineArtifact&) { calls.Hit(); }); // joins its own, done, flight
            (void)pipelines.InFlightCount(); (void)pipelines.Find(1);
            pipelines.PublishReady(1);
            calls.Hit(); // last: the test may end once all three are counted
        });
        if (manual) executor.Drain();
        CHECK(calls.WaitFor(3)); CHECK(calls.Count() == 3);
    }
}

void PipelineFixedPool() {
    constexpr uint32_t kRequests = 64;
    org::services::PipelineService pipelines;
    std::atomic_uint32_t builds{};
    std::vector<std::thread::id> buildThreads; std::mutex buildThreadsMutex;
    Counter calls;
    std::atomic_uint32_t foreignCallbacks{};
    {
        FixedPool pool(2);
        pipelines.Configure(pool.Submitter());
        std::vector<std::shared_future<org::services::PipelineArtifact>> futures;
        for (uint32_t i = 0; i < kRequests; ++i)
            futures.push_back(pipelines.Request(Recipe("tests.pool", 100 + i, &builds, &buildThreads, &buildThreadsMutex),
                [&](const org::services::PipelineArtifact& artifact) {
                    if (!pool.Owns(std::this_thread::get_id())) ++foreignCallbacks;
                    CHECK(artifact); calls.Hit();
                }));
        CHECK(calls.WaitFor(kRequests));
        for (auto& future : futures) CHECK(future.get());
        for (const auto id : buildThreads) CHECK(pool.Owns(id)); // every build ran on the pool's two threads
    }
    CHECK(builds == kRequests); CHECK(calls.Count() == kRequests); CHECK(foreignCallbacks == 0);
    pipelines.PublishReady(1); CHECK(pipelines.InFlightCount() == 0);
}

// Joiners racing a completing build: every caller's callback exactly once.
void PipelineConcurrentJoiners() {
    constexpr int kKeys = 200, kJoiners = 6;
    org::services::PipelineService pipelines;
    Counter calls;
    std::atomic_uint32_t builds{};
    FixedPool pool(3);
    pipelines.Configure(pool.Submitter());
    for (int k = 0; k < kKeys; ++k) {
        auto recipe = Recipe("tests.race", 1000 + k, &builds);
        std::vector<std::thread> joiners;
        for (int j = 0; j < kJoiners; ++j)
            joiners.emplace_back([&] { pipelines.Request(recipe, [&](const org::services::PipelineArtifact& artifact) { CHECK(artifact); calls.Hit(); }); });
        for (auto& joiner : joiners) joiner.join();
        if (k % 16 == 0) pipelines.PublishReady(k); // some later keys' joiners race a publish too
    }
    CHECK(calls.WaitFor(size_t(kKeys) * kJoiners));
    std::this_thread::sleep_for(50ms);
    CHECK(calls.Count() == size_t(kKeys) * kJoiners);
    CHECK(builds == kKeys);
}

#if defined(ORG_MODULE_SERVICES_HAS_DXC)
constexpr char kShader[] = "[numthreads(1,1,1)] void main() {}";
constexpr char kBrokenShader[] = "this is not hlsl";

org::services::ShaderCompileRequest ShaderRequest(const char* source, int permutation = 0) {
    org::services::ShaderCompileRequest request{};
    request.sourceName = "completion-test.hlsl";
    request.source = { reinterpret_cast<const std::byte*>(source), std::strlen(source) };
    request.entryPoint = L"main"; request.target = L"cs_6_0";
    request.defines.push_back({ L"PERMUTATION", std::to_wstring(permutation) });
    return request;
}

std::filesystem::path CompilerDirectory() {
    if (const char* directory = std::getenv("ORG_SERVICES_TEST_DXC_DIR")) return directory;
    return {};
}

// Whether a valid shader compiles here (DXC present, and a validator to sign DXIL).
bool shaderCompilesSucceed = false;

void ShaderDefaultUnchanged() {
    org::services::ShaderCompiler compiler({}, CompilerDirectory());
    std::printf("  DXC available: %s\n", compiler.Available() ? "yes" : "no");
    auto artifact = compiler.Compile(ShaderRequest(kShader));
    shaderCompilesSucceed = static_cast<bool>(artifact);
    std::printf("  valid shader compiles: %s%s%s\n", shaderCompilesSucceed ? "yes" : "no", shaderCompilesSucceed ? "" : ": ",
        shaderCompilesSucceed ? "" : artifact.diagnostics.c_str());
    if (compiler.Available()) CHECK(!artifact.diagnostics.empty() || artifact);
    else CHECK(!artifact && !artifact.diagnostics.empty());
    auto a = compiler.CompileAsync(ShaderRequest(kShader, 1));
    auto b = compiler.CompileAsync(ShaderRequest(kShader, 1));
    CHECK(static_cast<bool>(a.get()) == shaderCompilesSucceed); CHECK(static_cast<bool>(b.get()) == shaderCompilesSucceed);
    // With a callback, default executor: once, on another thread.
    Counter calls; std::thread::id callbackThread;
    auto future = compiler.CompileAsync(ShaderRequest(kShader, 2), [&](const org::services::ShaderArtifact&) {
        callbackThread = std::this_thread::get_id(); calls.Hit();
    });
    CHECK(calls.WaitFor(1)); future.get();
    CHECK(calls.Count() == 1); CHECK(callbackThread != std::this_thread::get_id());
}

void ShaderCallbacksThroughManualQueue() {
    ManualExecutor executor;
    org::services::ShaderCompiler compiler({}, CompilerDirectory());
    compiler.Configure(executor.Submitter());
    int a = 0, b = 0, c = 0;
    std::vector<std::thread::id> threads;
    std::shared_future<org::services::ShaderArtifact> first;
    bool firstReadyInCallback = false;
    first = compiler.CompileAsync(ShaderRequest(kShader, 10), [&](const org::services::ShaderArtifact& artifact) {
        firstReadyInCallback = IsReady(first); // set before the drain below
        CHECK(static_cast<bool>(artifact) == shaderCompilesSucceed); ++a; threads.push_back(std::this_thread::get_id());
    });
    auto joined = compiler.CompileAsync(ShaderRequest(kShader, 10), [&](const org::services::ShaderArtifact& artifact) {
        CHECK(static_cast<bool>(artifact) == shaderCompilesSucceed); ++b; threads.push_back(std::this_thread::get_id());
    });
    auto plainJoin = compiler.CompileAsync(ShaderRequest(kShader, 10));
    CHECK(executor.Pending() == 1); CHECK(!IsReady(first) && !IsReady(joined) && !IsReady(plainJoin)); CHECK(a == 0 && b == 0);
    CHECK(executor.Drain() == 1);
    CHECK(a == 1 && b == 1); CHECK(firstReadyInCallback); CHECK(IsReady(joined) && IsReady(plainJoin));
    for (const auto id : threads) CHECK(id == std::this_thread::get_id());
    // After completion: a memory-cache hit at once on this thread (or, if the compile failed, a fresh flight).
    auto again = compiler.CompileAsync(ShaderRequest(kShader, 10), [&](const org::services::ShaderArtifact& artifact) {
        if (shaderCompilesSucceed) CHECK(artifact.fromCache);
        ++c;
    });
    if (shaderCompilesSucceed) { CHECK(c == 1); CHECK(IsReady(again)); CHECK(executor.Pending() == 0); }
    else { CHECK(c == 0); CHECK(executor.Pending() == 1); executor.Drain(); CHECK(c == 1); }
}

void ShaderFailureReachesCallbacks() {
    ManualExecutor executor;
    org::services::ShaderCompiler compiler({}, CompilerDirectory());
    compiler.Configure(executor.Submitter());
    int calls = 0;
    auto expectFailure = [&](const org::services::ShaderArtifact& artifact) { CHECK(!artifact); CHECK(!artifact.diagnostics.empty()); ++calls; };
    auto f = compiler.CompileAsync(ShaderRequest(kBrokenShader), expectFailure);
    compiler.CompileAsync(ShaderRequest(kBrokenShader), expectFailure);
    CHECK(executor.Drain() == 1); CHECK(calls == 2); CHECK(!f.get());
    // Failures are not memory-cached: the next request compiles again.
    compiler.CompileAsync(ShaderRequest(kBrokenShader), expectFailure);
    CHECK(executor.Drain() == 1); CHECK(calls == 3);
    org::services::ShaderCompiler refusing({}, CompilerDirectory());
    refusing.Configure([](ServiceTask) { return false; });
    auto refused = refusing.CompileAsync(ShaderRequest(kShader, 20), expectFailure);
    CHECK(calls == 4); CHECK(IsReady(refused)); CHECK(!refused.get());
    // The refused request left no flight behind: a retry submits again.
    refusing.Configure(executor.Submitter());
    refusing.CompileAsync(ShaderRequest(kShader, 20));
    CHECK(executor.Pending() == 1); executor.Drain();
}

void ShaderCallbackReentersWithoutDeadlock() {
    for (const bool manual : { true, false }) {
        ManualExecutor executor;
        org::services::ShaderCompiler compiler({}, CompilerDirectory());
        if (manual) compiler.Configure(executor.Submitter());
        Counter calls;
        compiler.CompileAsync(ShaderRequest(kShader, 30), [&](const org::services::ShaderArtifact&) {
            compiler.CompileAsync(ShaderRequest(kShader, 31), [&](const org::services::ShaderArtifact&) { calls.Hit(); });
            compiler.CompileAsync(ShaderRequest(kShader, 30), [&](const org::services::ShaderArtifact&) { calls.Hit(); });
            (void)compiler.BuildKey(ShaderRequest(kShader, 32));
            compiler.ClearMemoryCache();
            calls.Hit(); // last: the test may end once all three are counted
        });
        if (manual) executor.Drain();
        CHECK(calls.WaitFor(3)); CHECK(calls.Count() == 3);
    }
}

void ShaderFixedPoolAndNoLibraryThreads() {
    constexpr int kRequests = 16;
    {
        // Queued, never drained: no compile runs anywhere until the test runs it.
        ManualExecutor executor;
        org::services::ShaderCompiler compiler({}, CompilerDirectory());
        compiler.Configure(executor.Submitter());
        std::vector<std::shared_future<org::services::ShaderArtifact>> futures;
        int calls = 0;
        for (int i = 0; i < kRequests; ++i) futures.push_back(compiler.CompileAsync(ShaderRequest(kShader, 100 + i), [&](const auto&) { ++calls; }));
        std::this_thread::sleep_for(20ms);
        CHECK(executor.Pending() == size_t(kRequests));
        for (auto& future : futures) CHECK(!IsReady(future));
        CHECK(calls == 0);
        CHECK(executor.Drain() == size_t(kRequests)); CHECK(calls == kRequests);
    }
    org::services::ShaderCompiler compiler({}, CompilerDirectory());
    Counter calls; std::atomic_uint32_t foreign{};
    FixedPool pool(2);
    compiler.Configure(pool.Submitter());
    for (int i = 0; i < kRequests; ++i)
        compiler.CompileAsync(ShaderRequest(kShader, 200 + i), [&](const org::services::ShaderArtifact& artifact) {
            if (!pool.Owns(std::this_thread::get_id())) ++foreign; // the callback runs on the compiling thread
            CHECK(static_cast<bool>(artifact) == shaderCompilesSucceed); calls.Hit();
        });
    CHECK(calls.WaitFor(kRequests)); CHECK(foreign == 0);
}

void ShaderConcurrentJoiners() {
    constexpr int kKeys = 40, kJoiners = 6;
    org::services::ShaderCompiler compiler({}, CompilerDirectory());
    Counter calls;
    FixedPool pool(3);
    compiler.Configure(pool.Submitter());
    for (int k = 0; k < kKeys; ++k) {
        std::vector<std::thread> joiners;
        for (int j = 0; j < kJoiners; ++j)
            joiners.emplace_back([&, k] { compiler.CompileAsync(ShaderRequest(kShader, 1000 + k), [&](const org::services::ShaderArtifact&) { calls.Hit(); }); });
        for (auto& joiner : joiners) joiner.join();
    }
    CHECK(calls.WaitFor(size_t(kKeys) * kJoiners));
    std::this_thread::sleep_for(50ms);
    CHECK(calls.Count() == size_t(kKeys) * kJoiners);
}

void ShaderCompilerOutlivedByQueuedWork() {
    ManualExecutor executor;
    int calls = 0;
    std::shared_future<org::services::ShaderArtifact> future;
    {
        org::services::ShaderCompiler compiler({}, CompilerDirectory());
        compiler.Configure(executor.Submitter());
        future = compiler.CompileAsync(ShaderRequest(kShader, 500), [&](const org::services::ShaderArtifact&) { ++calls; });
    }
    CHECK(executor.Drain() == 1); CHECK(calls == 1); CHECK(IsReady(future));
}
#endif

struct Test { const char* name; void (*run)(); };

}

int main() {
    const Test tests[] = {
        { "PipelineDefaultUnchanged", PipelineDefaultUnchanged },
        { "PipelineCallbacksThroughManualQueue", PipelineCallbacksThroughManualQueue },
        { "PipelineFailuresReachCallbacks", PipelineFailuresReachCallbacks },
        { "PipelineCallbackReentersWithoutDeadlock", PipelineCallbackReentersWithoutDeadlock },
        { "PipelineFixedPool", PipelineFixedPool },
        { "PipelineConcurrentJoiners", PipelineConcurrentJoiners },
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
        { "ShaderDefaultUnchanged", ShaderDefaultUnchanged },
        { "ShaderCallbacksThroughManualQueue", ShaderCallbacksThroughManualQueue },
        { "ShaderFailureReachesCallbacks", ShaderFailureReachesCallbacks },
        { "ShaderCallbackReentersWithoutDeadlock", ShaderCallbackReentersWithoutDeadlock },
        { "ShaderFixedPoolAndNoLibraryThreads", ShaderFixedPoolAndNoLibraryThreads },
        { "ShaderConcurrentJoiners", ShaderConcurrentJoiners },
        { "ShaderCompilerOutlivedByQueuedWork", ShaderCompilerOutlivedByQueuedWork },
#endif
    };
#if !defined(ORG_MODULE_SERVICES_HAS_DXC)
    std::printf("ShaderCompiler tests skipped: built without DXC\n");
#endif
    for (const auto& test : tests) {
        const int before = failures;
        std::printf("[ RUN  ] %s\n", test.name);
        std::fflush(stdout);
        test.run();
        std::printf("[ %s ] %s\n", failures == before ? " OK " : "FAIL", test.name);
    }
    std::printf("%s: %d failed check(s)\n", failures ? "FAILED" : "PASSED", failures.load());
    return failures ? 1 : 0;
}
