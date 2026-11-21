#include <ORGModuleServices/Async/FrameClock.h>
#include <ORGModuleServices/Async/StarvationWatch.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

void TestFrameClock() {
    org::async::FrameClock clock{4};
    assert(clock.Slots() == 4);
    auto reading = clock.Read();
    assert(reading.beginFrame == 0 && reading.slot == 0 && reading.presentSerial == 1 && reading.gpuCompleted == 0);
    for (std::uint64_t frame = 1; frame <= 9; ++frame) {
        assert(clock.Begin() == frame);
        assert(clock.BeginFrame() == frame && clock.Slot() == frame % 4);
    }
    assert(clock.SlotOf(13) == 1);
    clock.SealPresent();
    clock.SealPresent();
    assert(clock.PresentSerial() == 3);
    // Completion only moves forward.
    clock.NoteGpuCompleted(5);
    clock.NoteGpuCompleted(3);
    assert(clock.GpuCompleted() == 5);
    reading = clock.Read();
    assert(reading.beginFrame == 9 && reading.slot == 1 && reading.presentSerial == 3 && reading.gpuCompleted == 5);
    // Zero slots count as one.
    const org::async::FrameClock single{0};
    assert(single.Slots() == 1 && single.SlotOf(7) == 0);
}

void TestConcurrentGpuCompleted() {
    org::async::FrameClock clock{3};
    constexpr std::uint64_t kFrames = 20000;
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            for (std::uint64_t f = 1 + t; f <= kFrames; f += 4) clock.NoteGpuCompleted(f);
        });
    for (auto& thread : threads) thread.join();
    assert(clock.GpuCompleted() == kFrames);
}

void TestStarvationWatch() {
    struct Report {
        std::string what, why;
        std::uint64_t misses = 0;
    };
    std::vector<Report> reports;
    org::async::StarvationWatch watch{"the work", 3, [&](std::string_view what, std::string_view why, std::uint64_t misses) {
                                          reports.push_back({std::string(what), std::string(why), misses});
                                      }};
    assert(watch.What() == "the work" && watch.StarvedAfter() == 3);
    // Up to the limit, misses only count.
    for (int i = 0; i < 3; ++i) assert(!watch.NoteMiss("dropped"));
    assert(watch.Misses() == 3 && reports.empty());
    // A new input starts the count again.
    watch.NoteInput();
    assert(watch.Misses() == 0);
    for (int i = 0; i < 3; ++i) assert(!watch.NoteMiss("dropped"));
    // The miss past the limit reports, with its reason and count, and the count starts again.
    assert(watch.NoteMiss("asked again"));
    assert(reports.size() == 1 && reports[0].what == "the work" && reports[0].why == "asked again" && reports[0].misses == 4);
    assert(watch.Misses() == 0);
    for (int i = 0; i < 3; ++i) assert(!watch.NoteMiss("dropped"));
    assert(watch.NoteMiss("again") && reports.size() == 2);
    // A watch without a callback still counts and resets.
    org::async::StarvationWatch silent{"silent", 0, {}};
    assert(silent.NoteMiss("first") && silent.Misses() == 0);
}

} // namespace

int main() {
    TestFrameClock();
    TestConcurrentGpuCompleted();
    TestStarvationWatch();
}
