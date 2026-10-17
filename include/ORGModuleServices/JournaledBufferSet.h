#pragma once

#include <ORGModuleServices/Async/ArtifactResources.h>
#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/GrowthPolicy.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace org {
struct StreamingUploadSegment;
struct StreamingUploadRange;
struct TrackedUploadTicket;
namespace runtime {
class IUploadService;
}
}

namespace org::services {

/** @brief One part of a journaled set: buffers of this kind, pooled (one BackingPool per part). */
struct JournaledPartConfig {
    std::string name;    // the backings' names (and the pool's)
    std::uint32_t stride = 0;  // bytes per unit: the backings are structured buffers of this stride (0: raw buffers, units are bytes)
    BackingFlags flags;  // deviceAddress for parts whose rows hold their own address
    // The backing class of a need: policy.Next(highWater, units), where highWater is the largest class the part has made (a part's
    // class only grows, so content that shrinks keeps reusing its backings).
    GrowthPolicy policy = GrowthPolicy::Doubling(64);
};

/** @brief The backing a part's emit writes into, and what it already holds. */
struct JournaledTarget {
    std::uint64_t contentEpoch = 0;   // the journal position the backing holds (0: nothing; write everything)
    std::uint64_t address = 0;        // its device address (flags.deviceAddress), for rows that embed it (patch every row emitted)
    std::uint32_t srvIndex = 0;
    std::uint64_t capacityUnits = 0;  // units it holds room for (>= the request's units)
    bool allocated = false;           // made by this publication
};

/**
 * @brief Where a part's emit sends its runs: each Write queues one upload into the target backing on the dedicated uploader (a
 * WorkerOwnedDestination of ownership PooledBackingLease: no queue reads a leased backing until the frame that binds it). The
 * uploader copies the data into its staging pages when the write is queued, so the caller's memory may go after the call. An
 * append (JournaledPartRequest::appendInto) writes a backing published versions name: only at or past its sealed mark, which no
 * reader reads.
 * Throws when the uploader refuses a write, it lies past the backing, or (an append) it begins below the sealed mark.
 */
class JournaledSink {
public:
    /** @brief `bytes` from `data` at byte `offset` of the backing. The signature of DCLF's KeptView::Emit senders. */
    void operator()(const void* data, std::size_t bytes, std::size_t offset) { Write(data, bytes, offset); }
    void Write(const void* data, std::size_t bytes, std::uint64_t offset);
    /** @brief A gathered run (segments in order, `totalBytes` together) at byte `offset`. */
    void Write(std::span<const StreamingUploadSegment> segments, std::size_t totalBytes, std::uint64_t offset);
    /**
     * @brief Many runs at once (each `size` bytes from `data` at byte `dstOffset`), one batched upload
     * (IUploadService::QueueTrackedStreamingUploadRanges: one ticket, the staging page mapped once). Each counts as a run. The data
     * must live until the call returns (the uploader stages it before).
     */
    void Write(std::span<const StreamingUploadRange> ranges);
    [[nodiscard]] std::uint64_t Bytes() const noexcept { return m_bytes; }
    [[nodiscard]] std::uint64_t Runs() const noexcept { return m_runs; }
    /** @brief The lowest byte written (~0: none). */
    [[nodiscard]] std::uint64_t FirstByte() const noexcept { return m_firstByte; }

private:
    friend class JournaledBufferSet;
    JournaledSink(runtime::IUploadService& uploads, const Backing& target, std::vector<std::shared_ptr<TrackedUploadTicket>>& tickets,
        std::uint64_t floor = 0) noexcept
        : m_uploads(&uploads), m_target(&target), m_tickets(&tickets), m_floor(floor) {}
    void CheckRange(std::uint64_t offset, std::uint64_t bytes) const;
    runtime::IUploadService* m_uploads;
    const Backing* m_target;
    std::vector<std::shared_ptr<TrackedUploadTicket>>* m_tickets;
    std::uint64_t m_floor = 0;  // an append's sealed mark: no write begins below it (0: a fresh fill, written anywhere)
    std::uint64_t m_bytes = 0, m_runs = 0;
    std::uint64_t m_firstByte = ~std::uint64_t{0};
};

/** @brief What one publication asks of one part. */
struct JournaledPartRequest {
    // The units this publication holds (0 with no emit: the part is absent from this publication, and gets no backing).
    std::uint64_t units = 0;
    // The journal's position now (KeptView/ChangeJournal Version()): the pool prefers the free backing nearest at or below it.
    std::uint64_t preferEpoch = BackingPool::kAnyEpoch;
    /**
     * @brief Writes the part into `target`: every run since target.contentEpoch (everything when it is 0), rows that hold an address
     * patched with target.address, whole tails written whole. Returns the journal position the backing now holds (its new
     * contentEpoch; 0 when what it wrote is not a journal position, so the next publication writes it whole). Throws on failure.
     */
    std::function<std::uint64_t(const JournaledTarget& target, JournaledSink& sink)> emit;
    /**
     * @brief Optional: a backing of this part's pool that published versions name (the part's head), to append into instead of
     * leasing one. The emit is sent its contentEpoch, and every write must begin at or past its sealed mark (Backing::sealedBytes;
     * the sink refuses one below, and the publication throws). The caller decides that the runs since its contentEpoch all lie
     * there. Ignored (a lease as without it) when `units` outgrow its capacity.
     */
    BackingLease appendInto;
};

/** @brief One part of a publication: the leased backing and what was sent into it. */
struct JournaledPartVersion {
    BackingLease lease;               // empty: the part was absent
    std::uint64_t address = 0;
    std::uint32_t srvIndex = 0;
    std::uint64_t units = 0;          // the request's
    std::uint64_t capacityUnits = 0;
    std::uint64_t caughtUpFrom = 0;   // the backing's contentEpoch before (0: written whole)
    std::uint64_t contentEpoch = 0;   // after (the emit's)
    std::uint64_t bytes = 0, runs = 0;
    bool allocated = false;
    bool appended = false;              // written into the request's appendInto (published versions name it below sealedFrom)
    std::uint64_t sealedFrom = 0;       // an append's sealed mark before it (bytes; 0 for a lease)
    std::uint64_t firstByte = ~std::uint64_t{0};  // the lowest byte written (~0: none)
};

/** @brief A publication: every part's version, ready once all their uploads completed. Immutable once returned. */
struct JournaledPublication {
    std::vector<JournaledPartVersion> parts;              // by part
    std::shared_ptr<const async::GpuSubmissionSet> ready; // every part's uploads (null: nothing was sent; ready now)
    std::uint64_t bytes = 0, runs = 0;

    /** @brief Any thread: binds every part's backing (BackingLease::Bind): not reused before the clock reaches `safeAt`. */
    void Bind(std::uint64_t safeAt) const noexcept {
        for (const auto& part : parts)
            if (part.lease) part.lease.Bind(safeAt);
    }
};

/**
 * @brief Buffers published together by journal catch-up into free backings: the DCLF payload model (D5). Each part has its own
 * pool; a publication leases one backing per present part (preferring the one nearest the journal), lets the part's emit send what
 * changed since that backing's contentEpoch, sets the new contentEpoch, and returns the leases with a readiness token over the
 * uploads. Nothing waits on the GPU: a backing is reused only once its leases are gone and its clock reached what the frames that
 * used it bound. A part may instead append into a backing published versions name (JournaledPartRequest::appendInto): only past
 * its sealed mark, the most any of them names, so none sees a byte change.
 *
 * Single publisher: Publish, Discard and OldestContentEpoch run on one thread (the pools' single acquirer). The publication's
 * leases are released from any thread.
 */
class JournaledBufferSet {
public:
    /** @brief One pool per part, made with `hooks`, `clock` and `config` (its name: the part's). Throws for a part without a name. */
    JournaledBufferSet(std::vector<JournaledPartConfig> parts, BackingPoolHooks hooks, std::shared_ptr<RetirementClock> clock, BackingPool::Config config);
    JournaledBufferSet(const JournaledBufferSet&) = delete;
    JournaledBufferSet& operator=(const JournaledBufferSet&) = delete;
    ~JournaledBufferSet();

    [[nodiscard]] std::size_t Parts() const noexcept { return m_parts.size(); }
    [[nodiscard]] const JournaledPartConfig& Part(std::size_t part) const { return m_parts.at(part).config; }

    /**
     * @brief Publisher: one publication, `requests` by part (one per part). Acquires, emits and sets each present part's backing,
     * then builds `ready` from the uploads. Throws (std::invalid_argument for a request count that is not Parts(); the emit's or the
     * pool's failures as they are): the leases taken so far are released, and their backings marked as holding nothing.
     */
    [[nodiscard]] JournaledPublication Publish(std::span<const JournaledPartRequest> requests, runtime::IUploadService& uploads);
    /**
     * @brief Publisher: the publication's uploads failed (`ready` failed): its backings are marked as holding nothing (contentEpoch
     * 0), so they are written whole when next leased.
     */
    void Discard(const JournaledPublication& publication);
    /** @brief Publisher: the part's journal trim floor (BackingPool::OldestContentEpoch). nullopt: no backing holds anything. */
    [[nodiscard]] std::optional<std::uint64_t> OldestContentEpoch(std::size_t part) const;
    [[nodiscard]] const std::shared_ptr<BackingPool>& Pool(std::size_t part) const { return m_parts.at(part).pool; }

private:
    struct PartState {
        JournaledPartConfig config;
        std::shared_ptr<BackingPool> pool;
        std::uint64_t highWater = 0;  // the largest class made (units)
    };
    std::vector<PartState> m_parts;
};

} // namespace org::services
