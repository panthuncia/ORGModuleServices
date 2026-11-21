#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <ORGModuleServices/ServiceExecutor.h>

namespace org::services {

// A built pipeline, as its module keeps it (an rhi::PipelinePtr, say).
using PipelinePayload = std::shared_ptr<void>;

// One version of a live pipeline: the payload it binds and the registry generation it was published at. Immutable: a publish
// makes a new version and never changes one a holder has.
struct PipelineVersion {
    PipelinePayload payload;
    uint64_t generation{};
};
using PipelineVersionPtr = std::shared_ptr<const PipelineVersion>;

// The registry generation a holder binds (PipelineRegistry::Pin), such as one graph build: its recordings bind each slot's
// version at that generation (LivePipelineSlot::At). The registry keeps the versions a live pin needs.
struct PipelinePin {
    uint64_t generation{};
};

// A live pipeline's slot: its versions, oldest first. PipelineRegistry adds a version when a reload rebuilt it, and drops the
// versions no live pin binds any more (Release). Readers take a version without a lock.
class LivePipelineSlot {
public:
    explicit LivePipelineSlot(PipelineVersionPtr first);
    LivePipelineSlot(const LivePipelineSlot&) = delete;
    LivePipelineSlot& operator=(const LivePipelineSlot&) = delete;

    // The newest version.
    PipelineVersionPtr Current() const noexcept;
    // The version a pin of `generation` binds: the newest one published at or before it. A slot registered after it has none
    // that old, and gives its first version.
    PipelineVersionPtr At(uint64_t generation) const noexcept;
    // The newest version's payload.
    PipelinePayload Load() const noexcept { return Current()->payload; }

private:
    friend class PipelineRegistry;
    using Versions = std::vector<PipelineVersionPtr>;  // oldest first, never empty
    // The registry's, under its lock.
    void Append(PipelineVersionPtr version);
    // Drops every version older than the one `oldest` binds. Their payloads go to `released`.
    void DropBefore(uint64_t oldest, std::vector<PipelinePayload>& released);
    std::atomic<std::shared_ptr<const Versions>> versions_;
};

// A rebuild's result. A payload: the new pipeline. None with an error: it failed (the slot keeps its payload). None without one:
// unchanged (its fingerprint is the slot's, and the rebuild was not forced).
struct LiveBuild {
    PipelinePayload payload;
    uint64_t fingerprint{};  // what it was built from (its shaders' binaries, say)
    std::string error;
};
struct LiveRebuildRequest {
    uint64_t fingerprint{};  // the slot's payload's
    bool force{};            // build it even when the fingerprint is unchanged
};
using LiveRebuildDone = std::function<void(LiveBuild)>;
// Builds the pipeline again from its shader files as they are now. It must not block: it may run on a thread a compile it waits for
// needs. It calls done exactly once, on any thread (a compile's completion, say), or inline.
using LiveRebuild = std::function<void(const LiveRebuildRequest&, LiveRebuildDone done)>;

// Builds a pipeline once on submit's executor (one new thread when it is empty) and calls done exactly once with it, on the
// thread that built it. A throw or a null payload is an error. A refused submission calls done at once with an error. Nothing
// waits: the caller keeps its own record of what it asked for and admits the result when done says so.
void BuildPipeline(const ServiceSubmit& submit, std::function<PipelinePayload()> build, LiveRebuildDone done);

struct LivePipelineDesc {
    std::string id;  // the logs' name
    // The shader files it is compiled from (paths relative to the shader directory or with it: "Lighting.hlsl",
    // "Data/Shaders/DrawcallLimitFix/BuildDrawsCS.hlsl"). A reload of a changed file rebuilds the entries that name it; a file no
    // entry names (an include) rebuilds every entry (RequestReload).
    std::vector<std::string> sources;
    LiveRebuild rebuild;
};

// Every live pipeline a module registered, for hot reload (BasicRenderer's PSOManager model).
//
// A module registers each pipeline it builds, once, with how to build it again, and gets a slot its consumers hold and bind through.
// An entry is weak: it dies with its slot's last holder. RequestReload (any thread) asks for the entries of a changed shader file
// (or all) to be rebuilt; the owner's Service starts them on the registry's executor and, once every rebuild is in, publishes the
// changed payloads as new versions of their slots at the next Generation. Nothing a holder has changes: a holder binds the version
// its pin names (Pin, LivePipelineSlot::At). Release drops the versions no live pin binds any more; their payloads are held until
// Retire passes the point they were dropped at. A failed rebuild leaves its slot alone, logged.
class PipelineRegistry {
public:
    PipelineRegistry() = default;
    ~PipelineRegistry();
    PipelineRegistry(const PipelineRegistry&) = delete;
    PipelineRegistry& operator=(const PipelineRegistry&) = delete;

    // Where a reload's rebuilds start (one task per reload, starting each in turn; the rebuilds do not block). Empty (the default):
    // one new thread per reload. Setup: before the registry is shared with other threads.
    void Configure(ServiceSubmit submit);

    // Any thread. payload: the pipeline as built, the slot's first version (at the current Generation); fingerprint: what it was
    // built from, which a rebuild compares against; builtFor: SourcesGeneration as it was when its shader files were read. An entry
    // built for an earlier generation (its build started before a reload began reading the files) is rebuilt by the next Service,
    // so no pipeline keeps the files of before.
    std::shared_ptr<LivePipelineSlot> Register(LivePipelineDesc desc, PipelinePayload payload, uint64_t fingerprint, uint64_t builtFor);

    // Any thread: asks for the entries built from changedFile to be rebuilt (empty: every entry). force: each is built and
    // published even when unchanged (the swap tested without an edit). Requests coalesce until the owner's next Service.
    void RequestReload(std::string_view changedFile = {}, bool force = false);

    // Any thread: a pin of the current Generation. Each slot keeps the version it binds while the pin lives.
    std::shared_ptr<const PipelinePin> Pin();

    // The owner thread, once a frame (cheap when nothing is asked): finishes the reload whose rebuilds are all in, publishing its
    // new payloads as versions of the next Generation (LastReload has its counts and errors, for the owner's log line), and starts
    // the one asked for, after moving SourcesGeneration and telling the listeners.
    struct ServiceResult {
        bool finished{};       // a reload finished in this call
        uint32_t published{};  // the new versions it published; Generation moved when nonzero
    };
    ServiceResult Service();

    // The owner thread, once a frame: the versions no live pin binds (older than the one the oldest pin binds) are dropped from
    // their slots, and their payloads held until Retire passes retirementPoint. Cheap while the oldest pin does not move.
    void Release(uint64_t retirementPoint);
    // Any thread: the dropped payloads released with a point up to completedPoint.
    void Retire(uint64_t completedPoint) noexcept;
    // Every dropped payload released (the device idle, or going away).
    void DrainRetired() noexcept;

    // The newest versions' generation: moves with every reload that published a version.
    uint64_t Generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    // Moves when a reload starts (before any of its rebuilds reads a file): what a module's cached sources are checked against.
    uint64_t SourcesGeneration() const noexcept { return sourcesGeneration_.load(std::memory_order_acquire); }
    // Told on the owner thread when a reload starts (SourcesGeneration moved) and when a reload published versions. Setup.
    void AddListener(std::function<void()> listener);

    struct ReloadOutcome {
        uint64_t serial{};         // reloads finished
        uint32_t entries{};        // rebuilt this reload (M)
        uint32_t rebuilt{};        // published with a new version (N)
        uint32_t failed{};         // kept their payload (K)
        uint32_t unchanged{};      // built from the same inputs (U)
        uint64_t generation{};     // the Generation its versions were published at (unchanged when N is 0)
        double seconds{};
        std::vector<std::string> errors;  // the first failures, "id: error"
    };
    // Any thread: the last finished reload's outcome.
    ReloadOutcome LastReload() const;
    // Any thread: the entries registered (live or not yet pruned).
    size_t EntryCount() const;

private:
    struct Entry;
    struct Batch;
    struct Retired {
        uint64_t point{};
        PipelinePayload payload;
    };
    void StartBatch();
    uint32_t FinishBatch();
    void Notify();
    void PruneLocked();
    uint64_t OldestPinnedLocked();

    mutable std::mutex mutex_;  // entries_, request_, pins_, listeners_, outcome_, slots' versions
    ServiceSubmit submit_;
    std::vector<std::shared_ptr<Entry>> entries_;
    size_t pruneAt_{ 64 };
    struct Request {
        bool asked{};
        bool all{};
        bool force{};
        std::vector<std::string> files;  // normalized
    } request_;
    std::atomic<bool> asked_{ false };  // request_ or a stale registration
    std::shared_ptr<Batch> running_;     // the owner's
    std::vector<std::weak_ptr<const PipelinePin>> pins_;
    size_t prunePinsAt_{ 64 };
    uint64_t releasedTo_{ 0 };  // the oldest pinned generation at the last Release that dropped versions (owner)
    std::vector<std::function<void()>> listeners_;
    ReloadOutcome outcome_;
    std::atomic<uint64_t> generation_{ 0 };
    std::atomic<uint64_t> sourcesGeneration_{ 0 };
    std::mutex retiredMutex_;
    std::deque<Retired> retired_;
};

}
