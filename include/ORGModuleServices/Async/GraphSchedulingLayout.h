#pragma once

#include <ORGModuleServices/Async/GraphScheduler.h>
#include <cstddef>
#include <optional>
#include <stdexcept>

namespace org::async {

// Immutable startup configuration. Counts and class IDs come from the host;
// the graph never derives its queue dimensions from a renderer enum.
struct GraphSchedulingLayout {
    std::uint32_t laneCount = 1;
    std::uint32_t domainCount = 1;
    TaskClass control{};
    TaskClass legacyAcceptance{};

    [[nodiscard]] bool Contains(TaskClass cls) const noexcept {
        return cls.lane < laneCount && cls.domain < domainCount;
    }
    [[nodiscard]] bool Valid() const noexcept {
        // Bound allocation and index arithmetic. This is an admission limit,
        // not an inventory of host domains. Raise deliberately if a host needs it.
        return laneCount != 0 && domainCount != 0 &&
            laneCount <= 256 && domainCount <= 256 &&
            Contains(control) && Contains(legacyAcceptance);
    }
    [[nodiscard]] std::size_t MailboxCount() const noexcept {
        return static_cast<std::size_t>(laneCount) * domainCount;
    }
    [[nodiscard]] std::optional<std::size_t> Index(TaskClass cls) const noexcept {
        if (!Valid() || !Contains(cls)) return {};
        return static_cast<std::size_t>(cls.domain) * laneCount + cls.lane;
    }
    [[nodiscard]] TaskClass ClassAt(std::size_t index) const {
        if (!Valid() || index >= MailboxCount()) throw std::out_of_range("graph mailbox index");
        return {static_cast<std::uint32_t>(index % laneCount),
            static_cast<std::uint32_t>(index / laneCount)};
    }
};

} // namespace org::async
