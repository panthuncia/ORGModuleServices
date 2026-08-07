#include <ORGModuleServices/DescriptorViewCache.h>

namespace org::services {
namespace { uint64_t Mix(uint64_t seed, uint64_t value) noexcept { return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2)); } }
size_t DescriptorViewKeyHash::operator()(const DescriptorViewKey& key) const noexcept {
    return static_cast<size_t>(Mix(Mix(key.resourceIdentity, key.descriptionHash), key.heapType));
}
DescriptorViewHandle DescriptorViewCache::GetOrCreate(const DescriptorViewKey& key, DescriptorViewFactory factory) {
    {
        std::scoped_lock lock(mutex_);
        if (const auto found = byKey_.find(key); found != byKey_.end()) return { found->second, slots_[found->second].generation };
    }
    auto view = factory ? factory() : std::nullopt; if (!view) return {};
    std::scoped_lock lock(mutex_);
    if (const auto found = byKey_.find(key); found != byKey_.end()) return { found->second, slots_[found->second].generation };
    uint32_t index{};
    if (free_.empty()) { index = static_cast<uint32_t>(slots_.size()); slots_.emplace_back(); }
    else { index = free_.back(); free_.pop_back(); }
    auto& slot = slots_[index]; slot.occupied = true; slot.key = key; slot.view = std::move(*view); byKey_[key] = index;
    return { index, slot.generation };
}
std::optional<DescriptorView> DescriptorViewCache::Resolve(DescriptorViewHandle handle) const {
    std::scoped_lock lock(mutex_);
    if (!handle || handle.index >= slots_.size()) return std::nullopt;
    const auto& slot = slots_[handle.index]; if (!slot.occupied || slot.generation != handle.generation) return std::nullopt; return slot.view;
}
void DescriptorViewCache::InvalidateResource(uint64_t identity) {
    std::scoped_lock lock(mutex_);
    for (uint32_t index = 0; index < slots_.size(); ++index) { auto& slot = slots_[index];
        if (!slot.occupied || slot.key.resourceIdentity != identity) continue;
        byKey_.erase(slot.key); slot.view = {}; slot.occupied = false; if (++slot.generation == 0) ++slot.generation; free_.push_back(index);
    }
}
void DescriptorViewCache::Clear() {
    std::scoped_lock lock(mutex_); byKey_.clear(); free_.clear();
    for (uint32_t index = 0; index < slots_.size(); ++index) { auto& slot = slots_[index]; slot.view = {}; slot.occupied = false;
        if (++slot.generation == 0) ++slot.generation; free_.push_back(index); }
}
size_t DescriptorViewCache::Size() const noexcept { std::scoped_lock lock(mutex_); return byKey_.size(); }
}
