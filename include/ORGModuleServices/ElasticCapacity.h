#pragma once

#include <ORGModuleServices/GrowthPolicy.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace org::services {

/**
 * @brief When a growable capacity gives memory back: once the need stayed at or below `usedPercent` of the capacity for at least
 * `settle` and `observations` asks in a row, it shrinks to what its GrowthPolicy would make fresh for the window's peak need. Doubling
 * makes a fresh capacity at least half used, so with a quarter's threshold a shrunk capacity never shrinks again at once, and it grows
 * back only past the peak: hysteresis, no thrash.
 *
 * - Never() (the default): grow-only, as before.
 * - WhenBelow(pct, settle, observations): the shrinking policy.
 */
struct ShrinkPolicy {
    std::uint32_t usedPercent = 0;  // 0: never shrinks
    std::chrono::milliseconds settle{0};
    std::uint32_t observations = 1;

    [[nodiscard]] static constexpr ShrinkPolicy Never() noexcept { return {}; }
    [[nodiscard]] static constexpr ShrinkPolicy WhenBelow(std::uint32_t pct, std::chrono::milliseconds settle, std::uint32_t observations) noexcept {
        return {pct, settle, observations ? observations : 1u};
    }
    [[nodiscard]] constexpr bool Shrinks() const noexcept { return usedPercent != 0; }
    friend constexpr bool operator==(const ShrinkPolicy&, const ShrinkPolicy&) = default;
};

/**
 * @brief One capacity's run of low asks (ShrinkPolicy's window). Not thread-safe: its owner's (the producer of the capacity). A window
 * whose every ask was 0 shrinks the capacity to 0: an owner that needs at least one unit asks for one.
 */
struct ShrinkWindow {
    using Clock = std::chrono::steady_clock;

    std::uint64_t peak = 0;   // the most asked in the window
    std::uint32_t seen = 0;   // asks in the window (0: none open)
    Clock::time_point since{};

    void Reset() noexcept {
        peak = 0;
        seen = 0;
    }
    /**
     * @brief Whether the next ask no larger than the window's peak would close it now. An owner that asks only when its contents
     * change (a version chain) asks again for this, or a buffer whose scene went quiet would keep its size.
     */
    [[nodiscard]] bool Due(const ShrinkPolicy& policy, Clock::time_point now) const noexcept {
        return policy.Shrinks() && seen && seen + 1 >= policy.observations && now - since >= policy.settle;
    }
    /**
     * @brief One ask of `need` against `capacity`: the smaller capacity to make when the policy's shrink is due (the window closes),
     * else nullopt. An ask above the threshold restarts the window. Never returns `capacity` or more.
     */
    [[nodiscard]] std::optional<std::uint64_t> Observe(const ShrinkPolicy& policy, const GrowthPolicy& growth, std::uint64_t capacity,
        std::uint64_t need, Clock::time_point now) {
        if (!policy.Shrinks() || !capacity) return std::nullopt;
        // need * 100 > capacity * pct, without overflow
        const bool low = need <= capacity / 100 * policy.usedPercent + capacity % 100 * policy.usedPercent / 100;
        if (!low) {
            Reset();
            return std::nullopt;
        }
        if (!seen) {
            since = now;
            peak = need;
        } else if (need > peak) {
            peak = need;
        }
        if (seen < (std::numeric_limits<std::uint32_t>::max)()) ++seen;
        if (seen < policy.observations || now - since < policy.settle) return std::nullopt;
        // Nothing asked through the window: none (a layout's range that no draw uses); else the policy's fresh size for the peak.
        const std::uint64_t target = peak ? growth.Next(0, peak) : 0;
        Reset();
        if (target >= capacity) return std::nullopt;
        return target;
    }
};

/**
 * @brief A capacity held in T that follows its need both ways: grown by `growth` past the need (as GrowthPolicy::Next), shrunk by
 * `shrink`'s window. Returns whether `a_capacity` moved. Throws when a grown capacity does not fit T.
 */
template <class T>
bool FitCapacity(T& a_capacity, ShrinkWindow& a_window, std::uint64_t a_need, const GrowthPolicy& a_growth, const ShrinkPolicy& a_shrink,
    ShrinkWindow::Clock::time_point a_now = ShrinkWindow::Clock::now()) {
    const auto current = static_cast<std::uint64_t>(a_capacity);
    if (a_need > current) {
        a_capacity = a_growth.NextAs<T>(a_capacity, a_need);
        a_window.Reset();
        return static_cast<std::uint64_t>(a_capacity) != current;
    }
    if (const auto target = a_window.Observe(a_shrink, a_growth, current, a_need, a_now)) {
        a_capacity = static_cast<T>(*target);
        return true;
    }
    return false;
}

/** @brief A capacity and its window, for an owner that keeps them together. */
template <class T = std::uint64_t>
struct ElasticCapacity {
    T value{};
    ShrinkWindow window;

    /** @brief FitCapacity: whether the value moved. */
    bool Fit(std::uint64_t need, const GrowthPolicy& growth, const ShrinkPolicy& shrink, ShrinkWindow::Clock::time_point now = ShrinkWindow::Clock::now()) {
        return FitCapacity(value, window, need, growth, shrink, now);
    }
};

} // namespace org::services
