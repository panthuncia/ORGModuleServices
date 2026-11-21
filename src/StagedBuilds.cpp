#include <ORGModuleServices/StagedBuilds.h>

#include <OpenRenderGraph/GraphHost.h>
#include <Resources/ResourceIdentifier.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace org::services {

namespace {
using Clock = std::chrono::steady_clock;

double MillisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// A request made off the owner thread, applied at the next poll.
struct PendingRequest {
    enum class Kind { Epoch, All, For, Change };
    Kind kind = Kind::All;
    std::uint32_t epoch = 0;
    std::string id, reason;
    std::function<void()> change;
};

struct Window {
    std::uint64_t builds = 0, synchronous = 0, attached = 0, superseded = 0, rejected = 0, failed = 0, stale = 0;
    double buildMs = 0.0, buildMaxMs = 0.0, attachMs = 0.0, attachMaxMs = 0.0;

    bool Empty() const { return !builds && !synchronous && !attached && !superseded && !rejected && !failed && !stale; }
};

} // namespace

struct StagedBuilds::Impl {
    struct Job {
        std::uint32_t epoch = 0;
        std::uint64_t group = 0;
        std::uint64_t requests = 0;    // the epoch's request count when the job was handed out
        std::uint64_t generation = 0;  // the epoch's build generation then
        std::uint64_t frame = 0;
        std::string reason;
        // Taken on the owner thread when the job is made, so a build on any thread reads the state of its hand-out.
        std::shared_ptr<const void> input;
    };

    struct Done {
        Job job;
        std::unique_ptr<org::GraphHost::EpochGraph> graph;
        BuildContext context;
        std::string error;  // a throw out of BuildEpochGraph itself
    };

    struct Group {
        std::size_t remaining = 0;
        std::vector<std::unique_ptr<Done>> done;
    };

    Desc desc;
    std::unordered_set<std::uint32_t> staged;
    std::unordered_set<std::uint32_t> laneOnly;
    // Owner thread. Run by AttachStaged.
    std::unordered_map<std::uint32_t, std::vector<std::function<void()>>> stagedAttach;
    std::function<void(std::span<const std::uint32_t>)> pollListener;  // owner thread
    std::function<std::shared_ptr<const void>()> input;                // owner thread

    // The build lane's results.
    mutable std::mutex mutex;  // finished, stop
    std::vector<std::unique_ptr<Done>> finished;
    bool stop = false;
    // Held around every build on either thread. Extensions' build-time state assumes one build at a time.
    std::mutex buildMutex;

    // Owner thread.
    std::atomic<std::thread::id> owner{std::this_thread::get_id()};  // read by Submit on any thread
    std::map<std::uint64_t, Group> groups;
    std::unordered_set<std::uint32_t> inFlight;
    std::unordered_map<std::uint32_t, std::uint64_t> failedAt;  // the request count its last build failed at
    std::unordered_map<std::uint32_t, std::uint32_t> supersededInARow;
    std::unordered_map<std::uint32_t, std::uint32_t> rejectedInARow;
    std::uint64_t groupSerial = 0;
    std::uint64_t frames = 0;
    bool steady = false;
    std::uint64_t warnings = 0;
    std::uint64_t attachLogs = 0;
    Window window;

    std::mutex pendingMutex;
    std::vector<PendingRequest> pending;

    // The build lane: one build at a time, in request order. Last, so it stops first.
    org::async::JobLane<Job> lane;

    explicit Impl(Desc a_desc) :
        desc(std::move(a_desc)), staged(desc.staged.begin(), desc.staged.end()), laneOnly(desc.laneOnly.begin(), desc.laneOnly.end()),
        lane(*desc.executor, desc.lane, [this](Job& job) { RunJob(std::move(job)); }) {}

    template <class... Args>
    void Log(BuildLogLevel level, std::format_string<Args...> format, Args&&... args) const {
        if (desc.log)
            desc.log(level, std::format(format, std::forward<Args>(args)...));
    }

    std::string Label(std::uint32_t epoch) const { return desc.label ? desc.label(epoch) : std::format("epoch {}", epoch); }

    bool OnOwner() const { return std::this_thread::get_id() == owner.load(std::memory_order_acquire); }

    void NoteRefusal(BuildRefusalSite site, org::GraphHost::AttachRefusal refusal) const {
        if (desc.observer)
            desc.observer->Refused(site, org::GraphHost::AttachRefusalName(refusal));
    }

    void NoteAttached(std::uint32_t epoch) const {
        if (desc.observer)
            desc.observer->Attached(Label(epoch));
    }

    // One queued build, on the build lane.
    void RunJob(Job job) {
        auto done = Build(std::move(job));
        std::scoped_lock lock(mutex);
        finished.push_back(std::move(done));
    }

    std::unique_ptr<Done> Build(Job job) {
        auto done = std::make_unique<Done>();
        done->job = std::move(job);
        done->context.input = done->job.input;
        std::scoped_lock lock(buildMutex);
        BuildScope scope(done->context);
        try {
            done->graph = desc.host->BuildEpochGraph(done->job.epoch, done->job.reason);
            // The host's diagnostics read the graph on the build thread, before the attach takes it.
            if (done->graph && done->graph->Built() && desc.afterBuild)
                desc.afterBuild(done->job.epoch, *done->graph->Graph());
        } catch (const std::exception& e) {
            done->error = e.what();
        } catch (...) {
            done->error = "unknown exception";
        }
        return done;
    }

    // Owner thread: the input a build made now reads.
    std::shared_ptr<const void> TakeInput() const { return input ? input() : nullptr; }

    void Apply(const PendingRequest& request) {
        auto& host = *desc.host;
        switch (request.kind) {
        case PendingRequest::Kind::Epoch:
            host.RequestRebuild(request.epoch, request.reason);
            break;
        case PendingRequest::Kind::All:
            host.RequestRebuild(request.reason);
            break;
        case PendingRequest::Kind::For:
            (void)host.RequestRebuildFor(org::ResourceIdentifier(request.id), request.reason);
            break;
        case PendingRequest::Kind::Change:
            // No build may capture what the change edits while it runs. A build handed out before is dropped at its attach
            // and built again.
            if (request.change) {
                std::scoped_lock lock(buildMutex);
                request.change();
            }
            host.RequestRebuild(request.reason);
            break;
        }
    }

    void Submit(PendingRequest request) {
        // A change always waits for the poll, since here it could meet a build this thread holds.
        if (OnOwner() && request.kind != PendingRequest::Kind::Change) {
            Apply(request);
            return;
        }
        std::scoped_lock lock(pendingMutex);
        pending.push_back(std::move(request));
    }

    void ApplyPending() {
        std::vector<PendingRequest> requests;
        {
            std::scoped_lock lock(pendingMutex);
            requests.swap(pending);
        }
        for (const auto& request : requests)
            Apply(request);
    }

    // The epoch is asked for and has a graph to keep running, or is lane-only and has no graph yet. It is not being built
    // or known to fail.
    bool Due(std::uint32_t epoch) const {
        auto& host = *desc.host;
        if (inFlight.contains(epoch))
            return false;
        if (!host.Graph(epoch)) {
            // Other epochs with no graph yet are built by the host at their first submission. A lane-only one is built here
            // once there is a build input, and its build attaches when it is staged. A failed one waits to be asked for again.
            if (!laneOnly.contains(epoch) || !input || host.EpochFailed(epoch))
                return false;
        } else if (!host.RebuildRequested(epoch)) {
            return false;
        }
        const auto failed = failedAt.find(epoch);
        return failed == failedAt.end() || failed->second != host.RebuildRequests(epoch);
    }

    Job MakeJob(std::uint32_t epoch, std::uint64_t group) const {
        auto& host = *desc.host;
        Job job;
        job.epoch = epoch;
        job.group = group;
        job.requests = host.RebuildRequests(epoch);
        job.generation = host.BuildGeneration(epoch);
        job.frame = frames;
        job.reason = host.RebuildReasons(epoch);
        if (job.reason.empty())
            job.reason = "no request named";
        job.input = TakeInput();
        return job;
    }

    void Collect() {
        std::vector<std::unique_ptr<Done>> taken;
        {
            std::scoped_lock lock(mutex);
            taken.swap(finished);
        }
        for (auto& done : taken) {
            auto& group = groups[done->job.group];
            if (group.remaining)
                --group.remaining;
            group.done.push_back(std::move(done));
        }
    }

    // Logs a failed build once per request count and keeps the old graph. notify is false for repeated host rejections.
    void Fail(Done& done, const std::string& error, bool notify = true) {
        const auto epoch = done.job.epoch;
        auto& host = *desc.host;
        ++window.failed;
        const bool logged = failedAt.contains(epoch) && failedAt[epoch] == done.job.requests;
        failedAt[epoch] = done.job.requests;
        const bool kept = host.Graph(epoch) != nullptr;
        if (!logged)
            Log(BuildLogLevel::Error,
                "[ORG] build manager: {} failed to build ({}); not built again until it is asked for again. Why it was built: {}. "
                "Error: {}",
                Label(epoch), kept ? "its previous graph keeps running" : "it has no graph", done.job.reason, error);
        if (!notify)
            return;
        if (!done.context.failed.empty()) {
            for (auto& failed : done.context.failed)
                if (failed)
                    failed(error);
        } else if (desc.onFailed) {
            desc.onFailed(epoch, kept, error);
        }
    }

    // Attaches one finished build or drops it. Returns whether it was attached or staged.
    bool Finish(Done& done) {
        auto& host = *desc.host;
        const auto epoch = done.job.epoch;
        if (!done.graph || !done.graph->Built()) {
            Fail(done, done.graph ? done.graph->Error() : done.error);
            return false;
        }
        // Read now, since an attach takes the graph.
        const double buildMs = done.graph->BuildMilliseconds();
        ++window.builds;
        window.buildMs += buildMs;
        window.buildMaxMs = (std::max)(window.buildMaxMs, buildMs);
        // Requested again since it was handed out, so drop it and rebuild.
        // After too many in a row it goes in anyway, since it is newer than the running graph.
        if (host.RebuildRequests(epoch) != done.job.requests && supersededInARow[epoch] < desc.maxSupersededInARow) {
            ++supersededInARow[epoch];
            ++window.superseded;
            return false;
        }
        const auto begin = Clock::now();
        // A staged epoch is recorded by the host's thread first, and AttachStaged attaches it later.
        // The host attaches an epoch's first build at once, since it has no graph to keep.
        if (staged.contains(epoch)) {
            const auto stagedResult = host.StageEpochGraph(done.graph);
            if (stagedResult.staged) {
                supersededInARow.erase(epoch);
                rejectedInARow.erase(epoch);
                failedAt.erase(epoch);
                for (auto& fn : done.context.staged)
                    if (fn)
                        fn();
                stagedAttach[epoch] = std::move(done.context.attached);
                Log(BuildLogLevel::Info,
                    "[ORG] build manager: {} staged as build {}: built in {:.2f} ms off the render thread, staged in {:.3f} ms, {} frames after "
                    "it was asked for; it attaches with a frame revision; why: {}",
                    Label(epoch), stagedResult.generation, buildMs, MillisecondsSince(begin), frames - done.job.frame, done.job.reason);
                return true;
            }
            if (!stagedResult.attached) {
                ++window.rejected;
                NoteRefusal(BuildRefusalSite::Staging, stagedResult.refusal);
                Log(BuildLogLevel::Info, "[ORG] build manager: {}'s build rejected at its staging ({}); built again", Label(epoch),
                    stagedResult.reason);
                host.RequestRebuild(epoch, "its last build was rejected at its staging");
                return false;
            }
        }
        const auto result = done.graph ? host.AttachEpochGraph(done.graph) : org::GraphHost::AttachResult{true, {}};
        const double attachMs = MillisecondsSince(begin);
        if (!result.attached) {
            ++window.rejected;
            NoteRefusal(BuildRefusalSite::Attach, result.refusal);
            // If no other build answered meanwhile, ask again so the next poll builds against the host as it is now.
            const bool replaced = host.BuildGeneration(epoch) != done.job.generation;
            Log(BuildLogLevel::Info, "[ORG] build manager: {}'s build rejected at the attach ({}){}", Label(epoch), result.reason,
                replaced ? "; another build replaced the graph meanwhile" : "; built again");
            if (replaced) {
                rejectedInARow.erase(epoch);
            } else if (++rejectedInARow[epoch] > desc.maxRejectedInARow) {
                rejectedInARow.erase(epoch);
                Fail(done, "rejected at the attach " + std::to_string(desc.maxRejectedInARow + 1) + " times in a row: " + result.reason, false);
            } else {
                host.RequestRebuild(epoch, "its last build was rejected at the attach");
            }
            return false;
        }
        supersededInARow.erase(epoch);
        rejectedInARow.erase(epoch);
        failedAt.erase(epoch);
        if (done.graph)
            NoteAttached(epoch);
        ++window.attached;
        window.attachMs += attachMs;
        window.attachMaxMs = (std::max)(window.attachMaxMs, attachMs);
        for (auto& attached : done.context.attached)
            if (attached)
                attached();
        if (++attachLogs <= 200 || attachLogs % 100 == 0)
            Log(BuildLogLevel::Info,
                "[ORG] build manager: {} attached{}: built in {:.2f} ms off the render thread, attached in {:.3f} ms on the render thread, "
                "{} frames after it was asked for; why: {}",
                Label(epoch), attachLogs > 200 ? std::format(" (attach {})", attachLogs) : std::string(), buildMs, attachMs,
                frames - done.job.frame, done.job.reason);
        return true;
    }

    // Attaches or stages every group whose builds have all finished. changed gets each epoch staged or attached.
    bool AttachFinished(std::vector<std::uint32_t>& changed) {
        Collect();
        bool attached = false;
        for (auto it = groups.begin(); it != groups.end();) {
            if (it->second.remaining) {
                ++it;
                continue;
            }
            for (auto& done : it->second.done) {
                inFlight.erase(done->job.epoch);
                const bool one = Finish(*done);
                attached = one || attached;
                if (one)
                    changed.push_back(done->job.epoch);
            }
            it = groups.erase(it);
        }
        return attached;
    }

    // Asks again for epochs whose alias placements are in a pool generation another build replaced.
    void RequestStale() {
        auto& host = *desc.host;
        for (const auto epoch : host.StaleAliasPlacementEpochs()) {
            if (host.RebuildRequested(epoch) || inFlight.contains(epoch))
                continue;
            host.RequestRebuild(epoch, "its alias placements are in a pool another epoch's build grew");
            ++window.stale;
        }
    }

    void WarnSynchronous(std::string_view what, std::string_view caller) {
        if (!steady)
            return;
        if (++warnings <= 50 || warnings % 100 == 0)
            Log(BuildLogLevel::Warning, "[ORG] build manager: graph built on the render thread after the {} ({}): {}; caller: {}",
                desc.steadyStateName, warnings, what, caller);
    }

    bool Settled() const {
        auto& host = *desc.host;
        for (std::size_t i = 0; i < host.GraphCount(); ++i) {
            const auto epoch = host.EpochAt(i);
            if (!laneOnly.contains(epoch))
                continue;
            if (inFlight.contains(epoch))
                return false;
            // A lane-only epoch with no graph waits for its first build, once there is a build input, unless that failed.
            if (!host.Graph(epoch)) {
                if (!input || host.EpochFailed(epoch))
                    continue;
            } else if (!host.RebuildRequested(epoch)) {
                continue;
            }
            const auto failed = failedAt.find(epoch);
            if (failed == failedAt.end() || failed->second != host.RebuildRequests(epoch))
                return false;
        }
        return true;
    }

    // Hands the due epochs to the build lane: the lane-only ones as one group, the others as another.
    void Dispatch() {
        auto& host = *desc.host;
        // Each group is attached or staged at the poll that finds all its builds finished.
        for (const bool laneOnlyGroup : {true, false}) {
            std::vector<Job> jobs;
            const auto group = ++groupSerial;
            for (std::size_t i = 0; i < host.GraphCount(); ++i) {
                const auto epoch = host.EpochAt(i);
                if (laneOnly.contains(epoch) != laneOnlyGroup || !Due(epoch))
                    continue;
                jobs.push_back(MakeJob(epoch, group));
                inFlight.insert(epoch);
            }
            if (jobs.empty())
                continue;
            groups[group].remaining = jobs.size();
            for (auto& job : jobs)
                if (!lane.Post(std::move(job)))
                    return;  // stopped: the builds are shutting down
        }
    }

    void LogWindow() {
        if (window.Empty())
            return;
        Log(BuildLogLevel::Info,
            "[ORG] build manager, last {} frames: {} built off the render thread (mean {:.2f} ms, max {:.2f} ms), {} attached (mean {:.3f} ms, "
            "max {:.3f} ms on the render thread), {} superseded, {} rejected, {} failed, {} built on the render thread, {} stale alias rebuilds",
            desc.statsFrames, window.builds, window.builds ? window.buildMs / static_cast<double>(window.builds) : 0.0, window.buildMaxMs,
            window.attached, window.attached ? window.attachMs / static_cast<double>(window.attached) : 0.0, window.attachMaxMs,
            window.superseded, window.rejected, window.failed, window.synchronous, window.stale);
        window = {};
    }

    void Stop() {
        {
            std::scoped_lock lock(mutex);
            if (stop)
                return;
            stop = true;
        }
        // Drops the queued builds and waits for a running one.
        lane.Stop();
        // Never attached, so never run. Dropped here on the owner thread.
        finished.clear();
        groups.clear();
        inFlight.clear();
    }

    // Poll's body, run through the observer.
    struct PollCall {
        Impl* impl;
        bool changed = false;
    };

    static void RunPoll(void* context) {
        auto& call = *static_cast<PollCall*>(context);
        auto& m = *call.impl;
        m.owner.store(std::this_thread::get_id(), std::memory_order_release);
        m.ApplyPending();
        std::vector<std::uint32_t> changedEpochs;
        call.changed = m.AttachFinished(changedEpochs);
        m.RequestStale();
        m.Dispatch();
        // The listener hears of every poll, since any may stage its epochs.
        if (m.pollListener)
            m.pollListener(changedEpochs);
    }
};

StagedBuilds::StagedBuilds(Desc desc) : impl_(std::make_unique<Impl>(std::move(desc))) {
    impl_->Log(BuildLogLevel::Info,
        "[ORG] build manager: structural builds run on a lane of their own and are attached at frame boundaries; {} staged and attached "
        "with a frame revision or a fragment, {} of them never built at a submission",
        impl_->staged.size(), impl_->laneOnly.size());
}

StagedBuilds::~StagedBuilds() { Shutdown(); }

void StagedBuilds::Shutdown() {
    if (impl_)
        impl_->Stop();
}

void StagedBuilds::Request(std::uint32_t epoch, std::string_view reason) {
    impl_->Submit({PendingRequest::Kind::Epoch, epoch, {}, std::string(reason)});
}

void StagedBuilds::RequestAll(std::string_view reason) { impl_->Submit({PendingRequest::Kind::All, 0, {}, std::string(reason)}); }

void StagedBuilds::RequestFor(std::string_view id, std::string_view reason) {
    impl_->Submit({PendingRequest::Kind::For, 0, std::string(id), std::string(reason)});
}

void StagedBuilds::Change(std::function<void()> change, std::string_view reason) {
    PendingRequest request{PendingRequest::Kind::Change, 0, {}, std::string(reason)};
    request.change = std::move(change);
    impl_->Submit(std::move(request));
}

std::string StagedBuilds::AttachStaged(std::span<const std::uint32_t> epochs) {
    auto& m = *impl_;
    auto& host = *m.desc.host;
    const auto begin = Clock::now();
    const auto result = host.AttachStagedEpochGraphs(epochs);
    if (!result.attached) {
        m.NoteRefusal(BuildRefusalSite::StagedSet, result.refusal);
        return result.reason.empty() ? std::string("refused") : result.reason;
    }
    const double ms = MillisecondsSince(begin);
    m.window.attachMs += ms;
    m.window.attachMaxMs = (std::max)(m.window.attachMaxMs, ms);
    for (const auto epoch : epochs) {
        m.NoteAttached(epoch);
        ++m.window.attached;
        auto deferred = std::move(m.stagedAttach[epoch]);
        m.stagedAttach.erase(epoch);
        for (auto& fn : deferred)
            if (fn)
                fn();
    }
    return {};
}

void StagedBuilds::SetPollListener(std::function<void(std::span<const std::uint32_t>)> listener) {
    impl_->pollListener = std::move(listener);
}

void StagedBuilds::SetBuildInput(std::function<std::shared_ptr<const void>()> input) { impl_->input = std::move(input); }

std::shared_ptr<const void> StagedBuilds::OwnerInput() const { return impl_->OnOwner() ? impl_->TakeInput() : nullptr; }

bool StagedBuilds::Poll(std::string_view point) {
    Impl::PollCall call{impl_.get()};
    if (impl_->desc.observer)
        impl_->desc.observer->InPoll(point, &Impl::RunPoll, &call);
    else
        Impl::RunPoll(&call);
    return call.changed;
}

bool StagedBuilds::Settled() const { return impl_->Settled(); }

bool StagedBuilds::UnstagedRebuildRequested() const {
    auto& host = *impl_->desc.host;
    for (std::size_t i = 0; i < host.GraphCount(); ++i)
        if (const auto epoch = host.EpochAt(i); host.RebuildRequested(epoch) && !impl_->staged.contains(epoch))
            return true;
    return false;
}

bool StagedBuilds::BuildSetupGraphs(std::string_view caller) {
    auto& m = *impl_;
    if (m.steady) {
        (void)Poll(caller);
        return false;
    }
    m.owner.store(std::this_thread::get_id(), std::memory_order_release);
    m.ApplyPending();
    std::scoped_lock lock(m.buildMutex);
    (void)m.desc.host->BuildIfRequested();
    return true;
}

bool StagedBuilds::Idle() const { return impl_->groups.empty(); }

void StagedBuilds::NoteFrameEnd() {
    auto& m = *impl_;
    ++m.frames;
    if (!m.steady && (!m.desc.isSteadyState || m.desc.isSteadyState())) {
        m.steady = true;
        m.Log(BuildLogLevel::Info, "[ORG] build manager: {}; from here a graph built on the render thread is logged as a warning",
            m.desc.steadyStateName);
    }
    if (m.desc.statsFrames && m.frames % m.desc.statsFrames == 0)
        m.LogWindow();
}

void StagedBuilds::WarnSynchronous(std::string_view what, std::string_view caller) { impl_->WarnSynchronous(what, caller); }

} // namespace org::services
