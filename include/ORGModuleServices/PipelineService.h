#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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
    uint64_t BuildKey(const PipelineRecipe& recipe) const noexcept;
    std::shared_future<PipelineArtifact> Request(PipelineRecipe recipe);
    void PublishReady(uint64_t retirementFenceValue);
    PipelineArtifact Find(uint64_t key) const;
    void Retire(uint64_t completedFenceValue) noexcept;
    size_t InFlightCount() const noexcept;

private:
    struct Retired { uint64_t fence{}; PipelinePayload payload; };
    mutable std::mutex mutex_;
    uint64_t nextGeneration_{ 1 };
    std::unordered_map<uint64_t, std::shared_future<PipelineArtifact>> inFlight_;
    std::unordered_map<uint64_t, PipelineArtifact> ready_;
    std::unordered_map<uint64_t, PipelineArtifact> active_;
	std::unordered_map<uint64_t, std::string> idsByKey_;
	std::unordered_map<std::string, uint64_t> activeKeyById_;
    std::deque<Retired> retired_;
};

}
