#include <ORGModuleServices/GraphHostBackings.h>

#include <OpenRenderGraph/GraphHost.h>

#include "Render/Runtime/ResourceCleanupQueue.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Resource.h"

#include <cassert>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace org::services {

std::uint64_t GraphHostDeviceAddress(const GraphHostAccess& host, const Buffer& buffer) {
    const auto* graph = host ? host() : nullptr;
    if (!graph) return 0;
    auto device = graph->Device();
    return device.GetBufferDeviceAddress({const_cast<Buffer&>(buffer).GetAPIResource().GetHandle(), 0});
}

BackingAllocation AllocateGraphHostBacking(const GraphHostAccess& host, const BackingRequest& request) {
    Resource::ScopedECSRegistrationSuppression suppressECS;
    std::shared_ptr<Buffer> buffer;
    if (request.like) {
        buffer = request.like->UnmaterializedLike();
        if (request.stride)
            buffer->ResizeStructured(static_cast<std::uint32_t>(request.capacityClass));
        else
            buffer->ResizeBytes(request.capacityClass);
    } else if (request.stride) {
        if (request.capacityClass > (std::numeric_limits<std::uint32_t>::max)())
            throw std::length_error(std::string(request.name) + ": " + std::to_string(request.capacityClass) + " rows are past a structured buffer's elements");
        buffer = Buffer::CreateUnmaterializedStructuredBuffer(static_cast<std::uint32_t>(request.capacityClass), request.stride, request.flags.unorderedAccess);
    } else {
        buffer = Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, request.capacityClass, request.flags.unorderedAccess);
    }
    if (!request.name.empty()) buffer->SetName(std::string(request.name).c_str());
    buffer->Materialize();
    BackingAllocation allocation;
    allocation.address = request.flags.deviceAddress ? GraphHostDeviceAddress(host, *buffer) : 0;
    assert(allocation.address % 16 == 0);  // row and block offsets keep 16-byte alignment
    allocation.srvIndex = buffer->HasSRV() ? buffer->GetSRVInfo(0).slot.index : 0;
    allocation.buffer = std::move(buffer);
    return allocation;
}

void RetireGraphHostBacking(const GraphHostAccess& host, std::shared_ptr<Buffer> buffer) {
    if (!buffer) return;
    const auto* graph = host ? host() : nullptr;
    auto cleanup = graph ? graph->ResourceCleanup() : nullptr;
    if (!cleanup) return;  // no graph host at process exit
    // The queue destroys the buffer on its cleanup lane.
    (void)cleanup->Make<std::shared_ptr<Buffer>>(std::move(buffer));
}

BackingPoolHooks GraphHostBackingHooks(GraphHostAccess host) {
    BackingPoolHooks hooks;
    hooks.allocate = [host](const BackingRequest& request) { return AllocateGraphHostBacking(host, request); };
    hooks.retire = [host = std::move(host)](std::shared_ptr<Buffer> buffer) { RetireGraphHostBacking(host, std::move(buffer)); };
    return hooks;
}

} // namespace org::services
