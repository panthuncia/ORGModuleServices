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

#include <ORGModuleServices/PipelineService.h>
#include <ORGModuleServices/ServiceExecutor.h>

namespace org::services {

// A live pipeline's slot: the payload its consumers bind. PipelineRegistry swaps it at its owner's safe point (Publish); nothing
// else writes it. The same shape as org::PipelineStateSlot (an atomic payload and a non-owning identity), with the registry's
// type-erased payload: a registered pipeline may be one rhi::PipelinePtr or an aggregate of several (a module's variants).
class LivePipelineSlot {
public:
    explicit LivePipelineSlot(PipelinePayload payload) noexcept : payload_(payload), peek_(payload.get()) {}
    LivePipelineSlot(const LivePipelineSlot&) = delete;
    LivePipelineSlot& operator=(const LivePipelineSlot&) = delete;

    PipelinePayload Load() const noexcept { return payload_.load(std::memory_order_acquire); }
    // The current payload's identity, for "did it change" checks: no reference taken. Between two of the owner's safe points the
    // payload does not change, so a recording may read through it.
    const void* Peek() const noexcept { return peek_.load(std::memory_order_acquire); }

private:
    friend class PipelineRegistry;
    PipelinePayload Exchange(PipelinePayload payload) noexcept {
        peek_.store(payload.get(), std::memory_order_release);
        return payload_.exchange(std::move(payload), std::memory_order_acq_rel);
    }
    std::atomic<PipelinePayload> payload_;
    std::atomic<const void*> peek_;
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
// (or all) to be rebuilt; the owner's Service starts them on the registry's executor and, once every rebuild is in, stages the
// changed payloads. Publish, at the owner's safe point (no recording reads a slot meanwhile), swaps them into their slots and moves
// Generation; a payload a slot gave up is held until Retire passes the point it was published at. A failed rebuild leaves its
// slot alone, logged.
class PipelineRegistry {
public:
    PipelineRegistry() = default;
    ~PipelineRegistry();
    PipelineRegistry(const PipelineRegistry&) = delete;
    PipelineRegistry& operator=(const PipelineRegistry&) = delete;

    // Where a reload's rebuilds start (one task per reload, starting each in turn; the rebuilds do not block). Empty (the default):
    // one new thread per reload. Setup: before the registry is shared with other threads.
    void Configure(ServiceSubmit submit);

    // Any thread. payload: the pipeline as built; fingerprint: what it was built from, which a rebuild compares against; builtFor:
    // SourcesGeneration as it was when its shader files were read. An entry built for an earlier generation (its build started
    // before a reload began reading the files) is rebuilt by the next Service, so no pipeline keeps the files of before.
    std::shared_ptr<LivePipelineSlot> Register(LivePipelineDesc desc, PipelinePayload payload, uint64_t fingerprint, uint64_t builtFor);

    // Any thread: asks for the entries built from changedFile to be rebuilt (empty: every entry). force: each is built and
    // published even when unchanged (the swap tested without an edit). Requests coalesce until the owner's next Service.
    void RequestReload(std::string_view changedFile = {}, bool force = false);

    // The owner thread, once a frame (cheap when nothing is asked): finishes the reload whose rebuilds are all in, staging its new
    // payloads (LastReload has its counts and errors, for the owner's log line), and starts the one asked for, after moving
    // SourcesGeneration and telling the listeners. staged: payloads wait for Publish, which the owner reaches as soon as it can.
    struct ServiceResult {
        bool finished{};  // a reload finished in this call
        bool staged{};    // payloads are staged
    };
    ServiceResult Service();

    // The owner's safe point: the staged payloads swapped into their slots (those whose slots are gone dropped); the ones they replace
    // held until Retire passes retirementPoint. Generation moves, and the listeners are told, when any slot changed. Returns how many.
    uint32_t Publish(uint64_t retirementPoint);
    // Any thread: the replaced payloads published with a point up to completedPoint released.
    void Retire(uint64_t completedPoint) noexcept;
    // Every replaced and staged payload released (the device idle, or going away).
    void DrainRetired() noexcept;

    // Moves with every Publish that changed a slot.
    uint64_t Generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    // Moves when a reload starts (before any of its rebuilds reads a file): what a module's cached sources are checked against.
    uint64_t SourcesGeneration() const noexcept { return sourcesGeneration_.load(std::memory_order_acquire); }
    // Told on the owner thread when a reload starts (SourcesGeneration moved) and when Publish changed slots. Setup.
    void AddListener(std::function<void()> listener);

    struct ReloadOutcome {
        uint64_t serial{};         // reloads finished
        uint32_t entries{};        // rebuilt this reload (M)
        uint32_t rebuilt{};        // staged with a new payload (N)
        uint32_t failed{};         // kept their payload (K)
        uint32_t unchanged{};      // built from the same inputs (U)
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
    struct Staged {
        std::weak_ptr<LivePipelineSlot> slot;
        std::weak_ptr<Entry> entry;
        PipelinePayload payload;
        uint64_t fingerprint{};
    };
    struct Retired {
        uint64_t point{};
        PipelinePayload payload;
    };
    void StartBatch();
    void FinishBatch();
    void Notify();
    void PruneLocked();

    mutable std::mutex mutex_;  // entries_, request_, staged_, listeners_, outcome_
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
    std::vector<Staged> staged_;
    std::vector<std::function<void()>> listeners_;
    ReloadOutcome outcome_;
    std::atomic<uint64_t> generation_{ 0 };
    std::atomic<uint64_t> sourcesGeneration_{ 0 };
    std::mutex retiredMutex_;
    std::deque<Retired> retired_;
};

}
