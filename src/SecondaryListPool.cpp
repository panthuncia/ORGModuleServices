#include <ORGModuleServices/SecondaryListPool.h>

#include <utility>

namespace org::services {

rhi::CommandList* SecondaryListPool::Next(rhi::Device& device) {
    if (!m_allocator && device.CreateCommandAllocator(queue, m_allocator) != rhi::Result::Ok) return nullptr;
    if (m_used == m_lists.size()) {
        rhi::CommandListPtr list;
        if (device.CreateSecondaryCommandList(queue, m_allocator.Get(), list) != rhi::Result::Ok) return nullptr;
        m_lists.push_back(std::move(list));
    }
    auto& list = m_lists[m_used++].Get();
    list.Recycle(m_allocator.Get());
    return &list;
}

void SecondaryListPool::Reset() {
    if (m_allocator && m_used) m_allocator->Recycle();
    m_used = 0;
}

} // namespace org::services
