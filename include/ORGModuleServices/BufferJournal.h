#pragma once

#include <ORGModuleServices/BufferImage.h>
#include <ORGModuleServices/VersionedBufferBuild.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace org::services {

class BackingPool;

/**
 * @brief A mutable manager-facing allocator's desired state (BasicRenderer's VersionedGpuBufferJournal): a paged image plus the
 * writes since the last acknowledged version. Captures are immutable and handed to the versioned-buffer producer
 * (VersionedBufferBuildInput) without a pointer to the manager or its storage.
 *
 * Trim floor: with a backing pool set (SetBackingPool), Acknowledge keeps the writes back to min(acknowledged sequence,
 * pool.OldestContentEpoch()), so a reused backing at an older contentEpoch still catches up from the journal instead of being written
 * whole. Without one it keeps only the writes past the acknowledged version.
 *
 * Thread-safe; its mutex is BasicRenderer's (moved with it, not new). DCLF does not use it (it keeps KeptView/ChangeJournal).
 */
class BufferJournal {
public:
    struct Capture {
        std::uint64_t writeSequence = 0;
        std::uint64_t elementCount = 0;
        std::uint64_t capacity = 0;
        std::shared_ptr<const PublishedBufferVersion> previous;
        std::vector<BufferWrite> writes;
        std::shared_ptr<const BufferImage> image;
        // Every write after it is in `writes`: the acknowledged sequence, or lower down to the pool's trim floor.
        std::uint64_t journalBaseSequence = 0;
    };

    explicit BufferJournal(std::uint32_t elementStride = 0);

    /** @brief Resets to `bytes` (elementCount rows of the stride) at `capacity`, sequence 1. Throws for a mismatched image. */
    void Initialize(std::span<const std::byte> bytes, std::uint64_t elementCount, std::uint64_t capacity);
    /** @brief Writes rows at `elementOffset`; the logical extent becomes at least `resultingElementCount`. Returns the new sequence. */
    std::uint64_t AppendWrite(std::uint64_t elementOffset, std::span<const std::byte> bytes, std::uint64_t resultingElementCount);
    /** @brief Replaces the whole image (one write covering it). Returns the new sequence. */
    std::uint64_t ReplaceImage(std::span<const std::byte> bytes, std::uint64_t elementCount, std::uint64_t capacity);
    /** @brief Raises the desired capacity (a zero-length checkpoint write closes the sequence). */
    void RequestCapacity(std::uint64_t capacity);
    [[nodiscard]] Capture CaptureDesired() const;
    /**
     * @brief The pool this journal's versions are built into (not owned): Acknowledge trims to its OldestContentEpoch. Reads it on
     * the acknowledging thread, outside the journal's mutex (BasicRenderer's pools serialize it through their exclusive hook).
     */
    void SetBackingPool(const std::shared_ptr<const BackingPool>& pool);
    /**
     * @brief A version published from a capture: it becomes `previous`, and the writes no backing still needs are dropped (those
     * at or below min(its sequence, the pool's trim floor)).
     */
    void Acknowledge(const std::shared_ptr<const PublishedBufferVersion>& version);
    [[nodiscard]] std::uint64_t DesiredSequence() const;
    [[nodiscard]] bool HasUnpublishedChanges() const;

private:
    std::uint32_t m_elementStride = 0;
    mutable std::mutex m_mutex;
    std::uint64_t m_writeSequence = 0;
    std::uint64_t m_elementCount = 0;
    std::uint64_t m_capacity = 0;
    std::shared_ptr<const PublishedBufferVersion> m_previous;
    std::vector<BufferWrite> m_writes;
    std::uint64_t m_journalBaseSequence = 0;  // every write after it is in m_writes
    std::weak_ptr<const BackingPool> m_backingPool;
    std::size_t m_byteSize = 0;
    std::vector<std::shared_ptr<BufferImage::Page>> m_pages;
};

} // namespace org::services
