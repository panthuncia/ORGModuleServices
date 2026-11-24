#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace org::services {

/**
 * @brief An immutable paged CPU image of a buffer's contents (BasicRenderer's VersionedGpuBufferImage): dense, zero-filled where
 * nothing was written, split into 64 KB pages shared between images (a journal's writes clone only the pages they touch). What a
 * versioned buffer's version holds of its contents, and what a pooled backing catches up from.
 */
class BufferImage {
public:
    static constexpr std::size_t PageBytes = 64u * 1024u;
    using Page = std::vector<std::byte>;

    /** @brief An image of `bytes`, copied into pages. */
    [[nodiscard]] static std::shared_ptr<const BufferImage> FromBytes(std::span<const std::byte> bytes);
    /** @brief An image of `pages` (each PageBytes but the last) holding `byteSize` bytes; the pages are shared, not copied. */
    [[nodiscard]] static std::shared_ptr<const BufferImage> FromPages(std::vector<std::shared_ptr<const Page>> pages, std::size_t byteSize);
    /** @brief The contents as one contiguous copy. */
    [[nodiscard]] std::shared_ptr<const std::vector<std::byte>> Materialize() const;
    [[nodiscard]] std::size_t ByteSize() const noexcept { return m_byteSize; }
    [[nodiscard]] const std::vector<std::shared_ptr<const Page>>& Pages() const noexcept { return m_pages; }

private:
    friend class BufferJournal;
    std::size_t m_byteSize = 0;
    std::vector<std::shared_ptr<const Page>> m_pages;
};

} // namespace org::services
