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
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Resources/Buffers/VersionedBuffer.h"

namespace org {
class Buffer;
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
 * @brief How a buffer joins the set through GrowableBufferSet::Make. Its name is its first version's.
 */
struct GrowableBufferSpec {
    BufferExtent extent;
    GrowthPolicy policy = GrowthPolicy::Doubling();
    GrowableContents contents = GrowableContents::Fresh;
    GrowableCapture capture;  // Capture's
    ShrinkPolicy shrink = ShrinkPolicy::Never();
    bool address = false;  // its entries carry the version's device address
};

/** @brief What a latch block holds: a region per run, or one region every run reads, such as a block of zeros. */
enum class LatchRegions : std::uint8_t {
    PerRun,  // a region per run, from the runtime's frame arena
    Shared,  // one dedicated region
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
 *
 * A texture set (Textures) names texture versions instead, and no buffer. A frame names both through ComposedVersions.
 */
class NamedVersions final : public IResourceVersions {
public:
    /** @brief A texture and the version of it a set names. */
    using TextureEntry = std::pair<std::shared_ptr<VersionedTexture>, std::shared_ptr<const TextureVersion>>;

    /**
     * @brief Any thread: a set that names these texture versions and nothing else, with a new Serial. A version may be pending
     * (VersionedTexture::Make): what is recorded against the set binds it before it is current. The textures are held weakly.
     * Throws on a null texture or version, or a texture named twice.
     */
    [[nodiscard]] static std::shared_ptr<const NamedVersions> Textures(std::span<const TextureEntry> textures);

    std::shared_ptr<const BufferVersion> Find(const VersionedBuffer& buffer) const noexcept override;
    std::shared_ptr<const TextureVersion> FindTexture(const VersionedTexture& texture) const noexcept override;

    /** @brief Process-unique: moves exactly when any named version or latch block does. */
    [[nodiscard]] std::uint64_t Serial() const noexcept { return m_serial; }
    /**
     * @brief The set's buffer moves this set's device addresses follow (GrowableBufferSet::NoteBuffersMoved). A set below
     * GrowableBufferSet::MovedEpoch may name addresses a buffer had before it moved. 0 for a texture set.
     */
    [[nodiscard]] std::uint64_t AddressEpoch() const noexcept { return m_addressEpoch; }
    [[nodiscard]] bool Names(GrowableHandle handle) const noexcept { return handle.index < m_entries.size() && m_entries[handle.index].version; }
    [[nodiscard]] bool Names(const VersionedBuffer& buffer) const noexcept { return IndexOf(buffer) != UINT32_MAX; }
    [[nodiscard]] bool Names(const std::shared_ptr<VersionedBuffer>& buffer) const noexcept { return buffer && Names(*buffer); }
    [[nodiscard]] bool Names(const VersionedTexture& texture) const noexcept;
    /** @brief How many texture versions the set names. */
    [[nodiscard]] std::size_t TextureCount() const noexcept { return m_textures.size(); }
    /** @brief The handle's entry; throws when the set does not name it. */
    [[nodiscard]] const NamedEntry& Of(GrowableHandle handle) const;
    /** @brief The buffer's entry, found by its key; throws when the set does not name it. */
    [[nodiscard]] const NamedEntry& Of(const VersionedBuffer& buffer) const;
    [[nodiscard]] const NamedEntry& Of(const std::shared_ptr<VersionedBuffer>& buffer) const;
    /** @brief Units in the version the set names, or 0 for a null buffer or one the set does not name (a set older than it). */
    [[nodiscard]] std::uint64_t UnitsOf(const std::shared_ptr<VersionedBuffer>& buffer) const { return Names(buffer) ? Of(*buffer).units : 0; }
    /** @brief The handle's latch block; null before its first requirement. */
    [[nodiscard]] std::shared_ptr<LatchBlock> Latch(LatchHandle handle) const noexcept;
    /**
     * @brief Owner thread: makes every named version current (VersionedBuffer::Adopt, VersionedTexture::Adopt). Returns how many
     * were not already. An expired buffer or texture is skipped.
     */
    std::size_t AdoptAll() const;
    /**
     * @brief Any thread: binds every named pooled version's backing (BackingLease::Bind, max-merged): not reused before the
     * pools' clock reaches `safeAt`, even once the last reference to its version is gone.
     *
     * The frame's contract: a frame that adopts or reads the set binds it after its last submission, with the point that retires
     * once that submission is complete on the GPU, while it still holds the set. Binding at
     * the frame's start would let a backing go back to the pool while that frame's GPU work still reads it. A set no frame bound
     * holds versions no GPU work read: their backings are reusable as soon as their last reference goes. The growth's own fills
     * need no bind: a version is named only once its copies completed.
     */
    void Bind(std::uint64_t safeAt) const noexcept;
    /**
     * @brief Diagnostics: calls for a resource of the set's kind that it does not name (a recording resolved an unnamed one). A
     * buffer set counts Find calls, a texture set (Textures) FindTexture calls. Asking for the other kind is not a miss.
     */
    [[nodiscard]] std::uint64_t Misses() const noexcept { return m_misses.load(std::memory_order_relaxed); }

private:
    friend class GrowableBufferSet;
    [[nodiscard]] std::uint32_t IndexOf(const VersionedBuffer& buffer) const noexcept;
    struct NamedTexture;
    [[nodiscard]] const NamedTexture* TextureOf(const VersionedTexture& texture) const noexcept;
    std::uint64_t m_serial = 0;
    std::uint64_t m_addressEpoch = 0;
    std::vector<NamedEntry> m_entries;                       // by handle (version null: not named)
    std::vector<std::weak_ptr<VersionedBuffer>> m_buffers;   // by handle
    std::vector<std::pair<const void*, std::uint32_t>> m_byKey;  // (VersionedBuffer::Key, handle), sorted
    std::vector<std::shared_ptr<LatchBlock>> m_latches;      // by latch handle
    struct NamedTexture {
        const void* key = nullptr;  // VersionedTexture::Key
        std::weak_ptr<VersionedTexture> texture;
        std::shared_ptr<const TextureVersion> version;
    };
    std::vector<NamedTexture> m_textures;  // sorted by key
    bool m_textureSet = false;             // made by Textures: it names textures and no buffer
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
    /**
     * @brief Any thread. Registers a latch block by what it holds: Shared makes a dedicated block (one region), PerRun a block of
     * the runtime's frame arena.
     */
    LatchHandle Register(std::string name, LatchRegions regions, GrowthPolicy policy = GrowthPolicy::Headroom(0));
    /**
     * @brief Any thread. Wraps `first` as the first version of a new buffer of the set and registers it, named as `first`.
     * Null for a null `first`. Throws as Register does.
     */
    std::shared_ptr<VersionedBuffer> Make(std::shared_ptr<Buffer> first, GrowableBufferSpec spec);
    /** @brief Producer: a registered buffer's handle (registrations taken first); throws for one never registered or gone. */
    [[nodiscard]] GrowableHandle HandleOf(const VersionedBuffer& buffer);

    /** @brief Producer: Build::Require for a buffer of the set. A null buffer or 0 units asks nothing. */
    void Require(Build& build, const std::shared_ptr<VersionedBuffer>& buffer, std::uint64_t units);
    /** @brief Producer: Build::RequireNewer for a buffer of the set. A null buffer or serial 0 asks nothing. */
    void RequireNewer(Build& build, const std::shared_ptr<VersionedBuffer>& buffer, std::uint64_t serial);
    /** @brief Producer: Build::Units for a buffer of the set; 0 for a null buffer. */
    [[nodiscard]] std::uint64_t Units(const Build& build, const std::shared_ptr<VersionedBuffer>& buffer);
    /** @brief Producer: the most asked of a buffer of the set (Build::Asked), at least what it holds; 0 for a null buffer. */
    [[nodiscard]] std::uint64_t MostAsked(const Build& build, const std::shared_ptr<VersionedBuffer>& buffer);

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

    /**
     * @brief Any thread: the host moved buffers (a defragmentation while the GPU was idle), so device addresses the set names may
     * be old. The producer's next seal reads every named version's address again and names a new set, whose AddressEpoch is at
     * least the returned value. Lock-free.
     */
    std::uint64_t NoteBuffersMoved() noexcept { return m_movedEpoch.fetch_add(1, std::memory_order_acq_rel) + 1; }
    /** @brief Any thread: how many NoteBuffersMoved calls there were. A sealed set whose AddressEpoch is below it may name old addresses. */
    [[nodiscard]] std::uint64_t MovedEpoch() const noexcept { return m_movedEpoch.load(std::memory_order_acquire); }
    /** @brief Producer: whether a buffer move is not followed by a seal yet. A producer that skips its seals seals again for this. */
    [[nodiscard]] bool AddressesMoved() const noexcept { return MovedEpoch() != m_addressEpoch; }

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
    // Reads the named versions' addresses again after NoteBuffersMoved.
    void RefreshAddresses();
    NamedEntry EntryOf(const Slot& slot, std::shared_ptr<const BufferVersion> version, std::optional<std::uint64_t> stamp, BackingLease lease) const;
    // One batch: each (slot, units) a version of exactly those units.
    bool Post(const std::vector<std::pair<std::uint32_t, std::uint64_t>>& asks);

    GrowableBufferSetHooks m_hooks;
    // Any thread: registrations not yet taken (Treiber stack) and the handles handed out.
    std::atomic<Registered*> m_registered{nullptr};
    std::atomic<std::uint32_t> m_nextHandle{0}, m_nextLatch{0};
    std::atomic<std::uint64_t> m_movedEpoch{0};  // NoteBuffersMoved calls
    // The producer's.
    std::vector<Slot> m_slots;
    std::vector<LatchSlot> m_latchSlots;
    std::vector<std::pair<const void*, std::uint32_t>> m_handles;  // (VersionedBuffer::Key, handle), sorted
    std::shared_ptr<Batch> m_batch;  // in flight
    bool m_changed = true;           // since the last sealed set
    std::uint64_t m_addressEpoch = 0;  // the moves `current`'s addresses follow
    Sealed m_last;                   // the last sealed set: returned again while nothing changed
    std::string m_failure;           // the last batch's, until a seal reports it
    Stats m_stats;
};

/**
 * @brief Buffers of one set indexed alike, each with its own extent. One requirement fans out to every member, and the group holds
 * the least any member holds.
 */
struct GrowableUnitGroup {
    std::vector<std::shared_ptr<VersionedBuffer>> members;

    /** @brief Adds a member; a null buffer is skipped. */
    GrowableUnitGroup& Add(const std::shared_ptr<VersionedBuffer>& buffer) {
        if (buffer) members.push_back(buffer);
        return *this;
    }
    /** @brief Producer. Requires at least `units` of every member. */
    void Require(GrowableBufferSet& set, GrowableBufferSet::Build& build, std::uint64_t units) const {
        for (const auto& member : members) set.Require(build, member, units);
    }
    /** @brief The units every member's named version holds, or 0 without members. Throws for a member `sealed` does not name. */
    [[nodiscard]] std::uint64_t Units(const NamedVersions& sealed) const {
        std::uint64_t units = members.empty() ? 0 : ~0ull;
        for (const auto& member : members) {
            const std::uint64_t held = sealed.Of(member).units;
            units = held < units ? held : units;
        }
        return units;
    }
    /** @brief Producer. The most asked of any member (GrowableBufferSet::MostAsked). */
    [[nodiscard]] std::uint64_t Asked(GrowableBufferSet& set, const GrowableBufferSet::Build& build) const {
        std::uint64_t asked = 0;
        for (const auto& member : members) {
            const std::uint64_t most = set.MostAsked(build, member);
            asked = most > asked ? most : asked;
        }
        return asked;
    }
};

/**
 * @brief Fixed-stride rows in one buffer of a set that grows instead of capping. A row past the sealed capacity waits for the
 * growth requirement: nothing is dropped and nothing falls back. The host makes the buffer (GrowableBufferSet::Make with a
 * structured extent of `stride`).
 */
struct GrowableRows {
    std::shared_ptr<VersionedBuffer> buffer;
    std::uint32_t stride = 0;
    std::string name;

    /** @brief The row capacity of the version `sealed` names. Throws when it does not name the buffer. */
    [[nodiscard]] std::uint32_t RowsIn(const NamedVersions& sealed) const { return static_cast<std::uint32_t>(sealed.Of(buffer).units); }
};

} // namespace org::services
