#pragma once

#include <ORGModuleServices/Async/ArtifactSnapshot.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace org::async {

// Hosts retain their own scheduling vocabulary and defaults. The generic
// contract uses a single numeric lane/domain and no admission group.
struct DefaultArtifactScheduling {
    using Lane = std::uint32_t;
    using Domain = std::uint32_t;
    static constexpr Lane initialLane = 0;
    static constexpr Lane continuationLane = 0;
    static constexpr Domain acceptanceDomain = 0;
    static constexpr Domain producerDomain = 0;
    static constexpr std::uint8_t admissionGroup = 0;
};

template <class Kind>
struct ArtifactBuildContext {
    using ArtifactKey = org::async::ArtifactKey<Kind>;
    using ArtifactSnapshot = org::async::ArtifactSnapshot<Kind>;
    template <class T> using ArtifactHandle = org::async::ArtifactHandle<T, Kind>;
    ArtifactKey key;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    std::vector<ArtifactSnapshot> dependencies;
    ArtifactPayload input;
    ArtifactPayload checkpoint;
    std::function<bool()> stopRequested;

    template <class T>
    [[nodiscard]] ArtifactHandle<T> Dependency(ArtifactKey dependencyKey) const {
        const auto found = std::ranges::find_if(dependencies,
            [&](const ArtifactSnapshot& dependency) {
                return dependency.key == dependencyKey;
            });
        return found == dependencies.end() ? ArtifactHandle<T>{}
                                           : org::async::MakeArtifactHandle<T>(*found);
    }
};

enum class ArtifactRequestStatus : std::uint8_t {
    Accepted,
    AlreadyDesired,
    StaleRevision,
    ConflictingRevision,
    MissingFingerprint,
    TypeMismatch,
    ShuttingDown,
};

template <class Kind>
struct ArtifactRequestResult {
    using ArtifactVersionID = org::async::ArtifactVersionID<Kind>;
    using ArtifactVersionHandle = org::async::ArtifactVersionHandle<Kind>;
    ArtifactRequestStatus status = ArtifactRequestStatus::ShuttingDown;
    std::uint64_t generation = 0;
    ArtifactVersionID version;
    ArtifactLease lease;
    constexpr operator bool() const noexcept {
        return status == ArtifactRequestStatus::Accepted ||
            status == ArtifactRequestStatus::AlreadyDesired;
    }
    [[nodiscard]] ArtifactVersionHandle Handle() const {
        return { version, lease };
    }
};

template <class Kind>
struct ArtifactRequest {
    using ArtifactKey = org::async::ArtifactKey<Kind>;
    using ArtifactRequirement = org::async::ArtifactRequirement<Kind>;
    ArtifactKey key;
    std::uint64_t desiredRevision = 0;
    std::vector<ArtifactRequirement> requirements;
    ArtifactPayload input;
    std::uint64_t requestFingerprint = 0;
};

// A mutable desired-state update. Unlike an exact ArtifactRequest, newer
// intents for the same address may replace queued work before it starts.
template <class Kind>
using ArtifactIntent = ArtifactRequest<Kind>;

template <class Kind>
class ArtifactObservation {
public:
    using ArtifactSnapshot = org::async::ArtifactSnapshot<Kind>;
    ArtifactObservation() = default;
    ArtifactObservation(std::uint64_t subscriptionValue, std::uint64_t sequenceValue,
        ArtifactSnapshot snapshotValue, std::function<void()> unsubscribeValue)
        : subscription(subscriptionValue), sequence(sequenceValue),
          snapshot(std::move(snapshotValue)), m_unsubscribe(std::move(unsubscribeValue)) {}
    ~ArtifactObservation() { Reset(); }
    ArtifactObservation(const ArtifactObservation&) = delete;
    ArtifactObservation& operator=(const ArtifactObservation&) = delete;
    ArtifactObservation(ArtifactObservation&& other) noexcept
        : subscription(std::exchange(other.subscription, 0)),
          sequence(other.sequence), snapshot(std::move(other.snapshot)),
          m_unsubscribe(std::move(other.m_unsubscribe)) {}
    ArtifactObservation& operator=(ArtifactObservation&& other) noexcept {
        if (this == &other) return *this;
        Reset();
        subscription = std::exchange(other.subscription, 0);
        sequence = other.sequence;
        snapshot = std::move(other.snapshot);
        m_unsubscribe = std::move(other.m_unsubscribe);
        return *this;
    }
    void Reset() noexcept {
        if (!m_unsubscribe) return;
        auto unsubscribe = std::move(m_unsubscribe);
        subscription = 0;
        unsubscribe();
    }

    std::uint64_t subscription = 0;
    std::uint64_t sequence = 0;
    ArtifactSnapshot snapshot;

private:
    std::function<void()> m_unsubscribe;
};

// Delivered when an awaited version reaches a terminal state instead of its
// milestone: it failed, was cancelled, was superseded, or the request that
// would have produced it was refused. Deliberately not an ArtifactSnapshot.
// A terminal version has no payload and never will, so a continuation must not
// be able to reach for one; the two outcomes are different parameters to
// different callables rather than one value that has to be interrogated.
template <class Kind>
struct ArtifactTermination {
    using ArtifactVersionID = org::async::ArtifactVersionID<Kind>;
    ArtifactVersionID version;
    ArtifactReadiness readiness = ArtifactReadiness::Failed;
    std::string error;

    [[nodiscard]] bool Refused() const noexcept { return refused; }
    // True only for a version the graph refused to admit at request time, as
    // opposed to one that was admitted and then failed, was cancelled or was
    // superseded. Callers that retry a refusal use this to avoid retrying work
    // that genuinely ran and failed.
    bool refused = false;
};

// Move-only registration for one immutable version milestone. Unlike the
// address/kind observations, this is a correctness primitive: registration and
// the initial exact-version sample must be serialized by the graph backend.
template <class Kind>
class ArtifactAwaiter {
public:
    using ArtifactSnapshot = org::async::ArtifactSnapshot<Kind>;
    ArtifactAwaiter() = default;
    ArtifactAwaiter(std::uint64_t subscriptionValue, ArtifactSnapshot snapshotValue,
        std::function<void()> cancelValue)
        : subscription(subscriptionValue), snapshot(std::move(snapshotValue)),
          m_cancel(std::move(cancelValue)) {}
    ~ArtifactAwaiter() { Reset(); }
    ArtifactAwaiter(const ArtifactAwaiter&) = delete;
    ArtifactAwaiter& operator=(const ArtifactAwaiter&) = delete;
    ArtifactAwaiter(ArtifactAwaiter&& other) noexcept
        : subscription(std::exchange(other.subscription, 0)),
          snapshot(std::move(other.snapshot)), m_cancel(std::move(other.m_cancel)) {}
    ArtifactAwaiter& operator=(ArtifactAwaiter&& other) noexcept {
        if (this == &other) return *this;
        Reset();
        subscription = std::exchange(other.subscription, 0);
        snapshot = std::move(other.snapshot);
        m_cancel = std::move(other.m_cancel);
        return *this;
    }
    void Reset() noexcept {
        if (!m_cancel) return;
        auto cancel = std::move(m_cancel);
        subscription = 0;
        cancel();
    }

    std::uint64_t subscription = 0;
    ArtifactSnapshot snapshot;

private:
    std::function<void()> m_cancel;
};

enum class ArtifactSuspensionKind : std::uint8_t {
    ExactDependency,
    Capacity,
    ExternalOperation,
    TransientRetry
};

// A level-triggered reason why a producer cannot make progress.  identity is
// supplied by the provider and must identify one immutable operation/grant.
// Notifications may arrive before the producer returns Suspend(); the graph
// latches them and reconciles the exact artifact generation when registered.
template <class Kind>
struct ArtifactSuspension {
    using ArtifactVersionID = org::async::ArtifactVersionID<Kind>;
    ArtifactSuspensionKind kind = ArtifactSuspensionKind::ExternalOperation;
    std::uint64_t identity = 0;
    ArtifactVersionID dependency;
    ArtifactReadiness milestone = ArtifactReadiness::GpuReady;
    std::chrono::steady_clock::time_point deadline{};
    std::uint32_t maximumAttempts = 0;
    std::string reason;

    static ArtifactSuspension Exact(ArtifactVersionID dependencyValue,
        ArtifactReadiness milestoneValue) {
        ArtifactSuspension suspension;
        suspension.kind = ArtifactSuspensionKind::ExactDependency;
        suspension.dependency = dependencyValue;
        suspension.milestone = milestoneValue;
        return suspension;
    }
    static ArtifactSuspension Capacity(std::uint64_t identityValue,
        std::string reasonValue = {}) {
        ArtifactSuspension suspension;
        suspension.kind = ArtifactSuspensionKind::Capacity;
        suspension.identity = identityValue;
        suspension.reason = std::move(reasonValue);
        return suspension;
    }
    static ArtifactSuspension External(std::uint64_t identityValue,
        std::string reasonValue = {}) {
        ArtifactSuspension suspension;
        suspension.kind = ArtifactSuspensionKind::ExternalOperation;
        suspension.identity = identityValue;
        suspension.reason = std::move(reasonValue);
        return suspension;
    }
    static ArtifactSuspension Transient(std::uint64_t identityValue,
        std::chrono::steady_clock::time_point deadlineValue, std::uint32_t maximumAttemptsValue,
        std::string reasonValue) {
        ArtifactSuspension suspension;
        suspension.kind = ArtifactSuspensionKind::TransientRetry;
        suspension.identity = identityValue;
        suspension.deadline = deadlineValue;
        suspension.maximumAttempts = maximumAttemptsValue;
        suspension.reason = std::move(reasonValue);
        return suspension;
    }
};

template <class Kind, class Scheduling = DefaultArtifactScheduling>
struct ArtifactAcceptanceRegistration {
    using ArtifactSnapshot = org::async::ArtifactSnapshot<Kind>;
    using TaskLane = typename Scheduling::Lane;
    using TaskDomain = typename Scheduling::Domain;
    TaskLane lane = Scheduling::initialLane;
    TaskDomain domain = Scheduling::acceptanceDomain;
    std::function<void(const ArtifactSnapshot&)> action;
};

template <class Kind, class Scheduling = DefaultArtifactScheduling>
struct ArtifactBuildResult {
    using ArtifactSnapshot = org::async::ArtifactSnapshot<Kind>;
    using ArtifactRequirement = org::async::ArtifactRequirement<Kind>;
    using ArtifactSuspension = org::async::ArtifactSuspension<Kind>;
    using ArtifactAcceptanceRegistration = org::async::ArtifactAcceptanceRegistration<Kind, Scheduling>;
    enum class Outcome : std::uint8_t {
        Ready, NeedsDependencies, RetryAfter, Failed, Cancelled, Suspended
    };
    Outcome outcome = Outcome::Failed;
    ArtifactPayload payload;
    ArtifactPayload checkpoint;
    std::vector<ArtifactRequirement> requirements;
    std::shared_ptr<const GpuSubmissionSet> gpuSubmissions;
    std::chrono::steady_clock::duration retryDelay{};
    std::optional<ArtifactSuspension> suspension;
    std::string error;
	ArtifactAcceptanceRegistration acceptance;
	// Runs outside the graph mutex only after this exact producer result has
	// passed generation/dependency validation. Deprecated compatibility adapter;
	// new producers should register acceptance with an explicit lane/domain.
	std::function<void(const ArtifactSnapshot&)> onAccepted;

    static ArtifactBuildResult Ready(ArtifactPayload payload,
        std::shared_ptr<const GpuSubmissionSet> gpuSubmissions = {}) {
        ArtifactBuildResult result;
        result.outcome = Outcome::Ready;
        result.payload = std::move(payload);
        result.gpuSubmissions = std::move(gpuSubmissions);
        return result;
    }
    static ArtifactBuildResult Needs(std::vector<ArtifactRequirement> requirements,
        ArtifactPayload checkpoint = {}) {
        ArtifactBuildResult result;
        result.outcome = Outcome::NeedsDependencies;
        result.requirements = std::move(requirements);
        result.checkpoint = std::move(checkpoint);
        return result;
    }
    static ArtifactBuildResult Retry(std::chrono::steady_clock::duration delay,
        ArtifactPayload checkpoint = {}) {
        ArtifactBuildResult result;
        result.outcome = Outcome::RetryAfter;
        result.retryDelay = delay;
        result.checkpoint = std::move(checkpoint);
        return result;
    }
    static ArtifactBuildResult Suspend(ArtifactSuspension suspension,
        ArtifactPayload checkpoint = {}) {
        ArtifactBuildResult result;
        result.outcome = Outcome::Suspended;
        result.suspension = std::move(suspension);
        result.checkpoint = std::move(checkpoint);
        return result;
    }
    static ArtifactBuildResult Failure(std::string error) {
        ArtifactBuildResult result;
        result.outcome = Outcome::Failed;
        result.error = std::move(error);
        return result;
    }
    static ArtifactBuildResult Cancelled() {
        ArtifactBuildResult result;
        result.outcome = Outcome::Cancelled;
        return result;
    }
};

template <class Kind, class Scheduling = DefaultArtifactScheduling>
using ArtifactProducer = std::function<ArtifactBuildResult<Kind, Scheduling>(const ArtifactBuildContext<Kind>&)>;

enum class ArtifactWorkClass : std::uint8_t {
    Publication, Continuation, Ingestion, Maintenance
};

template <class Scheduling = DefaultArtifactScheduling>
struct ArtifactSchedulingPolicy {
    using TaskLane = typename Scheduling::Lane;
    using TaskDomain = typename Scheduling::Domain;
    ArtifactWorkClass initialClass = ArtifactWorkClass::Ingestion;
    TaskLane initialLane = Scheduling::initialLane;
    TaskLane continuationLane = Scheduling::continuationLane;
    // The host selects the default bounded-work admission group.
    std::uint8_t admissionGroup = Scheduling::admissionGroup;
    std::uint64_t admissionKey = 0;
};

template <class Kind, class Scheduling = DefaultArtifactScheduling>
struct ArtifactProducerRegistration {
    using ArtifactSchedulingPolicy = org::async::ArtifactSchedulingPolicy<Scheduling>;
    using ArtifactProducer = org::async::ArtifactProducer<Kind, Scheduling>;
    using TaskLane = typename Scheduling::Lane;
    using TaskDomain = typename Scheduling::Domain;
    TaskLane lane = Scheduling::initialLane;
    TaskDomain domain = Scheduling::producerDomain;
    std::string taskName;
    ArtifactProducer producer;
    std::type_index inputType{ typeid(void) };
    std::type_index outputType{ typeid(void) };
    ArtifactSchedulingPolicy scheduling{};
};

template <class Kind>
struct ArtifactDiagnostic {
    using ArtifactSnapshot = org::async::ArtifactSnapshot<Kind>;
    using ArtifactRequirement = org::async::ArtifactRequirement<Kind>;
    ArtifactSnapshot artifact;
    std::uint64_t desiredRevision = 0;
    std::vector<ArtifactRequirement> blockers;
    std::string error;
    std::string blockerChain;
    std::chrono::microseconds stateAge{};
};

template <std::size_t KindCount>
struct AsyncStateGraphStats {
    std::uint64_t requests = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t buildsStarted = 0;
    std::uint64_t buildsCompleted = 0;
    std::uint64_t staleCompletions = 0;
    std::uint64_t failed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t cycles = 0;
    std::uint64_t gpuWaiting = 0;
    std::uint64_t retries = 0;
    std::uint64_t queueWaitMicros = 0;
    std::uint64_t buildMicros = 0;
    std::uint64_t gpuWaitMicros = 0;
    std::uint64_t controlQueueWaitMicros = 0;
    std::uint64_t maxControlQueueWaitMicros = 0;
    std::uint64_t completionApplyMicros = 0;
    std::uint64_t maxCompletionApplyMicros = 0;
    std::uint64_t gpuApplyMicros = 0;
    std::uint64_t maxGpuApplyMicros = 0;
    std::uint64_t dependencyEvaluations = 0;
    std::uint64_t coalescedIntents = 0;
    std::uint64_t intentBatches = 0;
    std::uint64_t supersededBuilds = 0;
    std::uint64_t reclaimCandidates = 0;
    std::uint64_t archivedVersions = 0;
    std::uint64_t reclaimedVersions = 0;
    std::uint64_t externallyLeasedVersions = 0;
    std::uint64_t desiredVersions = 0;
    std::uint64_t recipePinnedVersions = 0;
    std::uint64_t unclassifiedRetainedVersions = 0;
	std::uint64_t exactWaiters = 0;
    std::array<std::uint64_t, KindCount> intentsByKind{};
    std::array<std::uint64_t, KindCount> coalescedByKind{};
    std::array<std::uint64_t, KindCount> buildsStartedByKind{};
    std::array<std::uint64_t, KindCount> buildsCompletedByKind{};
    std::array<std::uint64_t, KindCount> queueWaitMicrosByKind{};
    std::array<std::uint64_t, static_cast<std::size_t>(ArtifactReadiness::Failed) + 1u> stateCounts{};
};


} // namespace org::async
