#pragma once

#include <ORGModuleServices/Async/StateGraph.h>
#include <ORGModuleServices/Async/SuspensionIdentity.h>
#include <ORGModuleServices/VersionedBufferBuild.h>

#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace org::services {

/** @brief The host's ties to the versioned-buffer producer (all optional). */
template <class Binding>
struct VersionedBufferProducerHooks {
    using Graph = async::StateGraph<Binding>;
    // Producer thread, before the version is frozen: host state seeded from it (BasicRenderer: PublishedBufferVersion::hostState,
    // its PreparedBackingState under SARP_PERSISTENT_PUBLICATIONS). Throws to fail the build.
    std::function<void(PublishedBufferVersion& version, const VersionedBufferBuildInput& input)> decorate;
    // Producer thread: the artifact payload for the version (BasicRenderer: a RendererStateFragmentArtifact carrying the version,
    // its catalog entries made from input.tag, and a hold on the version's lease). Null: the version itself
    // (ArtifactPayload::Make<PublishedBufferVersion>).
    std::function<async::ArtifactPayload(const std::shared_ptr<const PublishedBufferVersion>& version, const VersionedBufferBuildInput& input,
        const typename Graph::ArtifactBuildContext& context)>
        wrap;
};

/**
 * @brief Registers the versioned-buffer producer (BuildVersionedBuffer as graph work, modelled on RegisterVersionGrowthProducer) for
 * each of `kinds` on (lane, domain). Its input is a VersionedBufferBuildInput; it is ready with its uploads' completion; an
 * exhausted pool suspends it (ArtifactSuspension::Capacity) until the pool's availability wakes the graph.
 */
template <class Binding>
void RegisterVersionedBufferProducer(async::StateGraph<Binding>& graph, std::span<const typename Binding::ArtifactKind> kinds,
    typename Binding::TaskLane lane, typename Binding::TaskDomain domain, std::string taskName, VersionedBufferProducerHooks<Binding> hooks = {}) {
    using Graph = async::StateGraph<Binding>;
    using Result = typename Graph::ArtifactBuildResult;
    using Suspension = typename Graph::ArtifactSuspension;
    typename Graph::ArtifactProducerRegistration registration;
    registration.lane = lane;
    registration.domain = domain;
    registration.taskName = std::move(taskName);
    registration.producer = [notify = graph.MakeSuspensionNotifier(), hooks = std::move(hooks)](const typename Graph::ArtifactBuildContext& context) {
        const auto input = context.input.template Get<VersionedBufferBuildInput>();
        if (!input) return Result::Failure("versioned buffer input missing");
        VersionedBufferBuildContext build;
        build.revision = context.revision;
        build.stopRequested = context.stopRequested;
        build.allocateSuspension = [] { return async::AllocateArtifactSuspensionIdentity(); };
        build.notifySuspension = notify;
        auto outcome = BuildVersionedBuffer(*input, build);
        switch (outcome.kind) {
        case VersionedBufferBuildOutcome::Kind::Cancelled:
            return Result::Cancelled();
        case VersionedBufferBuildOutcome::Kind::Suspended:
            return Result::Suspend(Suspension::Capacity(outcome.suspension, std::move(outcome.error)));
        case VersionedBufferBuildOutcome::Kind::Failed:
            return Result::Failure(std::move(outcome.error));
        case VersionedBufferBuildOutcome::Kind::Ready:
            break;
        }
        if (!outcome.version) return Result::Failure("versioned buffer build produced no version");
        try {
            if (hooks.decorate) hooks.decorate(*outcome.version, *input);
            std::shared_ptr<const PublishedBufferVersion> version = std::move(outcome.version);
            auto payload = hooks.wrap ? hooks.wrap(version, *input, context) : async::ArtifactPayload::Make(version);
            return Result::Ready(std::move(payload), std::move(outcome.ready));
        } catch (const std::exception& error) {
            return Result::Failure(error.what());
        }
    };
    for (const auto kind : kinds) graph.RegisterProducer(kind, registration);
}

} // namespace org::services
