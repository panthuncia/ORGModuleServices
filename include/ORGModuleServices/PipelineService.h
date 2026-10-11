#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <ORGModuleServices/ServiceExecutor.h>

namespace org::services {

using PipelinePayload = std::shared_ptr<void>;
using PipelineFactory = std::function<PipelinePayload()>;

struct PipelineRecipe {
    std::string id;
    uint64_t layoutKey{};
    uint64_t shaderKey{};
    uint64_t fixedFunctionKey{};
    uint64_t deviceKey{};
    PipelineFactory build;
};

struct PipelineArtifact {
    uint64_t key{};
    uint64_t generation{};
    PipelinePayload payload;
    std::string error;
    explicit operator bool() const noexcept { return payload != nullptr; }
};

class PipelineService {
public:
    using Submit = ServiceSubmit;
    // Receives the artifact of a Request. A failure is an artifact without a payload whose error says why.
    using CompletionCallback = std::function<void(const PipelineArtifact&)>;

    uint64_t BuildKey(const PipelineRecipe& recipe) const noexcept;
    std::shared_future<PipelineArtifact> Request(PipelineRecipe recipe);
    // As above, and invokes onComplete exactly once with the artifact, after it is available through the returned
    // future, never while the service holds its lock (it may call back into the service):
    // - an active (published) pipeline: at once, on the calling thread, and no work is submitted;
    // - joining a build in flight for the same key: on the thread that completes it, or at once on the calling thread
    //   when it has already completed (a completed build stays in flight until PublishReady);
    // - otherwise: on the thread that runs the build (the submitter's, see Configure); a refused submission completes
    //   at once on the calling thread with an error artifact.
    std::shared_future<PipelineArtifact> Request(PipelineRecipe recipe, CompletionCallback onComplete);
    // Installs the executor every build is submitted to; empty restores the default (one new thread per build).
    // Setup: call before the service is shared with other threads. Requests submitted earlier keep their executor.
    void Configure(Submit submit);
    void PublishReady(uint64_t retirementFenceValue);
    PipelineArtifact Find(uint64_t key) const;
    void Retire(uint64_t completedFenceValue) noexcept;
    size_t InFlightCount() const noexcept;

private:
    struct Retired { uint64_t fence{}; PipelinePayload payload; };
    struct Flight;
    mutable std::mutex mutex_;
    uint64_t nextGeneration_{ 1 };
    Submit submit_;
    std::unordered_map<uint64_t, std::shared_ptr<Flight>> inFlight_;
    std::unordered_map<uint64_t, PipelineArtifact> ready_;
    std::unordered_map<uint64_t, PipelineArtifact> active_;
	std::unordered_map<uint64_t, std::string> idsByKey_;
	std::unordered_map<std::string, uint64_t> activeKeyById_;
    std::deque<Retired> retired_;
};

}
