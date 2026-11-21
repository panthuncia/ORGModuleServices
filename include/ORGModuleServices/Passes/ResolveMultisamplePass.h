#pragma once

#include <ORGModuleServices/Passes/ComputeKernel.h>

#include <cstdint>
#include <memory>

namespace org {
class RenderPass;
class Resource;
}

namespace org::services {

// The shader, relative to the host's shader folder, as for kGenerateMipsShader.
inline constexpr const char* kResolveMultisampleShader = "ORGModuleServices/ResolveMultisample.hlsl";
// The shader's ResolveConstants: source and destination views, the size, the sample count, the flags.
inline constexpr uint32_t kResolveMultisampleConstantWords = 6;

// Resolves a multisampled 2D texture into a single-sampled one, averaging each texel's samples, with the host's build of
// kResolveMultisampleShader. It matches a hardware resolve for float, UNORM and SNORM formats. The source needs a
// shader-resource view and the destination unordered access. The resolved area is the smaller of the two sizes. An sRGB
// source is averaged in linear space; an sRGB destination needs a UNORM unordered-access view.
// Null without a kernel: the host makes the pass once its build is in.
std::shared_ptr<RenderPass> MakeResolveMultisamplePass(std::shared_ptr<const ComputeKernel> kernel, std::shared_ptr<Resource> source,
    std::shared_ptr<Resource> destination, uint32_t destinationMip = 0);

}
