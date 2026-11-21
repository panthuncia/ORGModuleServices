#pragma once

#include <ORGModuleServices/Passes/ComputeKernel.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace org {
class LatchBlock;
class RenderPass;
class Resource;
}

namespace org::services {

// The shader, relative to the host's shader folder. CMake names the folder it is in ORG_MODULE_SERVICES_SHADER_DIR; the host
// ships that folder's files under "ORGModuleServices/".
inline constexpr const char* kGenerateMipsShader = "ORGModuleServices/GenerateMips.hlsl";
// Defined to 1 for the kernel of 2D arrays and cube maps.
inline constexpr const wchar_t* kGenerateMipsArrayDefine = L"ORG_GENERATE_MIPS_ARRAY";
// The shader's GenerateMipsConstants: source and destination views, their sizes, the flags.
inline constexpr uint32_t kGenerateMipsConstantWords = 7;
// The flags' sRGB bit: the texture is averaged in linear space.
inline constexpr uint32_t kGenerateMipsSrgbFlag = 1u;
inline constexpr uint32_t kGenerateMipsGroupSize = 8;

// The host's build of the shader: the kernel, and whether it was built with kGenerateMipsArrayDefine.
struct GenerateMipsProgram {
    std::shared_ptr<const ComputeKernel> kernel;
    bool array = false;
};

// The size of one mip step: the source mip and the mip it builds.
struct GenerateMipsStep {
    uint32_t sourceWidth = 0, sourceHeight = 0;
    uint32_t destinationWidth = 0, destinationHeight = 0;
};

// The steps that build every mip of a texture from mip 0. Mip sizes halve and round down, and stop at 1.
std::vector<GenerateMipsStep> PlanGenerateMips(uint32_t width, uint32_t height, uint32_t mipLevels);

// One step's group counts, then a pad to 16 bytes.
struct GenerateMipsGroups {
    uint32_t x = 0, y = 0, z = 0, pad = 0;
};

// The group counts of each step from mip 1, for a gate's latch region.
std::vector<GenerateMipsGroups> PlanGenerateMipsGroups(uint32_t width, uint32_t height, uint32_t mipLevels, uint32_t slices);

// Makes the dispatches conditional while the recording is kept. Step i reads its counts at offset + i * 16 in the latch
// region. The commit writes PlanGenerateMipsGroups there, or zeros to skip the chain.
struct GenerateMipsGate {
    std::shared_ptr<const LatchBlock> latch;
    uint32_t offset = 0;
};

// Builds mips 1 to N-1 of a 2D texture, texture array or cube map from mip 0, one dispatch per mip. Each mip is the
// area-weighted average of the one above. Every mip needs unordered access, and the format must allow typed loads. An sRGB
// texture is averaged in linear space through UNORM views. Without a gate it dispatches every execution.
// Null when the program has no kernel: the host makes the pass once its build is in.
std::shared_ptr<RenderPass> MakeGenerateMipsPass(GenerateMipsProgram program, std::shared_ptr<Resource> texture, GenerateMipsGate gate = {});

}
