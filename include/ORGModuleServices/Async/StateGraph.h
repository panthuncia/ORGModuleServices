#pragma once

#include <ORGModuleServices/Async/StateGraphTypes.h>
#include <ORGModuleServices/Async/ArtifactKindPolicy.h>
#include <ORGModuleServices/Async/GraphSchedulingLayout.h>
#include <span>
#include <string_view>

namespace org::async {

template <class Binding> class StateGraph;
template <class Binding>
struct StateGraphHooks {
    using ArtifactKey = typename Binding::ArtifactKey;
    using AsyncStateGraphTraceConfig = typename Binding::AsyncStateGraphTraceConfig;
    using GraphTraceSession = typename Binding::GraphTraceSession;

    // Fixed for the graph lifetime; the coordinator and posted-request callers
    // may read these without synchronization. Generic defaults retain exact
    // content even for replaceable intents. The renderer installs its legacy
    // policy explicitly in MakeRendererGraphHooks().
    std::array<org::async::ArtifactKindPolicy, Binding::kArtifactKindCount> artifactPolicies{};
    org::async::GraphSchedulingLayout scheduling{};
    std::vector<org::async::TaskDispatch> producerDispatch{org::async::TaskDispatch::Cpu};
    // Runtime callbacks must not throw. Trace-session creation runs on the
    // explicit StartTrace control path and may fail like any other allocation.
    // The host interprets its payloads and wakes its resource brokers.
    std::function<void()> versionsRetired;
    std::function<std::shared_ptr<GraphTraceSession>(AsyncStateGraphTraceConfig)> createTraceSession;
    std::function<void(StateGraph<Binding>&, ArtifactKey, std::uint64_t, std::uint64_t,
        const ArtifactPayload&)> traceAcceptedInput;
};

template <class Binding>
class StateGraph {
public:
    using ArtifactAddress = typename Binding::ArtifactAddress;
    using ArtifactKey = typename Binding::ArtifactKey;
    using ArtifactVersionID = typename Binding::ArtifactVersionID;
    using ArtifactVersionHandle = typename Binding::ArtifactVersionHandle;
    using ArtifactRequirement = typename Binding::ArtifactRequirement;
    using HandleRequirement = typename Binding::HandleRequirement;
    using Require = typename Binding::Require;
    using Optional = typename Binding::Optional;
    using FirstReady = typename Binding::FirstReady;
    using AnyReady = typename Binding::AnyReady;
    using DependencyExpression = typename Binding::DependencyExpression;
    using ArtifactSnapshot = typename Binding::ArtifactSnapshot;
    using ArtifactBuildContext = typename Binding::ArtifactBuildContext;
    using ArtifactRequestResult = typename Binding::ArtifactRequestResult;
    using ArtifactRequest = typename Binding::ArtifactRequest;
    using ArtifactIntent = typename Binding::ArtifactIntent;
    using ArtifactObservation = typename Binding::ArtifactObservation;
    using ArtifactTermination = typename Binding::ArtifactTermination;
    using ArtifactAwaiter = typename Binding::ArtifactAwaiter;
    using ArtifactSuspension = typename Binding::ArtifactSuspension;
    using ArtifactDiagnostic = typename Binding::ArtifactDiagnostic;
    using ArtifactAcceptanceRegistration = typename Binding::ArtifactAcceptanceRegistration;
    using ArtifactBuildResult = typename Binding::ArtifactBuildResult;
    using ArtifactProducer = typename Binding::ArtifactProducer;
    using ArtifactProducerRegistration = typename Binding::ArtifactProducerRegistration;
    using ArtifactSchedulingPolicy = typename Binding::ArtifactSchedulingPolicy;
    using AsyncStateGraphStats = typename Binding::AsyncStateGraphStats;
    using AsyncStateGraphTraceConfig = typename Binding::AsyncStateGraphTraceConfig;
    using AsyncStateGraphTraceEventID = typename Binding::AsyncStateGraphTraceEventID;
    using AsyncStateGraphTraceReport = typename Binding::AsyncStateGraphTraceReport;
    using ArtifactKind = typename Binding::ArtifactKind;
    using TaskLane = typename Binding::TaskLane;
    using TaskDomain = typename Binding::TaskDomain;
    using GraphTraceSession = typename Binding::GraphTraceSession;
    static constexpr std::size_t kArtifactKindCount = Binding::kArtifactKindCount;
    explicit StateGraph(std::shared_ptr<org::async::GraphScheduler> scheduler,
        std::string_view name = "StateGraph", StateGraphHooks<Binding> hooks = {});
    ~StateGraph();
    StateGraph(const StateGraph&) = delete;
    StateGraph& operator=(const StateGraph&) = delete;

    void RegisterProducer(ArtifactKind kind, ArtifactProducerRegistration registration);
    template <class Input, class Output>
    void RegisterTypedProducer(ArtifactKind kind, TaskLane lane, TaskDomain domain,
        std::string taskName,
        std::function<ArtifactBuildResult(const ArtifactBuildContext&,
            std::shared_ptr<const Input>)> producer) {
        ArtifactProducerRegistration registration;
        registration.lane = lane;
        registration.domain = domain;
        registration.taskName = std::move(taskName);
        registration.inputType = std::type_index(typeid(Input));
        registration.outputType = std::type_index(typeid(Output));
        registration.producer = [producer = std::move(producer)](const ArtifactBuildContext& context) {
            const auto input = context.input.template Get<Input>();
            if (!input) return ArtifactBuildResult::Failure("artifact input type mismatch");
            auto result = producer(context, input);
            if (result.outcome == ArtifactBuildResult::Outcome::Ready &&
				!result.payload.template Get<Output>()) {
                return ArtifactBuildResult::Failure("artifact output type mismatch");
            }
            return result;
        };
        RegisterProducer(kind, std::move(registration));
    }
    ArtifactRequestResult Request(ArtifactKey key, std::uint64_t desiredRevision,
        std::vector<ArtifactRequirement> requirements = {}, ArtifactPayload input = {},
        std::uint64_t requestFingerprint = 0);
    // Derived latest-value intent. Intermediate successors that were submitted
    // through this API and have not started may be replaced; exact Request
    // versions are never coalesced.
    ArtifactRequestStatus SubmitLatestIntent(ArtifactKey key, std::uint64_t desiredRevision,
        std::vector<ArtifactRequirement> requirements = {}, ArtifactPayload input = {},
        std::uint64_t requestFingerprint = 0);
    std::vector<ArtifactRequestResult> SubmitLatestIntentBatch(
        std::vector<ArtifactIntent> intents);
    // Lock-free intent submission for threads that must never enter the graph
    // mutex (the renderer owner thread). Intents are queued and applied in
    // order by the graph control drain; a sibling posted in the same batch is
    // referenced by (address, revision) with requiredGeneration = 0.
    // Host kinds that disallow coalescing are queued as ordered exact requests;
    // this fire-and-forget API must not silently discard lifecycle updates.
    void PostIntents(std::vector<ArtifactIntent> intents);
    // Lock-free submission that still returns a usable handle: the generation is
    // allocated and leased by the calling thread, and the drain adopts it when it
    // applies the request. The handle may be used immediately as an Exact()
    // requirement of a sibling request or as an AwaitExact() target. If the drain
    // finds the address already carries another generation for this revision, the
    // predicted version becomes an alias of the installed one; if the request is
    // refused, the predicted version is archived in a terminal Failed state, so
    // it fails the dependents that required it and dispatches its awaiters.
    ArtifactRequestResult PostRequest(ArtifactIntent intent, bool coalescible = true);
    std::vector<ArtifactRequestResult> PostIntentBatch(std::vector<ArtifactIntent> intents);
    std::vector<ArtifactRequestResult> RequestBatch(std::vector<ArtifactRequest> requests);
    ArtifactRequestResult RequestExpressions(ArtifactKey key, std::uint64_t desiredRevision,
        std::vector<DependencyExpression> dependencies, ArtifactPayload input = {},
        std::uint64_t requestFingerprint = 0);
    bool Invalidate(ArtifactKey key, std::uint64_t desiredRevision);
    void Cancel(ArtifactKey key);
    void Release(ArtifactKey key);
    void ReleaseBatch(std::span<const ArtifactKey> keys);
    void MarkPublished(ArtifactKey key, std::uint64_t revision);
    void MarkPublished(ArtifactVersionID version);
    void MarkPublished(std::span<const ArtifactVersionID> versions);
    void PumpGpuCompletions();
    // External suspension identities share one process-wide namespace because
    // completion routing is keyed only by identity. Producers must obtain IDs
    // here rather than maintaining independent counters.
    void NotifySuspensionSatisfied(std::uint64_t identity);
    [[nodiscard]] std::function<void(std::uint64_t)> MakeSuspensionNotifier() const;
    void SetReadyCallback(std::function<void(const ArtifactSnapshot&)> callback);
    [[nodiscard]] std::uint64_t AddReadyCallback(
        std::function<void(const ArtifactSnapshot&)> callback);
    void RemoveReadyCallback(std::uint64_t subscription);
    [[nodiscard]] ArtifactObservation ObserveWithSnapshot(ArtifactKey address,
        std::function<void(std::uint64_t, const ArtifactSnapshot&)> callback);
	// Installs one level-triggered wakeup for a resource family. Consumers still
	// reconcile exact versions from graph state; the notification is never an
	// ownership-transfer event.
	[[nodiscard]] ArtifactObservation ObserveKind(ArtifactKind kind,
		std::function<void(std::uint64_t, const ArtifactSnapshot&)> callback);
	// onTerminal is required, and is the reason this takes two callables rather
	// than one snapshot: an awaited version can end in a state that will never
	// carry a payload, and a single continuation made that outcome
	// indistinguishable from success at the call site. Exactly one of the two
	// runs, exactly once.
	[[nodiscard]] ArtifactAwaiter AwaitExact(ArtifactVersionHandle handle,
		ArtifactReadiness milestone, TaskLane lane, TaskDomain domain,
		std::function<void(const ArtifactSnapshot&)> onReached,
		std::function<void(const ArtifactTermination&)> onTerminal);

    [[nodiscard]] ArtifactSnapshot Snapshot(ArtifactKey key) const;
    [[nodiscard]] ArtifactSnapshot Snapshot(ArtifactVersionID version) const;
    [[nodiscard]] ArtifactDiagnostic Diagnose(ArtifactKey key) const;
    // Latest requested revision of an address, without Diagnose's blocker-chain
    // walk or payload snapshot. Callers that only compare revisions must use
    // this; Diagnose is a human-readable stall report.
    [[nodiscard]] std::uint64_t DesiredRevision(ArtifactKey key) const;
    [[nodiscard]] AsyncStateGraphStats Stats() const;
    [[nodiscard]] std::uint64_t Outstanding(ArtifactKind kind) const;
    void StartTrace(AsyncStateGraphTraceConfig config = {});
    [[nodiscard]] bool TraceActive() const;
    // Declares the calling thread as the renderer owner thread. That thread must
    // never enter the graph mutex (it only observes published leases); every
    // lock it does take is counted as SARP.StateGraph.OwnerThreadLocks.
    void SetOwnerThread();
    [[nodiscard]] std::uint64_t OwnerThreadLocks() const;
    AsyncStateGraphTraceReport StopTraceAndWriteReport(const std::filesystem::path& outputDirectory);
    void TraceEvent(AsyncStateGraphTraceEventID event, ArtifactAddress address,
        std::uint64_t revision = 0, std::uint64_t generation = 0,
        AsyncStateGraphTracePayload payload = {}, ArtifactAddress related = {},
        std::uint64_t relatedRevision = 0);
    void WaitIdle() const;
    void Shutdown();

private:
    struct RequestDeferredCleanup;
    struct RequestPreparedState;
    // preassignedGeneration/preassignedLease carry a handle a posting thread
    // already returned to its caller (see PostRequest). The drain adopts them for
    // a fresh version, records a generation alias when the address already carries
    // another generation, and archives the predicted version as a terminal
    // Failed one on refusal.
    ArtifactRequestResult RequestInternal(ArtifactKey key, std::uint64_t desiredRevision,
        std::vector<ArtifactRequirement> requirements, ArtifactPayload input,
        std::uint64_t requestFingerprint, bool coalescibleIntent, bool callerOwnsMutex = false,
        RequestDeferredCleanup* deferredCleanup = nullptr,
        RequestPreparedState* preparedState = nullptr,
        std::uint64_t preassignedGeneration = 0,
        std::shared_ptr<const void> preassignedLease = {});
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};


using AsyncStateGraph = StateGraph<DefaultStateGraphTypes>;

} // namespace org::async
