#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <rhi.h>

#include "Render/Runtime/StreamingUploadTypes.h"

namespace org::services {

/** @brief One texel as four floats (red, green, blue, alpha), before it is encoded in a format. */
using TexelValue = std::array<float, 4>;

/**
 * @brief Encodes `value` as one texel of `format` into `out` (zeroed first). Returns the texel's size in bytes, or 0 when the
 * format has no encoding here: block-compressed, depth, packed depth-stencil, signed integer and the other formats not listed
 * in the source. Any thread.
 *
 * - Float formats store the value. Half floats round to nearest even. R11G11B10 clamps negatives to zero and large values
 *   to the largest finite value.
 * - UNorm and SNorm clamp to their range and round to nearest. sRGB encodes red, green and blue, not alpha.
 * - UInt formats convert each float with a plain cast (truncation toward zero).
 * - B8G8R8A8 stores blue first. Typeless formats encode as the float or UNorm format of the same layout.
 */
std::uint32_t EncodeTexel(rhi::Format format, const TexelValue& value, std::array<std::byte, 16>& out);

/** @brief The size of one texel EncodeTexel writes for `format`, or 0 when it has no encoding. Any thread. */
std::uint32_t EncodedTexelBytes(rhi::Format format);

/**
 * @brief A fill writer that writes `value`'s texel (EncodeTexel) into every texel of each subresource it is given. Null when
 * `format` has no encoding. It fails a subresource whose row pitch is shorter than its row of texels. Any thread.
 */
TextureFillWriter ValueFillWriter(rhi::Format format, const TexelValue& value);

} // namespace org::services
