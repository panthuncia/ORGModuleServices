// The sphere cull's AVX2 kernel, eight lanes at a time. Built with AVX2 enabled, and used only where the CPU supports it.

#include <ORGModuleServices/Cpu/SphereCull.h>

#include <array>
#include <cstring>
#include <immintrin.h>

namespace org::cpu {

namespace
{
    // An 8-lane verdict mask as eight 0/1 bytes.
    constexpr std::array<std::uint64_t, 256> MakeBytes()
    {
        std::array<std::uint64_t, 256> bytes{};
        for (std::uint32_t m = 0; m < 256; ++m)
            for (std::uint32_t k = 0; k < 8; ++k)
                bytes[m] |= std::uint64_t((m >> k) & 1u) << (8 * k);
        return bytes;
    }
    constexpr auto kBytes = MakeBytes();
}

void FrustumAvx2(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, const std::uint64_t* a_dead,
    std::uint8_t* a_pass)
{
    std::uint32_t i = 0;
    for (; i + 8 <= a_count; i += 8) {
        const __m256 x = _mm256_loadu_ps(a_spheres.x + i), y = _mm256_loadu_ps(a_spheres.y + i), z = _mm256_loadu_ps(a_spheres.z + i);
        const __m256 r = _mm256_loadu_ps(a_spheres.r + i);
        const __m256 reach = _mm256_sub_ps(_mm256_setzero_ps(), r);
        int keep = 0xFF;
        for (std::uint32_t p = 0; p < a_planeCount && keep; ++p) {
            const float* plane = a_planes[p];
            // Same order as the SSE pass: ((a * x + b * y) + c * z) - d against -r.
            const __m256 d = _mm256_sub_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(plane[0]), x), _mm256_mul_ps(_mm256_set1_ps(plane[1]), y)),
                                               _mm256_mul_ps(_mm256_set1_ps(plane[2]), z)),
                _mm256_set1_ps(plane[3]));
            keep &= _mm256_movemask_ps(_mm256_cmp_ps(d, reach, _CMP_GE_OQ));
        }
        keep |= _mm256_movemask_ps(_mm256_cmp_ps(r, _mm256_setzero_ps(), _CMP_LT_OQ));
        if (a_dead)
            keep &= ~static_cast<int>((a_dead[i >> 6] >> (i & 63)) & 0xFFu);
        std::memcpy(a_pass + i, &kBytes[static_cast<std::uint32_t>(keep) & 0xFFu], 8);
    }
    for (; i < a_count; ++i) {
        bool keep = true;
        for (std::uint32_t p = 0; p < a_planeCount && keep; ++p) {
            const float* plane = a_planes[p];
            keep = plane[0] * a_spheres.x[i] + plane[1] * a_spheres.y[i] + plane[2] * a_spheres.z[i] - plane[3] >= -a_spheres.r[i];
        }
        keep = keep || a_spheres.r[i] < 0.0f;
        if (a_dead && ((a_dead[i >> 6] >> (i & 63)) & 1u))
            keep = false;
        a_pass[i] = keep ? 1u : 0u;
    }
}

} // namespace org::cpu
