#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace org::services {

struct DescriptorViewKey {
    uint64_t resourceIdentity{};
    uint64_t descriptionHash{};
    uint32_t heapType{};
    bool operator==(const DescriptorViewKey&) const = default;
};
struct DescriptorViewKeyHash {
    size_t operator()(const DescriptorViewKey& key) const noexcept;
};
struct DescriptorView {
    uint64_t cpuHandle{};
    uint64_t gpuHandle{};
    std::shared_ptr<void> lifetime;
};
struct DescriptorViewHandle {
    uint32_t index{ UINT32_MAX };
    uint32_t generation{};
    explicit operator bool() const noexcept { return index != UINT32_MAX && generation != 0; }
};
using DescriptorViewFactory = std::function<std::optional<DescriptorView>()>;

class DescriptorViewCache {
public:
    DescriptorViewHandle GetOrCreate(const DescriptorViewKey& key, DescriptorViewFactory factory);
    std::optional<DescriptorView> Resolve(DescriptorViewHandle handle) const;
    void InvalidateResource(uint64_t resourceIdentity);
    void Clear();
    size_t Size() const noexcept;

private:
    struct Slot { uint32_t generation{ 1 }; bool occupied{}; DescriptorViewKey key{}; DescriptorView view{}; };
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
    std::unordered_map<DescriptorViewKey, uint32_t, DescriptorViewKeyHash> byKey_;
};

}
