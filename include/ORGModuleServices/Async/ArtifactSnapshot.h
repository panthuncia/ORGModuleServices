#pragma once

#include <ORGModuleServices/Async/ArtifactIdentity.h>

namespace org::async {

template <class Kind>
struct ArtifactSnapshot {
    ArtifactKey<Kind> key;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    ArtifactReadiness readiness = ArtifactReadiness::Missing;
    ArtifactPayload payload;
    std::shared_ptr<const GpuSubmissionSet> gpuSubmissions;
    ArtifactLease lease;

    [[nodiscard]] ArtifactVersionID<Kind> Version() const noexcept {
        return { key, revision, generation };
    }
};

template <class T, class Kind>
struct ArtifactHandle {
    ArtifactKey<Kind> key;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    ArtifactReadiness readiness = ArtifactReadiness::Missing;
    std::shared_ptr<const T> payload;
    std::shared_ptr<const GpuSubmissionSet> gpuSubmissions;
    ArtifactLease lease;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(payload);
    }

    [[nodiscard]] ArtifactVersionID<Kind> Version() const noexcept {
        return { key, revision, generation };
    }
};

template <class T, class Kind>
[[nodiscard]] ArtifactHandle<T, Kind> MakeArtifactHandle(const ArtifactSnapshot<Kind>& snapshot) {
    return { snapshot.key, snapshot.revision, snapshot.generation, snapshot.readiness,
        snapshot.payload.template Get<T>(), snapshot.gpuSubmissions, snapshot.lease };
}

} // namespace org::async
