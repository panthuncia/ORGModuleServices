#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace org::async {

// A level-triggered single-consumer task pump.
//
// Notify(), the consumer's claim and its Running->Idle transition are one atomic word: the consumer state in its low
// two bits and the requested epoch above them. A producer therefore never takes a lock, and can never observe an owned
// pump after the consumer has decided to exit: the consumer goes Idle only by a compare-exchange that also proves no
// epoch was requested since it started draining, and a producer that requests one after it schedules the consumer
// again. Only the delayed notifications (NotifyAfter) take a mutex of their own, which Notify never touches.
//
// Configure() installs the callbacks and must not race any other call: it is setup, before the pump is shared.
class SerializedTaskPump {
public:
    using Task = std::function<void()>;
    using Submit = std::function<bool(Task)>;
    using SubmitDelayed = std::function<bool(std::chrono::steady_clock::duration, Task)>;
    using Drain = std::function<void()>;
    using Reject = std::function<void()>;

    enum class HandoffMode : std::uint8_t {
        Inline,
        Resubmit,
    };

    struct Stats {
        std::uint64_t requestedEpoch = 0;
        std::uint64_t drainedEpoch = 0;
        std::uint64_t notifications = 0;
        std::uint64_t coalescedNotifications = 0;
        std::uint64_t drainPasses = 0;
        std::uint64_t handoffRetries = 0;
        std::uint64_t handoffResubmissions = 0;
        std::uint64_t delayedRequests = 0;
        std::uint64_t delayedCoalesced = 0;
        std::uint64_t delayedFired = 0;
        std::uint64_t staleDelayed = 0;
        std::uint64_t rejected = 0;
        bool runnerActive = false;
        bool delayedArmed = false;
    };

    SerializedTaskPump() = default;
    SerializedTaskPump(const SerializedTaskPump&) = delete;
    SerializedTaskPump& operator=(const SerializedTaskPump&) = delete;

    void Configure(Submit submit, Drain drain, Reject reject = {}, SubmitDelayed submitDelayed = {},
        HandoffMode handoffMode = HandoffMode::Inline) {
        {
            std::lock_guard lock(m_delayedMutex);
            ++m_delayedGeneration;
            m_delayedDue.reset();
        }
        m_submit = std::move(submit);
        m_submitDelayed = std::move(submitDelayed);
        m_drain = std::move(drain);
        m_reject = std::move(reject);
        m_handoffMode = handoffMode;
        m_drainedEpoch.store(0, std::memory_order_relaxed);
        for (auto* counter : { &m_notifications, &m_coalesced, &m_drainPasses, &m_handoffRetries, &m_handoffResubmissions,
                 &m_delayedRequests, &m_delayedCoalesced, &m_delayedFired, &m_staleDelayed, &m_rejected })
            counter->store(0, std::memory_order_relaxed);
        // Last, with release: a thread that sees the pump Idle sees the callbacks.
        m_word.store(Word(State::Idle, 0), std::memory_order_release);
    }

    [[nodiscard]] bool Notify() {
        std::uint64_t word = m_word.load(std::memory_order_acquire);
        bool schedule = false;
        for (;;) {
            if (StateOf(word) == State::Stopping || !m_submit || !m_drain) return false;
            schedule = StateOf(word) == State::Idle;
            const std::uint64_t next = Word(schedule ? State::Scheduled : StateOf(word), EpochOf(word) + 1);
            if (m_word.compare_exchange_weak(word, next, std::memory_order_acq_rel, std::memory_order_acquire)) break;
        }
        m_notifications.fetch_add(1, std::memory_order_relaxed);
        if (!schedule) {
            m_coalesced.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (m_submit([this] { Run(); })) return true;
        // A rejected task never acquired consumer ownership. Permanently close this pump so queued work is failed rather
        // than stranded.
        Fail();
        return false;
    }

    // Delayed ownership is deliberately independent from immediate runner
    // ownership. A pending timer can therefore never suppress Notify(). When a
    // new earlier deadline replaces an existing one, the old token becomes a
    // harmless stale callback.
    [[nodiscard]] bool NotifyAfter(std::chrono::steady_clock::duration delay) {
        if (delay <= std::chrono::steady_clock::duration::zero()) return Notify();
        std::uint64_t generation = 0;
        const auto due = std::chrono::steady_clock::now() + delay;
        {
            std::lock_guard lock(m_delayedMutex);
            if (Stopped() || !m_submitDelayed) return false;
            m_delayedRequests.fetch_add(1, std::memory_order_relaxed);
            if (m_delayedDue && *m_delayedDue <= due) {
                m_delayedCoalesced.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            generation = ++m_delayedGeneration;
            m_delayedDue = due;
        }
        if (m_submitDelayed(delay, [this, generation] { FireDelayed(generation); })) return true;
        {
            std::lock_guard lock(m_delayedMutex);
            if (m_delayedGeneration == generation) m_delayedDue.reset();
        }
        m_rejected.fetch_add(1, std::memory_order_relaxed);
        if (m_reject) m_reject();
        return false;
    }

    // Runs the drain on the calling thread if no consumer is currently running.
    // A caller that must observe the drain's effect but cannot wait for the
    // scheduler (its domain may be occupied, or it may be the only thread that
    // can make progress) uses this instead of blocking. Returns false when
    // another thread already owns the consumer role, which is then guaranteed to
    // be making progress.
    [[nodiscard]] bool TryRunInline() {
        std::uint64_t word = m_word.load(std::memory_order_acquire);
        for (;;) {
            if (StateOf(word) == State::Stopping || StateOf(word) == State::Running || !m_drain) return false;
            // Idle or Scheduled: take consumer ownership. A task submitted for the Scheduled state finds Running and
            // returns without draining.
            if (m_word.compare_exchange_weak(word, Word(State::Running, EpochOf(word) + 1), std::memory_order_acq_rel,
                    std::memory_order_acquire))
                break;
        }
        RunOwned();
        return true;
    }

    void Stop() {
        m_word.fetch_or(kStateMask, std::memory_order_acq_rel);  // Stopping is every state bit
        std::lock_guard lock(m_delayedMutex);
        ++m_delayedGeneration;
        m_delayedDue.reset();
    }

    [[nodiscard]] bool IsIdle() const {
        return StateOf(m_word.load(std::memory_order_acquire)) == State::Idle;
    }

    [[nodiscard]] Stats GetStats() const {
        Stats result;
        const std::uint64_t word = m_word.load(std::memory_order_acquire);
        result.requestedEpoch = EpochOf(word);
        result.drainedEpoch = m_drainedEpoch.load(std::memory_order_relaxed);
        result.notifications = m_notifications.load(std::memory_order_relaxed);
        result.coalescedNotifications = m_coalesced.load(std::memory_order_relaxed);
        result.drainPasses = m_drainPasses.load(std::memory_order_relaxed);
        result.handoffRetries = m_handoffRetries.load(std::memory_order_relaxed);
        result.handoffResubmissions = m_handoffResubmissions.load(std::memory_order_relaxed);
        result.delayedRequests = m_delayedRequests.load(std::memory_order_relaxed);
        result.delayedCoalesced = m_delayedCoalesced.load(std::memory_order_relaxed);
        result.delayedFired = m_delayedFired.load(std::memory_order_relaxed);
        result.staleDelayed = m_staleDelayed.load(std::memory_order_relaxed);
        result.rejected = m_rejected.load(std::memory_order_relaxed);
        result.runnerActive = StateOf(word) == State::Scheduled || StateOf(word) == State::Running;
        {
            std::lock_guard lock(m_delayedMutex);
            result.delayedArmed = m_delayedDue.has_value();
        }
        return result;
    }

private:
    // Stopping is every state bit, so Stop and Fail are one fetch_or.
    enum class State : std::uint8_t { Idle = 0, Scheduled = 1, Running = 2, Stopping = 3 };
    static constexpr std::uint64_t kStateMask = 3;
    static constexpr std::uint64_t Word(State state, std::uint64_t epoch) { return (epoch << 2) | static_cast<std::uint64_t>(state); }
    static constexpr State StateOf(std::uint64_t word) { return static_cast<State>(word & kStateMask); }
    static constexpr std::uint64_t EpochOf(std::uint64_t word) { return word >> 2; }
    bool Stopped() const { return StateOf(m_word.load(std::memory_order_acquire)) == State::Stopping; }

    void Fail() {
        m_word.fetch_or(kStateMask, std::memory_order_acq_rel);
        m_rejected.fetch_add(1, std::memory_order_relaxed);
        if (m_reject) m_reject();
    }

    void FireDelayed(std::uint64_t generation) {
        {
            std::lock_guard lock(m_delayedMutex);
            if (Stopped()) return;
            if (!m_delayedDue || generation != m_delayedGeneration) {
                m_staleDelayed.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            m_delayedDue.reset();
            m_delayedFired.fetch_add(1, std::memory_order_relaxed);
        }
        (void)Notify();
    }

    void Run() noexcept {
        // TryRunInline may have taken consumer ownership after this task was submitted. Exactly one thread drains at a
        // time; that owner also observes this task's notification through the epoch. Ownership is claimed by the same
        // compare-exchange as the test, so an inline caller cannot slip in between.
        std::uint64_t word = m_word.load(std::memory_order_acquire);
        for (;;) {
            if (StateOf(word) == State::Stopping || StateOf(word) == State::Running) return;
            if (m_word.compare_exchange_weak(word, Word(State::Running, EpochOf(word)), std::memory_order_acq_rel,
                    std::memory_order_acquire))
                break;
        }
        RunOwned();
    }

    // Called with consumer ownership already held (state Running).
    void RunOwned() noexcept {
        for (;;) {
            std::uint64_t word = m_word.load(std::memory_order_acquire);
            if (StateOf(word) == State::Stopping) return;
            const std::uint64_t observedEpoch = EpochOf(word);
            try {
                m_drain();
            } catch (...) {
                Fail();
                return;
            }
            m_drainedEpoch.store(observedEpoch, std::memory_order_relaxed);
            m_drainPasses.fetch_add(1, std::memory_order_relaxed);
            // Idle only when no epoch was requested since the drain began; the producers that request one meanwhile see
            // Running and coalesce, so the consumer must take their work.
            word = m_word.load(std::memory_order_acquire);
            bool resubmit = false;
            for (;;) {
                if (StateOf(word) == State::Stopping) return;
                if (EpochOf(word) == observedEpoch) {
                    if (m_word.compare_exchange_weak(word, Word(State::Idle, observedEpoch), std::memory_order_acq_rel,
                            std::memory_order_acquire))
                        return;
                    continue;
                }
                // Work was signalled while the consumer was running. Most pumps retain ownership for the cheapest
                // possible handoff. A bounded drain can opt into resubmission so its scheduler task actually yields
                // between passes instead of extending one task indefinitely.
                if (m_handoffMode != HandoffMode::Resubmit) break;
                if (m_word.compare_exchange_weak(word, Word(State::Scheduled, EpochOf(word)), std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    resubmit = true;
                    break;
                }
            }
            m_handoffRetries.fetch_add(1, std::memory_order_relaxed);
            if (!resubmit) continue;
            m_handoffResubmissions.fetch_add(1, std::memory_order_relaxed);
            if (m_submit([this] { Run(); })) return;
            Fail();
            return;
        }
    }

    std::atomic<std::uint64_t> m_word{ Word(State::Stopping, 0) };
    std::atomic<std::uint64_t> m_drainedEpoch{ 0 };
    std::atomic<std::uint64_t> m_notifications{ 0 }, m_coalesced{ 0 }, m_drainPasses{ 0 }, m_handoffRetries{ 0 },
        m_handoffResubmissions{ 0 }, m_delayedRequests{ 0 }, m_delayedCoalesced{ 0 }, m_delayedFired{ 0 },
        m_staleDelayed{ 0 }, m_rejected{ 0 };
    // The delayed notifications' own state; Notify never takes it.
    mutable std::mutex m_delayedMutex;
    std::uint64_t m_delayedGeneration = 0;
    std::optional<std::chrono::steady_clock::time_point> m_delayedDue;
    Submit m_submit;
    SubmitDelayed m_submitDelayed;
    Drain m_drain;
    Reject m_reject;
    HandoffMode m_handoffMode = HandoffMode::Inline;
};

} // namespace org::async
