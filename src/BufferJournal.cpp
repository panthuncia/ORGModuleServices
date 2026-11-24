#include <ORGModuleServices/BufferJournal.h>

#include <ORGModuleServices/BackingPool.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>

namespace org::services {

BufferJournal::BufferJournal(std::uint32_t elementStride) : m_elementStride(elementStride) {}

void BufferJournal::Initialize(std::span<const std::byte> bytes, std::uint64_t elementCount, std::uint64_t capacity) {
    if (m_elementStride == 0 || elementCount > capacity || bytes.size() != elementCount * m_elementStride)
        throw std::invalid_argument("invalid buffer journal initial image");
    std::lock_guard lock(m_mutex);
    m_previous.reset();
    m_writes.clear();
    m_journalBaseSequence = 0;
    m_byteSize = bytes.size();
    m_pages.clear();
    m_pages.reserve((bytes.size() + BufferImage::PageBytes - 1u) / BufferImage::PageBytes);
    for (std::size_t offset = 0; offset < bytes.size(); offset += BufferImage::PageBytes) {
        const auto size = (std::min)(BufferImage::PageBytes, bytes.size() - offset);
        m_pages.push_back(std::make_shared<BufferImage::Page>(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)));
    }
    m_elementCount = elementCount;
    m_capacity = capacity;
    m_writeSequence = 1;
}

std::uint64_t BufferJournal::AppendWrite(std::uint64_t elementOffset, std::span<const std::byte> bytes, std::uint64_t resultingElementCount) {
    if (m_elementStride == 0 || bytes.empty() || bytes.size() % m_elementStride != 0)
        throw std::invalid_argument("buffer journal write is not row aligned");
    const auto writeElements = bytes.size() / m_elementStride;
    if (elementOffset > (std::numeric_limits<std::uint64_t>::max)() - writeElements || elementOffset + writeElements > resultingElementCount)
        throw std::out_of_range("buffer journal write exceeds logical extent");
    std::lock_guard lock(m_mutex);
    if (resultingElementCount > m_capacity) throw std::out_of_range("buffer journal write exceeds desired capacity");
    const auto sequence = ++m_writeSequence;
    const auto logicalElementCount = (std::max)(m_elementCount, resultingElementCount);
    const auto desiredSize = static_cast<std::size_t>(logicalElementCount * m_elementStride);
    const auto byteOffset = static_cast<std::size_t>(elementOffset * m_elementStride);
    const auto requiredPages = (desiredSize + BufferImage::PageBytes - 1u) / BufferImage::PageBytes;
    while (m_pages.size() < requiredPages) {
        const auto pageOffset = m_pages.size() * BufferImage::PageBytes;
        const auto pageSize = (std::min)(BufferImage::PageBytes, desiredSize - pageOffset);
        m_pages.push_back(std::make_shared<BufferImage::Page>(pageSize));
    }
    // Sparse growth can begin in a later page, leaving the former tail page shorter than its newly covered extent. The image is a
    // dense zero-filled byte image even when its journal is sparse, so every newly covered tail grows before the write lands.
    for (std::size_t pageIndex = 0; pageIndex < requiredPages; ++pageIndex) {
        const auto pageBegin = pageIndex * BufferImage::PageBytes;
        const auto requiredPageSize = (std::min)(BufferImage::PageBytes, desiredSize - pageBegin);
        if (m_pages[pageIndex]->size() >= requiredPageSize) continue;
        if (m_pages[pageIndex].use_count() != 1) m_pages[pageIndex] = std::make_shared<BufferImage::Page>(*m_pages[pageIndex]);
        m_pages[pageIndex]->resize(requiredPageSize);
    }
    std::size_t sourceOffset = 0;
    while (sourceOffset < bytes.size()) {
        const auto absoluteOffset = byteOffset + sourceOffset;
        const auto pageIndex = absoluteOffset / BufferImage::PageBytes;
        const auto pageOffset = absoluteOffset % BufferImage::PageBytes;
        // A page a capture still shares is cloned before the write (captures are immutable).
        if (m_pages[pageIndex].use_count() != 1) m_pages[pageIndex] = std::make_shared<BufferImage::Page>(*m_pages[pageIndex]);
        const auto copySize = (std::min)(bytes.size() - sourceOffset, m_pages[pageIndex]->size() - pageOffset);
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(sourceOffset), copySize,
            m_pages[pageIndex]->begin() + static_cast<std::ptrdiff_t>(pageOffset));
        sourceOffset += copySize;
    }
    m_byteSize = desiredSize;
    m_writes.push_back(BufferWrite{sequence, byteOffset, static_cast<std::uint64_t>(bytes.size())});
    m_elementCount = logicalElementCount;
    return m_writeSequence;
}

std::uint64_t BufferJournal::ReplaceImage(std::span<const std::byte> bytes, std::uint64_t elementCount, std::uint64_t capacity) {
    if (m_elementStride == 0 || elementCount > capacity || bytes.size() != elementCount * m_elementStride)
        throw std::invalid_argument("invalid buffer journal replacement image");
    std::lock_guard lock(m_mutex);
    m_capacity = (std::max)(m_capacity, capacity);
    m_elementCount = elementCount;
    ++m_writeSequence;
    m_byteSize = bytes.size();
    m_pages.clear();
    m_pages.reserve((bytes.size() + BufferImage::PageBytes - 1u) / BufferImage::PageBytes);
    for (std::size_t offset = 0; offset < bytes.size(); offset += BufferImage::PageBytes) {
        const auto size = (std::min)(BufferImage::PageBytes, bytes.size() - offset);
        m_pages.push_back(std::make_shared<BufferImage::Page>(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)));
    }
    m_writes.push_back(BufferWrite{m_writeSequence, 0u, static_cast<std::uint64_t>(bytes.size())});
    return m_writeSequence;
}

void BufferJournal::RequestCapacity(std::uint64_t capacity) {
    std::lock_guard lock(m_mutex);
    if (capacity <= m_capacity) return;
    m_capacity = capacity;
    ++m_writeSequence;
    // A capacity-only revision needs a zero-length checkpoint to close the captured sequence without inventing a data write.
    m_writes.push_back(BufferWrite{m_writeSequence, m_elementCount * m_elementStride, 0u});
}

BufferJournal::Capture BufferJournal::CaptureDesired() const {
    std::lock_guard lock(m_mutex);
    Capture capture;
    capture.writeSequence = m_writeSequence;
    capture.elementCount = m_elementCount;
    capture.capacity = m_capacity;
    capture.previous = m_previous;
    capture.writes = m_writes;
    std::vector<std::shared_ptr<const BufferImage::Page>> pages(m_pages.begin(), m_pages.end());
    capture.image = BufferImage::FromPages(std::move(pages), m_byteSize);
    capture.journalBaseSequence = m_journalBaseSequence;
    return capture;
}

void BufferJournal::SetBackingPool(const std::shared_ptr<const BackingPool>& pool) {
    std::lock_guard lock(m_mutex);
    m_backingPool = pool;
}

void BufferJournal::Acknowledge(const std::shared_ptr<const PublishedBufferVersion>& version) {
    if (!version) return;
    std::shared_ptr<const BackingPool> pool;
    {
        std::lock_guard lock(m_mutex);
        pool = m_backingPool.lock();
    }
    // The floor is read outside the journal's mutex (the pool serializes it on its own). It only decides how far back the journal
    // reaches: a backing below the base the next capture names is written whole, so a floor that moved meanwhile costs uploads,
    // never correctness.
    const auto floor = pool ? pool->OldestContentEpoch() : std::nullopt;
    std::lock_guard lock(m_mutex);
    if (version->writeSequence > m_writeSequence || (m_previous && version->writeSequence <= m_previous->writeSequence)) return;
    m_previous = version;
    const auto base = floor ? (std::min)(version->writeSequence, *floor) : version->writeSequence;
    // Never back past writes already dropped.
    m_journalBaseSequence = (std::max)(m_journalBaseSequence, base);
    std::erase_if(m_writes, [&](const BufferWrite& write) { return write.sequence <= m_journalBaseSequence; });
}

std::uint64_t BufferJournal::DesiredSequence() const {
    std::lock_guard lock(m_mutex);
    return m_writeSequence;
}

bool BufferJournal::HasUnpublishedChanges() const {
    std::lock_guard lock(m_mutex);
    return !m_previous || m_previous->writeSequence != m_writeSequence;
}

} // namespace org::services
