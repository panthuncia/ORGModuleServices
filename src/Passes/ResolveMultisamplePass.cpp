#include <ORGModuleServices/Passes/ResolveMultisamplePass.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#include <rhi_helpers.h>

#include "Render/PassBuilders.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/Resource.h"

namespace org::services {

namespace {

constexpr uint32_t kSrgbDestinationFlag = 1u;
constexpr uint32_t kGroupSize = 8;

struct ResolveBindings {
    DeclaredViewToken source;       // the multisampled texture's shader-resource view
    DeclaredViewToken destination;  // the destination mip's unordered-access view
};

struct ResolveFrame {
    std::shared_ptr<const ComputeKernel> kernel;
    uint32_t width = 0, height = 0, samples = 0, flags = 0;
};

class ResolveMultisamplePass final : public TypedRenderGraphPass<ResolveMultisamplePass, ResolveFrame, ResolveBindings> {
public:
    ResolveMultisamplePass(std::shared_ptr<const ComputeKernel> kernel, std::shared_ptr<Resource> source, std::shared_ptr<Resource> destination,
        uint32_t destinationMip)
        : m_kernel(std::move(kernel)), m_source(std::move(source)), m_destination(std::move(destination)), m_destinationMip(destinationMip) {}

    ResolveBindings Declare(PassBuilder& builder) {
        ResolveBindings bindings;
        bindings.source = builder.ShaderResource(m_source).View();
        bindings.destination = builder.UnorderedAccess(Subresources(m_destination, Mip{m_destinationMip, 1}),
            UavView{UINT32_MAX, m_destinationMip, 0}).View();
        return bindings;
    }

    void Revision(std::vector<uint64_t>& out) const {
        out.push_back(reinterpret_cast<std::uintptr_t>(m_kernel.get()));
        out.push_back(reinterpret_cast<std::uintptr_t>(m_source.get()));
        out.push_back(reinterpret_cast<std::uintptr_t>(m_destination.get()));
        out.push_back(m_destinationMip);
    }

    ResolveFrame Prepare(const ResolveBindings& bindings, const PassPrepareContext& preparation) const {
        const auto& source = preparation.Describe(bindings.source).texture;
        const auto& destination = preparation.Describe(bindings.destination).texture;
        if (source.sampleCount < 2) throw std::invalid_argument("ResolveMultisamplePass: the source is not multisampled");
        if (destination.sampleCount > 1) throw std::invalid_argument("ResolveMultisamplePass: the destination is multisampled");
        ResolveFrame frame;
        frame.kernel = m_kernel;
        frame.width = (std::min)(source.width, (std::max)(1u, destination.width >> m_destinationMip));
        frame.height = (std::min)(source.height, (std::max)(1u, destination.height >> m_destinationMip));
        frame.samples = source.sampleCount;
        if (rhi::helpers::IsSRGB(destination.format)) frame.flags |= kSrgbDestinationFlag;
        return frame;
    }

    static void Record(const ResolveBindings& bindings, const ResolveFrame& frame, PassRecordContext& recording) {
        if (!frame.kernel || !frame.width || !frame.height) return;
        const std::array<uint32_t, kResolveMultisampleConstantWords> constants{recording.Index(bindings.source),
            recording.Index(bindings.destination), frame.width, frame.height, frame.samples, frame.flags};
        frame.kernel->Dispatch(recording, constants, (frame.width + kGroupSize - 1) / kGroupSize, (frame.height + kGroupSize - 1) / kGroupSize, 1);
    }

private:
    std::shared_ptr<const ComputeKernel> m_kernel;
    std::shared_ptr<Resource> m_source;
    std::shared_ptr<Resource> m_destination;
    uint32_t m_destinationMip = 0;
};

}  // namespace

std::shared_ptr<RenderPass> MakeResolveMultisamplePass(std::shared_ptr<const ComputeKernel> kernel, std::shared_ptr<Resource> source,
    std::shared_ptr<Resource> destination, uint32_t destinationMip) {
    if (!kernel || !source || !destination) return nullptr;
    return std::make_shared<ResolveMultisamplePass>(std::move(kernel), std::move(source), std::move(destination), destinationMip);
}

}
