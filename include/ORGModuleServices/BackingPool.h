#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace org {
class Buffer;
}

namespace org::services {

class BufferImage;
class BackingPool;
namespace detail {
struct BackingHome;
}

/**
 * @brief The GPU's progress as a host retires it: a monotonic point (a host frame number, a renderer frame count) that only moves
 * forward. Every point at or below Completed() has retired: nothing the GPU does any more reads what a lease bound at such a point
 * guarded. Replaces BasicRenderer's process-global NotifyVersionedGpuBufferFrameRetirement: one clock per host (CS: one for DCLF,
 * advanced from the graph host's completed-frame callback; BasicRenderer: one per renderer).
 *
 * Lock-free: Advance publishes the point atomically and runs the waiters it reached, popped from a Treiber stack. Any thread.
 * Held by shared_ptr (pools and leases keep it).
 */
class RetirementClock {
public:
    /** @brief A one-shot wake: called once with the completed point that reached it. Must not throw. */
    using Wake = std::function<void(std::uint64_t completed)>;

    RetirementClock() = default;
    ~RetirementClock();
    RetirementClock(const RetirementClock&) = delete;
    RetirementClock& operator=(const RetirementClock&) = delete;

    /**
     * @brief Any thread: every point at or below `completed` has retired (a lower value than Completed() changes nothing). Runs, on
     * this thread, every waiter whose point is now reached.
     */
    void Advance(std::uint64_t completed) noexcept;
    /** @brief Any thread: the highest point advanced (0: none). */
    [[nodiscard]] std::uint64_t Completed() const noexcept { return m_completed.load(std::memory_order_acquire); }
    [[nodiscard]] bool Reached(std::uint64_t point) const noexcept { return Completed() >= point; }
    /**
     * @brief Any thread: `wake` runs exactly once, when Completed() >= `point`: inline when it already is, else on the thread whose
     * Advance reaches it. A pool's availability (ArtifactSuspension::Capacity) is woken this way.
     */
    void Subscribe(std::uint64_t point, Wake wake);
    /** @brief Diagnostics: waiters not yet woken. */
    [[nodiscard]] std::uint64_t Waiting() const noexcept { return m_waiting.load(std::memory_order_relaxed); }

private:
    struct Waiter;
    void RunReached() noexcept;
    std::atomic<std::uint64_t> m_completed{0};
    std::atomic<Waiter*> m_waiters{nullptr};  // Treiber stack
    std::atomic<std::uint64_t> m_waiting{0};
};

/** @brief What a backing is made as, beside its class and stride. Backings are reused only for equal flags. */
struct BackingFlags {
    bool unorderedAccess = false;
    bool indirectArguments = false;  // BasicRenderer: its indexed structured buffer's counter/argument variant
    bool deviceAddress = false;      // the allocation reports its device address (Backing::address)
    friend constexpr bool operator==(const BackingFlags&, const BackingFlags&) = default;
};

/**
 * @brief What Acquire asks of a pool. A free backing is reused only for an equal (capacityClass, stride, flags, compatibility).
 * - capacityClass: elements of `stride` (a structured buffer), or bytes when `stride` is 0 (a raw buffer). The caller picks the
 *   class (a GrowthPolicy's size), so backings of one class are interchangeable.
 * - compatibility: a host key for whatever else the allocation depends on (e.g. a growable registration's buffer kind).
 * - like: optional, a buffer the allocation copies the kind of (heap, views, sharing): VersionedBuffer growth's.
 */
struct BackingRequest {
    std::uint64_t capacityClass = 0;
    std::uint32_t stride = 0;
    BackingFlags flags;
    std::uint64_t compatibility = 0;
    std::string_view name;
    std::shared_ptr<Buffer> like;

    [[nodiscard]] constexpr std::uint64_t ByteCapacity() const noexcept { return stride ? capacityClass * stride : capacityClass; }
};

/** @brief A new backing's allocation (BackingPoolHooks::allocate). */
struct BackingAllocation {
    std::shared_ptr<Buffer> buffer;  // materialized; created with rhi::QueueSharing::Concurrent (the uploader writes it)
    std::uint64_t address = 0;       // its device address (flags.deviceAddress), else 0
    std::uint32_t srvIndex = 0;      // its SRV slot (0: none)
};

/** @brief A pool's telemetry events (BackingPoolHooks::telemetry). `value`: 1 for a count, else the gauge or byte amount named. */
enum class BackingPoolEvent : std::uint8_t {
    Allocated,       // a new backing (value: its bytes)
    Reused,          // a free backing reused (value: 1)
    Released,        // a free backing released (a spare, or another class than asked; value: its bytes)
    Exhausted,       // Acquire returned no lease (Config::maxPerClass reached)
    Suspended,       // SubscribeAvailability registered a waiter
    Woken,           // waiters woken (value: how many)
    LiveBackings,    // gauge: backings the pool holds
    LiveBytes,       // gauge: their bytes
    FreeBackings,    // gauge: backings with no lease
};

/** @brief A pool's ties to its host. Called on the acquirer's thread unless noted. */
struct BackingPoolHooks {
    // Required: a new backing for the request. Throws when it cannot be made (no fallback: the caller fails).
    // BasicRenderer: CreateIndexedStructuredBuffer; CS: org::Buffer::CreateUnmaterializedStructuredBuffer + Materialize, its SRV.
    std::function<BackingAllocation(const BackingRequest& request)> allocate;
    // Optional: memory attribution of a new backing (BasicRenderer: SetResourceUsageHint/SetResourceMemoryIdentifier by name).
    std::function<void(Buffer& buffer, std::string_view name)> tag;
    // Optional, any thread: counters and gauges (BasicTelemetry in BasicRenderer, the DCLF report in CS).
    std::function<void(BackingPoolEvent event, std::string_view name, std::uint64_t value)> telemetry;
    // Optional: a portable environment read (replaces _dupenv_s), for the pool's and the producer's trace switches.
    std::function<std::optional<std::string>(std::string_view name)> env;
    // Optional: runs the acquirer-side bodies (Acquire, Trim, OldestContentEpoch, Visit) as `body()` inside the host's own
    // serialization, for a host whose pools see several acquirers (BasicRenderer: its pool mutex, moved). Empty (CS): the
    // single-acquirer contract holds and the bodies run directly.
    std::function<void(const std::function<void()>& body)> exclusive;
    // Optional: takes the buffer of a backing the pool released (already safe to destroy), so the host decides which thread
    // destroys it (CS: not the render thread). Empty: the pool's reference drops on the acquirer's thread.
    std::function<void(std::shared_ptr<Buffer> buffer)> retire;
};

/**
 * @brief One pooled GPU allocation. The pool owns it (shared_ptr); a BackingLease names it while a version, a payload or a frame
 * uses it. Reused only when it has no lease, no binding consumer and no readback pin, and its clock has reached safeAt.
 *
 * The fixed fields are set when it is made. contentEpoch and image belong to the lease holder that fills it (the single acquirer's
 * side): they say what it holds, so the next acquirer catches up from there (BufferJournal writes, a KeptView's runs).
 */
struct Backing {
    std::shared_ptr<Buffer> buffer;
    std::uint64_t generation = 0;     // pool-unique, from 1
    std::uint64_t capacityClass = 0;  // BackingRequest's
    std::uint64_t byteCapacity = 0;
    std::uint64_t address = 0;        // device address (flags.deviceAddress), else 0
    std::uint32_t srvIndex = 0;
    std::uint32_t stride = 0;
    BackingFlags flags;
    std::uint64_t compatibility = 0;

    // The journal position its contents hold (0: nothing; written whole). Set by the lease holder after it queued the catch-up.
    std::atomic<std::uint64_t> contentEpoch{0};
    // The sealed mark: the most bytes any published version names of it (monotonic while leased; 0 when the pool reuses it). A
    // reader of a published version reads only below its own units, so writes at or past the mark (JournaledPartRequest::
    // appendInto) reach nothing a frame may read. The lease holder's, like contentEpoch.
    std::atomic<std::uint64_t> sealedBytes{0};
    // BasicRenderer: the image its contents are (journal catch-up needs the same byte size). The lease holder's.
    std::shared_ptr<const BufferImage> image;
    // Reusable once the clock reached it (max of every Bind).
    std::atomic<std::uint64_t> safeAt{0};
    std::atomic<std::uint32_t> leases{0};
    // Set (seq_cst) by the pool before it reuses or releases the backing; a binding consumer pins by incrementing
    // bindingConsumers, then checking this (one side always sees the other: lock-free, BasicRenderer's TryClaimBacking).
    std::atomic<bool> retired{false};
    // Readback completion may trail retirement: a diagnostic capture keeps the contents immutable while it observes them.
    std::atomic<std::uint32_t> readbackPins{0};
    std::atomic<std::uint32_t> bindingConsumers{0};

    Backing() = default;
    Backing(const Backing&) = delete;
    Backing& operator=(const Backing&) = delete;

private:
    friend class BackingPool;
    friend class BackingLease;
    std::shared_ptr<detail::BackingHome> m_home;  // the pool's return stack (outlives the pool while leases live)
    Backing* m_nextReturned = nullptr;            // Treiber link while on the return stack
};

/**
 * @brief A claim on a backing: while any lease of it lives, the pool does not reuse it. Copyable (each copy counts); the last one
 * released, on any thread, returns the backing to its pool lock-free (a Treiber return stack the acquirer drains). Bind sets the
 * clock point the backing must not be reused before: a frame that uses it binds the point that retires with that frame, so the
 * backing stays immutable for the GPU after the last lease goes.
 */
class BackingLease {
public:
    BackingLease() = default;
    BackingLease(const BackingLease& other) noexcept;
    BackingLease(BackingLease&& other) noexcept;
    BackingLease& operator=(const BackingLease& other) noexcept;
    BackingLease& operator=(BackingLease&& other) noexcept;
    ~BackingLease();

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(m_backing); }
    [[nodiscard]] Backing* operator->() const noexcept { return m_backing.get(); }
    [[nodiscard]] Backing& operator*() const noexcept { return *m_backing; }
    [[nodiscard]] const std::shared_ptr<Backing>& Get() const noexcept { return m_backing; }
    /** @brief Any thread: not reused before the clock reaches `safeAt` (max-merged with earlier binds; lock-free). */
    void Bind(std::uint64_t safeAt) const noexcept;
    /** @brief Releases this lease (the backing returns when it was the last). */
    void Reset() noexcept;

private:
    friend class BackingPool;
    explicit BackingLease(std::shared_ptr<Backing> backing) noexcept;  // takes a lease (leases + 1)
    std::shared_ptr<Backing> m_backing;
};

/**
 * @brief Pooled GPU backings, reused once retired (BasicRenderer's VersionedGpuBufferBackingPool, generalised): versions are made
 * into backings leased from here, filled by journal catch-up from what the backing already holds (Backing::contentEpoch), and a
 * backing is reused only after the frames that used it retired on its clock and its last lease went.
 *
 * - Single acquirer: Acquire, Trim, OldestContentEpoch and Visit run on one thread at a time (a documented contract, asserted in
 *   debug builds; BasicRenderer's adapter keeps its mutex around them). No mutex inside.
 * - Leases are released from any thread, lock-free.
 * - Config::maxPerClass 0: no cap (CS); a backing is made whenever none is reusable. BasicRenderer keeps 2·FIF + 2 per class:
 *   Acquire then returns an empty lease and the caller suspends (SubscribeAvailability).
 */
class BackingPool : public std::enable_shared_from_this<BackingPool> {
public:
    struct Config {
        std::string name;                 // diagnostics
        std::uint32_t maxPerClass = 0;    // backings of one class at most (0: none)
        std::uint32_t framesInFlight = 3; // the host's, for callers' safety windows (BasicRenderer: Bind(Completed() + 2·FIF + 1))
        std::uint32_t spareFree = 1;      // free backings of the asked class kept warm; free ones past it are released
    };
    /** @brief Acquire's preferEpoch default: any free backing. */
    static constexpr std::uint64_t kAnyEpoch = (std::numeric_limits<std::uint64_t>::max)();

    /** @brief Throws without hooks.allocate or a clock. */
    [[nodiscard]] static std::shared_ptr<BackingPool> Create(BackingPoolHooks hooks, std::shared_ptr<RetirementClock> clock, Config config);
    ~BackingPool();
    BackingPool(const BackingPool&) = delete;
    BackingPool& operator=(const BackingPool&) = delete;

    struct AcquireInfo {
        bool allocated = false;     // a new backing (holds nothing: written whole)
        std::uint32_t released = 0; // free backings released by this call (other classes, spares)
    };
    /**
     * @brief Acquirer: a lease on a backing for `request`. Among the reusable ones (no lease, no binding consumer, no readback pin,
     * the clock at safeAt, the same class/stride/flags/compatibility) it prefers the highest contentEpoch <= preferEpoch, which
     * minimises the catch-up; else a new one. Releases free backings of other classes. Empty only when Config::maxPerClass is
     * reached. Throws when an allocation fails.
     */
    [[nodiscard]] BackingLease Acquire(const BackingRequest& request, std::uint64_t preferEpoch = kAnyEpoch, AcquireInfo* info = nullptr);
    /**
     * @brief Any thread: `wake(identity)` runs once a backing may have become reusable (a lease returned, a pin released, the clock
     * advanced past a free backing's safeAt). `identity` is the caller's (async::AllocateArtifactSuspensionIdentity, for
     * ArtifactSuspension::Capacity). Lock-free.
     */
    void SubscribeAvailability(std::uint64_t identity, std::function<void(std::uint64_t identity)> wake);
    /**
     * @brief Any thread: a binding consumer's pin (the resource's semantic consumer lease, Resource::SetSemanticConsumerLeaseFactory,
     * installed on every backing): the backing is not reused while the pin lives. Throws std::invalid_argument for a backing the
     * pool already claimed (retired).
     */
    [[nodiscard]] std::shared_ptr<const void> PinBindingConsumer(const std::shared_ptr<Backing>& backing);
    /**
     * @brief Acquirer: the oldest journal position a backing in circulation holds (leased, or free and kept), ignoring backings
     * that hold nothing (written whole). A journal keeps its changes back to here (the trim floor). nullopt: none holds anything.
     */
    [[nodiscard]] std::optional<std::uint64_t> OldestContentEpoch() const;
    /** @brief Acquirer: takes returned backings in and releases spares (what Acquire does first). */
    void Trim();
    /** @brief Acquirer: every backing the pool holds, for diagnostics (BasicRenderer's lifetime trace). */
    void Visit(const std::function<void(const Backing&)>& visit) const;

    /** @brief Any thread: the pool's census (atomics; approximate while leases move). */
    struct Census {
        std::uint64_t live = 0, leased = 0, free = 0, bytes = 0;
        std::uint64_t allocated = 0, reused = 0, released = 0, exhausted = 0;  // since creation
    };
    [[nodiscard]] Census Count() const noexcept;
    [[nodiscard]] const std::shared_ptr<RetirementClock>& Clock() const noexcept { return m_clock; }
    [[nodiscard]] const Config& Configuration() const noexcept { return m_config; }
    [[nodiscard]] const BackingPoolHooks& Hooks() const noexcept { return m_hooks; }

private:
    friend class BackingLease;
    BackingPool(BackingPoolHooks hooks, std::shared_ptr<RetirementClock> clock, Config config);
    static void Returned(Backing& backing) noexcept;  // a backing's last lease went (any thread)
    void TakeReturned();
    [[nodiscard]] BackingLease Allocate(const BackingRequest& request);
    void ReleaseBacking(const std::shared_ptr<Backing>& backing);
    void ArmAvailability(const BackingRequest& request);
    void EmitGauges() const noexcept;
    struct State;  // the acquirer's backings and free lists
    BackingPoolHooks m_hooks;
    std::shared_ptr<RetirementClock> m_clock;
    Config m_config;
    std::shared_ptr<detail::BackingHome> m_home;  // return stack and availability waiters (lock-free; shared with the backings)
    std::unique_ptr<State> m_state;
    mutable std::atomic<bool> m_acquiring{false};  // the single-acquirer assertion (checked in debug builds; kept for one layout)
};

} // namespace org::services
