#pragma once

#include <ORGModuleServices/Async/StateGraph.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace org::async {

/**
 * @brief A growth as graph work (the SARP/BasicRenderer model, BuildVersionedGpuBuffer): an artifact of the host's version kind
 * whose producer makes the versions and queues their fills on the dedicated uploader (VersionedBufferGrowth's GrowVersions), and
 * is ready once those copies completed (the uploader's tickets notify); `done` then runs on the poster's lane. Type-erased, so the
 * graph's library need not see ORG's buffers.
 */
struct VersionGrowthWork {
    struct Produced {
        std::shared_ptr<const void> value;           // what `done` is given
        std::shared_ptr<const GpuSubmissionSet> ready;  // null: ready now
    };
    std::function<Produced()> produce;  // the producer's lane; throws when a version cannot be made or filled
    std::function<void(std::shared_ptr<const void> value, std::string error)> done;  // the done lane, once: null and why on failure
};

/**
 * @brief Registers the producer of `kind`: a growth's work on (lane, domain), ready with its uploads' completion (the graph waits
 * for it, nothing else does). The host's hooks keep `kind` exact (allowCoalescing false): each growth is its own address.
 */
template <class Binding>
void RegisterVersionGrowthProducer(StateGraph<Binding>& graph, typename Binding::ArtifactKind kind, typename Binding::TaskLane lane,
    typename Binding::TaskDomain domain, std::string taskName) {
    using Graph = StateGraph<Binding>;
    typename Graph::ArtifactProducerRegistration registration;
    registration.lane = lane;
    registration.domain = domain;
    registration.taskName = std::move(taskName);
    registration.producer = [](const typename Graph::ArtifactBuildContext& context) {
        using Result = typename Graph::ArtifactBuildResult;
        const auto work = context.input.template Get<VersionGrowthWork>();
        if (!work || !work->produce) return Result::Failure("a growth without its work");
        try {
            auto produced = work->produce();
            if (!produced.value) return Result::Failure("a growth produced nothing");
            auto ready = produced.ready;
            return Result::Ready(ArtifactPayload::Make(std::make_shared<const VersionGrowthWork::Produced>(std::move(produced))), std::move(ready));
        } catch (const std::exception& error) {
            return Result::Failure(error.what());
        }
    };
    graph.RegisterProducer(kind, std::move(registration));
}

template <class Binding>
class VersionGrowthPosts;
template <class Binding>
bool PostVersionGrowth(StateGraph<Binding>& graph, VersionGrowthPosts<Binding>& posts, VersionGrowthWork work);

/**
 * @brief One poster's growths on a graph: the address ids it hands out and the waits it keeps. One thread posts; it drops the
 * waits that finished at its next post.
 */
template <class Binding>
class VersionGrowthPosts {
public:
    VersionGrowthPosts(typename Binding::ArtifactKind kind, typename Binding::TaskLane doneLane, typename Binding::TaskDomain doneDomain)
        : m_kind(kind), m_doneLane(doneLane), m_doneDomain(doneDomain) {}
    VersionGrowthPosts(const VersionGrowthPosts&) = delete;
    VersionGrowthPosts& operator=(const VersionGrowthPosts&) = delete;

private:
    template <class B>
    friend bool PostVersionGrowth(StateGraph<B>& graph, VersionGrowthPosts<B>& posts, VersionGrowthWork work);
    struct Wait {
        typename StateGraph<Binding>::ArtifactAwaiter awaiter;
        std::atomic<bool> finished = false;
    };
    typename Binding::ArtifactKind m_kind;
    typename Binding::TaskLane m_doneLane;
    typename Binding::TaskDomain m_doneDomain;
    std::vector<std::shared_ptr<Wait>> m_waits;
    std::uint64_t m_ids = 0;
};

/**
 * @brief Posts a growth (lock-free: the request and its wait are posted to the graph's drain), each at an address of its own,
 * released once `done` ran. False when the graph refused it (shutting down): `done` does not run. The posting thread only.
 */
template <class Binding>
bool PostVersionGrowth(StateGraph<Binding>& graph, VersionGrowthPosts<Binding>& posts, VersionGrowthWork work) {
    using Graph = StateGraph<Binding>;
    using Wait = typename VersionGrowthPosts<Binding>::Wait;
    std::erase_if(posts.m_waits, [](const auto& wait) { return wait->finished.load(std::memory_order_acquire); });
    auto done = std::move(work.done);
    const typename Graph::ArtifactKey key{posts.m_kind, ++posts.m_ids, 0};
    typename Graph::ArtifactIntent intent;
    intent.key = key;
    intent.desiredRevision = 1;
    intent.input = ArtifactPayload::Make(std::make_shared<const VersionGrowthWork>(std::move(work)));
    intent.requestFingerprint = key.primaryID;  // an input needs one: each growth is its own
    const auto request = graph.PostRequest(std::move(intent), false);
    if (!request) return false;
    auto wait = std::make_shared<Wait>();
    // The done lane runs exactly one of these, once; the address is released after (nothing else requests it).
    auto finish = [&graph, key, done, weak = std::weak_ptr<Wait>(wait)](std::shared_ptr<const void> value, std::string error) {
        done(std::move(value), std::move(error));
        graph.Release(key);
        if (const auto held = weak.lock()) held->finished.store(true, std::memory_order_release);
    };
    wait->awaiter = graph.AwaitExact(request.Handle(), ArtifactReadiness::GpuReady, posts.m_doneLane, posts.m_doneDomain,
        [finish](const typename Graph::ArtifactSnapshot& snapshot) {
            const auto produced = snapshot.payload.template Get<VersionGrowthWork::Produced>();
            if (produced && produced->ready && produced->ready->Failed())
                finish(nullptr, produced->ready->Failure());
            else
                finish(produced ? produced->value : nullptr, produced ? std::string() : std::string("no version"));
        },
        [finish](const typename Graph::ArtifactTermination& termination) {
            finish(nullptr, termination.error.empty() ? std::string("the growth ended without a version") : termination.error);
        });
    posts.m_waits.push_back(std::move(wait));
    return true;
}

} // namespace org::async
