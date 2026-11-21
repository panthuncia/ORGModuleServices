#pragma once

#include <ORGModuleServices/Async/StateGraph.h>
#include <ORGModuleServices/VersionedBufferBuild.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace org::services {

/** @brief A versioned buffer build's outcome on the done lane: the version once GPU ready, or null and why. */
using VersionedBufferDone = std::function<void(std::shared_ptr<const PublishedBufferVersion> version, std::string error)>;

template <class Binding>
class VersionedBufferPosts;
template <class Binding>
bool PostVersionedBuffer(async::StateGraph<Binding>& graph, VersionedBufferPosts<Binding>& posts, VersionedBufferBuildInput input, VersionedBufferDone done);

/**
 * @brief One poster's versioned buffer builds on a graph whose producer for the kind is RegisterVersionedBufferProducer's,
 * without a wrap hook (the payload is the version). It hands out the address ids and keeps the waits. One thread posts;
 * it drops the waits that finished at its next post. The host keeps the kind exact (allowCoalescing false), since a
 * buffer's versions form a chain built one from the next.
 */
template <class Binding>
class VersionedBufferPosts {
public:
    VersionedBufferPosts(typename Binding::ArtifactKind kind, typename Binding::TaskLane doneLane, typename Binding::TaskDomain doneDomain)
        : m_kind(kind), m_doneLane(doneLane), m_doneDomain(doneDomain) {}
    VersionedBufferPosts(const VersionedBufferPosts&) = delete;
    VersionedBufferPosts& operator=(const VersionedBufferPosts&) = delete;

private:
    template <class B>
    friend bool PostVersionedBuffer(async::StateGraph<B>& graph, VersionedBufferPosts<B>& posts, VersionedBufferBuildInput input, VersionedBufferDone done);
    struct Wait {
        typename async::StateGraph<Binding>::ArtifactAwaiter awaiter;
        std::atomic<bool> finished = false;
    };
    typename Binding::ArtifactKind m_kind;
    typename Binding::TaskLane m_doneLane;
    typename Binding::TaskDomain m_doneDomain;
    std::vector<std::shared_ptr<Wait>> m_waits;
    std::uint64_t m_ids = 0;
};

/**
 * @brief Posts a versioned buffer's next version, lock-free, each at an address of its own, released once `done` ran on
 * the done lane at GPU readiness or the build's end. False when the graph refused it (shutting down): `done` does not run.
 * The posting thread only.
 */
template <class Binding>
bool PostVersionedBuffer(async::StateGraph<Binding>& graph, VersionedBufferPosts<Binding>& posts, VersionedBufferBuildInput input, VersionedBufferDone done) {
    using Graph = async::StateGraph<Binding>;
    using Wait = typename VersionedBufferPosts<Binding>::Wait;
    std::erase_if(posts.m_waits, [](const auto& wait) { return wait->finished.load(std::memory_order_acquire); });
    const typename Graph::ArtifactKey key{posts.m_kind, ++posts.m_ids, 0};
    typename Graph::ArtifactIntent intent;
    intent.key = key;
    intent.desiredRevision = 1;
    intent.input = async::ArtifactPayload::Make(std::make_shared<const VersionedBufferBuildInput>(std::move(input)));
    intent.requestFingerprint = key.primaryID;  // an input needs one, and each version is its own
    const auto request = graph.PostRequest(std::move(intent), false);
    if (!request) return false;
    auto wait = std::make_shared<Wait>();
    // The done lane runs exactly one of these, once. Nothing else requests the address, so it is released after.
    auto finish = [&graph, key, done = std::move(done), weak = std::weak_ptr<Wait>(wait)](
                      std::shared_ptr<const PublishedBufferVersion> version, std::string error) {
        done(std::move(version), std::move(error));
        graph.Release(key);
        if (const auto held = weak.lock()) held->finished.store(true, std::memory_order_release);
    };
    wait->awaiter = graph.AwaitExact(request.Handle(), async::ArtifactReadiness::GpuReady, posts.m_doneLane, posts.m_doneDomain,
        [finish](const typename Graph::ArtifactSnapshot& snapshot) {
            auto version = snapshot.payload.template Get<PublishedBufferVersion>();
            if (snapshot.gpuSubmissions && snapshot.gpuSubmissions->Failed())
                finish(nullptr, snapshot.gpuSubmissions->Failure());
            else if (!version)
                finish(nullptr, "the build made no version");
            else
                finish(std::move(version), {});
        },
        [finish](const typename Graph::ArtifactTermination& termination) {
            finish(nullptr, termination.error.empty() ? std::string("the build ended without a version") : termination.error);
        });
    posts.m_waits.push_back(std::move(wait));
    return true;
}

} // namespace org::services
