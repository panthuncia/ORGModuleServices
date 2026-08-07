#pragma once

#include <ORGModuleServices/FrameUploadArena.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace org::services {
struct StreamingUploadChunk { UploadAllocation allocation; uint64_t sourceOffset{}; };
inline std::vector<StreamingUploadChunk> AllocateStreamingUpload(FrameUploadArena& arena, std::span<const std::byte> source,
    uint64_t completionValue, uint64_t chunkSize = 4 * 1024 * 1024) {
    std::vector<StreamingUploadChunk> chunks; if (!chunkSize) return chunks;
    for (uint64_t offset = 0; offset < source.size();) {
        const uint64_t size = std::min<uint64_t>(chunkSize, source.size() - offset);
        auto allocation = arena.Allocate(size, 256, completionValue);
        std::memcpy(allocation.cpuAddress, source.data() + offset, static_cast<size_t>(size));
        chunks.push_back({ allocation, offset }); offset += size;
    }
    return chunks;
}
}
