#pragma once

#include <ORGModuleServices/BackingPool.h>

#include <Render/Runtime/EventQueue.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>

namespace org::services {

/**
 * @brief Joins two sources of GPU progress into one RetirementClock: a graph host's submissions and a CPU pipeline's frame
 * timeline. It tracks exact completion over everything the host submits, which every pooled lease is bound against.
 *
 * Host frame numbers can't serve: an async epoch's ticket is numbered when prepared, so a later-numbered completion says nothing
 * about work submitted after it. A point counts both sources when taken, and retires once both have passed it. Point 0 means
 * nothing. Before the first CPU frame the points are the host's submission counts.
 *
 * Threads: SubmittedPoint on the submitting thread, the CPU frame hooks on the thread that runs the CPU frames, the rest on any
 * thread. Nothing waits: Poll runs one at a time and a call that finds another running returns at once.
 */
class RetirementJoin {
public:
    /** @brief Where the join reads its two counts. */
    class Source {
    public:
        virtual ~Source() = default;
        /** @brief Submitting thread. The graph submissions made so far, or nullopt without a graph (points are then 0). */
        [[nodiscard]] virtual std::optional<std::uint64_t> SubmissionsMade() const = 0;
        /** @brief Any thread. The CPU frame timeline's completed value, or 0 without a timeline. */
        [[nodiscard]] virtual std::uint64_t CpuFramesCompleted() const = 0;
    };

    /** @brief `source` is required. The clock may be shared with pools made before the join. */
    explicit RetirementJoin(std::shared_ptr<const Source> source, std::shared_ptr<RetirementClock> clock = std::make_shared<RetirementClock>());
    RetirementJoin(const RetirementJoin&) = delete;
    RetirementJoin& operator=(const RetirementJoin&) = delete;

    /** @brief The joined clock. */
    [[nodiscard]] const std::shared_ptr<RetirementClock>& Clock() const noexcept { return m_clock; }

    /**
     * @brief Submitting thread. The point that retires once everything submitted so far is done, or 0 without a graph.
     * A frame binds what it read with this after its last submission, so a backing is not reused before the frame completes.
     */
    std::uint64_t SubmittedPoint();

    /** @brief Any thread: every graph submission up to `submissions` is complete (the host's retired callback). */
    void NoteGraphSubmissionsRetired(std::uint64_t submissions);

    /**
     * @brief CPU frame thread, at a CPU frame's start. `value` is the signal the frame's end will queue.
     * Every point taken until then includes it, so nothing the frame binds retires before its work.
     */
    void NoteCpuFrameOpened(std::uint64_t value);
    /** @brief CPU frame thread. The frame's signal `value` is queued behind its work. */
    void NoteCpuFrameSignalled(std::uint64_t value);
    /** @brief CPU frame thread, at the frame's start. Joins the CPU frames signalled so far, all before the last present, into the sealed count. */
    void SealCpuFrames();

    /** @brief Any thread. The last CPU frame signal queued (0: none). */
    [[nodiscard]] std::uint64_t CpuFramesSignalled() const noexcept { return m_cpuSignalled.load(std::memory_order_relaxed); }
    /** @brief Any thread. The CPU frames signalled before the last seal (0: none). */
    [[nodiscard]] std::uint64_t CpuFramesSealed() const noexcept { return m_cpuSealed.load(std::memory_order_acquire); }

    /**
     * @brief Any thread. Advances the clock to the newest point both sources passed.
     * Only one call runs at a time. Others return at once and the next run catches up, so nobody waits.
     */
    void Poll();

    /** @brief Any thread, read racily for a stall's message. The clock, the noted points and both sources' counts. */
    [[nodiscard]] std::string StateText() const;

private:
    /** @brief A point taken and the two counts it stands for. */
    struct TakenPoint {
        std::uint64_t point = 0, host = 0, cpu = 0;
    };

    std::shared_ptr<const Source> m_source;
    std::shared_ptr<RetirementClock> m_clock;
    // Points taken, passed to the advancing run, oldest first.
    org::runtime::EventQueue<TakenPoint, 256> m_taken;
    // The host's retired submissions, from any thread, and the CPU frames signalled. The source's timeline is read only after an
    // acquire of the open frame.
    std::atomic<std::uint64_t> m_hostRetired{0};
    std::atomic<std::uint64_t> m_cpuSignalled{0};
    // The value the open CPU frame signals at its end, or 0 before the first. What it binds mid-frame retires after its own work.
    std::atomic<std::uint64_t> m_cpuOpen{0};
    // Only one run at a time, and it owns the noted points it has not passed yet.
    std::atomic<bool> m_running{false};
    std::deque<TakenPoint> m_pending;
    // The newest point noted. Callers are on several threads.
    std::atomic<std::uint64_t> m_lastTaken{0};
    // CPU frames signalled before the last seal.
    std::atomic<std::uint64_t> m_cpuSealed{0};
};

} // namespace org::services
