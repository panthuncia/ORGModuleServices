#include <ORGModuleServices/PipelineService.h>

#include <utility>
#include <chrono>
#include <stdexcept>

namespace org::services {
namespace {
uint64_t Mix(uint64_t seed, uint64_t value) noexcept {
    value ^= value >> 30; value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27; value *= 0x94d049bb133111ebULL; value ^= value >> 31;
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}
uint64_t HashString(std::string_view value) noexcept {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : value) { hash ^= byte; hash *= 1099511628211ULL; }
    return hash;
}
}

uint64_t PipelineService::BuildKey(const PipelineRecipe& recipe) const noexcept {
    uint64_t key = HashString(recipe.id);
    key = Mix(key, recipe.layoutKey); key = Mix(key, recipe.shaderKey);
    key = Mix(key, recipe.fixedFunctionKey); return Mix(key, recipe.deviceKey);
}

std::shared_future<PipelineArtifact> PipelineService::Request(PipelineRecipe recipe) {
    const uint64_t key = BuildKey(recipe);
    std::scoped_lock lock(mutex_);
    if (auto found = active_.find(key); found != active_.end()) {
        std::promise<PipelineArtifact> promise; promise.set_value(found->second); return promise.get_future().share();
    }
    if (auto found = inFlight_.find(key); found != inFlight_.end()) return found->second;
    const uint64_t generation = nextGeneration_++;
	idsByKey_[key] = recipe.id;
    auto future = std::async(std::launch::async, [key, generation, recipe = std::move(recipe)]() mutable {
        PipelineArtifact result{ key, generation };
        try { if (!recipe.build) throw std::runtime_error("pipeline recipe has no factory"); result.payload = recipe.build();
            if (!result.payload) result.error = "pipeline factory returned no payload";
        } catch (const std::exception& e) { result.error = e.what(); } catch (...) { result.error = "pipeline factory failed"; }
        return result;
    }).share();
    inFlight_.emplace(key, future);
    return future;
}

void PipelineService::PublishReady(uint64_t retirementFenceValue) {
    std::scoped_lock lock(mutex_);
    for (auto it = inFlight_.begin(); it != inFlight_.end();) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
        auto artifact = it->second.get();
        if (artifact) {
			const auto id = idsByKey_[it->first];
			if (auto priorKey = activeKeyById_.find(id); priorKey != activeKeyById_.end() && priorKey->second != it->first) {
				if (auto prior = active_.find(priorKey->second); prior != active_.end()) {
					if (prior->second.payload) retired_.push_back({ retirementFenceValue, std::move(prior->second.payload) });
					active_.erase(prior);
				}
			}
            if (auto old = active_.find(it->first); old != active_.end() && old->second.payload)
                retired_.push_back({ retirementFenceValue, std::move(old->second.payload) });
            active_[it->first] = artifact;
			activeKeyById_[id] = it->first;
        }
        ready_[it->first] = std::move(artifact);
        it = inFlight_.erase(it);
    }
}

PipelineArtifact PipelineService::Find(uint64_t key) const {
    std::scoped_lock lock(mutex_); const auto found = active_.find(key);
    return found == active_.end() ? PipelineArtifact{} : found->second;
}
void PipelineService::Retire(uint64_t completedFenceValue) noexcept {
    std::scoped_lock lock(mutex_); while (!retired_.empty() && retired_.front().fence <= completedFenceValue) retired_.pop_front();
}
size_t PipelineService::InFlightCount() const noexcept { std::scoped_lock lock(mutex_); return inFlight_.size(); }
}
