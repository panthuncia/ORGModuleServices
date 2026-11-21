#pragma once

#include <atomic>
#include <cstdint>

namespace org::async {

// A host's frame clock. The host owns one, moves it, and everyone else only
// reads it.
//
// - beginFrame moves at each frame begin, so the begin reads the frame it
//   starts. It is 0 before the first begin.
// - slot is beginFrame modulo the slot count: it selects per-slot copies.
// - presentSerial is the present the current work goes to. It starts at 1 and
//   moves at each sealed present.
// - gpuCompleted is the newest beginFrame whose GPU work has finished. It is 0
//   before the first and never decreases.
//
// Any thread may read. Begin and SealPresent are for the one thread that owns
// the frame. NoteGpuCompleted may be called from any thread.
class FrameClock {
public:
    struct Reading {
        std::uint64_t beginFrame = 0;
        std::uint32_t slot = 0;
        std::uint64_t presentSerial = 1;
        std::uint64_t gpuCompleted = 0;
    };

    // slots is the number of frames the GPU work may run behind the frame
    // owner. Zero counts as one.
    explicit constexpr FrameClock(std::uint32_t slots) noexcept : m_slots(slots ? slots : 1) {}
    FrameClock(const FrameClock&) = delete;
    FrameClock& operator=(const FrameClock&) = delete;

    [[nodiscard]] std::uint32_t Slots() const noexcept { return m_slots; }
    [[nodiscard]] std::uint64_t BeginFrame() const noexcept { return m_beginFrame.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint32_t Slot() const noexcept { return SlotOf(BeginFrame()); }
    [[nodiscard]] std::uint32_t SlotOf(std::uint64_t frame) const noexcept {
        return static_cast<std::uint32_t>(frame % m_slots);
    }
    [[nodiscard]] std::uint64_t PresentSerial() const noexcept { return m_presentSerial.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t GpuCompleted() const noexcept { return m_gpuCompleted.load(std::memory_order_acquire); }

    // All four. Each is read on its own, so they may straddle a move on
    // another thread.
    [[nodiscard]] Reading Read() const noexcept {
        const auto frame = BeginFrame();
        return {frame, SlotOf(frame), PresentSerial(), GpuCompleted()};
    }

    // Frame owner: a new frame begins. Returns its beginFrame.
    std::uint64_t Begin() noexcept { return m_beginFrame.fetch_add(1, std::memory_order_acq_rel) + 1; }

    // Frame owner: the present is sealed. Work from here goes to the next one.
    void SealPresent() noexcept { m_presentSerial.fetch_add(1, std::memory_order_acq_rel); }

    // The GPU finished frame's work. An older frame than the newest seen
    // changes nothing.
    void NoteGpuCompleted(std::uint64_t frame) noexcept {
        for (auto seen = m_gpuCompleted.load(std::memory_order_relaxed);
             frame > seen && !m_gpuCompleted.compare_exchange_weak(seen, frame, std::memory_order_acq_rel);) {
        }
    }

private:
    std::uint32_t m_slots;
    std::atomic<std::uint64_t> m_beginFrame{0};
    std::atomic<std::uint64_t> m_presentSerial{1};
    std::atomic<std::uint64_t> m_gpuCompleted{0};
};

} // namespace org::async
