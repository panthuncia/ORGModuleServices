#include <ORGModuleServices/Passes/GenerateMipsPass.h>

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include <rhi_helpers.h>

#include "Render/LatchBlock.h"
#include "Render/PassBuilders.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/GloballyIndexedResource.h"
#include "Resources/Resource.h"

namespace org::services {

namespace {

struct MipsBindings {
    std::vector<DeclaredViewToken> mips;  // a UAV of each mip, all slices
    uint32_t slices = 1;
};

struct MipsFrame {
    std::shared_ptr<const ComputeKernel> kernel;
    std::shared_ptr<const LatchBlock> latch;
    uint32_t offset = 0;
    std::vector<GenerateMipsStep> steps;
    uint32_t flags = 0;
};

class GenerateMipsPass final : public TypedRenderGraphPass<GenerateMipsPass, MipsFrame, MipsBindings> {
public:
    GenerateMipsPass(GenerateMipsProgram program, std::shared_ptr<Resource> texture, GenerateMipsGate gate)
        : m_program(std::move(program)), m_texture(std::move(texture)), m_gate(std::move(gate)) {}

    MipsBindings Declare(PassBuilder& builder) {
        builder.PreferQueue(QueueKind::Graphics);
        rhi::ResourceDesc desc{};
        if (!m_texture->TryGetRHIResourceDesc(desc) || desc.type != rhi::ResourceType::Texture2D)
            throw std::invalid_argument("GenerateMipsPass: '" + m_texture->GetName() + "' is not a 2D texture");
        if (desc.texture.sampleCount > 1)
            throw std::invalid_argument("GenerateMipsPass: '" + m_texture->GetName() + "' is multisampled");
        MipsBindings bindings;
        bindings.slices = (std::max)(1u, static_cast<uint32_t>(desc.texture.depthOrLayers));
        const bool array = bindings.slices > 1;
        if (array != m_program.array)
            throw std::invalid_argument("GenerateMipsPass: '" + m_texture->GetName() + "' needs the kernel built for " +
                (array ? "arrays" : "2D textures"));
        const uint32_t mips = desc.texture.mipLevels;
        if (mips < 2) return bindings;
        // One declaration for the whole chain. Each dispatch reads one mip and writes the next.
        std::vector<UavView> views(mips);
        for (uint32_t mip = 0; mip < mips; ++mip)
            views[mip] = {array ? static_cast<uint32_t>(UAVViewType::Texture2DArrayFull) : UINT32_MAX, mip, 0};
        bindings.mips = builder.UnorderedAccess(m_texture, std::span<const UavView>(views)).views;
        return bindings;
    }

    void Revision(std::vector<uint64_t>& out) const {
        out.push_back(reinterpret_cast<std::uintptr_t>(m_program.kernel.get()));
        out.push_back(reinterpret_cast<std::uintptr_t>(m_texture.get()));
        out.push_back(reinterpret_cast<std::uintptr_t>(m_gate.latch.get()));
        out.push_back(m_gate.offset);
    }

    MipsFrame Prepare(const MipsBindings& bindings, const PassPrepareContext& preparation) const {
        MipsFrame frame{m_program.kernel, m_gate.latch, m_gate.offset};
        if (bindings.mips.size() < 2) return frame;
        // The frame's backing may differ from the one the pass was declared with.
        const auto& desc = preparation.Describe(bindings.mips.front()).texture;
        frame.steps = PlanGenerateMips(desc.width, desc.height,
            (std::min)(static_cast<uint32_t>(desc.mipLevels), static_cast<uint32_t>(bindings.mips.size())));
        if (rhi::helpers::IsSRGB(desc.format)) frame.flags |= kGenerateMipsSrgbFlag;
        return frame;
    }

    static void Record(const MipsBindings& bindings, const MipsFrame& frame, PassRecordContext& recording) {
        if (!frame.kernel || frame.steps.empty()) return;
        for (size_t i = 0; i < frame.steps.size(); ++i) {
            const auto& step = frame.steps[i];
            // The previous dispatch wrote this one's source.
            if (i) recording.UavBarrier();
            const std::array<uint32_t, kGenerateMipsConstantWords> constants{recording.Index(bindings.mips[i]),
                recording.Index(bindings.mips[i + 1]), step.sourceWidth, step.sourceHeight, step.destinationWidth,
                step.destinationHeight, frame.flags};
            if (!frame.latch) {
                frame.kernel->Dispatch(recording, constants, (step.destinationWidth + kGenerateMipsGroupSize - 1) / kGenerateMipsGroupSize,
                    (step.destinationHeight + kGenerateMipsGroupSize - 1) / kGenerateMipsGroupSize, bindings.slices);
                continue;
            }
            const auto offset = static_cast<uint32_t>(frame.offset + i * sizeof(GenerateMipsGroups));
            frame.kernel->DispatchIndirect(recording, constants, *frame.latch, offset);
        }
    }

private:
    GenerateMipsProgram m_program;
    std::shared_ptr<Resource> m_texture;
    GenerateMipsGate m_gate;
};

}  // namespace

std::vector<GenerateMipsStep> PlanGenerateMips(uint32_t width, uint32_t height, uint32_t mipLevels) {
    std::vector<GenerateMipsStep> steps;
    if (!width || !height || mipLevels < 2) return steps;
    steps.reserve(mipLevels - 1);
    for (uint32_t mip = 1; mip < mipLevels; ++mip)
        steps.push_back({(std::max)(1u, width >> (mip - 1)), (std::max)(1u, height >> (mip - 1)), (std::max)(1u, width >> mip),
            (std::max)(1u, height >> mip)});
    return steps;
}

std::vector<GenerateMipsGroups> PlanGenerateMipsGroups(uint32_t width, uint32_t height, uint32_t mipLevels, uint32_t slices) {
    std::vector<GenerateMipsGroups> groups;
    for (const auto& step : PlanGenerateMips(width, height, mipLevels))
        groups.push_back({(step.destinationWidth + kGenerateMipsGroupSize - 1) / kGenerateMipsGroupSize,
            (step.destinationHeight + kGenerateMipsGroupSize - 1) / kGenerateMipsGroupSize, (std::max)(slices, 1u), 0});
    return groups;
}

std::shared_ptr<RenderPass> MakeGenerateMipsPass(GenerateMipsProgram program, std::shared_ptr<Resource> texture, GenerateMipsGate gate) {
    if (!program.kernel || !texture) return nullptr;
    return std::make_shared<GenerateMipsPass>(std::move(program), std::move(texture), std::move(gate));
}

}
