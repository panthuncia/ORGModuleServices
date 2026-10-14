#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace org::services {

/**
 * @brief How a growable buffer's capacity grows: the capacity to make for a need the current one does not hold. Next never clamps
 * and never returns less than the need: a capacity past what its type or the device holds is a hard failure (it throws), never a
 * smaller buffer.
 *
 * - Doubling(min): the current capacity (at least `min`), doubled until it holds the need: a filling table grows a handful of
 *   times, not every frame.
 * - Headroom(pct, min): the need plus `pct` percent of it (at least `min`): a table sized from what it last held grows ahead of it.
 */
struct GrowthPolicy {
    enum class Kind : std::uint8_t { Doubling, Headroom };
    Kind kind = Kind::Doubling;
    std::uint32_t percent = 0;  // Headroom's
    std::uint64_t minimum = 1;

    [[nodiscard]] static constexpr GrowthPolicy Doubling(std::uint64_t min = 1) noexcept { return {Kind::Doubling, 0, min}; }
    [[nodiscard]] static constexpr GrowthPolicy Headroom(std::uint32_t pct, std::uint64_t min = 1) noexcept { return {Kind::Headroom, pct, min}; }

    /** @brief The capacity for `needed`: `current` (at least the minimum) while it holds it, else grown by the policy. */
    [[nodiscard]] constexpr std::uint64_t Next(std::uint64_t current, std::uint64_t needed) const {
        constexpr auto kMax = (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t capacity = current > minimum ? current : minimum;
        if (capacity >= needed) return capacity;
        if (kind == Kind::Headroom) {
            const std::uint64_t extra = needed / 100 * percent + needed % 100 * percent / 100;
            if (extra > kMax - needed) throw std::length_error("GrowthPolicy: a capacity past 64 bits");
            return needed + extra > minimum ? needed + extra : minimum;
        }
        if (!capacity) capacity = 1;
        while (capacity < needed) {
            if (capacity > kMax / 2) throw std::length_error("GrowthPolicy: a capacity past 64 bits");
            capacity *= 2;
        }
        return capacity;
    }
    /** @brief Next for a capacity held in T; throws when it does not fit T. */
    template <class T>
    [[nodiscard]] constexpr T NextAs(T current, std::uint64_t needed) const {
        const std::uint64_t capacity = Next(static_cast<std::uint64_t>(current), needed);
        if (capacity > static_cast<std::uint64_t>((std::numeric_limits<T>::max)())) throw std::length_error("GrowthPolicy: a capacity past its type");
        return static_cast<T>(capacity);
    }
};

} // namespace org::services
