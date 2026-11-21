#include <ORGModuleServices/Cpu/SphereCull.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

using org::cpu::FrustumFn;
using org::cpu::Level;
using org::cpu::Spheres;

struct Batch {
    std::vector<float> x, y, z, r;
    std::vector<std::uint64_t> dead;
    std::vector<float> planes;  // four floats per plane

    Spheres Columns() const { return { x.data(), y.data(), z.data(), r.data() }; }
    const float (*Planes() const)[4] { return reinterpret_cast<const float (*)[4]>(planes.data()); }
    std::uint32_t PlaneCount() const { return static_cast<std::uint32_t>(planes.size() / 4); }
    bool Dead(std::uint32_t a_lane) const { return !dead.empty() && ((dead[a_lane >> 6] >> (a_lane & 63)) & 1u); }
};

// The test in its plain form, in the kernels' operation order.
std::uint8_t Reference(const Batch& a_batch, std::uint32_t a_lane) {
    if (a_batch.Dead(a_lane)) return 0;
    if (a_batch.r[a_lane] < 0.0f) return 1;
    for (std::uint32_t p = 0; p < a_batch.PlaneCount(); ++p) {
        const float* plane = &a_batch.planes[std::size_t(p) * 4];
        const float d = ((plane[0] * a_batch.x[a_lane] + plane[1] * a_batch.y[a_lane]) + plane[2] * a_batch.z[a_lane]) - plane[3];
        if (!(d >= -a_batch.r[a_lane])) return 0;
    }
    return 1;
}

Batch RandomBatch(std::mt19937& a_rng, std::uint32_t a_count, std::uint32_t a_planes, bool a_dead) {
    std::uniform_real_distribution<float> position(-100.0f, 100.0f), radius(-5.0f, 30.0f), normal(-1.0f, 1.0f), offset(-50.0f, 50.0f);
    Batch batch;
    batch.x.resize(a_count), batch.y.resize(a_count), batch.z.resize(a_count), batch.r.resize(a_count);
    for (std::uint32_t i = 0; i < a_count; ++i) {
        batch.x[i] = position(a_rng), batch.y[i] = position(a_rng), batch.z[i] = position(a_rng);
        batch.r[i] = (i % 13 == 5) ? 0.0f : radius(a_rng);
    }
    for (std::uint32_t p = 0; p < a_planes; ++p) {
        float n[3] = { normal(a_rng), normal(a_rng), normal(a_rng) };
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const float scale = length > 0.0f ? 1.0f / length : 0.0f;
        batch.planes.insert(batch.planes.end(), { n[0] * scale, n[1] * scale, n[2] * scale, offset(a_rng) });
    }
    if (a_dead) {
        batch.dead.resize((a_count + 63) / 64 + 1);
        for (auto& word : batch.dead) word = (std::uint64_t(a_rng()) << 32) | a_rng();
    }
    return batch;
}

// Spheres that touch a plane exactly: the distance equals -r, which counts as inside.
Batch TouchingBatch(std::mt19937& a_rng, std::uint32_t a_count) {
    Batch batch = RandomBatch(a_rng, a_count, 1, false);
    const float* plane = batch.planes.data();
    for (std::uint32_t i = 0; i < a_count; ++i) {
        volatile float d = ((plane[0] * batch.x[i] + plane[1] * batch.y[i]) + plane[2] * batch.z[i]) - plane[3];
        batch.r[i] = -d;
    }
    return batch;
}

// The SSE pass with dead lanes cleared after it, as a host does.
std::vector<std::uint8_t> RunSse(const Batch& a_batch) {
    const auto count = static_cast<std::uint32_t>(a_batch.x.size());
    std::vector<std::uint8_t> pass(count + 16, 0xCD);
    org::cpu::FrustumSse(a_batch.Columns(), count, a_batch.Planes(), a_batch.PlaneCount(), pass.data());
    for (std::uint32_t i = 0; i < count; ++i)
        if (a_batch.Dead(i)) pass[i] = 0;
    return pass;
}

std::vector<std::uint8_t> RunWide(FrustumFn a_kernel, const Batch& a_batch) {
    const auto count = static_cast<std::uint32_t>(a_batch.x.size());
    std::vector<std::uint8_t> pass(count + 16, 0xCD);
    a_kernel(a_batch.Columns(), count, a_batch.Planes(), a_batch.PlaneCount(), a_batch.dead.empty() ? nullptr : a_batch.dead.data(), pass.data());
    return pass;
}

std::vector<Level> LevelsToRun() {
    std::vector<Level> levels{ Level::Sse };
    const Level supported = org::cpu::Supported();
    if (supported == Level::Avx2 || supported == Level::Avx512) levels.push_back(Level::Avx2);
    if (supported == Level::Avx512) levels.push_back(Level::Avx512);
    return levels;
}

void Compare(const Batch& a_batch, const std::vector<Level>& a_levels) {
    const auto count = static_cast<std::uint32_t>(a_batch.x.size());
    const auto sse = RunSse(a_batch);
    for (std::uint32_t i = 0; i < count; ++i) assert(sse[i] == Reference(a_batch, i));
    // No kernel writes past the batch.
    for (std::uint32_t i = count; i < count + 16; ++i) assert(sse[i] == 0xCD);
    for (const Level level : a_levels) {
        const FrustumFn kernel = org::cpu::WideFrustum(level);
        if (!kernel) continue;
        const auto wide = RunWide(kernel, a_batch);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (wide[i] != sse[i]) {
                std::fprintf(stderr, "%s lane %u of %u, %u planes: %u, SSE %u\n", org::cpu::NameOf(level), i, count, a_batch.PlaneCount(), wide[i], sse[i]);
                assert(false);
            }
        }
        for (std::uint32_t i = count; i < count + 16; ++i) assert(wide[i] == 0xCD);
    }
}

void TestLevels() {
    const Level supported = org::cpu::Supported();
    assert(org::cpu::Supported() == supported);
    assert(org::cpu::WideFrustum(Level::Sse) == nullptr);
    assert(org::cpu::WideFrustum(Level::Avx2) == &org::cpu::FrustumAvx2);
    assert(org::cpu::WideFrustum(Level::Avx512) == &org::cpu::FrustumAvx512);
    assert(std::strcmp(org::cpu::NameOf(Level::Sse), "SSE") == 0);
    assert(std::strcmp(org::cpu::NameOf(Level::Avx2), "AVX2") == 0);
    assert(std::strcmp(org::cpu::NameOf(Level::Avx512), "AVX-512") == 0);
    std::printf("sphere cull: the CPU supports %s\n", org::cpu::NameOf(supported));
}

void TestRandomSpheres() {
    const auto levels = LevelsToRun();
    std::mt19937 rng(20261121u);
    const std::uint32_t counts[] = { 0, 1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 64, 100, 257, 1000, 4099 };
    const std::uint32_t planeCounts[] = { 0, 1, 4, 5, 6, 12 };
    for (int round = 0; round < 8; ++round)
        for (const std::uint32_t count : counts)
            for (const std::uint32_t planes : planeCounts)
                for (const bool dead : { false, true }) Compare(RandomBatch(rng, count, planes, dead), levels);
    for (const std::uint32_t count : counts) Compare(TouchingBatch(rng, count), levels);
}

void TestTouching() {
    std::mt19937 rng(7u);
    const Batch batch = TouchingBatch(rng, 40);
    const auto sse = RunSse(batch);
    for (std::uint32_t i = 0; i < 40; ++i) assert(sse[i] == 1);
}

} // namespace

int main() {
    TestLevels();
    TestTouching();
    TestRandomSpheres();
    return 0;
}
