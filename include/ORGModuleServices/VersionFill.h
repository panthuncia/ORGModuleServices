#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Render/Runtime/StreamingUploadTypes.h"

namespace org::services {

/** @brief Part of a new version's contents: segments gathered in order from `offset` (VersionGrowthRequest's fills). */
struct VersionFill {
    std::uint64_t offset = 0;                      // bytes into the new version
    std::vector<StreamingUploadSegment> segments;  // gathered in order; their memory is the request's `contents`
};

namespace detail {
// What a zero fill copies from: one static page, as many segments as the version needs.
inline constexpr std::size_t kZeroPageBytes = 64 * 1024;
inline const std::array<std::byte, kZeroPageBytes> kZeroPage{};
} // namespace detail

/**
 * @brief Zeros for `bytes` from offset 0, from one static page (no allocation per fill): a zero-filled version's contents. Asked
 * for the whole version (its buffer's size, not the units required), so a reused backing keeps none of its older bytes.
 */
[[nodiscard]] inline VersionFill ZeroFill(std::uint64_t bytes) {
    VersionFill fill;
    fill.segments.reserve(static_cast<std::size_t>((bytes + detail::kZeroPageBytes - 1) / detail::kZeroPageBytes));
    for (std::uint64_t at = 0; at < bytes; at += detail::kZeroPageBytes)
        fill.segments.push_back({detail::kZeroPage.data(), static_cast<std::size_t>((std::min<std::uint64_t>)(detail::kZeroPageBytes, bytes - at))});
    return fill;
}

} // namespace org::services
