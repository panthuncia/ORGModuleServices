// The sphere cull's SSE2 pass and the CPU detection. Built for the baseline instruction set.

#include <ORGModuleServices/Cpu/SphereCull.h>

#include <emmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace org::cpu {

void FrustumSse(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, std::uint8_t* a_pass)
{
    std::uint32_t i = 0;
    for (; i + 4 <= a_count; i += 4) {
        const __m128 x = _mm_loadu_ps(a_spheres.x + i), y = _mm_loadu_ps(a_spheres.y + i), z = _mm_loadu_ps(a_spheres.z + i);
        const __m128 r = _mm_loadu_ps(a_spheres.r + i);
        const __m128 reach = _mm_sub_ps(_mm_setzero_ps(), r);
        __m128 keep = _mm_castsi128_ps(_mm_set1_epi32(-1));
        for (std::uint32_t p = 0; p < a_planeCount; ++p) {
            const float* plane = a_planes[p];
            const __m128 d = _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(plane[0]), x), _mm_mul_ps(_mm_set1_ps(plane[1]), y)),
                                            _mm_mul_ps(_mm_set1_ps(plane[2]), z)),
                _mm_set1_ps(plane[3]));
            keep = _mm_and_ps(keep, _mm_cmpge_ps(d, reach));
        }
        keep = _mm_or_ps(keep, _mm_cmplt_ps(r, _mm_setzero_ps()));
        const int bits = _mm_movemask_ps(keep);
        for (std::uint32_t k = 0; k < 4; ++k)
            a_pass[i + k] = static_cast<std::uint8_t>((bits >> k) & 1);
    }
    for (; i < a_count; ++i) {
        bool keep = true;
        for (std::uint32_t p = 0; p < a_planeCount && keep; ++p) {
            const float* plane = a_planes[p];
            keep = plane[0] * a_spheres.x[i] + plane[1] * a_spheres.y[i] + plane[2] * a_spheres.z[i] - plane[3] >= -a_spheres.r[i];
        }
        a_pass[i] = keep || a_spheres.r[i] < 0.0f ? 1u : 0u;
    }
}

namespace {

#if defined(_MSC_VER)
void Cpuid(int (&a_info)[4], int a_leaf, int a_subleaf)
{
    __cpuidex(a_info, a_leaf, a_subleaf);
}
unsigned long long Xcr0()
{
    return _xgetbv(0);
}
#else
void Cpuid(int (&a_info)[4], int a_leaf, int a_subleaf)
{
    unsigned int a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(static_cast<unsigned int>(a_leaf), static_cast<unsigned int>(a_subleaf), a, b, c, d);
    a_info[0] = static_cast<int>(a), a_info[1] = static_cast<int>(b), a_info[2] = static_cast<int>(c), a_info[3] = static_cast<int>(d);
}
unsigned long long Xcr0()
{
    unsigned int lo = 0, hi = 0;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (static_cast<unsigned long long>(hi) << 32) | lo;
}
#endif

Level Detect()
{
    int info[4]{};
    Cpuid(info, 0, 0);
    const int leaves = info[0];
    Cpuid(info, 1, 0);
    const bool osxsave = (info[2] & (1 << 27)) != 0, avx = (info[2] & (1 << 28)) != 0;
    if (!osxsave || !avx || leaves < 7)
        return Level::Sse;
    const unsigned long long xcr0 = Xcr0();
    if ((xcr0 & 0x6) != 0x6)
        return Level::Sse;  // the OS does not keep the YMM state
    Cpuid(info, 7, 0);
    const bool avx2 = (info[1] & (1 << 5)) != 0;
    const bool avx512 = (info[1] & (1 << 16)) != 0 && (info[1] & (1 << 30)) != 0;  // F and BW
    if (!avx2)
        return Level::Sse;
    return avx512 && (xcr0 & 0xE6) == 0xE6 ? Level::Avx512 : Level::Avx2;  // the opmask and ZMM state must be kept too
}

} // namespace

Level Supported()
{
    static const Level level = Detect();
    return level;
}

const char* NameOf(Level a_level)
{
    return a_level == Level::Avx512 ? "AVX-512" : a_level == Level::Avx2 ? "AVX2" : "SSE";
}

FrustumFn WideFrustum(Level a_level)
{
    return a_level == Level::Avx512 ? &FrustumAvx512 : a_level == Level::Avx2 ? &FrustumAvx2 : nullptr;
}

} // namespace org::cpu
