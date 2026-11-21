#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <ORGModuleServices/PipelineRegistry.h>

namespace org {
class GraphHost;
class RecordingContext;
struct RecordingTagKey;
}

namespace org::services {

// The recording tag a graph's pipeline pin rides under (RenderGraph::SetRecordingTag). Its value is a PipelinePin.
const org::RecordingTagKey& PipelinePinTag() noexcept;

// Record time, any thread. The version of slot that recording binds: the one at its graph's pin (PipelinePinTag), or the
// newest for a recording of a graph nobody pinned. The pin keeps it alive for as long as the recording.
PipelineVersionPtr VersionFor(const LivePipelineSlot& slot, const org::RecordingContext& recording);

// The pipeline generation each epoch's graph builds pin, for a graph host.
//
// Attach adds an extension to the host that every graph build makes. It takes a pin of the registry (Desc::pin), tags the
// graph with it (PipelinePinTag), and adds no passes. A build counts once it is staged or attached; a dropped build never
// does. BuildPin and EveryEpochAt read what each epoch's attached and staged builds pin, so a host can wait for every graph
// to bind a reload's versions before it switches to them.
class PipelinePins {
public:
    // Called from the extension's build, on the building thread. When the build is handed to a host lane that stages it
    // before it attaches, it arranges for staged to run when the graph is staged and attached when it is attached (neither
    // when it is dropped), and returns true. It returns false for a build that attaches at once (the host's own build on its
    // owner thread).
    using DeferToBuild = std::function<bool(std::function<void()> staged, std::function<void()> attached)>;

    struct Desc {
        // A pin of the registry's current generation (PipelineRegistry::Pin). Called once per graph build, when its
        // extension is made.
        std::function<std::shared_ptr<const PipelinePin>()> pin;
        // Empty: every build attaches at once.
        DeferToBuild defer;
    };

    explicit PipelinePins(Desc desc);
    PipelinePins(const PipelinePins&) = delete;
    PipelinePins& operator=(const PipelinePins&) = delete;

    // Graph host setup, before any graph is built: every graph build pins the registry generation it is made at. id names
    // the extension (GraphHost::AddExtension). The pins outlive the host.
    void Attach(org::GraphHost& host, std::string id);

    // Owner thread. The registry generation epoch's build `build` pins, attached or staged. 0 when it is neither.
    uint64_t BuildPin(org::GraphHost& host, uint32_t epoch, uint64_t build);

    // Owner thread. Whether every epoch with a graph runs, or has staged, a build that pins generation or newer. why names
    // one that does not.
    bool EveryEpochAt(org::GraphHost& host, uint64_t generation, std::string& why);

private:
    class Extension;

    // The pins of one epoch's builds: the attached one and the staged one.
    struct EpochPins {
        uint64_t attachedBuild = 0;  // 0: a build that attached at once
        uint64_t attachedPin = 0;
        uint64_t stagedBuild = 0;
        uint64_t stagedPin = 0;
    };

    void NoteAttached(uint32_t epoch, uint64_t build, uint64_t pin);
    void NoteStaged(uint32_t epoch, uint64_t build, uint64_t pin);
    // The pins of epoch's attached and staged builds, as the host has them now. 0 for a build the table does not know.
    std::pair<uint64_t, uint64_t> PinsNow(org::GraphHost& host, uint32_t epoch);

    Desc desc_;
    std::mutex mutex_;  // epochs_
    std::map<uint32_t, EpochPins> epochs_;
};

}
