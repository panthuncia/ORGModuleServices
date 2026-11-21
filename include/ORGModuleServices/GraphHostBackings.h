#pragma once

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/ElasticCapacity.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace org {
class GraphHost;
}

namespace org::services {

/** @brief How a service reaches the graph host now: null while there is none (before it is made, or after it went). */
using GraphHostAccess = std::function<const GraphHost*()>;

// Default BackingPool hooks for a graph host: backings are made off the host's thread and released ones are destroyed on the
// host's cleanup lane. Spare, copy and shrink defaults for the pools follow.

/** @brief The device address of `buffer` on the host's device, or 0 without a host. */
[[nodiscard]] std::uint64_t GraphHostDeviceAddress(const GraphHostAccess& host, const Buffer& buffer);

/**
 * @brief BackingPoolHooks::allocate. Makes and materializes a buffer without an ECS entity, since an acquirer off the host's
 * thread may not touch the host's world. Copies `request.like`'s kind when set. Throws when it cannot be made.
 */
[[nodiscard]] BackingAllocation AllocateGraphHostBacking(const GraphHostAccess& host, const BackingRequest& request);

/**
 * @brief BackingPoolHooks::retire. Destroys a released buffer on the host's cleanup lane, never on the releasing thread. Without a
 * host (at process exit) the buffer drops here. Lock-free.
 */
void RetireGraphHostBacking(const GraphHostAccess& host, std::shared_ptr<Buffer> buffer);

/** @brief Hooks with allocate and retire set to the two above. */
[[nodiscard]] BackingPoolHooks GraphHostBackingHooks(GraphHostAccess host);

/**
 * @brief Config::spareFree for pools that publish about once a frame: one free backing per frame in flight but the newest, which
 * is still bound when the next is acquired. With fewer, the pool releases a surplus backing and remakes it a frame later.
 */
[[nodiscard]] constexpr std::uint32_t PerFrameSpares(std::uint32_t framesInFlight) noexcept {
    return framesInFlight ? framesInFlight - 1 : 0;
}

/**
 * @brief How many copies a per-frame pool keeps of a class: `asked`, at least 2 and at most `framesInFlight`. A producer reuses the
 * copy the oldest in-flight frame bound, and its uploads wait on the GPU (Config::reuseBound).
 */
[[nodiscard]] constexpr std::uint32_t PerFrameCopies(std::uint32_t asked, std::uint32_t framesInFlight) noexcept {
    return std::clamp(asked, 2u, (std::max)(framesInFlight, 2u));
}

/** @brief When growable buffers give memory back. */
enum class BackingShrink : std::uint8_t {
    Settled,  // once a buffer was asked at most a quarter of its capacity for 5 s (two asks)
    Never,    // grow-only
    Eager,    // at the first such ask
};

/** @brief The shrink policy for `mode`. */
[[nodiscard]] constexpr ShrinkPolicy BackingShrinkPolicy(BackingShrink mode) noexcept {
    switch (mode) {
    case BackingShrink::Never: return ShrinkPolicy::Never();
    case BackingShrink::Eager: return ShrinkPolicy::WhenBelow(25, std::chrono::milliseconds(0), 1);
    case BackingShrink::Settled: break;
    }
    return ShrinkPolicy::WhenBelow(25, std::chrono::milliseconds(5000), 2);
}

} // namespace org::services
