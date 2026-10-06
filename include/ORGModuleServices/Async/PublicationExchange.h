#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace org::async {

// One coordinator publishes complete immutable successors; one frame owner
// selects them. Three slots cover active, ready, and retired ownership. No
// atomic<shared_ptr>, allocation, scan, or last-reference release occurs during
// selection. This is NOT a GPU readiness gate: the acceptance predicate must
// reserve executable capacity and validate the candidate before ownership moves.
//
// Thread contract: TryPublish/ReclaimRetired are producer-only. TrySelect,
// Active, AcquireActive and ClearActive are consumer-only. Destruction requires
// both threads to be quiescent. Frame/GPU leases must be released on a cleanup
// lane; this exchange only controls its own references.
template <class T>
class PublicationExchange {
public:
    using Lease = std::shared_ptr<const T>;
    enum class Selection { Unchanged, Selected, Rejected };

    PublicationExchange() = default;
    PublicationExchange(const PublicationExchange&) = delete;
    PublicationExchange& operator=(const PublicationExchange&) = delete;

    // False leaves ownership with the coordinator: there is already a ready
    // successor or no free slot. The caller may replace its pending intent, but
    // must never partially edit the published candidate to make room.
    [[nodiscard]] bool TryPublish(Lease& candidate) {
        if (!candidate || m_ready.load(std::memory_order_acquire) != kNone) return false;
        ReclaimRetired();
        for (unsigned i = 0; i < m_slots.size(); ++i) {
            auto& slot = m_slots[i];
            if (slot.state.load(std::memory_order_acquire) != State::Empty) continue;
            slot.value = std::move(candidate);
            slot.state.store(State::Ready, std::memory_order_release);
            m_ready.store(i, std::memory_order_release);
            return true;
        }
        return false;
    }

    // Latest wins: takes back a ready successor the consumer has not selected
    // yet (retired here, released by ReclaimRetired) and publishes the
    // candidate in its place. Either the consumer's exchange or this one takes
    // the ready index, so a successor is never both selected and replaced.
    // With the taken-back slot reclaimed, a slot is always free.
    [[nodiscard]] bool TryReplace(Lease& candidate) {
        if (!candidate) return false;
        const auto ready = m_ready.exchange(kNone, std::memory_order_acq_rel);
        if (ready != kNone) m_slots[ready].state.store(State::Retired, std::memory_order_release);
        return TryPublish(candidate);
    }

    template <class Accept>
    [[nodiscard]] Selection TrySelect(Accept&& accept) noexcept {
        static_assert(std::is_nothrow_invocable_r_v<bool, Accept, const T&>,
            "Publication admission must be nonthrowing and nonblocking");
        const auto ready = m_ready.exchange(kNone, std::memory_order_acq_rel);
        if (ready == kNone) return Selection::Unchanged;
        auto& slot = m_slots[ready];
        if (!std::forward<Accept>(accept)(*slot.value)) {
            slot.state.store(State::Retired, std::memory_order_release);
            return Selection::Rejected;
        }
        const auto previous = m_active;
        m_active = ready;
        slot.state.store(State::Active, std::memory_order_release);
        if (previous != kNone) m_slots[previous].state.store(State::Retired, std::memory_order_release);
        return Selection::Selected;
    }

    [[nodiscard]] const T* Active() const noexcept {
        return m_active == kNone ? nullptr : m_slots[m_active].value.get();
    }
    [[nodiscard]] Lease AcquireActive() const noexcept {
        return m_active == kNone ? Lease{} : m_slots[m_active].value;
    }

    // E.g. world reset: revoke active ownership immediately, release its storage
    // later on the coordinator. Ready candidates are still validated/rejected
    // by their generation at the next selection.
    void ClearActive() noexcept {
        if (m_active == kNone) return;
        const auto previous = std::exchange(m_active, kNone);
        m_slots[previous].state.store(State::Retired, std::memory_order_release);
    }

    void ReclaimRetired() noexcept {
        for (auto& slot : m_slots) {
            if (slot.state.load(std::memory_order_acquire) != State::Retired) continue;
            slot.value.reset();
            slot.state.store(State::Empty, std::memory_order_release);
        }
    }

private:
    enum class State : unsigned { Empty, Ready, Active, Retired };
    static constexpr unsigned kNone = 3;
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    static_assert(std::atomic<State>::is_always_lock_free);
    struct Slot {
        Lease value;
        std::atomic<State> state{State::Empty};
    };
    std::array<Slot, 3> m_slots;
    std::atomic<unsigned> m_ready{kNone};
    unsigned m_active = kNone;
};

} // namespace org::async
