#include <ORGModuleServices/FrameUploadArena.h>

#include <d3d12.h>
#include <stdexcept>

namespace org::services {

FrameUploadArena::FrameUploadArena(ID3D12Device* device) : device_(device) {
    if (!device_) throw std::invalid_argument("FrameUploadArena requires a D3D12 device");
}

UploadAllocation FrameUploadArena::Allocate(uint64_t size, uint64_t alignment, uint64_t completionValue) {
    if (!size || !completionValue) throw std::invalid_argument("Invalid frame upload allocation");
    alignment = alignment ? alignment : 1;
    const uint64_t allocationSize = (size + alignment - 1) / alignment * alignment;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = allocationSize; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource))))
        throw std::runtime_error("D3D12 frame upload allocation failed");
    void* cpu{}; D3D12_RANGE noRead{};
    if (FAILED(resource->Map(0, &noRead, &cpu))) throw std::runtime_error("D3D12 frame upload map failed");
    UploadAllocation result{ cpu, resource->GetGPUVirtualAddress(), size, resource.Get() };
    std::scoped_lock lock(mutex_);
    bytesInFlight_ += allocationSize;
    Block block{}; block.completionValue = completionValue; block.size = allocationSize; block.resource = std::move(resource);
    blocks_.push_back(std::move(block));
    return result;
}

DescriptorAllocation FrameUploadArena::AllocateDescriptors(uint32_t heapType, uint32_t count, uint64_t completionValue) {
    if (!count || !completionValue || heapType > D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
        throw std::invalid_argument("Invalid descriptor allocation");
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(heapType); desc.NumDescriptors = count;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap))))
        throw std::runtime_error("D3D12 frame descriptor allocation failed");
    DescriptorAllocation result{ heap->GetCPUDescriptorHandleForHeapStart().ptr,
        heap->GetGPUDescriptorHandleForHeapStart().ptr, device_->GetDescriptorHandleIncrementSize(desc.Type), heap.Get() };
    std::scoped_lock lock(mutex_);
    Block block{}; block.completionValue = completionValue; block.descriptorHeap = std::move(heap);
    blocks_.push_back(std::move(block));
    return result;
}

void FrameUploadArena::Retire(uint64_t completedValue) noexcept {
    std::scoped_lock lock(mutex_);
    while (!blocks_.empty() && blocks_.front().completionValue <= completedValue) {
		if (blocks_.front().resource) blocks_.front().resource->Unmap(0, nullptr);
        bytesInFlight_ -= blocks_.front().size;
        blocks_.pop_front();
    }
}

uint64_t FrameUploadArena::BytesInFlight() const noexcept {
    std::scoped_lock lock(mutex_);
    return bytesInFlight_;
}

}
