#pragma once

#include <ORGModuleServices/Async/ArtifactResources.h>
#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/BufferImage.h>
#include <ORGModuleServices/ElasticCapacity.h>
#include <ORGModuleServices/GrowthPolicy.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace org {
class Buffer;
struct TrackedUploadTicket;
namespace runtime {
class IUploadService;
}
}

namespace org::services {

/** @brief How a version relates to its predecessor's backing class: Patch keeps the class (catch-up), Replace grows it. */
enum class BufferRevisionMode : std::uint8_t {
    Patch,
    Replace,
};

/** @brief One journal write: bytes [byteOffset, byteOffset + byteSize) changed at `sequence` (0 bytes: a capacity checkpoint). */
struct BufferWrite {
    std::uint64_t sequence = 0;
    std::uint64_t byteOffset = 0;
    std::uint64_t byteSize = 0;
};

/** @brief A byte range of a versioned buffer. */
struct BufferByteRange {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

struct PublishedBufferVersion;

/**
 * @brief An immutable request for one version of a versioned buffer (BasicRenderer's VersionedGpuBufferBuildInput, without its
 * catalog fields: those travel as the host's opaque `tag`). Either a complete image (`bytes`, or `image`) or the predecessor plus
 * the journal's writes since `journalBaseSequence`.
 */
struct VersionedBufferBuildInput {
    std::shared_ptr<runtime::IUploadService> uploadOwner;
    runtime::IUploadService* uploadService = nullptr;  // uploadOwner's, or one the host keeps alive
    std::string debugName;
    std::uint64_t writeSequence = 0;
    std::uint32_t elementStride = 0;
    std::uint64_t elementCount = 0;
    std::uint64_t capacity = 0;
    BackingFlags flags;
    bool gpuWritten = false;  // the GPU writes it whole: no initial contents are uploaded
    // The host's identity of the version (BasicRenderer: its catalog owner, usage and variant), copied to the version and handed
    // to the producer's wrap hook. Opaque to MS.
    std::shared_ptr<const void> tag;
    std::shared_ptr<const PublishedBufferVersion> previous;
    std::shared_ptr<BackingPool> backingPool;  // required
    std::vector<BufferWrite> writes;
    // Immutable paged view of the producer's authoritative desired image.
    std::shared_ptr<const BufferImage> image;
    // All writes after this sequence are in `writes`: a backing at or past it catches up without replaying the whole image.
    std::uint64_t journalBaseSequence = 0;
    // Optional complete initial image. Successors normally use previous + writes.
    std::vector<std::byte> bytes;

    // Predecessor copy (default off: the inputs above, BasicRenderer's). The version is `previous`'s contents plus this input's own
    // writes: the build GPU-copies from previous's backing what the leased backing lacks of it - changedSince(its contentEpoch), or
    // previous's used bytes whole when the backing is fresh, of another class, or changedSince returns nullopt - less the delta's
    // ranges, then uploads only the delta (emitDelta). Without `previous` the delta is the whole version, uploaded alone. Needs an
    // upload service that copies (IUploadService::QueueTrackedStreamingCopyRanges; null there: the build fails). The version's
    // image is left null.
    bool predecessorCopy = false;
    // The byte ranges written after journal position `from` through previous->writeSequence (any order, may overlap). nullopt: the
    // journal does not reach back to `from`, and previous is copied whole.
    std::function<std::optional<std::vector<BufferByteRange>>(std::uint64_t from)> changedSince;
    // This version's own writes since previous->writeSequence (everything without `previous`), each `bytes` from `data` at byte
    // `offset`. The data stays valid until BuildVersionedBuffer returns.
    std::function<void(const std::function<void(const void* data, std::size_t bytes, std::uint64_t offset)>& write)> emitDelta;
    // Held by the version until its uploads and copies complete (the host's source of the delta's data, or anything else).
    std::shared_ptr<const void> keep;
    // predecessorCopy: a grown class (Replace) is growth->Next(previous's class, capacity) instead of the power of two at least 1.5x
    // the predecessor's: a large buffer that changes every frame keeps a backing per frame in flight, so its headroom is paid that
    // many times. nullopt: the power of two.
    std::optional<GrowthPolicy> growth;
    // When the chain gives memory back (Never: a class only grows). A version that would Patch while its capacity stayed at most the
    // policy's share of previous's class for the window (carried by the versions: PublishedBufferVersion::shrinkWindow) is made a
    // Replace at the fresh class for the window's peak instead: `growth`'s Next(0, peak), else the power of two at least the peak.
    // With predecessorCopy the copy is previous's contents up to this version's used bytes, which the smaller class holds.
    ShrinkPolicy shrink = ShrinkPolicy::Never();
};

/**
 * @brief One published version of a versioned buffer (BasicRenderer's PublishedGpuBufferVersion): its backing's lease, so the
 * backing returns to its pool when the last holder of the version (a revision, a frame, a catalog hold) lets it go.
 */
struct PublishedBufferVersion {
    std::uint64_t revision = 0;
    std::uint64_t writeSequence = 0;
    std::uint64_t elementCount = 0;
    std::uint64_t capacity = 0;  // the backing's class (elements)
    std::uint32_t elementStride = 0;
    BufferRevisionMode revisionMode = BufferRevisionMode::Replace;
    std::uint64_t contentVersion = 0;
    std::uint64_t contentEpoch = 0;
    std::uint64_t backingEpoch = 0;  // the backing's generation
    BackingLease lease;
    std::shared_ptr<Buffer> resource;  // lease->buffer
    std::shared_ptr<const BufferImage> image;
    std::shared_ptr<const void> tag;   // the input's
    // The host's state seeded before visibility (BasicRenderer: org::compiler::PreparedBackingState, set by its decorate
    // hook). It owns no consumer pin, so journal retention cannot keep a backing from reuse.
    std::shared_ptr<const void> hostState;
    // Input::predecessorCopy's build (diagnostics): the delta's bytes uploaded from the CPU, the bytes GPU-copied from the predecessor,
    // and whether that copy was previous's used bytes whole (a fresh, grown or stale backing).
    std::uint64_t uploadedBytes = 0, copiedBytes = 0;
    bool copiedWhole = false;
    // The chain's run of low capacities (VersionedBufferBuildInput::shrink), handed to the next version's build.
    ShrinkWindow shrinkWindow;
    bool shrunk = false;  // a Replace at a smaller class than previous's

    [[nodiscard]] std::shared_ptr<const std::vector<std::byte>> MaterializeCpuImage() const { return image ? image->Materialize() : nullptr; }
};

/** @brief What BuildVersionedBuffer needs of its caller (the producer's graph context). */
struct VersionedBufferBuildContext {
    std::uint64_t revision = 0;
    std::function<bool()> stopRequested;                // optional
    std::function<std::uint64_t()> allocateSuspension;  // async::AllocateArtifactSuspensionIdentity, on exhaustion
    std::function<void(std::uint64_t)> notifySuspension; // StateGraph::MakeSuspensionNotifier's, the pool's availability wake
};

/** @brief BuildVersionedBuffer's outcome, mapped by the producer template onto its graph's ArtifactBuildResult. */
struct VersionedBufferBuildOutcome {
    enum class Kind : std::uint8_t { Ready, Suspended, Failed, Cancelled };
    Kind kind = Kind::Failed;
    std::shared_ptr<PublishedBufferVersion> version;              // Ready: mutable until the producer freezes it (decorate hook)
    std::shared_ptr<const async::GpuSubmissionSet> ready;         // Ready: its uploads (null: none)
    std::uint64_t suspension = 0;                                 // Suspended: the identity (ArtifactSuspension::Capacity)
    std::string error;                                            // Failed, Suspended: why
};

/**
 * @brief Producer thread: one version (BasicRenderer's BuildVersionedGpuBuffer). Acquires a backing (the predecessor's class for a
 * Patch, a grown class for a Replace), preferring the one whose contentEpoch is nearest the request's sequence; uploads what changed
 * since the backing's contentEpoch (or the whole image), as PooledBackingLease writes through the input's upload service; sets the
 * backing's contentEpoch and image. Suspended when the pool is exhausted (Config::maxPerClass), with the pool's availability
 * subscribed to notifySuspension. Never throws (failures are outcomes).
 */
VersionedBufferBuildOutcome BuildVersionedBuffer(const VersionedBufferBuildInput& input, const VersionedBufferBuildContext& context);

/** @brief The pure replay of an input's authoritative bytes, shared by the producer and deterministic journal tests. Null and why. */
std::shared_ptr<const std::vector<std::byte>> ReplayVersionedBuffer(const VersionedBufferBuildInput& input, std::string& error);

/**
 * @brief A readiness token over upload tickets: complete when all are, failed when one was cancelled; every ticket transition
 * notifies (completionNotificationsAreAuthoritative). The single one (BasicRenderer's TokenForTicket/TokenForTickets go).
 */
std::shared_ptr<const async::GpuSubmissionSet> TokenForTickets(std::vector<std::shared_ptr<TrackedUploadTicket>> tickets);

} // namespace org::services
