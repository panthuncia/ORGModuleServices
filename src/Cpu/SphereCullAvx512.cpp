// The sphere cull's AVX-512 kernel, sixteen lanes at a time. Built with AVX-512 enabled, and used only where the CPU
// supports F and BW.

#include <ORGModuleServices/Cpu/SphereCull.h>

#include <cstring>
#include <immintrin.h>

namespace org::cpu {

void FrustumAvx512(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, const std::uint64_t* a_dead,
    std::uint8_t* a_pass)
{
    const __m128i one = _mm_set1_epi8(1);
    std::uint32_t i = 0;
    for (; i + 16 <= a_count; i += 16) {
        const __m512 x = _mm512_loadu_ps(a_spheres.x + i), y = _mm512_loadu_ps(a_spheres.y + i), z = _mm512_loadu_ps(a_spheres.z + i);
        const __m512 r = _mm512_loadu_ps(a_spheres.r + i);
        const __m512 reach = _mm512_sub_ps(_mm512_setzero_ps(), r);
        __mmask16 keep = 0xFFFF;
        for (std::uint32_t p = 0; p < a_planeCount && keep; ++p) {
            const float* plane = a_planes[p];
            // Same order as the SSE pass: ((a * x + b * y) + c * z) - d against -r.
            const __m512 d = _mm512_sub_ps(_mm512_add_ps(_mm512_add_ps(_mm512_mul_ps(_mm512_set1_ps(plane[0]), x), _mm512_mul_ps(_mm512_set1_ps(plane[1]), y)),
                                               _mm512_mul_ps(_mm512_set1_ps(plane[2]), z)),
                _mm512_set1_ps(plane[3]));
            keep = _mm512_mask_cmp_ps_mask(keep, d, reach, _CMP_GE_OQ);
        }
        keep |= _mm512_cmp_ps_mask(r, _mm512_setzero_ps(), _CMP_LT_OQ);
        if (a_dead)
            keep &= static_cast<__mmask16>(~(a_dead[i >> 6] >> (i & 63)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(a_pass + i), _mm_maskz_mov_epi8(keep, one));
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
