#include <ORGModuleServices/Async/StateGraph.h>
#include <ORGModuleServices/Async/SuspensionIdentity.h>

#include <algorithm>
#include <cassert>
#include <deque>
#include <thread>

// Deterministic test scheduler: dispatch only enqueues, Wait executes queued work.
// Production hosts supply their own concurrent executor through the same interface.
class TestScheduler final : public org::async::GraphScheduler {
    struct TestScope final : org::async::TaskScope, std::enable_shared_from_this<TestScope> {
        const TestScheduler* owner;
        bool cancelled = false;
        mutable std::deque<std::pair<std::chrono::steady_clock::time_point, Task>> queue;
        explicit TestScope(const TestScheduler* scheduler) : owner(scheduler) {}
        bool StopRequested() const noexcept override { return cancelled; }
        void Cancel() noexcept override { cancelled = true; }
        void Wait() const override {
            while (!queue.empty()) {
                const auto next = std::min_element(queue.begin(), queue.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });
                auto work = std::move(*next);
                queue.erase(next);
                if (cancelled) continue;
                std::this_thread::sleep_until(work.first);
                work.second({[self = shared_from_this()] { return self->cancelled; }});
            }
        }
    };
public:
    org::async::Scope CreateScope(std::string_view) override { return std::make_shared<TestScope>(this); }
    bool Dispatch(const org::async::Scope& scope, org::async::TaskClass cls,
        org::async::TaskDispatch, std::string_view, Task task, org::async::TaskTraceMetadata) override {
        return DispatchAfter(scope, {}, cls, {}, std::move(task));
    }
    bool DispatchAfter(const org::async::Scope& scope, std::chrono::steady_clock::duration delay,
        org::async::TaskClass cls, std::string_view, Task task) override {
        const auto own = std::dynamic_pointer_cast<TestScope>(scope);
        if (!own || own->owner != this || own->cancelled || cls.lane || cls.domain) return false;
        own->queue.emplace_back(std::chrono::steady_clock::now() + delay, std::move(task));
        return true;
    }
};

int main() {
    using Graph = org::async::AsyncStateGraph;
    using Result = Graph::ArtifactBuildResult;
    using Context = Graph::ArtifactBuildContext;
    using org::async::ArtifactReadiness;
    auto scheduler = std::make_shared<TestScheduler>();
    org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> hooks;
    hooks.artifactPolicies[2].allowCoalescing = false;
    Graph graph(scheduler, "Standalone", hooks);
    unsigned groupBuilds = 0;
    Graph::ArtifactProducerRegistration group;
    group.producer = [&](const Context& context) {
        ++groupBuilds;
        return Result::Ready(org::async::ArtifactPayload::Make(std::make_shared<const std::uint64_t>(context.revision)));
    };
    graph.RegisterProducer(0, group);
    graph.RegisterProducer(2, group);
    const Graph::ArtifactKey key{0, 1, 0};
    const auto first = graph.PostRequest({key, 1}, false);
    assert(first && first.Handle());
    graph.WaitIdle();
    assert(graph.Snapshot(first.version).readiness == ArtifactReadiness::GpuReady);
    assert(*graph.Snapshot(first.version).payload.Get<std::uint64_t>() == 1);

    Graph::ArtifactProducerRegistration dependent;
    dependent.producer = [&](const Context& context) {
        const auto dependency = context.Dependency<std::uint64_t>(key);
        assert(dependency && dependency.Version() == first.version);
        return Result::Ready(dependency.payload ? org::async::ArtifactPayload::Make(dependency.payload) : org::async::ArtifactPayload{});
    };
    graph.RegisterProducer(1, dependent);
    const Graph::ArtifactKey consumer{1, 2, 0};
    const auto dependentVersion = graph.PostRequest({consumer, 1, {org::async::Exact(first.Handle())}}, false);
    graph.WaitIdle();
    bool reached = false;
    auto waiter = graph.AwaitExact(dependentVersion.Handle(), ArtifactReadiness::GpuReady, 0, 0,
        [&](const auto& snapshot) { reached = snapshot.Version() == dependentVersion.version; },
        [](const auto&) { assert(false); });
    graph.WaitIdle();
    assert(reached);

    const unsigned before = groupBuilds;
    std::vector<Graph::ArtifactIntent> intents;
    for (std::uint64_t revision = 2; revision != 52; ++revision) intents.push_back({key, revision});
    graph.PostIntents(std::move(intents));
    graph.WaitIdle();
    assert(graph.Snapshot(key).revision == 51 && groupBuilds < before + 50);
    assert(*graph.Snapshot(first.version).payload.Get<std::uint64_t>() == 1); // exact lease survives replacement
    const Graph::ArtifactKey lifecycle{2, 3, 0};
    graph.PostIntents({{lifecycle, 1}, {lifecycle, 2}, {lifecycle, 3}});
    graph.WaitIdle();
    assert(graph.Snapshot(lifecycle).revision == 3);
    assert(!graph.PostRequest({lifecycle, 4}, true));

    Graph::ArtifactProducerRegistration failing;
    failing.producer = [](const Context&) { return Result::Failure("expected failure"); };
    graph.RegisterProducer(3, failing);
    const auto failed = graph.PostRequest({{3, 4, 0}, 1}, false);
    bool terminal = false;
    auto failureWaiter = graph.AwaitExact(failed.Handle(), ArtifactReadiness::GpuReady, 0, 0,
        [](const auto&) { assert(false); }, [&](const auto& failure) {
            terminal = failure.version == failed.version && failure.readiness == ArtifactReadiness::Failed;
        });
    graph.WaitIdle();
    assert(terminal);
    assert(graph.Diagnose({3, 4, 0}).error == "expected failure");
    graph.Cancel(key);
    graph.WaitIdle();
    assert(graph.Snapshot(key).readiness == ArtifactReadiness::Cancelled);
    graph.Shutdown();
    assert(!graph.PostRequest({key, 52}, false));
}
