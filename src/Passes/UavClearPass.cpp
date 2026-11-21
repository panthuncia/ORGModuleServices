#include <ORGModuleServices/Passes/UavClearPass.h>

#include <cstring>
#include <utility>
#include <vector>

#include "Render/PassBuilders.h"
#include "Render/RunGate.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/Resource.h"

namespace org::services {

namespace {

struct ClearBindings {
    ResourceBindingToken resource;
    DeclaredViewToken shaderVisible, cpuVisible;
};

struct ClearFrame {
    std::array<float, 4> value{};
    std::array<uint32_t, 4> uintValue{};
    bool asUint = false;
    UavClearGate gate;
};

class UavClearPass final : public TypedRenderGraphPass<UavClearPass, ClearFrame, ClearBindings> {
public:
    UavClearPass(UavClearTarget target, std::array<float, 4> value, UavClearGate gate)
        : m_target(std::move(target)), m_value(value), m_gate(std::move(gate)) {}

    UavClearPass(UavClearTarget target, std::array<uint32_t, 4> value, UavClearGate gate)
        : m_target(std::move(target)), m_uintValue(value), m_asUint(true), m_gate(std::move(gate)) {}

    ClearBindings Declare(PassBuilder& builder) {
        builder.PreferQueue(QueueKind::Graphics);
        const auto use = builder.UnorderedAccessClear(m_target);
        return {use.resources.front(), use.views[0], use.views[1]};
    }

    void Revision(std::vector<uint64_t>& out) const {
        out.push_back(m_target.resolver ? reinterpret_cast<std::uintptr_t>(m_target.resolver.get()) : reinterpret_cast<std::uintptr_t>(m_target.resource.get()));
        for (const float component : m_value) {
            uint32_t bits = 0;
            std::memcpy(&bits, &component, sizeof(bits));
            out.push_back(bits);
        }
        for (const uint32_t component : m_uintValue) out.push_back(component);
        out.push_back(m_asUint ? 1 : 0);
    }

    ClearFrame Prepare(const ClearBindings&, const PassPrepareContext&) const { return {m_value, m_uintValue, m_asUint, m_gate}; }

    // Its commands depend on the run only through SkipsAtExecution, so a kept late batch keeps its list.
    static constexpr bool kRecordKeyedBySkip = true;

    // A clear whose gate entry the run left closed is not recorded.
    static bool SkipsAtExecution(const ClearBindings&, const ClearFrame& frame, const PassRecordContext& recording) {
        return frame.gate.gate && !frame.gate.gate->IsOpen(recording.Run(), frame.gate.entry);
    }

    static void Record(const ClearBindings& bindings, const ClearFrame& frame, PassRecordContext& recording) {
        rhi::UavClearInfo info{};
        info.shaderVisible = recording.View(bindings.shaderVisible);
        info.cpuVisible = recording.View(bindings.cpuVisible);
        info.resource = recording.Resolve(bindings.resource);
        if (frame.asUint)
            recording.Commands().ClearUavUint(info, rhi::UavClearUint(frame.uintValue));
        else
            recording.Commands().ClearUavFloat(info, rhi::UavClearFloat(frame.value));
    }

private:
    UavClearTarget m_target;
    std::array<float, 4> m_value{};
    std::array<uint32_t, 4> m_uintValue{};
    bool m_asUint = false;
    UavClearGate m_gate;
};

}  // namespace

std::shared_ptr<RenderPass> MakeUavClearPass(UavClearTarget target, std::array<float, 4> value, UavClearGate gate) {
    return std::make_shared<UavClearPass>(std::move(target), value, std::move(gate));
}

std::shared_ptr<RenderPass> MakeUavClearPassUint(UavClearTarget target, std::array<uint32_t, 4> value, UavClearGate gate) {
    return std::make_shared<UavClearPass>(std::move(target), value, std::move(gate));
}

}
