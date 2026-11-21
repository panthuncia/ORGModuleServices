#include <ORGModuleServices/RetirementJoin.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace org::services {

RetirementJoin::RetirementJoin(std::shared_ptr<const Source> source, std::shared_ptr<RetirementClock> clock)
    : m_source(std::move(source)), m_clock(std::move(clock)) {
    if (!m_source) throw std::invalid_argument("RetirementJoin needs a source");
    if (!m_clock) throw std::invalid_argument("RetirementJoin needs a clock");
}

std::uint64_t RetirementJoin::SubmittedPoint() {
    const auto made = m_source->SubmissionsMade();
    if (!made) return 0;
    const std::uint64_t submissions = *made;
    // A point taken inside a CPU frame covers that frame's own work, which its end signal completes.
    // The CPU frame may execute behind host submissions made meanwhile.
    const std::uint64_t cpu = (std::max)(m_cpuSignalled.load(std::memory_order_relaxed), m_cpuOpen.load(std::memory_order_relaxed));
    const std::uint64_t point = submissions + cpu;
    // Note every point whatever its source, so points taken before the first CPU frame also retire after it.
    std::uint64_t last = m_lastTaken.load(std::memory_order_relaxed);
    while (point > last && !m_lastTaken.compare_exchange_weak(last, point, std::memory_order_relaxed)) {
    }
    if (point > last) m_taken.Push({point, submissions, cpu});
    return point;
}

void RetirementJoin::NoteGraphSubmissionsRetired(std::uint64_t submissions) {
    std::uint64_t seen = m_hostRetired.load(std::memory_order_relaxed);
    while (seen < submissions && !m_hostRetired.compare_exchange_weak(seen, submissions, std::memory_order_release, std::memory_order_relaxed)) {
    }
    // Before any CPU frame the points are the host's counts.
    if (!m_cpuOpen.load(std::memory_order_acquire)) m_clock->Advance(submissions);
    Poll();
}

void RetirementJoin::NoteCpuFrameOpened(std::uint64_t value) {
    m_cpuOpen.store(value, std::memory_order_release);
}

void RetirementJoin::NoteCpuFrameSignalled(std::uint64_t value) {
    m_cpuSignalled.store(value, std::memory_order_release);
}

void RetirementJoin::SealCpuFrames() {
    m_cpuSealed.store(m_cpuSignalled.load(std::memory_order_relaxed), std::memory_order_release);
}

void RetirementJoin::Poll() {
    if (m_running.exchange(true, std::memory_order_acquire)) return;
    m_taken.Drain([&](TakenPoint&& point) { m_pending.push_back(point); });
    const std::uint64_t host = m_hostRetired.load(std::memory_order_acquire);
    const bool cpuFrames = m_cpuOpen.load(std::memory_order_acquire) != 0;
    const std::uint64_t cpu = cpuFrames ? m_source->CpuFramesCompleted() : 0;
    // Both counts only rise, so noted points retire in order and the newest one both passed is the clock's.
    std::uint64_t reached = 0;
    while (!m_pending.empty() && m_pending.front().host <= host && m_pending.front().cpu <= cpu) {
        reached = m_pending.front().point;
        m_pending.pop_front();
    }
    if (reached) m_clock->Advance(reached);
    m_running.store(false, std::memory_order_release);
}

std::string RetirementJoin::StateText() const {
    const auto front = m_pending.empty() ? TakenPoint{} : m_pending.front();
    const auto n = [](std::uint64_t value) { return std::to_string(value); };
    return "clock " + n(m_clock->Completed()) + " (" + n(m_clock->Waiting()) + " waiting), last point taken " + n(m_lastTaken.load()) + ", " +
        n(m_pending.size()) + " pending (first " + n(front.point) + ": host " + n(front.host) + " cpu " + n(front.cpu) + "), host retired " +
        n(m_hostRetired.load()) + " of " + n(m_source->SubmissionsMade().value_or(0)) + " made, CPU frames completed " +
        n(m_source->CpuFramesCompleted()) + " signalled " + n(m_cpuSignalled.load()) + " open " + n(m_cpuOpen.load());
}

} // namespace org::services
