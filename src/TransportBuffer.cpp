#include <ORGModuleServices/TransportBuffer.h>

#include <cstdint>
#include <stdexcept>

#include "Resources/Buffers/Buffer.h"

namespace org::services {

void ReplaceTransport(TransportBuffer& transport, std::uint64_t needed, std::uint32_t stride, const GrowthPolicy& policy, const std::string& name,
    const RetireTransport& retire) {
    const std::uint64_t capacity = policy.Next(transport.capacity, needed);
    if (capacity > UINT32_MAX) throw std::length_error("EnsureTransport: " + name + " past a structured buffer's elements");
    auto buffer = Buffer::CreateUnmaterializedStructuredBuffer(static_cast<std::uint32_t>(capacity), stride, false);
    buffer->SetName(name.c_str());
    buffer->Materialize();
    if (transport.buffer && retire) retire(std::move(transport.buffer));
    transport.buffer = std::move(buffer);
    transport.capacity = capacity;
    transport.srvIndex = transport.buffer->GetSRVInfo(0).slot.index;
}

} // namespace org::services
