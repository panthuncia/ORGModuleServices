#pragma once

#include <ORGModuleServices/Async/PublicationExchange.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <typeinfo>
#include <utility>
#include <vector>

namespace org::async {

// Revision assembly: the "same update" primitive. A revision names one exact
// artifact version (a fragment) per slot. It is published only once every
// fragment it names is ready, and the frame selects it as one cut, so
// fragments that share a parent (an epoch's recording and the layout it was
// recorded against) are never split across selections.
//
//   coordinator:  Begin -> Set(slot, fragment)... -> Seal; Collect
//   producers:    fragment->Resolve(value) or fragment->Fail(error), any thread
//   frame owner:  TrySelect, Active, AcquireActive
//
// - Closure: a fragment may require exact fragments in other slots. Seal
//   rejects a draft that names it beside anything else.
// - Unchanged slots inherit the newest sealed revision's fragments, pending
//   ones included, so a newer revision shares the work in flight rather than
//   restarting it.
// - Supersession: the newest complete revision wins. Older pending revisions
//   are abandoned when it is collected, and an older one completing later is
//   superseded. A newer revision never cancels an older one before it is
//   complete, so steady changes cannot starve publication.
// - Failure: a failed fragment fails every revision naming it. The active
//   revision stays; Collect reports the failure, and the coordinator names a
//   new fragment for the slot (later drafts inherit the failed one otherwise).
//
// Lock-free: fragments settle with one atomic exchange of their waiter list;
// revisions count down atomically and push themselves onto a completion stack,
// then call the assembler's notify (from the settling thread; keep it cheap,
// e.g. a post to the coordinator). No lock, no wait.
class RevisionFragment;

namespace detail {
struct PendingRevision;
struct RevisionShared;
void SettleWaiter(const std::shared_ptr<PendingRevision>& revision, const RevisionFragment& fragment);
} // namespace detail

class RevisionFragment {
public:
    enum class State : std::uint8_t { Pending, Ready, Failed };
    // This fragment is valid only in a revision whose `slot` holds exactly `fragment`.
    struct Requirement {
        std::uint32_t slot = 0;
        std::shared_ptr<const RevisionFragment> fragment;
    };

    explicit RevisionFragment(std::vector<Requirement> requirements = {})
        : m_requirements(std::move(requirements)) {}
    template <class T>
    [[nodiscard]] static std::shared_ptr<RevisionFragment> MakeReady(std::shared_ptr<T> value,
        std::vector<Requirement> requirements = {}) {
        auto fragment = std::make_shared<RevisionFragment>(std::move(requirements));
        (void)fragment->Resolve(std::move(value));
        return fragment;
    }
    RevisionFragment(const RevisionFragment&) = delete;
    RevisionFragment& operator=(const RevisionFragment&) = delete;
    ~RevisionFragment() {
        Waiter* head = m_waiters.load(std::memory_order_acquire);
        while (head && head != Closed()) delete std::exchange(head, head->next);
    }

    // Once, from any thread; false if already settled.
    template <class T>
    bool Resolve(std::shared_ptr<T> value) {
        if (!value) throw std::invalid_argument("RevisionFragment::Resolve: null value");
        return Settle(std::shared_ptr<const void>(std::move(value)), &typeid(T), {});
    }
    bool Fail(std::exception_ptr error) {
        if (!error) error = std::make_exception_ptr(std::runtime_error("revision fragment failed"));
        return Settle({}, nullptr, std::move(error));
    }

    [[nodiscard]] State GetState() const noexcept { return m_state.load(std::memory_order_acquire); }
    // Null until Ready, or when T is not the resolved type.
    template <class T>
    [[nodiscard]] std::shared_ptr<const T> Value() const noexcept {
        if (GetState() != State::Ready || *m_type != typeid(T)) return {};
        return std::static_pointer_cast<const T>(m_value);
    }
    [[nodiscard]] std::exception_ptr Error() const noexcept {
        return GetState() == State::Failed ? m_error : std::exception_ptr{};
    }
    [[nodiscard]] const std::vector<Requirement>& Requirements() const noexcept { return m_requirements; }

private:
    friend class RevisionAssembler;
    struct Waiter {
        std::weak_ptr<detail::PendingRevision> revision;
        Waiter* next = nullptr;
    };
    static Waiter* Closed() noexcept {
        static Waiter closed;
        return &closed;
    }

    bool Settle(std::shared_ptr<const void> value, const std::type_info* type, std::exception_ptr error) {
        if (m_claimed.exchange(true, std::memory_order_acq_rel)) return false;
        m_value = std::move(value);
        m_type = type;
        m_error = std::move(error);
        m_state.store(m_value ? State::Ready : State::Failed, std::memory_order_release);
        Waiter* head = m_waiters.exchange(Closed(), std::memory_order_acq_rel);
        while (head) {
            Waiter* waiter = std::exchange(head, head->next);
            if (auto revision = waiter->revision.lock()) detail::SettleWaiter(revision, *this);
            delete waiter;
        }
        return true;
    }
    // Coordinator (Seal): counts the revision down now if already settled.
    void Attach(const std::shared_ptr<detail::PendingRevision>& revision) const {
        auto* node = new Waiter{revision, nullptr};
        Waiter* head = m_waiters.load(std::memory_order_acquire);
        do {
            if (head == Closed()) {
                delete node;
                detail::SettleWaiter(revision, *this);
                return;
            }
            node->next = head;
        } while (!m_waiters.compare_exchange_weak(head, node, std::memory_order_release, std::memory_order_acquire));
    }

    std::vector<Requirement> m_requirements;
    std::atomic<bool> m_claimed{false};
    std::atomic<State> m_state{State::Pending};
    std::shared_ptr<const void> m_value;
    const std::type_info* m_type = nullptr;
    std::exception_ptr m_error;
    mutable std::atomic<Waiter*> m_waiters{nullptr};
};

// What the frame selects: the exact fragments of one complete revision.
class AssembledRevision {
public:
    [[nodiscard]] std::uint64_t Sequence() const noexcept { return m_sequence; }
    [[nodiscard]] std::size_t SlotCount() const noexcept { return m_slots.size(); }
    // The exact version (identity) in a slot; null if the revision names none.
    [[nodiscard]] const std::shared_ptr<const RevisionFragment>& Fragment(std::uint32_t slot) const {
        return m_slots.at(slot);
    }
    template <class T>
    [[nodiscard]] std::shared_ptr<const T> Get(std::uint32_t slot) const noexcept {
        return slot < m_slots.size() && m_slots[slot] ? m_slots[slot]->Value<T>() : std::shared_ptr<const T>{};
    }

private:
    friend class RevisionAssembler;
    std::uint64_t m_sequence = 0;
    std::vector<std::shared_ptr<const RevisionFragment>> m_slots;
};

namespace detail {
struct PendingRevision {
    std::uint64_t sequence = 0;
    std::vector<std::shared_ptr<const RevisionFragment>> slots;
    std::atomic<std::uint32_t> remaining{0};
    std::atomic<bool> failed{false};
    std::exception_ptr error; // written by the first failing fragment's settler, read after the count reaches zero
    std::shared_ptr<RevisionShared> shared;
};
struct RevisionShared {
    struct Completed {
        std::shared_ptr<PendingRevision> revision;
        Completed* next = nullptr;
    };
    std::atomic<Completed*> completed{nullptr};
    std::function<void()> notify;
    ~RevisionShared() {
        Completed* head = completed.exchange(nullptr, std::memory_order_acquire);
        while (head) delete std::exchange(head, head->next);
    }
};
inline void CountDown(const std::shared_ptr<PendingRevision>& revision) {
    if (revision->remaining.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    auto& shared = *revision->shared;
    auto* node = new RevisionShared::Completed{revision, shared.completed.load(std::memory_order_relaxed)};
    while (!shared.completed.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {}
    if (shared.notify) shared.notify();
}
inline void SettleWaiter(const std::shared_ptr<PendingRevision>& revision, const RevisionFragment& fragment) {
    if (fragment.GetState() == RevisionFragment::State::Failed && !revision->failed.exchange(true, std::memory_order_acq_rel))
        revision->error = fragment.Error();
    CountDown(revision);
}
} // namespace detail

class RevisionAssembler {
public:
    using Lease = std::shared_ptr<const AssembledRevision>;
    using Selection = PublicationExchange<AssembledRevision>::Selection;

    // A candidate revision: the newest sealed revision's fragments, edited by slot. Coordinator only.
    class Draft {
    public:
        void Set(std::uint32_t slot, std::shared_ptr<const RevisionFragment> fragment) { m_slots.at(slot) = std::move(fragment); }
        [[nodiscard]] const std::shared_ptr<const RevisionFragment>& Get(std::uint32_t slot) const { return m_slots.at(slot); }
        [[nodiscard]] std::uint64_t Base() const noexcept { return m_base; }

    private:
        friend class RevisionAssembler;
        std::uint64_t m_base = 0;
        std::vector<std::shared_ptr<const RevisionFragment>> m_slots;
    };

    struct Failure {
        std::uint64_t sequence = 0;
        std::exception_ptr error;
    };
    struct Collected {
        std::uint64_t published = 0; // the revision handed to the frame by this call, or 0
        std::vector<Failure> failures;
    };
    struct Stats {
        std::uint64_t sealed = 0;
        std::uint64_t published = 0;
        std::uint64_t failed = 0;
        std::uint64_t superseded = 0; // completed after a newer one was published
        std::uint64_t abandoned = 0;  // still pending when a newer one was published
    };

    // `notify` runs on the thread that completes a revision (any), once per revision.
    explicit RevisionAssembler(std::uint32_t slots, std::function<void()> notify = {})
        : m_shared(std::make_shared<detail::RevisionShared>()), m_latest(slots) {
        m_shared->notify = std::move(notify);
    }
    RevisionAssembler(const RevisionAssembler&) = delete;
    RevisionAssembler& operator=(const RevisionAssembler&) = delete;
    // A completed revision on the stack owns the shared state through its node; take them so it is released.
    ~RevisionAssembler() {
        auto* head = m_shared->completed.exchange(nullptr, std::memory_order_acquire);
        while (head) delete std::exchange(head, head->next);
    }

    // ---- coordinator ----
    [[nodiscard]] Draft Begin() const {
        Draft draft;
        draft.m_base = m_latestSequence;
        draft.m_slots = m_latest;
        return draft;
    }
    // Returns the revision's sequence. Throws std::logic_error for a draft begun before another seal, or one whose
    // closure is broken (a fragment's requirement is not exactly in its slot).
    std::uint64_t Seal(Draft draft) {
        if (draft.m_base != m_latestSequence) throw std::logic_error("RevisionAssembler::Seal: draft begun before another seal");
        for (const auto& fragment : draft.m_slots) {
            if (!fragment) continue;
            for (const auto& requirement : fragment->Requirements())
                if (requirement.slot >= draft.m_slots.size() || draft.m_slots[requirement.slot] != requirement.fragment)
                    throw std::logic_error("RevisionAssembler::Seal: a fragment's required version is not in the revision");
        }
        std::vector<const RevisionFragment*> distinct;
        for (const auto& fragment : draft.m_slots)
            if (fragment && std::find(distinct.begin(), distinct.end(), fragment.get()) == distinct.end()) distinct.push_back(fragment.get());
        auto revision = std::make_shared<detail::PendingRevision>();
        revision->sequence = ++m_latestSequence;
        revision->slots = draft.m_slots;
        revision->shared = m_shared;
        revision->remaining.store(static_cast<std::uint32_t>(distinct.size()) + 1, std::memory_order_relaxed);
        m_latest = std::move(draft.m_slots);
        m_pending.push_back(revision);
        ++m_stats.sealed;
        for (const auto* fragment : distinct) fragment->Attach(revision);
        detail::CountDown(revision); // the seal's own count
        return revision->sequence;
    }
    // Takes the completed revisions: hands the newest complete one to the frame (latest wins over one not yet
    // selected), reports failures, abandons older pending ones, and releases retired publications here.
    Collected Collect() {
        Collected result;
        m_exchange.ReclaimRetired();
        auto* head = m_shared->completed.exchange(nullptr, std::memory_order_acquire);
        std::shared_ptr<detail::PendingRevision> newest;
        while (head) {
            auto* node = std::exchange(head, head->next);
            auto revision = std::move(node->revision);
            delete node;
            // One a completer had locked just as a newer publication abandoned it was counted then.
            if (std::erase(m_pending, revision) == 0) continue;
            if (revision->failed.load(std::memory_order_acquire)) {
                ++m_stats.failed;
                result.failures.push_back({revision->sequence, revision->error});
            } else if (revision->sequence <= m_publishedSequence || (newest && revision->sequence < newest->sequence)) {
                ++m_stats.superseded;
            } else {
                if (newest) ++m_stats.superseded;
                newest = std::move(revision);
            }
        }
        if (newest) {
            auto assembled = std::make_shared<AssembledRevision>();
            assembled->m_sequence = newest->sequence;
            assembled->m_slots = std::move(newest->slots);
            m_publishedSequence = assembled->m_sequence;
            m_held = std::move(assembled);
            ++m_stats.published;
            const auto before = m_pending.size();
            std::erase_if(m_pending, [&](const auto& pending) { return pending->sequence < m_publishedSequence; });
            m_stats.abandoned += before - m_pending.size();
        }
        if (m_held) {
            const auto sequence = m_held->Sequence();
            if (m_exchange.TryReplace(m_held)) result.published = sequence;
        }
        return result;
    }
    [[nodiscard]] std::uint64_t LatestSequence() const noexcept { return m_latestSequence; }
    [[nodiscard]] std::size_t Pending() const noexcept { return m_pending.size(); }
    [[nodiscard]] const Stats& GetStats() const noexcept { return m_stats; }

    // ---- frame owner ----
    template <class Accept>
    [[nodiscard]] Selection TrySelect(Accept&& accept) noexcept { return m_exchange.TrySelect(std::forward<Accept>(accept)); }
    [[nodiscard]] Selection TrySelect() noexcept {
        return m_exchange.TrySelect([](const AssembledRevision&) noexcept { return true; });
    }
    [[nodiscard]] const AssembledRevision* Active() const noexcept { return m_exchange.Active(); }
    [[nodiscard]] Lease AcquireActive() const noexcept { return m_exchange.AcquireActive(); }

private:
    std::shared_ptr<detail::RevisionShared> m_shared;
    PublicationExchange<AssembledRevision> m_exchange;
    std::vector<std::shared_ptr<const RevisionFragment>> m_latest;
    std::uint64_t m_latestSequence = 0;
    std::uint64_t m_publishedSequence = 0;
    std::vector<std::shared_ptr<detail::PendingRevision>> m_pending;
    std::shared_ptr<const AssembledRevision> m_held;
    Stats m_stats;
};

} // namespace org::async
