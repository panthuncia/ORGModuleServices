#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>
#include <wrl/client.h>

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12DescriptorHeap;

namespace org::services {

struct UploadAllocation {
    void* cpuAddress{};
    uint64_t gpuAddress{};
    uint64_t size{};
    ID3D12Resource* resource{};
};

struct DescriptorAllocation {
    uint64_t cpuHandle{};
    uint64_t gpuHandle{};
    uint32_t descriptorSize{};
    ID3D12DescriptorHeap* heap{};
};

// A correctness-first D3D12 upload arena. Allocations are submission-owned and
// released only when the exact completion value for that submission has passed.
class FrameUploadArena {
public:
    explicit FrameUploadArena(ID3D12Device* device);
    UploadAllocation Allocate(uint64_t size, uint64_t alignment, uint64_t completionValue);
    DescriptorAllocation AllocateDescriptors(uint32_t heapType, uint32_t count, uint64_t completionValue);
    void Retire(uint64_t completedValue) noexcept;
    uint64_t BytesInFlight() const noexcept;

private:
    struct Block {
        uint64_t completionValue{};
        uint64_t size{};
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    };
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    mutable std::mutex mutex_;
    std::deque<Block> blocks_;
    uint64_t bytesInFlight_{};
};

}
