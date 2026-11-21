#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace org::async {

// One starvation rule. Work that is asked for again and again with no new
// input never lands: that is a schedule bug, not something to wait for. The
// owner notes each new input and each miss. Once the misses since the last
// input pass starvedAfter, the miss reports starvation and the count starts
// again.
//
// One thread owns a watch. It holds no lock.
class StarvationWatch {
public:
    // what names the watched work, why says what happened at the reporting
    // miss, and misses counts the misses since the last input, that one
    // included.
    using OnStarved = std::function<void(std::string_view what, std::string_view why, std::uint64_t misses)>;

    StarvationWatch(std::string what, std::uint64_t starvedAfter, OnStarved onStarved)
        : m_what(std::move(what)), m_starvedAfter(starvedAfter), m_onStarved(std::move(onStarved)) {}

    // A new input: misses before it are not starvation.
    void NoteInput() noexcept { m_misses = 0; }

    // One miss: the work was dropped, or came for other inputs, and is asked
    // for again. Returns true when this miss reported starvation.
    bool NoteMiss(std::string_view why) {
        if (++m_misses <= m_starvedAfter) return false;
        const auto misses = std::exchange(m_misses, 0);
        if (m_onStarved) m_onStarved(m_what, why, misses);
        return true;
    }

    // Misses since the last input or report.
    [[nodiscard]] std::uint64_t Misses() const noexcept { return m_misses; }
    [[nodiscard]] std::uint64_t StarvedAfter() const noexcept { return m_starvedAfter; }
    [[nodiscard]] const std::string& What() const noexcept { return m_what; }

private:
    std::string m_what;
    std::uint64_t m_starvedAfter;
    OnStarved m_onStarved;
    std::uint64_t m_misses = 0;
};

} // namespace org::async
