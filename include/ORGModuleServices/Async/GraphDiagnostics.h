#pragma once

#include <ORGModuleServices/Async/ArtifactBuild.h>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace org::async {

template <class Kind>
inline std::string KeyString(const ArtifactKey<Kind>& key) {
    return std::format("{}:{}:{}", static_cast<unsigned>(key.kind), key.primaryID, key.variantID);
}

inline std::string_view ReadinessName(ArtifactReadiness readiness) {
    static constexpr std::string_view names[]{ "Missing", "Blocked", "Queued", "Preparing",
        "CpuReady", "UploadSubmitted", "GpuReady", "Published", "Superseded", "Cancelled",
        "Failed" };
    const auto index = static_cast<std::size_t>(readiness);
    return index < std::size(names) ? names[index] : "Unknown";
}

inline std::string_view RequestStatusName(ArtifactRequestStatus status) {
    static constexpr std::string_view names[]{ "Accepted", "AlreadyDesired", "StaleRevision",
        "ConflictingRevision", "MissingFingerprint", "TypeMismatch", "ShuttingDown" };
    const auto index = static_cast<std::size_t>(status);
    return index < std::size(names) ? names[index] : "Unknown";
}

constexpr std::uint64_t StableTraceID(std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}

template <class Kind>
inline std::uint64_t TraceCorrelationID(const ArtifactKey<Kind>& key, std::uint64_t revision,
    std::uint64_t generation) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    const auto append = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    append(static_cast<std::uint64_t>(key.kind));
    append(key.primaryID);
    append(key.variantID);
    append(revision);
    append(generation);
    return hash == 0 ? 1u : hash;
}

enum class GraphMutexPhase : std::uint8_t {
    AcceptanceScheduleFailure, AcceptanceDequeue, AcceptanceCompletionEnqueue,
    AcceptanceDispatchEnqueue, DelayedDrainState, ProducerCompletionEnqueue,
    DrainGpuCollect, DrainApply, Reclaim, RegisterProducer, Request, RequestBatch,
    CancelLookup, CancelApply, Invalidate, MarkPublishedRevision,
    MarkPublishedVersions, GpuCompletionScan, SuspensionSatisfied,
    ReadyCallbackSet, ReadyCallbackAdd, ReadyCallbackRemove,
    ObservationRegister, ObservationRemove, ExactWaiterRegister, ExactWaiterRemove,
    Snapshot, Diagnose, Stats, Outstanding, WaitIdle, RecoveryResume, Shutdown, Count
};

constexpr std::string_view GraphMutexPhaseName(GraphMutexPhase phase) {
    switch (phase) {
    case GraphMutexPhase::AcceptanceScheduleFailure: return "AcceptanceScheduleFailure";
    case GraphMutexPhase::AcceptanceDequeue: return "AcceptanceDequeue";
    case GraphMutexPhase::AcceptanceCompletionEnqueue: return "AcceptanceCompletionEnqueue";
    case GraphMutexPhase::AcceptanceDispatchEnqueue: return "AcceptanceDispatchEnqueue";
    case GraphMutexPhase::DelayedDrainState: return "DelayedDrainState";
    case GraphMutexPhase::ProducerCompletionEnqueue: return "ProducerCompletionEnqueue";
    case GraphMutexPhase::DrainGpuCollect: return "DrainGpuCollect";
    case GraphMutexPhase::DrainApply: return "DrainApply";
	case GraphMutexPhase::Reclaim: return "Reclaim";
    case GraphMutexPhase::RegisterProducer: return "RegisterProducer";
    case GraphMutexPhase::Request: return "Request";
    case GraphMutexPhase::RequestBatch: return "RequestBatch";
    case GraphMutexPhase::CancelLookup: return "CancelLookup";
    case GraphMutexPhase::CancelApply: return "CancelApply";
    case GraphMutexPhase::Invalidate: return "Invalidate";
    case GraphMutexPhase::MarkPublishedRevision: return "MarkPublishedRevision";
    case GraphMutexPhase::MarkPublishedVersions: return "MarkPublishedVersions";
    case GraphMutexPhase::GpuCompletionScan: return "GpuCompletionScan";
    case GraphMutexPhase::SuspensionSatisfied: return "SuspensionSatisfied";
    case GraphMutexPhase::ReadyCallbackSet: return "ReadyCallbackSet";
    case GraphMutexPhase::ReadyCallbackAdd: return "ReadyCallbackAdd";
    case GraphMutexPhase::ReadyCallbackRemove: return "ReadyCallbackRemove";
    case GraphMutexPhase::ObservationRegister: return "ObservationRegister";
    case GraphMutexPhase::ObservationRemove: return "ObservationRemove";
    case GraphMutexPhase::ExactWaiterRegister: return "ExactWaiterRegister";
    case GraphMutexPhase::ExactWaiterRemove: return "ExactWaiterRemove";
    case GraphMutexPhase::Snapshot: return "Snapshot";
    case GraphMutexPhase::Diagnose: return "Diagnose";
    case GraphMutexPhase::Stats: return "Stats";
    case GraphMutexPhase::Outstanding: return "Outstanding";
    case GraphMutexPhase::WaitIdle: return "WaitIdle";
    case GraphMutexPhase::RecoveryResume: return "RecoveryResume";
    case GraphMutexPhase::Shutdown: return "Shutdown";
    case GraphMutexPhase::Count: break;
    }
    return "Unknown";
}

struct GraphMutexCounts {
    std::uint64_t nodes = 0;
    std::uint64_t versions = 0;
    std::uint64_t pending = 0;
    std::uint64_t completions = 0;
    std::uint64_t gpuRecovery = 0;
    std::uint64_t waiters = 0;
};

} // namespace org::async
