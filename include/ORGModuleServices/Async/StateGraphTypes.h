#pragma once

#include <ORGModuleServices/Async/ArtifactBuild.h>
#include <ORGModuleServices/Async/GraphTrace.h>

namespace org::async {

// Generic graph operations only; renderer-only event vocabulary stays in its adapter.
enum class StateGraphEvent : std::uint16_t {
    AcceptanceApplied,
    GraphControlStarted,
    StateChanged,
    VersionReclaimed,
    VersionsReclaimed,
    QueueNodePhase,
    DependencyBlocked,
    BuildSubmitted,
    BuildDependencyResolved,
    BuildStarted,
    BuildCompleted,
    BuildRejected,
    AcceptanceQueued,
    CompletionApplied,
    CompletionStale,
    SuspensionRegistered,
    SuspensionSatisfied,
    DrainStarted,
    DrainGpuCollectPhase,
    GpuNotificationApplied,
    GraphPopulation,
    ExactWaitSatisfied,
    DrainCompleted,
    RequestReceived,
    RequestPhase,
    DependencyDeclared,
    RequestConflict,
    RequestAlreadyDesired,
    SuccessorQueued,
    RequestAccepted,
    Invalidated,
    Cancelled,
    Released,
    Published,
    ObservationRegistered,
    ObservationCancelled,
    KindObservationRegistered,
    ExactWaitRegistered,
    ExactWaitCancelled,
    TraceStarted,
    TraceStopped
};

struct StateGraphTraceReport {
    std::filesystem::path eventsCsv;
    std::uint64_t capturedEvents = 0;
    std::uint64_t droppedEvents = 0;
    std::chrono::microseconds elapsed{};
};

template <class Kind, class Scheduling, std::size_t KindCount, class Event, class Report>
struct StateGraphTypes {
    using ArtifactAddress = org::async::ArtifactAddress<Kind>;
    using ArtifactKey = org::async::ArtifactKey<Kind>;
    using ArtifactVersionID = org::async::ArtifactVersionID<Kind>;
    using ArtifactVersionHandle = org::async::ArtifactVersionHandle<Kind>;
    using ArtifactRequirement = org::async::ArtifactRequirement<Kind>;
    using HandleRequirement = org::async::HandleRequirement<Kind>;
    using Require = org::async::Require<Kind>;
    using Optional = org::async::Optional<Kind>;
    using FirstReady = org::async::FirstReady<Kind>;
    using AnyReady = org::async::AnyReady<Kind>;
    using DependencyExpression = org::async::DependencyExpression<Kind>;
    using ArtifactSnapshot = org::async::ArtifactSnapshot<Kind>;
    using ArtifactBuildContext = org::async::ArtifactBuildContext<Kind>;
    using ArtifactRequestResult = org::async::ArtifactRequestResult<Kind>;
    using ArtifactRequest = org::async::ArtifactRequest<Kind>;
    using ArtifactIntent = org::async::ArtifactIntent<Kind>;
    using ArtifactObservation = org::async::ArtifactObservation<Kind>;
    using ArtifactTermination = org::async::ArtifactTermination<Kind>;
    using ArtifactAwaiter = org::async::ArtifactAwaiter<Kind>;
    using ArtifactSuspension = org::async::ArtifactSuspension<Kind>;
    using ArtifactDiagnostic = org::async::ArtifactDiagnostic<Kind>;
    using ArtifactAcceptanceRegistration = org::async::ArtifactAcceptanceRegistration<Kind, Scheduling>;
    using ArtifactBuildResult = org::async::ArtifactBuildResult<Kind, Scheduling>;
    using ArtifactProducer = org::async::ArtifactProducer<Kind, Scheduling>;
    using ArtifactProducerRegistration = org::async::ArtifactProducerRegistration<Kind, Scheduling>;
    using ArtifactSchedulingPolicy = org::async::ArtifactSchedulingPolicy<Scheduling>;
    using AsyncStateGraphStats = org::async::AsyncStateGraphStats<KindCount>;
    using AsyncStateGraphTraceConfig = org::async::AsyncStateGraphTraceConfig<KindCount>;
    using AsyncStateGraphTraceEventID = Event;
    using AsyncStateGraphTraceReport = Report;
    using ArtifactKind = Kind;
    using TaskLane = typename Scheduling::Lane;
    using TaskDomain = typename Scheduling::Domain;
    using GraphTraceSession = org::async::GraphTraceSession<Kind, Event, KindCount, Report>;
    static constexpr std::size_t kArtifactKindCount = KindCount;

};

// Numeric kinds are host assigned in [0, 64). Hosts needing a different inventory
// explicitly instantiate the same implementation with their own type binding.
using DefaultStateGraphTypes = StateGraphTypes<std::uint16_t, DefaultArtifactScheduling,
    64, StateGraphEvent, StateGraphTraceReport>;

} // namespace org::async
