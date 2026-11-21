#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace org::services {

// What graph builds are made from, as immutable values with a serial, and which value is built and which attached.
//
// The owner thread makes a new value when its live state changes (Next) and gives it to the builds it hands out
// (StagedBuilds::SetBuildInput). A build reads only that value. Once no build of a value is pending, the value is the
// built one (Settle). It is the attached one once the graphs built from it run (PublishAttached). Builds whose graphs
// attach at once (an epoch's first builds, with no graph to keep) make the built value the attached one too.
//
// State is copyable or movable and has a `std::uint64_t serial` member.
template <class State>
class BuildInputs {
public:
    using Ptr = std::shared_ptr<const State>;

    // What Settle did.
    enum class Settled : std::uint8_t {
        None,              // the newest value was built already, or a build is pending
        Built,             // the newest value is the built one now
        BuiltAndAttached,  // built, and the first builds attached with it: the caller publishes it attached
    };

    // Owner thread. next as a new value with the serial after current's, or current itself when same(*current, next)
    // holds, so the serial moves only on a change. current is null before the first value.
    template <class Same>
    static Ptr Next(const Ptr& current, State next, Same&& same) {
        if (current && same(*current, next))
            return current;
        next.serial = (current ? current->serial : 0) + 1;
        return std::make_shared<const State>(std::move(next));
    }

    // Owner thread, after each poll of the builds. serial is the newest value's, anyGraph whether any of the epochs
    // built from it has a graph. settled() says whether no build is pending; it runs only when serial is not built yet.
    // While no epoch has a graph, the next value built is attached too, since first builds attach when they are staged.
    template <class IsSettled>
    Settled Settle(std::uint64_t serial, bool anyGraph, IsSettled&& settled) {
        if (!anyGraph)
            firstBuildsPending_ = true;
        if (serial == BuiltSerial())
            return Settled::None;
        if (!settled())
            return Settled::None;
        PublishBuilt(serial);
        return std::exchange(firstBuildsPending_, false) ? Settled::BuiltAndAttached : Settled::Built;
    }

    // Owner thread: every build serial asked for is staged or attached.
    void PublishBuilt(std::uint64_t serial) { built_.store(serial, std::memory_order_release); }
    // Any thread: the built value's serial, 0 before the first.
    std::uint64_t BuiltSerial() const { return built_.load(std::memory_order_acquire); }

    // Owner thread: state is what the attached graphs are built from. Null is ignored.
    void PublishAttached(Ptr state) {
        if (!state)
            return;
        const auto serial = state->serial;
        attached_.store(std::move(state), std::memory_order_release);
        attachedSerial_.store(serial, std::memory_order_release);
    }
    // Any thread: the attached value, null before the first.
    Ptr Attached() const { return attached_.load(std::memory_order_acquire); }
    // Any thread: the attached value's serial, 0 before the first.
    std::uint64_t AttachedSerial() const { return attachedSerial_.load(std::memory_order_acquire); }

private:
    std::atomic<Ptr> attached_;
    std::atomic<std::uint64_t> attachedSerial_{0};
    std::atomic<std::uint64_t> built_{0};
    bool firstBuildsPending_ = false;  // owner thread
};

} // namespace org::services
