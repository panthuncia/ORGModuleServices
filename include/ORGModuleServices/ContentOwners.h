#pragma once

// Tracks which descriptors the bytes in GPU buffers currently name, by byte range.
// Bytes written once can be read by many later executions, so each execution holds a snapshot of the owners
// to keep those descriptors alive while the bytes are current.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace org::services {

/**
 * @brief Owners of uploaded descriptor-naming bytes. One thread only (the one that commits uploads).
 * Ranges are keyed by a target key, not a buffer: key a versioned buffer by VersionedBuffer::Key, so growth copies keep
 * their owners.
 */
class ContentOwners {
public:
    /**
     * @brief Makes one owner that holds all of owners. The host chooses how the owners are released, for example behind
     * the GPU work that reads the bytes. It may return null when nothing can read the bytes yet.
     */
    using Seal = std::function<std::shared_ptr<const void>(std::vector<std::shared_ptr<const void>> owners)>;

    /**
     * @brief Records the owners for a range, replacing every range it overlaps. Replaced owners live until the next
     * snapshot is released.
     */
    void Set(const void* target, std::uint64_t offset, std::uint64_t bytes, std::span<const std::shared_ptr<const void>> owners);

    /**
     * @brief One owner holding all current and recently replaced owners, made by seal. Rebuilt only after a change;
     * otherwise the last snapshot is returned and seal is not called. Null before the first change or when seal returns null.
     */
    std::shared_ptr<const void> Snapshot(const Seal& seal);

    struct Counts {
        std::size_t ranges = 0, owners = 0;
        std::uint64_t snapshotsMade = 0;
    };
    /** @brief The current ranges and owners, and the snapshots made since the last call. */
    Counts TakeCounts();

private:
    struct Range {
        std::uint64_t bytes = 0;
        std::vector<std::shared_ptr<const void>> owners;
    };
    std::map<std::pair<const void*, std::uint64_t>, Range> m_ranges;
    std::vector<std::shared_ptr<const void>> m_replaced; // owners of ranges replaced since the last snapshot
    std::shared_ptr<const void> m_sealed;
    bool m_changed = false;
    std::uint64_t m_snapshotsMade = 0;
};

} // namespace org::services
