#pragma once

#include <ORGModuleServices/Async/RevisionAssembly.h>
#include <ORGModuleServices/Async/VersionGrowthArtifact.h>
#include <ORGModuleServices/ElasticCapacity.h>
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <ORGModuleServices/GrowthPolicy.h>
#include <ORGModuleServices/VersionedBufferGrowth.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Resources/Buffers/VersionedBuffer.h"

namespace org {
class LatchBlock;
}

namespace org::services {

/**
 * @brief Producer thread: lays out a Capture registration's new version from immutable state, at its device address (0 without
 * `address`). Sets the fills and what keeps their memory alive (VersionGrowthRequest::contentsOf), and returns the contents stamp
 * the version holds.
 */
using GrowableCapture = std::function<std::uint64_t(const BufferVersion& version, std::uint64_t address, std::vector<VersionFill>& fills,
    std::shared_ptr<const void>& contents)>;

/** @brief A buffer of the set: its versions are what the set grows and names. */
struct GrowableRegistration {
    std::string name;
    std::shared_ptr<VersionedBuffer> buffer;  // held weakly: an expired buffer leaves the set
    BufferExtent extent;
    GrowthPolicy policy = GrowthPolicy::Doubling();
    // When a version gives memory back (ShrinkPolicy; Never: grow-only): a buffer asked at most the policy's share of what it holds
    // for its window gets a version at `policy`'s fresh size for the window's peak ask. Made like a growth (its contents as below,
    // nothing copied from the predecessor), in a batch of its own that does not hold the seal: the set names the smaller version
    // once it is ready.
    ShrinkPolicy shrink = ShrinkPolicy::Never();
    GrowableContents contents = GrowableContents::Fresh;
    GrowableCapture capture;  // Capture's
    bool address = false;     // its entries carry the version's device address (the hooks' addressOf)
    // The pool its versions are made into: a backing of the policy's class (BufferExtent::CapacityClassFor), made like its buffer
    // (BackingRequest::like) and reused only by this registration's versions. Null: the set's (GrowableBufferSetHooks::pool);
    // Register throws when both are (no fresh buffers). One acquirer at a time: a pool serves one set (its batches run in turn).
    std::shared_ptr<BackingPool> pool;
};

/**
 * @brief A latch block of the set: made inline at a seal (LatchBlock::Create, any thread) when its layout changes or it holds fewer
 * bytes than asked, never pending. A new layout makes a block of exactly the bytes asked; a block outgrown at the same layout grows
 * by `policy` (exact by default: a layout's bytes are already grown by its owner).
 */
struct LatchRegistration {
    std::string name;
    std::uint32_t slots = 0;  // frame slots
    GrowthPolicy policy = GrowthPolicy::Headroom(0);
};

struct GrowableHandle {
    std::uint32_t index = UINT32_MAX;
    [[nodiscard]] explicit operator bool() const noexcept { return index != UINT32_MAX; }
};
struct LatchHandle {
    std::uint32_t index = UINT32_MAX;
    [[nodiscard]] explicit operator bool() const noexcept { return index != UINT32_MAX; }
};

/**
 * @brief The versions a sealed set names: every live buffer of the set (an entry per handle) and every latch block made. Immutable;
 * a preparation for a revision carrying it resolves each VersionedBuffer here (IResourceVersions), whatever is current when it runs.
 */
class NamedVersions final : public IResourceVersions {
public:
    std::shared_ptr<const BufferVersion> Find(const VersionedBuffer& buffer) const noexcept override;

    /** @brief Process-unique: moves exactly when any named version or latch block does. */
    [[nodiscard]] std::uint64_t Serial() const noexcept { return m_serial; }
    [[nodiscard]] bool Names(GrowableHandle handle) const noexcept { return handle.index < m_entries.size() && m_entries[handle.index].version; }
    [[nodiscard]] bool Names(const VersionedBuffer& buffer) const noexcept { return IndexOf(buffer) != UINT32_MAX; }
    [[nodiscard]] bool Names(const std::shared_ptr<VersionedBuffer>& buffer) const noexcept { return buffer && Names(*buffer); }
    /** @brief The handle's entry; throws when the set does not name it. */
    [[nodiscard]] const NamedEntry& Of(GrowableHandle handle) const;
    /** @brief The buffer's entry, found by its key; throws when the set does not name it. */
    [[nodiscard]] const NamedEntry& Of(const VersionedBuffer& buffer) const;
    [[nodiscard]] const NamedEntry& Of(const std::shared_ptr<VersionedBuffer>& buffer) const;
    /** @brief The handle's latch block; null before its first requirement. */
    [[nodiscard]] std::shared_ptr<LatchBlock> Latch(LatchHandle handle) const noexcept;
    /** @brief Owner thread: makes every named version current (VersionedBuffer::Adopt). Returns how many were not already. */
    std::size_t AdoptAll() const;
    /**
     * @brief Any thread: binds every named pooled version's backing (BackingLease::Bind, max-merged): not reused before the
     * pools' clock reaches `safeAt`, even once the last reference to its version is gone.
     *
     * The frame's contract: a frame that adopts or reads the set binds it after its last submission, with the point that retires
     * once that submission is complete on the GPU (CS: DCLF::SubmittedRetirementPoint()), while it still holds the set. Binding at
     * the frame's start would let a backing go back to the pool while that frame's GPU work still reads it. A set no frame bound
     * holds versions no GPU work read: their backings are reusable as soon as their last reference goes. The growth's own fills
     * need no bind: a version is named only once its copies completed.
     */
    void Bind(std::uint64_t safeAt) const noexcept;
    /** @brief Diagnostics: Find calls for a buffer this set does not name (a recording resolved an unnamed buffer). */
    [[nodiscard]] std::uint64_t Misses() const noexcept { return m_misses.load(std::memory_order_relaxed); }

private:
    friend class GrowableBufferSet;
    [[nodiscard]] std::uint32_t IndexOf(const VersionedBuffer& buffer) const noexcept;
    std::uint64_t m_serial = 0;
    std::vector<NamedEntry> m_entries;                       // by handle (version null: not named)
    std::vector<std::weak_ptr<VersionedBuffer>> m_buffers;   // by handle
    std::vector<std::pair<const void*, std::uint32_t>> m_byKey;  // (VersionedBuffer::Key, handle), sorted
    std::vector<std::shared_ptr<LatchBlock>> m_latches;      // by latch handle
    mutable std::atomic<std::uint64_t> m_misses{0};
};

/** @brief The set's ties to its host. */
struct GrowableBufferSetHooks {
    // Producer thread: posts a growth batch as graph work (PostVersionGrowth); false when refused.
    std::function<bool(async::VersionGrowthWork)> post;
    // Any thread: a batch settled (ready or failed): the producer may build and seal again.
    std::function<void()> wake;
    // Any thread: the uploader Zero and Capture fills go through.
    std::function<std::shared_ptr<runtime::IUploadService>()> uploads;
    // Producer thread: a version's device address, for registrations with `address`.
    std::function<std::uint64_t(const Buffer&)> addressOf;
    // Producer thread, optional: a posted batch grows a buffer from `fromUnits` (its newest ready version's) to `toUnits` (`bytes`);
    // toUnits < fromUnits: a shrink (GrowableRegistration::shrink).
    std::function<void(const GrowableRegistration&, std::uint64_t fromUnits, std::uint64_t toUnits, std::uint64_t bytes)> grown;
    // The set's pool, for every registration without its own (GrowableRegistration::pool); the set's acquirer (one batch at a time).
    std::shared_ptr<BackingPool> pool;
};

/**
 * @brief Growable buffers named by one sealed set (the snapshot): requirement -> one growth batch -> the next sealed set.
 *
 * - Register (any thread, lock-free): a buffer or a latch block joins the set; its handle is valid at once. A Zero registration's
 *   first version (its owner's) is never named: the first seal that sees it makes a zero-filled version at its size.
 * - Begin, the Build's requirements, Seal: the producer's alone (one thread). Seal names the newest ready versions when they meet
 *   every requirement, and returns the very same set (shared_ptr, serial and fragment) when nothing changed, so what was recorded
 *   against it stays reused. Otherwise it posts one batch for every unmet buffer (GrowVersions as graph work) and is pending until
 *   the batch settles (the hooks' wake). A requirement a set cannot meet is never sealed: no caps, no fallback.
 * - Versions are pooled (GrowableRegistration::pool): each owns a lease of its backing, returned when the version is released.
 * - The frame adopts what it selects (NamedVersions::AdoptAll) and binds it after its last submission (NamedVersions::Bind), so
 *   no backing it read is reused before it retires. Nothing else flows from the frame to the producer.
 */
class GrowableBufferSet {
public:
    struct Sealed {
        std::shared_ptr<const NamedVersions> named;              // null while pending or failed
        std::shared_ptr<const async::RevisionFragment> fragment;  // `named`, ready: the same object while the set is
        bool pending = false;  // a growth batch is in flight: seal again after the wake
        std::string error;     // the last batch failed, or this one was refused: posted again by the next seal that needs it
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(named); }
    };

    /** @brief The producer's requirements for one seal. */
    class Build {
    public:
        /** @brief At least `units` (max-merged). */
        void Require(GrowableHandle handle, std::uint64_t units);
        /** @brief A version newer than `serial` (a relayout: a new version even at the same size). */
        void RequireNewer(GrowableHandle handle, std::uint64_t serial);
        /**
         * @brief A latch block of at least `bytes` per slot laid out as `layoutKey` (another key: a new block, so a frame in flight
         * never reads a block laid out anew). A replaced block moves the sealed set's Serial.
         */
        void Require(LatchHandle handle, std::uint32_t bytes, std::uint64_t layoutKey);
        /** @brief What the newest ready version holds (0: none). */
        [[nodiscard]] std::uint64_t Units(GrowableHandle handle) const;
        /** @brief The most asked of the buffer: this build's requirement, or a growth in flight. */
        [[nodiscard]] std::uint64_t Asked(GrowableHandle handle) const;
        /** @brief The newest ready version's serial (0: none). */
        [[nodiscard]] std::uint64_t Serial(GrowableHandle handle) const;

    private:
        friend class GrowableBufferSet;
        struct LatchAsk {
            bool asked = false;
            std::uint32_t bytes = 0;
            std::uint64_t layoutKey = 0;
        };
        explicit Build(const GrowableBufferSet& set) : m_set(&set) {}
        const GrowableBufferSet* m_set;
        std::vector<GrowableRequirement> m_required;  // by handle
        std::vector<LatchAsk> m_latches;              // by latch handle
    };

    explicit GrowableBufferSet(GrowableBufferSetHooks hooks);
    ~GrowableBufferSet();
    GrowableBufferSet(const GrowableBufferSet&) = delete;
    GrowableBufferSet& operator=(const GrowableBufferSet&) = delete;

    /** @brief Any thread. Throws for a registration without a buffer or unit size, or a Capture without its capture. */
    GrowableHandle Register(GrowableRegistration registration);
    /** @brief Any thread. */
    LatchHandle Register(LatchRegistration registration);
    /** @brief Producer: a registered buffer's handle (registrations taken first); throws for one never registered or gone. */
    [[nodiscard]] GrowableHandle HandleOf(const VersionedBuffer& buffer);

    /** @brief Producer: takes in registrations and a settled batch, and starts a build. */
    [[nodiscard]] Build Begin();
    /** @brief Producer: names the build's versions, or posts what they lack (pending). */
    Sealed Seal(Build&& build);

    /**
     * @brief Producer: whether a seal now would name other versions without any requirement moving: a batch settled (a shrink's
     * smaller versions wait to be named), or a buffer's shrink is due (GrowableRegistration::shrink; a seal observes it). A producer
     * that skips its seals while its requirements stand (a kept revision) seals again for this, or a quiet scene keeps its sizes.
     */
    [[nodiscard]] bool ShrinkPending(ShrinkWindow::Clock::time_point now = ShrinkWindow::Clock::now()) const;

    /** @brief Producer: counters since the last call (batches posted, buffers grown, failures, latch blocks made, seals). */
    struct Stats {
        std::uint64_t batches = 0, grown = 0, failed = 0, latches = 0, sealed = 0, kept = 0;
        std::uint64_t shrunk = 0;  // of `grown`: versions made smaller
    };
    Stats TakeStats();

private:
    struct Registered;
    struct Batch;
    struct Slot {
        std::shared_ptr<const GrowableRegistration> registration;  // weak buffer kept apart: the registration's is reset
        std::weak_ptr<VersionedBuffer> buffer;
        std::uint64_t key = 0;    // its backings' compatibility (BackingRequest::compatibility)
        NamedEntry current;   // the newest ready version
        std::uint64_t asked = 0;  // units of the growth in flight
        bool made = false;        // `current` is the set's (a Zero registration's first version is not)
        ShrinkWindow window;      // its asks while they fit (GrowableRegistration::shrink)
    };
    struct LatchSlot {
        std::shared_ptr<const LatchRegistration> registration;
        std::shared_ptr<LatchBlock> block;
        std::uint64_t layoutKey = 0;
    };

    void TakeRegistered();
    void Settle();
    NamedEntry EntryOf(const Slot& slot, std::shared_ptr<const BufferVersion> version, std::optional<std::uint64_t> stamp, BackingLease lease) const;
    // One batch: each (slot, units) a version of exactly those units.
    bool Post(const std::vector<std::pair<std::uint32_t, std::uint64_t>>& asks);

    GrowableBufferSetHooks m_hooks;
    // Any thread: registrations not yet taken (Treiber stack) and the handles handed out.
    std::atomic<Registered*> m_registered{nullptr};
    std::atomic<std::uint32_t> m_nextHandle{0}, m_nextLatch{0};
    // The producer's.
    std::vector<Slot> m_slots;
    std::vector<LatchSlot> m_latchSlots;
    std::vector<std::pair<const void*, std::uint32_t>> m_handles;  // (VersionedBuffer::Key, handle), sorted
    std::shared_ptr<Batch> m_batch;  // in flight
    bool m_changed = true;           // since the last sealed set
    Sealed m_last;                   // the last sealed set: returned again while nothing changed
    std::string m_failure;           // the last batch's, until a seal reports it
    Stats m_stats;
};

} // namespace org::services
