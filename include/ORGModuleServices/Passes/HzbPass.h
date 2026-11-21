#pragma once

#include <ORGModuleServices/Passes/ComputeKernel.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "Render/ResourceUseDeclaration.h"
#include "Resources/TextureDescription.h"

namespace org {
class Buffer;
class PixelBuffer;
class RenderPassBuilder;
class RenderPass;
struct FramePreparationContext;
using PassPrepareContext = FramePreparationContext;
}

namespace org::services {

// The shader, relative to the host's shader folder, as for kGenerateMipsShader. It includes ffx_a.h and ffx_spd.h from the
// same folder (AMD FidelityFX, MIT; FidelityFX-LICENSE.txt there). Shaders that test against the HZB include HzbTest.hlsli.
inline constexpr const char* kHzbShader = "ORGModuleServices/Hzb.hlsl";
// Defined to 1 for the kernel of the min reduction.
inline constexpr const wchar_t* kHzbReduceMinDefine = L"ORG_HZB_REDUCE_MIN";
inline constexpr uint32_t kHzbConstantWords = 24;
// Levels one dispatch writes, enough to take 4096 texels down to one.
inline constexpr uint32_t kHzbDispatchMips = 12;
// Each group reduces a tile of this many texels a side.
inline constexpr uint32_t kHzbTile = 64;

// The shader's push constants. The indices are filled in at recording.
struct HzbConstants {
    uint32_t sourceIndex;
    uint32_t fromDepth;
    uint32_t validSize[2];   // source texels that hold depth
    uint32_t domainSize[2];  // power-of-two extent; reads past validSize return farValue
    uint32_t targetSize[2];  // size of the first written level
    uint32_t mips;
    uint32_t workGroups;
    uint32_t counterIndex;
    float farValue;
    uint32_t targetIndices[kHzbDispatchMips];
};
static_assert(sizeof(HzbConstants) / 4 == kHzbConstantWords);

// Each level holds the farthest depth under it: the maximum for depth that grows with distance, the minimum for reversed depth.
enum class HzbReduction : uint8_t { Max, Min };

// The host's build of the shader: the kernel, and the reduction it was built for (kHzbReduceMinDefine for Min).
struct HzbProgram {
    std::shared_ptr<const ComputeKernel> kernel;
    HzbReduction reduction = HzbReduction::Max;
};

// The pyramid's mip 0 size and level count.
struct HzbLayout {
    uint32_t width = 0, height = 0, mips = 0;
};

// The pyramid for a depth size. Mip 0 is half the next power of two on each side, so log2 of a screen extent picks a mip, and
// the levels go down to one texel.
HzbLayout PlanHzbLayout(uint32_t depthWidth, uint32_t depthHeight);

// The pyramid texture's description: one R32_Float level per mip, with UAVs and SRVs.
TextureDescription HzbTextureDescription(const HzbLayout& layout);

// One dispatch of a build. Up to 4096 texels a side needs one; a larger domain splits the chain.
struct HzbDispatch {
    HzbConstants constants{};
    uint32_t level = 0;  // the first written level; at 0 the source is the depth
    uint32_t groupsX = 0, groupsY = 0;
};

// The dispatches that build every level of the layout from validWidth x validHeight depth texels. Depth past the layout's
// padded domain is not read.
std::vector<HzbDispatch> PlanHzbDispatches(const HzbLayout& layout, uint32_t validWidth, uint32_t validHeight, float farValue);

// The depth texels one execution reduces. A zero size skips the execution.
struct HzbSource {
    uint32_t width = 0, height = 0;
};

struct HzbDesc {
    HzbProgram program;
    // The depth texture, read through an SRV. Or declareDepth, for a depth the graph declares some other way (a versioned
    // texture, say); it must return the SRV's view and wins when set.
    std::shared_ptr<PixelBuffer> depth;
    std::function<DeclaredViewToken(RenderPassBuilder&)> declareDepth;
    // The pyramid (HzbTextureDescription(layout)), and a one-uint counter with unordered access. The counter must be zero
    // before the first execution; each dispatch's last group zeroes it again.
    std::shared_ptr<PixelBuffer> hzb;
    std::shared_ptr<Buffer> counter;
    HzbLayout layout;
    // What padding inside the domain loads as: the far plane's depth, so padding never occludes.
    float farValue = 1.0f;
    // Each execution's depth extent; null for the layout's whole domain. It may run on a worker, so it must read state
    // published for preparation.
    std::function<HzbSource(const PassPrepareContext&)> source;
    // Added to the pass's revision: anything `source` depends on beyond the preparation's host data.
    std::function<void(std::vector<uint64_t>&)> revision;
};

// Builds the HZB from the depth with FidelityFX SPD, with the program's reduction: one dispatch per kHzbDispatchMips levels,
// with a full barrier between dispatches. Null without a depth, the pyramid or the counter. Without a kernel the pass
// declares its resources and records nothing, so the host can make it before the kernel's build is in.
std::shared_ptr<RenderPass> MakeHzbPass(HzbDesc desc);

}
