#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>

#include "Resources/VersionedResource.h"

namespace org::services {

/**
 * @brief The versions of up to four sets as one (IResourceVersions): the first part that names a resource answers. Immutable.
 *
 * A frame revision can name a buffer set and a texture set this way without merging them. So a new texture set
 * never makes a new buffer set, and the reverse.
 */
class ComposedVersions final : public IResourceVersions {
public:
    static constexpr std::size_t kMaxParts = 4;

    /** @brief Any thread. Null parts are skipped. Throws with more than kMaxParts. The set gets a new Serial. */
    explicit ComposedVersions(std::initializer_list<std::shared_ptr<const IResourceVersions>> parts);

    std::shared_ptr<const BufferVersion> Find(const VersionedBuffer& buffer) const noexcept override;
    std::shared_ptr<const TextureVersion> FindTexture(const VersionedTexture& texture) const noexcept override;

    /** @brief Process-unique, from the sequence NamedVersions::Serial draws from. */
    [[nodiscard]] std::uint64_t Serial() const noexcept { return m_serial; }
    /** @brief How many parts it holds (null ones skipped). */
    [[nodiscard]] std::size_t PartCount() const noexcept { return m_count; }
    /** @brief A part, in answer order. */
    [[nodiscard]] const std::shared_ptr<const IResourceVersions>& Part(std::size_t index) const noexcept { return m_parts[index]; }

private:
    std::array<std::shared_ptr<const IResourceVersions>, kMaxParts> m_parts{};
    std::size_t m_count = 0;
    std::uint64_t m_serial = 0;
};

} // namespace org::services
