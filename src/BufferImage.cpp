#include <ORGModuleServices/BufferImage.h>

#include <algorithm>

namespace org::services {

std::shared_ptr<const BufferImage> BufferImage::FromBytes(std::span<const std::byte> bytes) {
    auto image = std::make_shared<BufferImage>();
    image->m_byteSize = bytes.size();
    image->m_pages.reserve((bytes.size() + PageBytes - 1u) / PageBytes);
    for (std::size_t offset = 0; offset < bytes.size(); offset += PageBytes) {
        const auto size = (std::min)(PageBytes, bytes.size() - offset);
        image->m_pages.push_back(std::make_shared<const Page>(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)));
    }
    return image;
}

std::shared_ptr<const BufferImage> BufferImage::FromPages(std::vector<std::shared_ptr<const Page>> pages, std::size_t byteSize) {
    auto image = std::make_shared<BufferImage>();
    image->m_byteSize = byteSize;
    image->m_pages = std::move(pages);
    return image;
}

std::shared_ptr<const std::vector<std::byte>> BufferImage::Materialize() const {
    auto bytes = std::make_shared<std::vector<std::byte>>(m_byteSize);
    std::size_t offset = 0;
    for (const auto& page : m_pages) {
        if (!page || offset >= m_byteSize) break;
        const auto size = (std::min)(page->size(), m_byteSize - offset);
        std::copy_n(page->begin(), size, bytes->begin() + static_cast<std::ptrdiff_t>(offset));
        offset += size;
    }
    return bytes;
}

} // namespace org::services
