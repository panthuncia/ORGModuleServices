#pragma once

// A CPU frustum test over columns of bounding spheres, with SSE2, AVX2 and AVX-512 kernels.
// Each wide kernel is compiled for its own instruction set, and a host picks one at run time from Supported().
// Every kernel gives the same verdicts bit for bit: they keep one operation order and use no FMA.
// The header uses plain types only, so files built for any instruction set can include it.

#include <cstdint>

namespace org::cpu {

/** @brief A batch's sphere columns: centres x, y, z and radius r. A negative radius means unknown. */
struct Spheres
{
    const float *x, *y, *z, *r;
};

/**
 * @brief The frustum over a_count lanes. a_pass[i] is 1 inside every plane or with a negative radius, else 0, and 0 for a
 * lane set in a_dead. A lane group stops testing once every lane in it is outside a plane.
 *
 * A plane is (a, b, c, d). A sphere is inside it when ((a * x + b * y) + c * z) - d >= -r. a_dead is a bit per lane, 64 to a
 * word, or null.
 */
using FrustumFn = void (*)(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, const std::uint64_t* a_dead,
    std::uint8_t* a_pass);

void FrustumAvx2(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, const std::uint64_t* a_dead,
    std::uint8_t* a_pass);
void FrustumAvx512(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, const std::uint64_t* a_dead,
    std::uint8_t* a_pass);

/**
 * @brief The same test four lanes at a time with SSE2, for any x64 CPU. It tests every plane and ignores dead lanes: the
 * caller clears those afterwards.
 */
void FrustumSse(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, std::uint8_t* a_pass);

/** @brief The widest kernels the CPU and OS support. AVX2 needs the YMM state, AVX-512 needs F, BW and the ZMM state. */
enum class Level : std::uint8_t
{
    Sse,
    Avx2,
    Avx512,
};

/** @brief The level from CPUID and XCR0, read once. */
Level Supported();

/** @brief "SSE", "AVX2" or "AVX-512". */
const char* NameOf(Level a_level);

/** @brief The wide frustum kernel for a_level, or null for Level::Sse (use FrustumSse). */
FrustumFn WideFrustum(Level a_level);

} // namespace org::cpu
