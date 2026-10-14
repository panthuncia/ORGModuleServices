#pragma once

#include <ORGModuleServices/GrowthPolicy.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace org {
class Buffer;
}

namespace org::services {

/**
 * @brief A frame-transport buffer: a structured buffer a frame's producer fills and the frame's work reads, replaced by hand when it
 * must hold more (never versioned, never named by a revision: its readers are handed it by value). A replacement holds nothing.
 */
struct TransportBuffer {
    std::shared_ptr<Buffer> buffer;
    std::uint64_t capacity = 0;  // elements
    std::uint32_t srvIndex = 0;  // the buffer's SRV slot
};

/** @brief Where a replaced transport buffer goes: kept until the frames that read it retire, released off the render thread. */
using RetireTransport = std::function<void(std::shared_ptr<Buffer>&&)>;

/**
 * @brief Owner thread: replaces the buffer with a new one at `policy.Next(capacity, needed)` elements of `stride` bytes, named
 * `name`; the old one goes to `retire`. The new one holds nothing. Throws when the capacity does not fit a structured buffer.
 */
void ReplaceTransport(TransportBuffer& transport, std::uint64_t needed, std::uint32_t stride, const GrowthPolicy& policy, const std::string& name,
    const RetireTransport& retire);

/**
 * @brief Owner thread: room for `needed` elements; ReplaceTransport when the buffer is missing or too small, its name made by
 * `name()` only then. True when it was replaced.
 */
template <class Name, class Retire>
bool EnsureTransport(TransportBuffer& transport, std::uint64_t needed, std::uint32_t stride, const GrowthPolicy& policy, const Name& name,
    const Retire& retire) {
    if (transport.buffer && transport.capacity >= needed) return false;
    ReplaceTransport(transport, needed, stride, policy, name(), retire);
    return true;
}

} // namespace org::services
