// Completion callbacks and the executor hook of ShaderCompiler and BuildPipeline.
#include <ORGModuleServices/PipelineRegistry.h>
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
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <span>
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

// A build runs on the submitter's thread and reaches done there, once.
void PipelineBuildThroughManualQueue() {
    ManualExecutor executor;
    Counter calls;
    std::thread::id buildThread, doneThread;
    org::services::BuildPipeline(executor.Submitter(), [&] {
        buildThread = std::this_thread::get_id();
        return std::static_pointer_cast<void>(std::make_shared<uint64_t>(7));
    }, [&](org::services::LiveBuild build) {
        CHECK(build.payload); CHECK(build.error.empty());
        doneThread = std::this_thread::get_id(); calls.Hit();
    });
    CHECK(executor.Pending() == 1); CHECK(calls.Count() == 0);
    std::thread drain([&] { executor.Drain(); });
    drain.join();
    CHECK(calls.Count() == 1); CHECK(buildThread == doneThread); CHECK(buildThread != std::this_thread::get_id());
}

// A null payload, a throw, no build function and a refused submission each reach done with an error.
void PipelineBuildFailuresReachDone() {
    Counter calls;
    auto expectFailure = [&] { return [&](org::services::LiveBuild build) { CHECK(!build.payload); CHECK(!build.error.empty()); calls.Hit(); }; };
    const org::services::ServiceSubmit inline_ = [](ServiceTask task) { task(); return true; };
    org::services::BuildPipeline(inline_, [] { return org::services::PipelinePayload{}; }, expectFailure());
    org::services::BuildPipeline(inline_, []() -> org::services::PipelinePayload { throw std::runtime_error("boom"); }, expectFailure());
    org::services::BuildPipeline(inline_, {}, expectFailure());
    org::services::BuildPipeline([](ServiceTask) { return false; }, [] { return org::services::PipelinePayload{}; }, expectFailure());
    org::services::BuildPipeline([](ServiceTask) -> bool { throw std::runtime_error("refused"); }, [] { return org::services::PipelinePayload{}; },
        expectFailure());
    CHECK(calls.Count() == 5);
}

// Many builds on a fixed pool all complete on its threads.
void PipelineBuildFixedPool() {
    constexpr size_t kBuilds = 64;
    Counter calls;
    std::atomic_uint32_t foreign{};
    {
        FixedPool pool(3);
        for (size_t i = 0; i < kBuilds; ++i)
            org::services::BuildPipeline(pool.Submitter(), [i] { return std::static_pointer_cast<void>(std::make_shared<size_t>(i)); },
                [&](org::services::LiveBuild build) {
                    CHECK(build.payload);
                    if (!pool.Owns(std::this_thread::get_id())) ++foreign;
                    calls.Hit();
                });
        CHECK(calls.WaitFor(kBuilds));
    }
    CHECK(foreign == 0);
}

// Without a submitter a build runs on a new thread.
void PipelineBuildDefaultThread() {
    Counter calls;
    const auto caller = std::this_thread::get_id();
    std::atomic_bool otherThread{};
    org::services::BuildPipeline({}, [] { return std::static_pointer_cast<void>(std::make_shared<int>(1)); }, [&](org::services::LiveBuild build) {
        CHECK(build.payload);
        otherThread = std::this_thread::get_id() != caller;
        calls.Hit();
    });
    CHECK(calls.WaitFor(1)); CHECK(otherThread);
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

// The cache as the compiler uses it: real DXC, real files, and a new compiler for each "boot".
namespace cache {

namespace fs = std::filesystem;

struct Folder {
    Folder() {
        static std::atomic<int> counter{};
        root = fs::temp_directory_path() / ("org_compiler_cache_test_" + std::to_string(++counter));
        fs::remove_all(root);
        Write("Shaders/Root.hlsl", "#include \"Common/Value.hlsli\"\nfloat4 main() : SV_Target { return Value(); }\n");
        Write("Shaders/Common/Value.hlsli", "float4 Value() { return float4(1, 2, 3, 4); }\n");
    }
    ~Folder() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path root;
    void Write(const std::string& relative, const std::string& text) const {
        fs::create_directories((root / relative).parent_path());
        std::ofstream(root / relative, std::ios::binary | std::ios::trunc) << text;
    }
    std::shared_ptr<org::services::ShaderStore> Store(bool persistent = true) const {
        org::services::ShaderStoreOptions options;
        options.sourceRoot = root;
        options.cacheDirectory = root / "Cache";
        options.saveInterval = std::chrono::milliseconds(0);
        options.persistent = persistent;
        return std::make_shared<org::services::ShaderStore>(std::move(options));
    }
    org::services::ShaderCompileRequest Request(const std::string& source, std::vector<org::services::ShaderDefine> defines = {}, bool debug = false) const {
        org::services::ShaderCompileRequest request;
        request.sourceName = (root / "Shaders" / "Root.hlsl").generic_string();
        request.source = std::as_bytes(std::span(source));
        request.entryPoint = L"main";
        request.target = L"ps_6_0";
        request.warningsAsErrors = false;
        request.includeDirectories = { root / "Shaders" };
        request.defines = std::move(defines);
        request.debugInfo = debug;
        return request;
    }
    std::string RootSource() const {
        std::ifstream stream(root / "Shaders/Root.hlsl", std::ios::binary);
        return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
    }
};

struct Boot {
    explicit Boot(const Folder& folder, bool persistent = true) : store(folder.Store(persistent)), compiler({}, CompilerDirectory(), store) {}
    uint64_t Stat(uint64_t org::services::ShaderStoreStats::*field) const { return store->Stats().*field; }
    std::shared_ptr<org::services::ShaderStore> store;
    org::services::ShaderCompiler compiler;
};

}  // namespace cache

void CacheSecondBootDoesNoWork() {
    if (!shaderCompilesSucceed) return;
    cache::Folder folder;
    const auto source = folder.RootSource();
    org::services::ShaderArtifact first;
    {
        cache::Boot boot(folder);
        first = boot.compiler.Compile(folder.Request(source));
        CHECK(first); CHECK(!first.fromCache);
        CHECK(boot.store->Stats().compiled == 1);
    }
    cache::Boot boot(folder);
    const auto second = boot.compiler.Compile(folder.Request(source));
    CHECK(second); CHECK(second.fromCache);
    CHECK(second.binary == first.binary);
    const auto stats = boot.store->Stats();
    CHECK(stats.hits == 1); CHECK(stats.compiled == 0); CHECK(stats.preprocessed == 0);
}

void CacheIncludeEditsThroughRealCompiler() {
    if (!shaderCompilesSucceed) return;
    cache::Folder folder;
    const auto source = folder.RootSource();
    org::services::ShaderArtifact original;
    { cache::Boot boot(folder); original = boot.compiler.Compile(folder.Request(source)); CHECK(original); }
    // A comment is a real file change that does not change the code: preprocess, no compile, the same bytes.
    folder.Write("Shaders/Common/Value.hlsli", "// a comment\n\nfloat4 Value() { return float4(1, 2, 3, 4); }\n");
    {
        cache::Boot boot(folder);
        const auto artifact = boot.compiler.Compile(folder.Request(source));
        CHECK(artifact.binary == original.binary);
        const auto stats = boot.store->Stats();
        CHECK(stats.absorbed == 1); CHECK(stats.compiled == 0); CHECK(stats.preprocessed == 1);
    }
    // A real edit compiles and the output changes.
    folder.Write("Shaders/Common/Value.hlsli", "float4 Value() { return float4(5, 6, 7, 8); }\n");
    {
        cache::Boot boot(folder);
        const auto artifact = boot.compiler.Compile(folder.Request(source));
        CHECK(artifact); CHECK(artifact.binary != original.binary);
        CHECK(boot.store->Stats().compiled == 1);
    }
    // Reverting finds the first artifact without a compile.
    folder.Write("Shaders/Common/Value.hlsli", "float4 Value() { return float4(1, 2, 3, 4); }\n");
    {
        cache::Boot boot(folder);
        CHECK(boot.compiler.Compile(folder.Request(source)).binary == original.binary);
        CHECK(boot.store->Stats().compiled == 0);
    }
}

void CacheRecordsMissingIncludePaths() {
    if (!shaderCompilesSucceed) return;
    // DXC tries the including file's folder first. A new file there must take over, and only the recorded miss can show it.
    cache::Folder folder;
    folder.Write("Shaders/Root.hlsl", "#include \"Common/Outer.hlsli\"\nfloat4 main() : SV_Target { return Outer(); }\n");
    folder.Write("Shaders/Common/Outer.hlsli", "#include \"Value.hlsli\"\nfloat4 Outer() { return Value(); }\n");
    folder.Write("Shaders/Value.hlsli", "float4 Value() { return float4(1, 1, 1, 1); }\n");
    cache::fs::remove(folder.root / "Shaders/Common/Value.hlsli");
    const auto source = folder.RootSource();
    org::services::ShaderArtifact original;
    { cache::Boot boot(folder); original = boot.compiler.Compile(folder.Request(source)); CHECK(original); }
    folder.Write("Shaders/Common/Value.hlsli", "float4 Value() { return float4(9, 9, 9, 9); }\n");
    cache::Boot boot(folder);
    const auto artifact = boot.compiler.Compile(folder.Request(source));
    CHECK(artifact); CHECK(artifact.binary != original.binary);
    CHECK(boot.store->Stats().misses[static_cast<size_t>(org::services::ShaderStoreMiss::DependencyChanged)] == 1);
}

void CacheSeparatesVariantsAndSharesEqualOutput() {
    if (!shaderCompilesSucceed) return;
    cache::Folder folder;
    folder.Write("Shaders/Root.hlsl", "#include \"Common/Value.hlsli\"\nfloat4 main() : SV_Target {\n#ifdef DOUBLE\n return Value() * 2;\n#else\n return Value();\n#endif\n}\n");
    const auto source = folder.RootSource();
    cache::Boot boot(folder);
    const auto plain = boot.compiler.Compile(folder.Request(source));
    const auto doubled = boot.compiler.Compile(folder.Request(source, { { L"DOUBLE", L"" } }));
    CHECK(plain); CHECK(doubled); CHECK(plain.binary != doubled.binary);
    // A define the shader never reads gives the same text, so it shares the artifact.
    const auto unused = boot.compiler.Compile(folder.Request(source, { { L"UNUSED", L"1" } }));
    CHECK(unused.binary == plain.binary);
    const auto stats = boot.store->Stats();
    CHECK(stats.compiled == 2); CHECK(stats.absorbed == 1);
}

void CacheDebugBuildsRebuildOnCommentEdits() {
    if (!shaderCompilesSucceed) return;
    cache::Folder folder;
    const auto source = folder.RootSource();
    { cache::Boot boot(folder); CHECK(boot.compiler.Compile(folder.Request(source, {}, true))); }
    folder.Write("Shaders/Common/Value.hlsli", "// comment\nfloat4 Value() { return float4(1, 2, 3, 4); }\n");
    cache::Boot boot(folder);
    CHECK(boot.compiler.Compile(folder.Request(source, {}, true)));
    CHECK(boot.store->Stats().compiled == 1);
}

void CacheRememberedFailureKeepsItsMessage() {
    if (!shaderCompilesSucceed) return;
    cache::Folder folder;
    folder.Write("Shaders/Common/Value.hlsli", "float4 Value() { return not_a_thing; }\n");
    const auto source = folder.RootSource();
    std::string message;
    {
        cache::Boot boot(folder);
        const auto artifact = boot.compiler.Compile(folder.Request(source));
        CHECK(!artifact); CHECK(!artifact.diagnostics.empty());
        message = artifact.diagnostics;
        CHECK(boot.store->Stats().failed == 1);
    }
    {
        cache::Boot boot(folder);
        const auto artifact = boot.compiler.Compile(folder.Request(source));
        CHECK(!artifact); CHECK(artifact.diagnostics == message);
        const auto stats = boot.store->Stats();
        CHECK(stats.failuresFromCache == 1); CHECK(stats.preprocessed == 0);
    }
    // A missing include fails in preprocessing, and is remembered the same way.
    folder.Write("Shaders/Root.hlsl", "#include \"Common/Missing.hlsli\"\nfloat4 main() : SV_Target { return 0; }\n");
    const auto broken = folder.RootSource();
    { cache::Boot boot(folder); CHECK(!boot.compiler.Compile(folder.Request(broken))); }
    {
        cache::Boot boot(folder);
        CHECK(!boot.compiler.Compile(folder.Request(broken)));
        CHECK(boot.store->Stats().failuresFromCache == 1);
    }
    folder.Write("Shaders/Common/Missing.hlsli", "\n");
    {
        cache::Boot boot(folder);
        CHECK(boot.compiler.Compile(folder.Request(broken)));
        CHECK(boot.store->Stats().compiled == 1);
    }
}

void CacheGenerationEndsMemoryResults() {
    if (!shaderCompilesSucceed) return;
    // A hot reload: the same compiler and the same request must see an edited include after a new generation, and not before.
    cache::Folder folder;
    const auto source = folder.RootSource();
    cache::Boot boot(folder);
    const auto first = boot.compiler.Compile(folder.Request(source));
    folder.Write("Shaders/Common/Value.hlsli", "float4 Value() { return float4(5, 6, 7, 8); }\n");
    CHECK(boot.compiler.Compile(folder.Request(source)).binary == first.binary);
    boot.store->BeginGeneration();
    const auto second = boot.compiler.Compile(folder.Request(source));
    CHECK(second); CHECK(second.binary != first.binary);
}

void CacheChangedRootBytesAreNotAHit() {
    if (!shaderCompilesSucceed) return;
    // Generated sources: same name and arguments, different bytes.
    cache::Folder folder;
    const std::string one = "float4 main() : SV_Target { return 1; }\n", two = "float4 main() : SV_Target { return 2; }\n";
    org::services::ShaderArtifact first;
    { cache::Boot boot(folder); first = boot.compiler.Compile(folder.Request(one)); CHECK(first); }
    cache::Boot boot(folder);
    const auto second = boot.compiler.Compile(folder.Request(two));
    CHECK(second); CHECK(second.binary != first.binary);
    CHECK(boot.store->Stats().misses[static_cast<size_t>(org::services::ShaderStoreMiss::RootChanged)] == 1);
}

void CacheConcurrentVariantsThroughThePool() {
    if (!shaderCompilesSucceed) return;
    cache::Folder folder;
    const auto source = folder.RootSource();
    cache::Boot boot(folder);
    std::vector<std::shared_future<org::services::ShaderArtifact>> futures;
    for (int i = 0; i < 16; ++i)
        futures.push_back(boot.compiler.CompileAsync(folder.Request(source, { { L"UNUSED", std::to_wstring(i) } })));
    for (auto& future : futures) CHECK(future.get());
    // All have the same text, so one compile serves them.
    const auto stats = boot.store->Stats();
    CHECK(stats.compiled == 1); CHECK(stats.absorbed == 15);
    for (auto& future : futures) CHECK(future.get().binary == futures[0].get().binary);
}

void CacheNotPersistentSavesNothing() {
    if (!shaderCompilesSucceed) return;
    cache::Folder folder;
    const auto source = folder.RootSource();
    { cache::Boot boot(folder, false); CHECK(boot.compiler.Compile(folder.Request(source))); }
    CHECK(!cache::fs::exists(folder.root / "Cache"));
}
#endif

struct Test { const char* name; void (*run)(); };

}

int main() {
    const Test tests[] = {
        { "PipelineBuildThroughManualQueue", PipelineBuildThroughManualQueue },
        { "PipelineBuildFailuresReachDone", PipelineBuildFailuresReachDone },
        { "PipelineBuildFixedPool", PipelineBuildFixedPool },
        { "PipelineBuildDefaultThread", PipelineBuildDefaultThread },
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
        { "ShaderDefaultUnchanged", ShaderDefaultUnchanged },
        { "ShaderCallbacksThroughManualQueue", ShaderCallbacksThroughManualQueue },
        { "ShaderFailureReachesCallbacks", ShaderFailureReachesCallbacks },
        { "ShaderCallbackReentersWithoutDeadlock", ShaderCallbackReentersWithoutDeadlock },
        { "ShaderFixedPoolAndNoLibraryThreads", ShaderFixedPoolAndNoLibraryThreads },
        { "ShaderConcurrentJoiners", ShaderConcurrentJoiners },
        { "ShaderCompilerOutlivedByQueuedWork", ShaderCompilerOutlivedByQueuedWork },
        { "CacheSecondBootDoesNoWork", CacheSecondBootDoesNoWork },
        { "CacheIncludeEditsThroughRealCompiler", CacheIncludeEditsThroughRealCompiler },
        { "CacheRecordsMissingIncludePaths", CacheRecordsMissingIncludePaths },
        { "CacheSeparatesVariantsAndSharesEqualOutput", CacheSeparatesVariantsAndSharesEqualOutput },
        { "CacheDebugBuildsRebuildOnCommentEdits", CacheDebugBuildsRebuildOnCommentEdits },
        { "CacheRememberedFailureKeepsItsMessage", CacheRememberedFailureKeepsItsMessage },
        { "CacheGenerationEndsMemoryResults", CacheGenerationEndsMemoryResults },
        { "CacheChangedRootBytesAreNotAHit", CacheChangedRootBytesAreNotAHit },
        { "CacheConcurrentVariantsThroughThePool", CacheConcurrentVariantsThroughThePool },
        { "CacheNotPersistentSavesNothing", CacheNotPersistentSavesNothing },
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
