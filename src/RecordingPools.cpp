#include <ORGModuleServices/RecordingPools.h>

#include <algorithm>

namespace org::services {

void* BumpArena::Allocate(std::size_t bytes, std::size_t align) {
    for (;;) {
        if (m_current < m_chunks.size()) {
            auto& chunk = m_chunks[m_current];
            const std::size_t at = (m_used + align - 1) & ~(align - 1);
            if (at + bytes <= chunk.size) {
                m_used = at + bytes;
                return chunk.bytes.get() + at;
            }
            ++m_current;
            m_used = 0;
            continue;
        }
        // A request larger than a chunk gets its own size. Old chunks never move.
        const std::size_t size = (std::max)(m_chunkBytes, bytes + align);
        m_chunks.push_back({std::make_unique<std::byte[]>(size), size});
        m_current = m_chunks.size() - 1;
        m_used = 0;
    }
}

void BumpArena::Reset() noexcept {
    m_current = 0;
    m_used = 0;
}

std::size_t BumpArena::BytesUsed() const noexcept {
    std::size_t bytes = m_used;
    for (std::size_t c = 0; c < m_current && c < m_chunks.size(); ++c) bytes += m_chunks[c].size;
    return bytes;
}

} // namespace org::services
