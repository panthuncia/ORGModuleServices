#include <ORGModuleServices/Passes/PrefixSumPass.h>

#include <span>
#include <utility>

#include <rhi.h>

#include "Render/PassBuilders.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/Buffers/Buffer.h"

namespace org::services {

namespace {

constexpr uint32_t kNoBuffer = ~0u;
constexpr uint32_t kClearCountsFlag = 1;

// The shader's PrefixSumConstants.
struct PrefixSumConstants {
    uint32_t countsIndex;
    uint32_t offsetsIndex;
    uint32_t blockSumsIndex;
    uint32_t totalIndex;
    uint32_t elements;
    uint32_t blocks;
    uint32_t flags;
    uint32_t padding;
};
static_assert(sizeof(PrefixSumConstants) / 4 == kPrefixSumConstantWords);

struct PrefixSumBindings {
    DeclaredViewToken counts, offsets, blockSums, total;
};

struct PrefixSumFrame {
    std::shared_ptr<const PrefixSumPrograms> programs;
    PrefixSumConstants constants{};
};

class PrefixSumPass final : public TypedRenderGraphPass<PrefixSumPass, PrefixSumFrame, PrefixSumBindings> {
public:
    explicit PrefixSumPass(PrefixSumDesc desc) : m_desc(std::move(desc)) {}

    PrefixSumBindings Declare(PassBuilder& builder) {
        builder.PreferQueue(QueueKind::Graphics);
        PrefixSumBindings bindings{};
        bindings.counts = m_desc.clearCounts ? builder.UnorderedAccess(m_desc.counts).View() : builder.ShaderResource(m_desc.counts).View();
        bindings.offsets = builder.UnorderedAccess(m_desc.offsets).View();
        bindings.blockSums = builder.UnorderedAccess(m_desc.blockSums).View();
        if (m_desc.total) bindings.total = builder.UnorderedAccess(m_desc.total).View();
        return bindings;
    }

    void Revision(std::vector<uint64_t>& out) const {
        if (m_desc.revision) m_desc.revision(out);
    }

    PrefixSumFrame Prepare(const PrefixSumBindings&, const PassPrepareContext& preparation) const {
        PrefixSumFrame frame{};
        if (!m_desc.programs || !m_desc.programs->blockScan || !m_desc.programs->blockOffsets || !m_desc.elements ||
            m_desc.elements > kPrefixSumMaxElements || (m_desc.active && !m_desc.active(preparation)))
            return frame;
        frame.programs = m_desc.programs;
        frame.constants.elements = m_desc.elements;
        frame.constants.blocks = PrefixSumBlocks(m_desc.elements);
        frame.constants.flags = m_desc.clearCounts ? kClearCountsFlag : 0u;
        return frame;
    }

    static void Record(const PrefixSumBindings& bindings, const PrefixSumFrame& frame, PassRecordContext& recording) {
        if (!frame.programs) return;
        auto constants = frame.constants;
        constants.countsIndex = recording.Index(bindings.counts);
        constants.offsetsIndex = recording.Index(bindings.offsets);
        constants.blockSumsIndex = recording.Index(bindings.blockSums);
        constants.totalIndex = bindings.total.layout ? recording.Index(bindings.total) : kNoBuffer;
        const std::span<const uint32_t> words(reinterpret_cast<const uint32_t*>(&constants), kPrefixSumConstantWords);
        frame.programs->blockScan->Dispatch(recording, words, constants.blocks, 1, 1);
        // One pass with a barrier rather than two passes, since the second dispatch reads and writes the offsets the first wrote.
        const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
        recording.Commands().Barriers(rhi::BarrierBatch{{}, {}, {&global, 1}});
        frame.programs->blockOffsets->Dispatch(recording, words, 1, 1, 1);
    }

private:
    PrefixSumDesc m_desc;
};

}  // namespace

std::shared_ptr<RenderPass> MakePrefixSumPass(PrefixSumDesc desc) {
    return std::make_shared<PrefixSumPass>(std::move(desc));
}

}
