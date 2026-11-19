#include <ORGModuleServices/ComposedVersions.h>

#include <stdexcept>

#include "VersionsSerial.h"

namespace org::services {

ComposedVersions::ComposedVersions(std::initializer_list<std::shared_ptr<const IResourceVersions>> parts) {
    for (const auto& part : parts) {
        if (!part) continue;
        if (m_count == kMaxParts) throw std::invalid_argument("ComposedVersions: more than four parts");
        m_parts[m_count++] = part;
    }
    m_serial = detail::NextVersionsSerial();
}

std::shared_ptr<const BufferVersion> ComposedVersions::Find(const VersionedBuffer& buffer) const noexcept {
    for (std::size_t i = 0; i < m_count; ++i)
        if (auto version = m_parts[i]->Find(buffer)) return version;
    return nullptr;
}

std::shared_ptr<const TextureVersion> ComposedVersions::FindTexture(const VersionedTexture& texture) const noexcept {
    for (std::size_t i = 0; i < m_count; ++i)
        if (auto version = m_parts[i]->FindTexture(texture)) return version;
    return nullptr;
}

} // namespace org::services
