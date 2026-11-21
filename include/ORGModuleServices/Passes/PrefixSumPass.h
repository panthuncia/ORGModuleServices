#pragma once

#include <ORGModuleServices/Passes/ComputeKernel.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace org {
class Buffer;
class RenderPass;
struct FramePreparationContext;
using PassPrepareContext = FramePreparationContext;
}

namespace org::services {

// The shader, relative to the host's shader folder, as for kGenerateMipsShader. It has two entry points, one per kernel.
inline constexpr const char* kPrefixSumShader = "ORGModuleServices/PrefixSum.hlsl";
inline constexpr const wchar_t* kPrefixSumBlockScanEntry = L"BlockScan";
inline constexpr const wchar_t* kPrefixSumBlockOffsetsEntry = L"BlockOffsets";
// The shader's PrefixSumConstants: four buffer views, the element and block counts, the flags and a pad.
inline constexpr uint32_t kPrefixSumConstantWords = 8;
// Elements one group scans. The second kernel scans the block sums in one group, which bounds a scan.
inline constexpr uint32_t kPrefixSumBlockSize = 1024;
inline constexpr uint32_t kPrefixSumMaxElements = kPrefixSumBlockSize * kPrefixSumBlockSize;

constexpr uint32_t PrefixSumBlocks(uint32_t elements) { return (elements + kPrefixSumBlockSize - 1) / kPrefixSumBlockSize; }

// The host's builds of the two entry points. One set can serve every prefix sum pass.
struct PrefixSumPrograms {
    std::shared_ptr<const ComputeKernel> blockScan, blockOffsets;
};

struct PrefixSumDesc {
    std::shared_ptr<const PrefixSumPrograms> programs;
    std::shared_ptr<Buffer> counts;     // uint[elements]
    std::shared_ptr<Buffer> offsets;    // uint[elements], counts' exclusive prefix sum
    std::shared_ptr<Buffer> blockSums;  // uint[PrefixSumBlocks(elements)], scratch
    std::shared_ptr<Buffer> total;      // optional uint, the sum of every count
    uint32_t elements = 0;              // at most kPrefixSumMaxElements
    // Zero the counts as they are read, so the next accumulation needs no clear. The counts must start at zero, and every
    // accumulation must be followed by this pass.
    bool clearCounts = false;
    // Whether an execution scans; null for every one. It may run on a worker, so it must read state published for preparation.
    std::function<bool(const PassPrepareContext&)> active;
    // Added to the pass's revision: anything `active` depends on beyond the preparation's host data.
    std::function<void(std::vector<uint64_t>&)> revision;
};

// The exclusive prefix sum of a uint buffer, as one pass of two dispatches with a full barrier between them. A two-level
// block scan. The usual pairing is a counting sort: a pass counts keys with atomics and keeps each item's rank, this pass
// turns the counts into each key's first slot, and a scatter (Passes/SortScatter.h) writes item i to offsets[key] + rank.
// The owner creates and registers the buffers with unordered access, and the graph orders the passes.
// Records nothing for an execution without programs, with no elements or more than kPrefixSumMaxElements.
std::shared_ptr<RenderPass> MakePrefixSumPass(PrefixSumDesc desc);

}
