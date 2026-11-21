#include <ORGModuleServices/TexelEncoding.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace org::services {

namespace {

template <class T>
void Put(std::array<std::byte, 16>& out, std::uint32_t index, T value) {
    std::memcpy(out.data() + index * sizeof(T), &value, sizeof(T));
}

std::uint32_t Unorm(float value, std::uint32_t bits) {
    const float max = static_cast<float>((1u << bits) - 1u);
    return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * max));
}

std::int32_t Snorm(float value, std::uint32_t bits) {
    const float max = static_cast<float>((1u << (bits - 1)) - 1u);
    return static_cast<std::int32_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * max));
}

float SrgbEncode(float linear) {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

std::uint32_t BitsOf(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// A float to a half float, rounding to nearest even. Bit for bit the software conversion of DirectXMath's
// XMConvertFloatToHalf, so a texel encodes alike on every platform.
std::uint16_t Half(float value) {
    std::uint32_t bits = BitsOf(value);
    const std::uint32_t sign = (bits & 0x80000000u) >> 16u;
    bits &= 0x7FFFFFFFu;
    std::uint32_t result;
    if (bits >= 0x47800000u) {
        // Too large for a half: infinity, or a quiet NaN.
        result = 0x7C00u | ((bits > 0x7F800000u) ? (0x200u | ((bits >> 13u) & 0x3FFu)) : 0u);
    } else if (bits <= 0x33000000u) {
        result = 0;
    } else if (bits < 0x38800000u) {
        // Too small for a normal half: a denormal one.
        const std::uint32_t shift = 125u - (bits >> 23u);
        bits = 0x800000u | (bits & 0x7FFFFFu);
        result = bits >> (shift + 1);
        const std::uint32_t sticky = (bits & ((1u << shift) - 1)) != 0;
        result += (result | sticky) & ((bits >> shift) & 1u);
    } else {
        // Rebias the exponent.
        bits += 0xC8000000u;
        result = ((bits + 0x0FFFu + ((bits >> 13u) & 1u)) >> 13u) & 0x7FFFu;
    }
    return static_cast<std::uint16_t>(result | sign);
}

// Red, green and blue packed as 11, 11 and 10 bit floats. Bit for bit DirectXMath's XMStoreFloat3PK. Its blue channel shifts
// a value below about 1e-13 by 32 or more; the shift count is masked to five bits, as x86's shift instruction does, so those
// values encode as they do there.
std::uint32_t Float3Packed(float red, float green, float blue) {
    const std::uint32_t in[3] = {BitsOf(red), BitsOf(green), BitsOf(blue)};
    std::uint32_t result[3];
    // Red and green: 5 bit exponent, 6 bit mantissa.
    for (std::uint32_t j = 0; j < 2; ++j) {
        const std::uint32_t sign = in[j] & 0x80000000u;
        std::uint32_t i = in[j] & 0x7FFFFFFFu;
        if ((i & 0x7F800000u) == 0x7F800000u) {
            // Infinity or NaN. Negative infinity clamps to zero: the format has no sign.
            result[j] = 0x7C0u;
            if ((i & 0x7FFFFFu) != 0)
                result[j] = 0x7FFu;
            else if (sign)
                result[j] = 0;
        } else if (sign || i < 0x35800000u) {
            result[j] = 0;
        } else if (i > 0x477E0000u) {
            // Too large: the largest finite value.
            result[j] = 0x7BFu;
        } else {
            if (i < 0x38800000u) {
                // Too small for a normal value: a denormal one.
                const std::uint32_t shift = 113u - (i >> 23u);
                i = (0x800000u | (i & 0x7FFFFFu)) >> shift;
            } else {
                i += 0xC8000000u;
            }
            result[j] = ((i + 0xFFFFu + ((i >> 17u) & 1u)) >> 17u) & 0x7FFu;
        }
    }
    // Blue: 5 bit exponent, 5 bit mantissa.
    const std::uint32_t sign = in[2] & 0x80000000u;
    std::uint32_t i = in[2] & 0x7FFFFFFFu;
    if ((i & 0x7F800000u) == 0x7F800000u) {
        result[2] = 0x3E0u;
        if (i & 0x7FFFFFu)
            result[2] = 0x3FFu;
        else if (sign || i < 0x36000000u)
            result[2] = 0;
    } else if (sign) {
        result[2] = 0;
    } else if (i > 0x477C0000u) {
        result[2] = 0x3DFu;
    } else {
        if (i < 0x38800000u) {
            const std::uint32_t shift = (113u - (i >> 23u)) & 31u;
            i = (0x800000u | (i & 0x7FFFFFu)) >> shift;
        } else {
            i += 0xC8000000u;
        }
        result[2] = ((i + 0x1FFFFu + ((i >> 18u) & 1u)) >> 18u) & 0x3FFu;
    }
    return (result[0] & 0x7FFu) | ((result[1] & 0x7FFu) << 11) | ((result[2] & 0x3FFu) << 22);
}

} // namespace

std::uint32_t EncodeTexel(rhi::Format format, const TexelValue& value, std::array<std::byte, 16>& out) {
    using F = rhi::Format;
    out = {};
    const auto& v = value;
    switch (format) {
    case F::R32G32B32A32_Typeless:
    case F::R32G32B32A32_Float:
        for (std::uint32_t i = 0; i < 4; ++i)
            Put(out, i, v[i]);
        return 16;
    case F::R32G32B32A32_UInt:
        for (std::uint32_t i = 0; i < 4; ++i)
            Put(out, i, static_cast<std::uint32_t>(v[i]));
        return 16;
    case F::R32G32B32_Float:
        for (std::uint32_t i = 0; i < 3; ++i)
            Put(out, i, v[i]);
        return 12;
    case F::R16G16B16A16_Typeless:
    case F::R16G16B16A16_Float:
        for (std::uint32_t i = 0; i < 4; ++i)
            Put(out, i, Half(v[i]));
        return 8;
    case F::R16G16B16A16_UNorm:
        for (std::uint32_t i = 0; i < 4; ++i)
            Put(out, i, static_cast<std::uint16_t>(Unorm(v[i], 16)));
        return 8;
    case F::R16G16B16A16_SNorm:
        for (std::uint32_t i = 0; i < 4; ++i)
            Put(out, i, static_cast<std::int16_t>(Snorm(v[i], 16)));
        return 8;
    case F::R32G32_Typeless:
    case F::R32G32_Float:
        for (std::uint32_t i = 0; i < 2; ++i)
            Put(out, i, v[i]);
        return 8;
    case F::R32G32_UInt:
        for (std::uint32_t i = 0; i < 2; ++i)
            Put(out, i, static_cast<std::uint32_t>(v[i]));
        return 8;
    case F::R10G10B10A2_Typeless:
    case F::R10G10B10A2_UNorm:
        Put(out, 0, Unorm(v[0], 10) | (Unorm(v[1], 10) << 10) | (Unorm(v[2], 10) << 20) | (Unorm(v[3], 2) << 30));
        return 4;
    case F::R11G11B10_Float:
        Put(out, 0, Float3Packed(v[0], v[1], v[2]));
        return 4;
    case F::R8G8B8A8_Typeless:
    case F::R8G8B8A8_UNorm:
        for (std::uint32_t i = 0; i < 4; ++i)
            Put(out, i, static_cast<std::uint8_t>(Unorm(v[i], 8)));
        return 4;
    case F::R8G8B8A8_UNorm_sRGB:
        for (std::uint32_t i = 0; i < 4; ++i)
            Put(out, i, static_cast<std::uint8_t>(Unorm(i < 3 ? SrgbEncode(v[i]) : v[i], 8)));
        return 4;
    case F::R8G8B8A8_SNorm:
        for (std::uint32_t i = 0; i < 4; ++i)
            Put(out, i, static_cast<std::int8_t>(Snorm(v[i], 8)));
        return 4;
    case F::B8G8R8A8_Typeless:
    case F::B8G8R8A8_UNorm:
        Put(out, 0, static_cast<std::uint8_t>(Unorm(v[2], 8)));
        Put(out, 1, static_cast<std::uint8_t>(Unorm(v[1], 8)));
        Put(out, 2, static_cast<std::uint8_t>(Unorm(v[0], 8)));
        Put(out, 3, static_cast<std::uint8_t>(Unorm(v[3], 8)));
        return 4;
    case F::R16G16_Typeless:
    case F::R16G16_Float:
        for (std::uint32_t i = 0; i < 2; ++i)
            Put(out, i, Half(v[i]));
        return 4;
    case F::R16G16_UNorm:
        for (std::uint32_t i = 0; i < 2; ++i)
            Put(out, i, static_cast<std::uint16_t>(Unorm(v[i], 16)));
        return 4;
    case F::R16G16_SNorm:
        for (std::uint32_t i = 0; i < 2; ++i)
            Put(out, i, static_cast<std::int16_t>(Snorm(v[i], 16)));
        return 4;
    case F::R32_Typeless:
    case F::R32_Float:
        Put(out, 0, v[0]);
        return 4;
    case F::R32_UInt:
        Put(out, 0, static_cast<std::uint32_t>(v[0]));
        return 4;
    case F::R8G8_Typeless:
    case F::R8G8_UNorm:
        for (std::uint32_t i = 0; i < 2; ++i)
            Put(out, i, static_cast<std::uint8_t>(Unorm(v[i], 8)));
        return 2;
    case F::R16_Typeless:
    case F::R16_Float:
        Put(out, 0, Half(v[0]));
        return 2;
    case F::R16_UNorm:
        Put(out, 0, static_cast<std::uint16_t>(Unorm(v[0], 16)));
        return 2;
    case F::R16_UInt:
        Put(out, 0, static_cast<std::uint16_t>(v[0]));
        return 2;
    case F::R8_Typeless:
    case F::R8_UNorm:
        Put(out, 0, static_cast<std::uint8_t>(Unorm(v[0], 8)));
        return 1;
    case F::R8_UInt:
        Put(out, 0, static_cast<std::uint8_t>(v[0]));
        return 1;
    default:
        return 0;
    }
}

std::uint32_t EncodedTexelBytes(rhi::Format format) {
    std::array<std::byte, 16> texel{};
    return EncodeTexel(format, {}, texel);
}

TextureFillWriter ValueFillWriter(rhi::Format format, const TexelValue& value) {
    std::array<std::byte, 16> texel{};
    const std::uint32_t bytes = EncodeTexel(format, value, texel);
    if (!bytes) return {};
    return [texel, bytes](std::uint32_t, std::uint32_t, std::uint32_t width, std::uint32_t rows, std::uint32_t depth, std::uint32_t rowPitch,
               std::byte* destination) {
        if (rowPitch < width * bytes) return false;
        for (std::uint32_t row = 0; row < rows * depth; ++row) {
            auto* line = destination + static_cast<std::size_t>(row) * rowPitch;
            for (std::uint32_t x = 0; x < width; ++x)
                std::memcpy(line + static_cast<std::size_t>(x) * bytes, texel.data(), bytes);
        }
        return true;
    };
}

} // namespace org::services
