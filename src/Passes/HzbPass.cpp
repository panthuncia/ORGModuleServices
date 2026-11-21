#include <ORGModuleServices/Passes/HzbPass.h>

#include <algorithm>
#include <span>
#include <utility>

#include <rhi.h>

#include "Render/PassBuilders.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

namespace org::services {

namespace {

struct HzbBindings {
    DeclaredViewToken depth;
    DeclaredViewToken counter;
    std::vector<DeclaredViewToken> mips;
};

struct HzbFrame {
    std::shared_ptr<const ComputeKernel> kernel;
    std::vector<HzbDispatch> dispatches;
};

class HzbPass final : public TypedRenderGraphPass<HzbPass, HzbFrame, HzbBindings> {
public:
    explicit HzbPass(HzbDesc desc) : m_desc(std::move(desc)) {}

    HzbBindings Declare(PassBuilder& builder) {
        builder.PreferQueue(QueueKind::Graphics);
        HzbBindings bindings{};
        bindings.depth = m_desc.declareDepth ? m_desc.declareDepth(builder) : builder.ShaderResource(m_desc.depth).View();
        bindings.counter = builder.UnorderedAccess(m_desc.counter).View();
        bindings.mips.reserve(m_desc.layout.mips);
        for (uint32_t mip = 0; mip < m_desc.layout.mips; ++mip)
            bindings.mips.push_back(builder.UnorderedAccess(m_desc.hzb, UavView{UINT32_MAX, mip}).View());
        return bindings;
    }

    void Revision(std::vector<uint64_t>& out) const {
        if (m_desc.revision) m_desc.revision(out);
    }

    HzbFrame Prepare(const HzbBindings&, const PassPrepareContext& preparation) const {
        HzbFrame frame{};
        if (!m_desc.program.kernel) return frame;
        HzbSource source{m_desc.layout.width * 2, m_desc.layout.height * 2};
        if (m_desc.source) source = m_desc.source(preparation);
        if (!source.width || !source.height) return frame;
        frame.kernel = m_desc.program.kernel;
        frame.dispatches = PlanHzbDispatches(m_desc.layout, source.width, source.height, m_desc.farValue);
        return frame;
    }

    static void Record(const HzbBindings& bindings, const HzbFrame& frame, PassRecordContext& recording) {
        if (!frame.kernel || frame.dispatches.empty()) return;
        const uint32_t counterIndex = recording.Index(bindings.counter);
        for (size_t i = 0; i < frame.dispatches.size(); ++i) {
            const auto& dispatch = frame.dispatches[i];
            if (i != 0) {
                // The previous dispatch's last level must be complete before this one reads it.
                const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
                recording.Commands().Barriers(rhi::BarrierBatch{{}, {}, {&global, 1}});
            }
            auto constants = dispatch.constants;
            constants.counterIndex = counterIndex;
            constants.sourceIndex = dispatch.level == 0 ? recording.Index(bindings.depth) : recording.Index(bindings.mips[dispatch.level - 1]);
            for (uint32_t m = 0; m < constants.mips; ++m)
                constants.targetIndices[m] = recording.Index(bindings.mips[dispatch.level + m]);
            frame.kernel->Dispatch(recording, std::span<const uint32_t>(reinterpret_cast<const uint32_t*>(&constants), kHzbConstantWords),
                dispatch.groupsX, dispatch.groupsY, 1);
        }
    }

private:
    HzbDesc m_desc;
};

}  // namespace

HzbLayout PlanHzbLayout(uint32_t depthWidth, uint32_t depthHeight) {
    const auto nextPowerOfTwo = [](uint32_t value) {
        uint32_t result = 1;
        while (result < value) result <<= 1;
        return result;
    };
    HzbLayout layout;
    layout.width = (std::max)(1u, nextPowerOfTwo(depthWidth) / 2);
    layout.height = (std::max)(1u, nextPowerOfTwo(depthHeight) / 2);
    layout.mips = 1;
    for (uint32_t size = (std::max)(layout.width, layout.height); size > 1; size >>= 1) ++layout.mips;
    return layout;
}

TextureDescription HzbTextureDescription(const HzbLayout& layout) {
    TextureDescription desc{};
    for (uint32_t mip = 0; mip < layout.mips; ++mip)
        desc.imageDimensions.push_back({(std::max)(1u, layout.width >> mip), (std::max)(1u, layout.height >> mip), 0, 0});
    desc.format = rhi::Format::R32_Float;
    desc.channels = 1;
    desc.hasUAV = true;
    desc.uavFormat = desc.format;
    desc.hasSRV = true;
    desc.srvFormat = desc.format;
    return desc;
}

std::vector<HzbDispatch> PlanHzbDispatches(const HzbLayout& layout, uint32_t validWidth, uint32_t validHeight, float farValue) {
    std::vector<HzbDispatch> dispatches;
    const auto levelSize = [&](uint32_t mip, uint32_t axis) { return (std::max)(1u, (axis == 0 ? layout.width : layout.height) >> mip); };
    for (uint32_t level = 0; level < layout.mips;) {
        HzbDispatch dispatch{};
        dispatch.level = level;
        auto& constants = dispatch.constants;
        constants.farValue = farValue;
        if (level == 0) {
            // Mip 0 is half the padded domain. The depth can be larger than the extent it holds.
            constants.fromDepth = 1;
            constants.domainSize[0] = layout.width * 2;
            constants.domainSize[1] = layout.height * 2;
            constants.validSize[0] = (std::min)(validWidth, constants.domainSize[0]);
            constants.validSize[1] = (std::min)(validHeight, constants.domainSize[1]);
        } else {
            constants.fromDepth = 0;
            constants.domainSize[0] = constants.validSize[0] = levelSize(level - 1, 0);
            constants.domainSize[1] = constants.validSize[1] = levelSize(level - 1, 1);
        }
        constants.targetSize[0] = levelSize(level, 0);
        constants.targetSize[1] = levelSize(level, 1);
        // A group reduces its tile through six levels. The last group goes further only when level six fits in one tile.
        const uint32_t domain = (std::max)(constants.domainSize[0], constants.domainSize[1]);
        const uint32_t reach = domain / kHzbTile <= kHzbTile ? kHzbDispatchMips : 6u;
        constants.mips = (std::min)(layout.mips - level, reach);
        dispatch.groupsX = (constants.domainSize[0] + kHzbTile - 1) / kHzbTile;
        dispatch.groupsY = (constants.domainSize[1] + kHzbTile - 1) / kHzbTile;
        constants.workGroups = dispatch.groupsX * dispatch.groupsY;
        level += constants.mips;
        dispatches.push_back(dispatch);
    }
    return dispatches;
}

std::shared_ptr<RenderPass> MakeHzbPass(HzbDesc desc) {
    if ((!desc.depth && !desc.declareDepth) || !desc.hzb || !desc.counter) return nullptr;
    return std::make_shared<HzbPass>(std::move(desc));
}

}
