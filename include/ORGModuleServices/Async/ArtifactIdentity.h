#pragma once

#include <ORGModuleServices/Async/ArtifactResources.h>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <variant>
#include <vector>

namespace org::async {

// Kind belongs to the host. Identity, revision/generation semantics and exact
// dependency recipes are shared without importing a renderer kind inventory.
template <class Kind>
struct ArtifactAddress {
    Kind kind{};
    std::uint64_t primaryID = 0;
    std::uint64_t variantID = 0;
    auto operator<=>(const ArtifactAddress<Kind>&) const = default;

    struct Hasher {
        std::size_t operator()(const ArtifactAddress<Kind>& key) const noexcept {
            auto value = static_cast<std::size_t>(key.kind);
            value ^= std::hash<std::uint64_t>{}(key.primaryID) + 0x9e3779b9u + (value << 6u) + (value >> 2u);
            value ^= std::hash<std::uint64_t>{}(key.variantID) + 0x9e3779b9u + (value << 6u) + (value >> 2u);
            return value;
        }
    };
};

// ArtifactKey<Kind> remains as a source-compatible spelling while callers migrate.
// It identifies a logical address only; revisions are never encoded into it.
template <class Kind>
using ArtifactKey = ArtifactAddress<Kind>;

template <class Kind>
struct ArtifactVersionID {
    ArtifactAddress<Kind> address;
    std::uint64_t revision = 0;
    // Assigned once by the graph. It is never reused and detects stale/ABA handles.
    std::uint64_t generation = 0;
    auto operator<=>(const ArtifactVersionID<Kind>&) const = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return revision != 0 && generation != 0;
    }
};


// Strong, untyped reference to one immutable version. Use this for queued
// orchestration work that has not yet installed a graph dependency. Pure
// ArtifactVersionID<Kind> values are appropriate only for diagnostics and metadata
// whose enclosing graph recipe or publication bundle already owns the pin.
template <class Kind>
struct ArtifactVersionHandle {
    ArtifactVersionID<Kind> version;
    ArtifactLease lease;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(version) && static_cast<bool>(lease);
    }
};


// How a requirement participates in dependency selection.
enum class DependencyPolicy : std::uint8_t { AllOf, AnyOf, Optional, FallbackAllowed };

// How a dependency changing after it has been selected affects its consumer.
// Latest preserves the original graph behaviour and is therefore the default.
enum class DependencyInvalidationPolicy : std::uint8_t {
    ExactSnapshot,
    Latest,
    ReadyGate,
    LifetimeHold,
};

template <class Kind>
struct ArtifactRequirement {
    ArtifactKey<Kind> key;
    std::uint64_t minimumRevision = 0;
    ArtifactReadiness requiredReadiness = ArtifactReadiness::CpuReady;
    DependencyPolicy policy = DependencyPolicy::AllOf;
    // Non-zero AnyOf requirements sharing a group form one alternative set.
    std::uint32_t alternativeGroup = 0;
    DependencyInvalidationPolicy invalidation = DependencyInvalidationPolicy::Latest;
    // Non-zero for handle-based requirements. This prevents an exact revision
    // from accidentally binding to a different internal incarnation (ABA).
    std::uint64_t requiredGeneration = 0;
    bool operator==(const ArtifactRequirement<Kind>& other) const noexcept {
        return key == other.key && minimumRevision == other.minimumRevision &&
            requiredReadiness == other.requiredReadiness && policy == other.policy &&
            alternativeGroup == other.alternativeGroup && invalidation == other.invalidation &&
            requiredGeneration == other.requiredGeneration;
    }
};

template <class Kind>
[[nodiscard]] inline ArtifactRequirement<Kind> Exact(ArtifactVersionID<Kind> version,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady,
    DependencyPolicy policy = DependencyPolicy::AllOf,
    std::uint32_t alternativeGroup = 0) {
    return { version.address, version.revision, readiness, policy, alternativeGroup,
        DependencyInvalidationPolicy::ExactSnapshot, version.generation };
}

template <class Kind>
[[nodiscard]] inline ArtifactRequirement<Kind> Exact(const ArtifactVersionHandle<Kind>& handle,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady,
    DependencyPolicy policy = DependencyPolicy::AllOf,
    std::uint32_t alternativeGroup = 0) {
    return org::async::Exact(handle.version, readiness, policy, alternativeGroup);
}

template <class Kind>
[[nodiscard]] inline ArtifactRequirement<Kind> Latest(ArtifactAddress<Kind> address,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady,
    DependencyPolicy policy = DependencyPolicy::AllOf,
    std::uint32_t alternativeGroup = 0) {
    return { address, 0, readiness, policy, alternativeGroup,
        DependencyInvalidationPolicy::Latest };
}

template <class Kind>
[[nodiscard]] inline ArtifactRequirement<Kind> LatestAtLeast(ArtifactAddress<Kind> address,
    std::uint64_t minimumRevision,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady,
    DependencyPolicy policy = DependencyPolicy::AllOf,
    std::uint32_t alternativeGroup = 0) {
    return { address, minimumRevision, readiness, policy, alternativeGroup,
        DependencyInvalidationPolicy::Latest };
}

// Exact-version readiness gate: authorizes the consumer's build once that version
// reaches the milestone, without pinning it. The requester must hold the
// version's handle until the consumer has built; afterwards it may be reclaimed.
template <class Kind>
[[nodiscard]] inline ArtifactRequirement<Kind> ReadyGate(ArtifactVersionID<Kind> version,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady) {
    return { version.address, version.revision, readiness, DependencyPolicy::AllOf, 0,
        DependencyInvalidationPolicy::ReadyGate, version.generation };
}

// Address-level readiness latch. It selects whichever immutable version of the
// address currently satisfies the milestone and deliberately does not retain or
// invalidate on later versions. Use the handle overload when one exact version
// is the gate.
template <class Kind>
[[nodiscard]] inline ArtifactRequirement<Kind> ReadyGate(ArtifactAddress<Kind> address,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady) {
    return { address, 0, readiness, DependencyPolicy::AllOf, 0,
        DependencyInvalidationPolicy::ReadyGate };
}

template <class Kind>
[[nodiscard]] inline ArtifactRequirement<Kind> LifetimeHold(ArtifactVersionID<Kind> version) {
    return { version.address, version.revision, ArtifactReadiness::CpuReady,
        DependencyPolicy::Optional, 0, DependencyInvalidationPolicy::LifetimeHold,
        version.generation };
}

template <class Kind>
struct HandleRequirement {
    ArtifactKey<Kind> key;
    std::uint64_t minimumRevision = 0;
    ArtifactReadiness requiredReadiness = ArtifactReadiness::CpuReady;
};
template <class Kind>
struct Require { HandleRequirement<Kind> requirement; };
template <class Kind>
struct Optional { HandleRequirement<Kind> requirement; };
template <class Kind>
struct FirstReady { std::vector<HandleRequirement<Kind>> alternatives; };
template <class Kind>
struct AnyReady { std::vector<HandleRequirement<Kind>> alternatives; };
template <class Kind>
using DependencyExpression = std::variant<Require<Kind>, Optional<Kind>, FirstReady<Kind>, AnyReady<Kind>>;

} // namespace org::async
