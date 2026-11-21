#pragma once

#include <rhi.h>

#include <cstddef>
#include <deque>

namespace org::services {

/**
 * @brief One frame slot's secondary command lists, from a command allocator of their own. Reset when the slot retires, through
 * ResetPools (RecordingPools.h), since a reset is a driver call. One thread at a time.
 */
class SecondaryListPool {
public:
    /** @brief A recycled secondary for recording, made on `device` at first use, or null when one could not be made. */
    rhi::CommandList* Next(rhi::Device& device);
    /** @brief Recycles the allocator. None of the lists may be in use. */
    void Reset();
    /** @brief The lists handed out since the last Reset. */
    [[nodiscard]] std::size_t Used() const noexcept { return m_used; }

    // The queue the lists are executed on. Set before the first Next.
    rhi::QueueKind queue = rhi::QueueKind::Graphics;

private:
    rhi::CommandAllocatorPtr m_allocator;
    std::deque<rhi::CommandListPtr> m_lists;  // never moved, since a recorder may hold the open one by address
    std::size_t m_used = 0;
};

} // namespace org::services
