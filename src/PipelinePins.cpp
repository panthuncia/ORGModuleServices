#include <ORGModuleServices/PipelinePins.h>

#include <OpenRenderGraph/GraphHost.h>
#include <Render/PreparedPass.h>
#include <Render/RecordingTags.h>
#include <Render/RenderGraph/RenderGraph.h>

#include <string>
#include <utility>
#include <vector>

namespace org::services {

namespace {
const org::RecordingTagKey kPipelinePinTag{"org.services.pipeline-pin"};
}

const org::RecordingTagKey& PipelinePinTag() noexcept { return kPipelinePinTag; }

PipelineVersionPtr VersionFor(const LivePipelineSlot& slot, const org::RecordingContext& recording) {
    // Only PipelinePins tags a graph under this key, so its value is a pin.
    if (const auto* pin = static_cast<const PipelinePin*>(recording.Tag(kPipelinePinTag)))
        return slot.At(pin->generation);
    return slot.Current();
}

// The extension every graph build makes: it pins the registry generation, and its graph's recordings carry the pin. It adds
// no passes.
class PipelinePins::Extension final : public org::RenderGraph::IRenderGraphExtension {
public:
    Extension(PipelinePins& pins, org::GraphHost& host, std::shared_ptr<const PipelinePin> pin) :
        pins_(&pins), host_(&host), pin_(std::move(pin)) {}

    void PrepareForBuild(org::RenderGraph& graph) override {
        graph.SetRecordingTag(kPipelinePinTag, pin_);
        auto* pins = pins_;
        auto* host = host_;
        const uint32_t epoch = graph.PassEpochFilter();
        const uint64_t generation = pin_->generation;
        // A build on the host's lane counts once it is staged or attached. A dropped one never does.
        if (pins->desc_.defer &&
            pins->desc_.defer([pins, host, epoch, generation] { pins->NoteStaged(epoch, host->StagedGeneration(epoch), generation); },
                [pins, host, epoch, generation] { pins->NoteAttached(epoch, host->BuildGeneration(epoch), generation); }))
            return;
        // The host's own builds on the owner thread attach at once.
        pins->NoteAttached(epoch, 0, generation);
    }

    void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>&) override {}

private:
    PipelinePins* pins_;
    org::GraphHost* host_;
    std::shared_ptr<const PipelinePin> pin_;
};

PipelinePins::PipelinePins(Desc desc) : desc_(std::move(desc)) {}

void PipelinePins::Attach(org::GraphHost& host, std::string id) {
    auto* self = this;
    auto* owner = &host;
    host.AddExtension(std::move(id), [self, owner]() -> std::unique_ptr<org::RenderGraph::IRenderGraphExtension> {
        return std::make_unique<Extension>(*self, *owner, self->desc_.pin());
    });
}

void PipelinePins::NoteAttached(uint32_t epoch, uint64_t build, uint64_t pin) {
    const std::lock_guard lock(mutex_);
    auto& pins = epochs_[epoch];
    pins.attachedBuild = build;
    pins.attachedPin = pin;
}

void PipelinePins::NoteStaged(uint32_t epoch, uint64_t build, uint64_t pin) {
    const std::lock_guard lock(mutex_);
    auto& pins = epochs_[epoch];
    pins.stagedBuild = build;
    pins.stagedPin = pin;
}

std::pair<uint64_t, uint64_t> PipelinePins::PinsNow(org::GraphHost& host, uint32_t epoch) {
    const std::lock_guard lock(mutex_);
    const auto it = epochs_.find(epoch);
    if (it == epochs_.end())
        return { 0, 0 };
    const auto& pins = it->second;
    const uint64_t attached = pins.attachedBuild == 0 || pins.attachedBuild == host.BuildGeneration(epoch) ? pins.attachedPin : 0;
    const uint64_t stagedBuild = host.StagedGeneration(epoch);
    const uint64_t staged = stagedBuild != 0 && pins.stagedBuild == stagedBuild ? pins.stagedPin : 0;
    return { attached, staged };
}

uint64_t PipelinePins::BuildPin(org::GraphHost& host, uint32_t epoch, uint64_t build) {
    if (build != 0 && build == host.StagedGeneration(epoch))
        return PinsNow(host, epoch).second;
    if (build == host.BuildGeneration(epoch))
        return PinsNow(host, epoch).first;
    return 0;
}

bool PipelinePins::EveryEpochAt(org::GraphHost& host, uint64_t generation, std::string& why) {
    for (size_t i = 0; i < host.GraphCount(); ++i) {
        const auto epoch = host.EpochAt(i);
        // An epoch's first build pins the newest generation.
        if (!host.Graph(epoch))
            continue;
        const auto [attached, staged] = PinsNow(host, epoch);
        if (attached >= generation || staged >= generation)
            continue;
        why = "epoch " + std::to_string(epoch) + " runs pipeline generation " + std::to_string(attached) + " (staged: " + std::to_string(staged) + ")";
        return false;
    }
    return true;
}

}
