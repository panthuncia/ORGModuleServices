#pragma once

#include <ORGModuleServices/Async/StateGraph.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace org::async {

/**
 * @brief Windows and gates on a graph. A window is a resource's current state made as graph work, one address per
 * resource, where a newer revision replaces an unstarted older one (the host's hooks keep the window kind coalescible).
 * A gate is a request that runs once a window reaches a revision and is GPU ready; the graph does the wake, nothing waits.
 * Each gate has an address of its own (the host keeps the gate kind exact, allowCoalescing false), released at the next
 * gate post once it ran.
 *
 * Shut the graph down before this object goes: the gate producer refers to it.
 */
template <class Binding>
class WindowGates {
public:
    using Graph = StateGraph<Binding>;
    using ArtifactKind = typename Binding::ArtifactKind;
    using TaskLane = typename Binding::TaskLane;
    using TaskDomain = typename Binding::TaskDomain;
    /** @brief Makes a window, returning what it still waits for (such as its upload), or null when it is GPU ready now. */
    using WindowBuild = std::function<std::shared_ptr<const GpuSubmissionSet>()>;

    WindowGates(ArtifactKind windowKind, ArtifactKind gateKind) : m_windowKind(windowKind), m_gateKind(gateKind) {}
    WindowGates(const WindowGates&) = delete;
    WindowGates& operator=(const WindowGates&) = delete;

    /** @brief Registers the window producer on (windowLane, windowDomain). A window's build may block, such as on file reads. */
    void RegisterWindows(Graph& graph, TaskLane lane, TaskDomain domain, std::string taskName) {
        graph.template RegisterTypedProducer<WindowInput, WindowBuilt>(m_windowKind, lane, domain, std::move(taskName),
            [](const typename Graph::ArtifactBuildContext&, std::shared_ptr<const WindowInput> input) {
                auto pending = input->build();
                return Graph::ArtifactBuildResult::Ready(ArtifactPayload::Make(std::make_shared<const WindowBuilt>()), std::move(pending));
            });
    }

    /** @brief Registers the gate producer on (lane, domain), which runs each gate's request once its window is ready. */
    void RegisterGates(Graph& graph, TaskLane lane, TaskDomain domain, std::string taskName) {
        graph.template RegisterTypedProducer<GateInput, GateRan>(m_gateKind, lane, domain, std::move(taskName),
            [this](const typename Graph::ArtifactBuildContext& context, std::shared_ptr<const GateInput> input) {
                input->run();
                {
                    std::scoped_lock lock(m_mutex);
                    m_ran.push_back(context.key);
                }
                return Graph::ArtifactBuildResult::Ready(ArtifactPayload::Make(std::make_shared<const GateRan>()));
            });
    }

    /** @brief Any thread, lock-free. Posts the window of resource `id` at `revision`, replacing an unstarted older one. */
    bool PostWindow(Graph& graph, std::uint64_t id, std::uint64_t revision, WindowBuild build) {
        typename Graph::ArtifactIntent intent;
        intent.key = typename Graph::ArtifactKey{ m_windowKind, id, 0 };
        intent.desiredRevision = revision;
        intent.input = ArtifactPayload::Make(std::make_shared<const WindowInput>(WindowInput{ std::move(build) }));
        intent.requestFingerprint = revision;
        return static_cast<bool>(graph.PostRequest(std::move(intent), true));
    }

    /** @brief The resource is gone, so its window's address goes too. */
    void RetireWindow(Graph& graph, std::uint64_t id) { graph.Release(typename Graph::ArtifactKey{ m_windowKind, id, 0 }); }

    /**
     * @brief Runs `run` on the gate lane once resource `id`'s window reaches `revision` and is GPU ready. Releases the gates
     * that ran since the last post. False when the graph is shutting down, and then `run` never runs.
     */
    bool PostGate(Graph& graph, std::uint64_t id, std::uint64_t revision, std::function<void()> run) {
        std::vector<typename Graph::ArtifactKey> ran;
        {
            std::scoped_lock lock(m_mutex);
            ran.swap(m_ran);
        }
        if (!ran.empty()) graph.ReleaseBatch(ran);
        typename Graph::ArtifactIntent intent;
        intent.key = typename Graph::ArtifactKey{ m_gateKind, m_ids.fetch_add(1, std::memory_order_relaxed) + 1, 0 };
        intent.desiredRevision = 1;
        intent.requirements.push_back(ReadyGateAtLeast(typename Graph::ArtifactKey{ m_windowKind, id, 0 }, revision, ArtifactReadiness::GpuReady));
        intent.input = ArtifactPayload::Make(std::make_shared<const GateInput>(GateInput{ std::move(run) }));
        intent.requestFingerprint = intent.key.primaryID;
        return static_cast<bool>(graph.PostRequest(std::move(intent), false));
    }

private:
    struct WindowInput {
        WindowBuild build;
    };
    struct WindowBuilt {
    };
    struct GateInput {
        std::function<void()> run;
    };
    struct GateRan {
        std::uint64_t id = 0;
    };

    ArtifactKind m_windowKind;
    ArtifactKind m_gateKind;
    std::atomic<std::uint64_t> m_ids{0};
    std::mutex m_mutex;
    std::vector<typename Graph::ArtifactKey> m_ran;  // gates that ran, released at the next post
};

} // namespace org::async
