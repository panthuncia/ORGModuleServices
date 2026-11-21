#pragma once

// A graph host's structural builds, kept off the owner thread.
//
// Rebuild requests only mark the epoch on the host. At each frame boundary the owner thread polls, which attaches finished
// builds and hands the remaining requests to a build lane on the host's executor. The old graph runs until the new one is
// attached. Staged epochs' builds are staged, not attached: the host attaches them later as one set (AttachStaged), with
// the recordings made on them.

#include <ORGModuleServices/Async/Lanes.h>
#include <ORGModuleServices/BuildContext.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace org {
class GraphHost;
class RenderGraph;
}

namespace org::services {

enum class BuildLogLevel : std::uint8_t { Info, Warning, Error };

// Where the host refused a build.
enum class BuildRefusalSite : std::uint8_t {
    Staging,    // a finished build, at its staging
    Attach,     // a finished build, at its attach
    StagedSet,  // staged graphs, switched in as a set (AttachStaged)
};

// What StagedBuilds tells its host about polls, attaches and refusals. Every call is on the owner thread.
class BuildObserver {
public:
    virtual ~BuildObserver() = default;
    // Poll runs its body through this, so the observer can mark the scope the poll's attaches happen in. Call body(context)
    // exactly once.
    virtual void InPoll(std::string_view point, void (*body)(void*), void* context) {
        (void)point;
        body(context);
    }
    // A graph was attached for an epoch, named by its label: a finished build, or a staged graph switched in.
    virtual void Attached(std::string_view epoch) { (void)epoch; }
    // The host refused a build. reason is GraphHost::AttachRefusalName's.
    virtual void Refused(BuildRefusalSite site, std::string_view reason) {
        (void)site;
        (void)reason;
    }
};

class StagedBuilds {
public:
    struct Desc {
        org::GraphHost* host = nullptr;  // required; it outlives the builds
        // Required. Builds run one at a time, in request order, on a lane of this executor. It outlives the builds.
        org::async::Executor* executor = nullptr;
        org::async::LaneDesc lane{"graph builds"};
        // Epochs whose finished builds are staged, not attached. The host attaches them with AttachStaged.
        std::vector<std::uint32_t> staged;
        // Staged epochs that the host never builds itself at a submission. Once SetBuildInput gave an input, their first
        // build runs on the build lane too, and attaches when it is staged, since there is no graph to keep. They form their
        // own group at each poll, and Settled() reads them. They must be in staged.
        std::vector<std::uint32_t> laneOnly;
        // An epoch's name in the log lines. Empty: "epoch <n>".
        std::function<std::string(std::uint32_t)> label;
        // A failed build with no failure handler of its own (DeferToAttach) goes here. kept means the old graph still runs.
        std::function<void(std::uint32_t, bool, const std::string&)> onFailed;
        // Runs on the building thread after a build that succeeded, inside the build (DeferToAttach works there) and under
        // the build lock. For diagnostics of the graph before it is attached. A throw fails the build.
        std::function<void(std::uint32_t epoch, org::RenderGraph& graph)> afterBuild;
        // Read at each frame end until it returns true. From then a graph built on the owner thread is logged as a
        // warning (WarnSynchronous). Empty: from the first frame end.
        std::function<bool()> isSteadyState;
        // The steady state's name in the log lines.
        std::string steadyStateName = "first steady frame";
        BuildObserver* observer = nullptr;  // optional; it outlives the builds
        std::function<void(BuildLogLevel, std::string_view)> log;  // optional
        // A build superseded by a newer request this many times in a row is attached anyway, then built again.
        std::uint32_t maxSupersededInARow = 3;
        // A build rejected at the attach more than this many times in a row fails the epoch until it is asked for again.
        std::uint32_t maxRejectedInARow = 3;
        // Frames between the build statistics lines. 0: no statistics lines.
        std::uint64_t statsFrames = 3000;
    };

    explicit StagedBuilds(Desc desc);
    ~StagedBuilds();
    StagedBuilds(const StagedBuilds&) = delete;
    StagedBuilds& operator=(const StagedBuilds&) = delete;

    // Owner thread. Stops the build lane (it finishes the build it is in) and drops every build not attached.
    void Shutdown();

    // Any thread: the epoch's graph is built again. Off the owner thread the request is applied at the next poll.
    void Request(std::uint32_t epoch, std::string_view reason);
    // Any thread: every epoch's graph is built again.
    void RequestAll(std::string_view reason);
    // Any thread: the graphs that looked id up are built again (GraphHost::RequestRebuildFor).
    void RequestFor(std::string_view id, std::string_view reason);
    // Any thread. Runs change on the owner thread at the next poll while no build runs, then rebuilds every epoch. For
    // changes to what a graph captures at its build, which must not race a build in progress.
    void Change(std::function<void()> change, std::string_view reason);

    // Owner thread, at a frame boundary. Attaches the staged graphs of epochs as one step (GraphHost::AttachStagedEpochGraphs)
    // and runs their deferred work. Empty when attached, else why it was refused.
    std::string AttachStaged(std::span<const std::uint32_t> epochs);

    // Owner thread: runs after every poll, with the epochs it staged or attached. Replaces the last one.
    void SetPollListener(std::function<void(std::span<const std::uint32_t> changed)> listener);

    // Owner thread: what builds are made from. input runs on the owner thread when a build is handed out, and the build
    // carries the value to whatever thread runs it (BuildContext::input). Replaces the last one.
    void SetBuildInput(std::function<std::shared_ptr<const void>()> input);
    // The input a build made on the calling thread now reads: SetBuildInput's value on the owner thread, else null.
    std::shared_ptr<const void> OwnerInput() const;

    // Owner thread, at a frame boundary. Attaches or stages finished builds and hands every other request to the build
    // lane. Returns whether a graph was attached or staged. point names the caller to the observer.
    bool Poll(std::string_view point);
    // Owner thread. Whether no lane-only epoch is requested or building, and each has a graph once there is a build input.
    bool Settled() const;
    // Owner thread. Whether a rebuild was asked for an epoch whose builds attach at once.
    bool UnstagedRebuildRequested() const;
    // Owner thread, at setup. Builds every request now (GraphHost::BuildIfRequested) before the steady state, else acts as
    // Poll. Returns whether it built at setup.
    bool BuildSetupGraphs(std::string_view caller);

    // Owner thread: no build queued, running or waiting to be attached.
    bool Idle() const;
    // Owner thread, at every frame's end: the steady-state latch and the periodic statistics line.
    void NoteFrameEnd();
    // Logs a graph built on the owner thread after the steady state began, rate-limited.
    void WarnSynchronous(std::string_view what, std::string_view caller);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace org::services
