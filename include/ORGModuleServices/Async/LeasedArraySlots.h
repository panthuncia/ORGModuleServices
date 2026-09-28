#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace org::async {

// Storage for an externally synchronized slot exchange. The exchange, not this
// class, grants exclusive ownership of each slot to its writer or reader.
// A reader may retain the immutable lease after giving the slot back. The next
// writer then replaces its backing instead of modifying an outstanding lease.
template <class T, std::size_t SlotCount>
class LeasedArraySlots {
public:
    using Lease = std::shared_ptr<const std::vector<T>>;

    // The caller must overwrite the entire returned span before publishing the
    // slot. This intentionally does not copy the previous generation's contents.
    [[nodiscard]] std::span<T> PrepareWrite(std::size_t slot, std::size_t count) {
        auto& backing = m_slots.at(slot);
        if (!backing || backing.use_count() != 1) backing = std::make_shared<std::vector<T>>(count);
        else backing->resize(count);
        return *backing;
    }

    // Only while the caller owns the read slot, after acquiring the publication.
    [[nodiscard]] Lease Acquire(std::size_t slot) const { return m_slots.at(slot); }

private:
    std::array<std::shared_ptr<std::vector<T>>, SlotCount> m_slots;
};

} // namespace org::async
