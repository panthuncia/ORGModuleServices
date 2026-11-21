// Texel encoding (EncodeTexel, ValueFillWriter) against a reference keyed on DXGI formats that converts half floats and
// R11G11B10 with DirectXMath. Every format must encode the same bytes for every value tried.

#include <ORGModuleServices/TexelEncoding.h>

#include <rhi.h>
#include <rhi_helpers.h>

#include <DirectXPackedVector.h>
#include <dxgiformat.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#define CHECK(...)                                                                          \
    do {                                                                                    \
        if (!(__VA_ARGS__)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);     \
            return __LINE__;                                                                \
        }                                                                                   \
    } while (false)

namespace {

using org::services::TexelValue;

// The reference: the DXGI-keyed encoder the library's encoder replaced, unchanged.
template <class T>
void RefPut(std::array<std::byte, 16>& out, std::uint32_t index, T value) {
    std::memcpy(out.data() + index * sizeof(T), &value, sizeof(T));
}

std::uint32_t RefUnorm(float value, std::uint32_t bits) {
    const float max = static_cast<float>((1u << bits) - 1u);
    return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * max));
}

std::int32_t RefSnorm(float value, std::uint32_t bits) {
    const float max = static_cast<float>((1u << (bits - 1)) - 1u);
    return static_cast<std::int32_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * max));
}

float RefSrgbEncode(float linear) {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

std::uint32_t RefEncode(DXGI_FORMAT format, const TexelValue& v, std::array<std::byte, 16>& out) {
    using namespace DirectX::PackedVector;
    out = {};
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        for (std::uint32_t i = 0; i < 4; ++i) RefPut(out, i, v[i]);
        return 16;
    case DXGI_FORMAT_R32G32B32A32_UINT:
        for (std::uint32_t i = 0; i < 4; ++i) RefPut(out, i, static_cast<std::uint32_t>(v[i]));
        return 16;
    case DXGI_FORMAT_R32G32B32_FLOAT:
        for (std::uint32_t i = 0; i < 3; ++i) RefPut(out, i, v[i]);
        return 12;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        for (std::uint32_t i = 0; i < 4; ++i) RefPut(out, i, XMConvertFloatToHalf(v[i]));
        return 8;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        for (std::uint32_t i = 0; i < 4; ++i) RefPut(out, i, static_cast<std::uint16_t>(RefUnorm(v[i], 16)));
        return 8;
    case DXGI_FORMAT_R16G16B16A16_SNORM:
        for (std::uint32_t i = 0; i < 4; ++i) RefPut(out, i, static_cast<std::int16_t>(RefSnorm(v[i], 16)));
        return 8;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
        for (std::uint32_t i = 0; i < 2; ++i) RefPut(out, i, v[i]);
        return 8;
    case DXGI_FORMAT_R32G32_UINT:
        for (std::uint32_t i = 0; i < 2; ++i) RefPut(out, i, static_cast<std::uint32_t>(v[i]));
        return 8;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        RefPut(out, 0, RefUnorm(v[0], 10) | (RefUnorm(v[1], 10) << 10) | (RefUnorm(v[2], 10) << 20) | (RefUnorm(v[3], 2) << 30));
        return 4;
    case DXGI_FORMAT_R11G11B10_FLOAT: {
        XMFLOAT3PK packed;
        const DirectX::XMFLOAT3 rgb{v[0], v[1], v[2]};
        XMStoreFloat3PK(&packed, DirectX::XMLoadFloat3(&rgb));
        RefPut(out, 0, packed.v);
        return 4;
    }
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        for (std::uint32_t i = 0; i < 4; ++i) RefPut(out, i, static_cast<std::uint8_t>(RefUnorm(v[i], 8)));
        return 4;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        for (std::uint32_t i = 0; i < 4; ++i)
            RefPut(out, i, static_cast<std::uint8_t>(RefUnorm(i < 3 ? RefSrgbEncode(v[i]) : v[i], 8)));
        return 4;
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        for (std::uint32_t i = 0; i < 4; ++i) RefPut(out, i, static_cast<std::int8_t>(RefSnorm(v[i], 8)));
        return 4;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        RefPut(out, 0, static_cast<std::uint8_t>(RefUnorm(v[2], 8)));
        RefPut(out, 1, static_cast<std::uint8_t>(RefUnorm(v[1], 8)));
        RefPut(out, 2, static_cast<std::uint8_t>(RefUnorm(v[0], 8)));
        RefPut(out, 3, static_cast<std::uint8_t>(RefUnorm(v[3], 8)));
        return 4;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
        for (std::uint32_t i = 0; i < 2; ++i) RefPut(out, i, XMConvertFloatToHalf(v[i]));
        return 4;
    case DXGI_FORMAT_R16G16_UNORM:
        for (std::uint32_t i = 0; i < 2; ++i) RefPut(out, i, static_cast<std::uint16_t>(RefUnorm(v[i], 16)));
        return 4;
    case DXGI_FORMAT_R16G16_SNORM:
        for (std::uint32_t i = 0; i < 2; ++i) RefPut(out, i, static_cast<std::int16_t>(RefSnorm(v[i], 16)));
        return 4;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
        RefPut(out, 0, v[0]);
        return 4;
    case DXGI_FORMAT_R32_UINT:
        RefPut(out, 0, static_cast<std::uint32_t>(v[0]));
        return 4;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
        for (std::uint32_t i = 0; i < 2; ++i) RefPut(out, i, static_cast<std::uint8_t>(RefUnorm(v[i], 8)));
        return 2;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
        RefPut(out, 0, XMConvertFloatToHalf(v[0]));
        return 2;
    case DXGI_FORMAT_R16_UNORM:
        RefPut(out, 0, static_cast<std::uint16_t>(RefUnorm(v[0], 16)));
        return 2;
    case DXGI_FORMAT_R16_UINT:
        RefPut(out, 0, static_cast<std::uint16_t>(v[0]));
        return 2;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
        RefPut(out, 0, static_cast<std::uint8_t>(RefUnorm(v[0], 8)));
        return 1;
    case DXGI_FORMAT_R8_UINT:
        RefPut(out, 0, static_cast<std::uint8_t>(v[0]));
        return 1;
    default:
        return 0;
    }
}

enum class Kind { Float, Norm, UInt, None };

struct FormatCase {
    DXGI_FORMAT format;
    Kind kind;
    double uintMax = 0;  // UInt: the largest value the cast holds
};

// Every format the reference encodes, and some it does not.
const FormatCase kFormats[] = {
    {DXGI_FORMAT_R32G32B32A32_TYPELESS, Kind::Float},
    {DXGI_FORMAT_R32G32B32A32_FLOAT, Kind::Float},
    {DXGI_FORMAT_R32G32B32A32_UINT, Kind::UInt, 4294967295.0},
    {DXGI_FORMAT_R32G32B32_FLOAT, Kind::Float},
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, Kind::Float},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, Kind::Float},
    {DXGI_FORMAT_R16G16B16A16_UNORM, Kind::Norm},
    {DXGI_FORMAT_R16G16B16A16_SNORM, Kind::Norm},
    {DXGI_FORMAT_R32G32_TYPELESS, Kind::Float},
    {DXGI_FORMAT_R32G32_FLOAT, Kind::Float},
    {DXGI_FORMAT_R32G32_UINT, Kind::UInt, 4294967295.0},
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, Kind::Norm},
    {DXGI_FORMAT_R10G10B10A2_UNORM, Kind::Norm},
    {DXGI_FORMAT_R11G11B10_FLOAT, Kind::Float},
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, Kind::Norm},
    {DXGI_FORMAT_R8G8B8A8_UNORM, Kind::Norm},
    {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, Kind::Norm},
    {DXGI_FORMAT_R8G8B8A8_SNORM, Kind::Norm},
    {DXGI_FORMAT_B8G8R8A8_TYPELESS, Kind::Norm},
    {DXGI_FORMAT_B8G8R8A8_UNORM, Kind::Norm},
    {DXGI_FORMAT_R16G16_TYPELESS, Kind::Float},
    {DXGI_FORMAT_R16G16_FLOAT, Kind::Float},
    {DXGI_FORMAT_R16G16_UNORM, Kind::Norm},
    {DXGI_FORMAT_R16G16_SNORM, Kind::Norm},
    {DXGI_FORMAT_R32_TYPELESS, Kind::Float},
    {DXGI_FORMAT_R32_FLOAT, Kind::Float},
    {DXGI_FORMAT_R32_UINT, Kind::UInt, 4294967295.0},
    {DXGI_FORMAT_R8G8_TYPELESS, Kind::Norm},
    {DXGI_FORMAT_R8G8_UNORM, Kind::Norm},
    {DXGI_FORMAT_R16_TYPELESS, Kind::Float},
    {DXGI_FORMAT_R16_FLOAT, Kind::Float},
    {DXGI_FORMAT_R16_UNORM, Kind::Norm},
    {DXGI_FORMAT_R16_UINT, Kind::UInt, 65535.0},
    {DXGI_FORMAT_R8_TYPELESS, Kind::Norm},
    {DXGI_FORMAT_R8_UNORM, Kind::Norm},
    {DXGI_FORMAT_R8_UINT, Kind::UInt, 255.0},
    // No encoding.
    {DXGI_FORMAT_UNKNOWN, Kind::None},
    {DXGI_FORMAT_R8G8B8A8_UINT, Kind::None},
    {DXGI_FORMAT_R16G16B16A16_UINT, Kind::None},
    {DXGI_FORMAT_R32_SINT, Kind::None},
    {DXGI_FORMAT_D32_FLOAT, Kind::None},
    {DXGI_FORMAT_D24_UNORM_S8_UINT, Kind::None},
    {DXGI_FORMAT_D16_UNORM, Kind::None},
    {DXGI_FORMAT_BC1_UNORM, Kind::None},
    {DXGI_FORMAT_BC7_UNORM_SRGB, Kind::None},
    {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, Kind::None},
    {DXGI_FORMAT_B8G8R8X8_UNORM, Kind::None},
    {DXGI_FORMAT_A8_UNORM, Kind::None},
};

// Deterministic values: fixed edge cases, then pseudo-random ones of the kind's range.
std::vector<float> ValuesFor(const FormatCase& c) {
    std::vector<float> values;
    const float fixed[] = {0.0f, -0.0f, 1.0f, 0.5f, 0.25f, 1.0f / 3.0f, 2.0f, 3.5449077f, 0.0031308f, 0.04045f, 0.999f, 0.001f, 255.0f,
        255.5f, 256.0f, 1023.0f, 65504.0f, 65519.0f, 65520.0f, 65536.0f, 70000.0f, 1.0e5f, 6.1e-5f, 6.0e-8f, 3.0e-8f, 1.0e-5f, 1.0e-7f,
        -1.0f, -0.5f, -2.0f, -65504.0f, -1.0e-6f, 0.4999f, 0.5001f};
    for (float f : fixed) values.push_back(f);
    std::uint32_t state = 0x12345678u ^ static_cast<std::uint32_t>(c.format);
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state;
    };
    for (int i = 0; i < 20000; ++i) {
        const std::uint32_t r = next();
        float f;
        switch (c.kind) {
        case Kind::Float: {
            // Any sign, exponents from 2^-40 to 2^20: denormal, normal and overflowing halves and small floats.
            const std::uint32_t exponent = 87u + (next() % 60u);
            const std::uint32_t bits = (r & 0x80000000u) | (exponent << 23) | (next() & 0x7FFFFFu);
            std::memcpy(&f, &bits, sizeof(f));
            break;
        }
        case Kind::Norm:
            f = static_cast<float>(static_cast<double>(r) / 4294967295.0 * 3.0 - 1.5);
            break;
        default:
            f = static_cast<float>(static_cast<double>(r) / 4294967295.0 * 300.0);
            break;
        }
        values.push_back(f);
    }
    return values;
}

// Whether a plain cast to the format's integer type is defined for `f`.
bool CastDefined(const FormatCase& c, float f) {
    return c.kind != Kind::UInt || (f >= 0.0f && static_cast<double>(f) < c.uintMax + 1.0);
}

int TestMatchesReference() {
    for (const auto& c : kFormats) {
        const rhi::Format format = rhi::helpers::ToRHI(c.format);
        std::array<std::byte, 16> expected{}, actual{};
        // A zero value gives each format's size.
        const std::uint32_t expectedBytes = RefEncode(c.format, {}, expected);
        CHECK(org::services::EncodedTexelBytes(format) == expectedBytes);
        CHECK((c.kind == Kind::None) == (expectedBytes == 0));
        if (c.kind == Kind::None) {
            CHECK(org::services::EncodeTexel(format, {1.0f, 1.0f, 1.0f, 1.0f}, actual) == 0);
            CHECK(!org::services::ValueFillWriter(format, {1.0f, 1.0f, 1.0f, 1.0f}));
            continue;
        }
        const auto values = ValuesFor(c);
        for (std::size_t i = 0; i < values.size(); ++i) {
            // Channels from a rotation of the values, so each channel sees each value.
            TexelValue value{values[i], values[(i + 7) % values.size()], values[(i + 13) % values.size()], values[(i + 29) % values.size()]};
            bool defined = true;
            for (float f : value) defined = defined && CastDefined(c, f);
            if (!defined) continue;
            // XMStoreFloat3PK shifts a tiny positive blue by 32 or more, which is undefined in C++, so the reference's
            // result there depends on the compiler.
            if (c.format == DXGI_FORMAT_R11G11B10_FLOAT && value[2] > 0.0f && value[2] < 1.0e-12f) continue;
            const std::uint32_t refBytes = RefEncode(c.format, value, expected);
            const std::uint32_t bytes = org::services::EncodeTexel(format, value, actual);
            if (bytes != refBytes || expected != actual) {
                std::fprintf(stderr, "format %d value (%a, %a, %a, %a) encodes differently\n", static_cast<int>(c.format), value[0], value[1],
                    value[2], value[3]);
                CHECK(false);
            }
        }
    }
    return 0;
}

// The births the formats in use get today, byte for byte.
int TestKnownTexels() {
    std::array<std::byte, 16> texel{};
    const auto u16 = [&](std::size_t i) {
        std::uint16_t v;
        std::memcpy(&v, texel.data() + i * 2, 2);
        return v;
    };
    // Unit SH in a half-float probe.
    CHECK(org::services::EncodeTexel(rhi::Format::R16G16B16A16_Float, {3.5449077f, 0.0f, 0.0f, 0.0f}, texel) == 8);
    CHECK(u16(0) == 0x4317 && u16(1) == 0 && u16(2) == 0 && u16(3) == 0);
    // Full transmittance.
    CHECK(org::services::EncodeTexel(rhi::Format::R16G16B16A16_Float, {1.0f, 1.0f, 1.0f, 1.0f}, texel) == 8);
    CHECK(u16(0) == 0x3C00 && u16(1) == 0x3C00 && u16(2) == 0x3C00 && u16(3) == 0x3C00);
    CHECK(org::services::EncodeTexel(rhi::Format::R16G16B16A16_Float, {0.0f, 0.0f, 0.0f, 1.0f}, texel) == 8);
    CHECK(u16(0) == 0 && u16(1) == 0 && u16(2) == 0 && u16(3) == 0x3C00);
    CHECK(org::services::EncodeTexel(rhi::Format::R16_Float, {1.0f, 1.0f, 1.0f, 1.0f}, texel) == 2);
    CHECK(u16(0) == 0x3C00 && u16(1) == 0);
    // Full visibility, and a coverage of 2.
    CHECK(org::services::EncodeTexel(rhi::Format::R8_UNorm, {1.0f, 1.0f, 1.0f, 1.0f}, texel) == 1);
    CHECK(texel[0] == std::byte{0xFF} && texel[1] == std::byte{0});
    CHECK(org::services::EncodeTexel(rhi::Format::R8_UInt, {2.0f, 0.0f, 0.0f, 0.0f}, texel) == 1);
    CHECK(texel[0] == std::byte{2});
    // A bitmask filled by its own writer still has a texel size.
    CHECK(org::services::EncodedTexelBytes(rhi::Format::R32_UInt) == 4);
    return 0;
}

// The writer repeats the texel over every row and slice, leaves the row padding alone, and fails a short row pitch.
int TestValueFillWriter() {
    const auto writer = org::services::ValueFillWriter(rhi::Format::R16G16_Float, {1.0f, 0.5f, 0.0f, 0.0f});
    CHECK(static_cast<bool>(writer));
    constexpr std::uint32_t width = 3, rows = 2, depth = 2, rowPitch = 16;
    std::vector<std::byte> staging(rowPitch * rows * depth, std::byte{0xAB});
    CHECK(writer(0, 0, width, rows, depth, rowPitch, staging.data()));
    for (std::uint32_t row = 0; row < rows * depth; ++row) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint16_t red, green;
            std::memcpy(&red, staging.data() + row * rowPitch + x * 4, 2);
            std::memcpy(&green, staging.data() + row * rowPitch + x * 4 + 2, 2);
            CHECK(red == 0x3C00 && green == 0x3800);
        }
        for (std::uint32_t pad = width * 4; pad < rowPitch; ++pad) CHECK(staging[row * rowPitch + pad] == std::byte{0xAB});
    }
    CHECK(!writer(0, 0, 5, 1, 1, 16, staging.data()));
    return 0;
}

} // namespace

int main() {
    if (const int line = TestMatchesReference()) return line;
    if (const int line = TestKnownTexels()) return line;
    if (const int line = TestValueFillWriter()) return line;
    std::puts("ORGTexelEncodingTests passed");
    return 0;
}
