#include <ORGModuleServices/ContentOwners.h>

#include <algorithm>
#include <iterator>

namespace org::services {

void ContentOwners::Set(const void* target, std::uint64_t offset, std::uint64_t bytes, std::span<const std::shared_ptr<const void>> owners) {
    if (!bytes || (m_ranges.empty() && owners.empty()))
        return;
    const std::uint64_t end = offset + bytes;
    // Same range and same owners, as for a row rewritten every frame with the same bindings.
    if (const auto same = m_ranges.find({target, offset});
        same != m_ranges.end() && same->second.bytes == bytes && std::ranges::equal(same->second.owners, owners))
        return;
    // Visit every overlapping range, starting with the one that begins before offset.
    auto it = m_ranges.lower_bound({target, offset});
    if (it != m_ranges.begin()) {
        const auto before = std::prev(it);
        if (before->first.first == target && before->first.second + before->second.bytes > offset)
            it = before;
    }
    while (it != m_ranges.end() && it->first.first == target && it->first.second < end) {
        for (auto& owner : it->second.owners)
            m_replaced.push_back(std::move(owner));
        it = m_ranges.erase(it);
        m_changed = true;
    }
    if (!owners.empty()) {
        m_ranges.insert_or_assign({target, offset}, Range{bytes, {owners.begin(), owners.end()}});
        m_changed = true;
    }
}

std::shared_ptr<const void> ContentOwners::Snapshot(const Seal& seal) {
    if (!m_changed)
        return m_sealed;
    std::vector<std::shared_ptr<const void>> owners = std::exchange(m_replaced, {});
    for (const auto& [key, range] : m_ranges)
        owners.insert(owners.end(), range.owners.begin(), range.owners.end());
    m_sealed = seal ? seal(std::move(owners)) : nullptr;
    m_changed = false;
    ++m_snapshotsMade;
    return m_sealed;
}

ContentOwners::Counts ContentOwners::TakeCounts() {
    Counts counts{.ranges = m_ranges.size(), .snapshotsMade = std::exchange(m_snapshotsMade, 0)};
    for (const auto& [key, range] : m_ranges)
        counts.owners += range.owners.size();
    return counts;
}

} // namespace org::services
