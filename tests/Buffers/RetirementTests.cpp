// RetirementJoin, the recording pools and the graph host pool defaults, standalone (std and header-only ORG containers only).
#include <ORGModuleServices/GraphHostBackings.h>
#include <ORGModuleServices/RecordingPools.h>
#include <ORGModuleServices/RetirementJoin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace org::services;

namespace {

void Check(bool condition, const std::source_location location = std::source_location::current()) {
    if (condition) return;
    std::fprintf(stderr, "check failed at %s:%u\n", location.file_name(), static_cast<unsigned>(location.line()));
    std::fflush(stderr);
    std::abort();
}

struct FakeSource final : RetirementJoin::Source {
    std::atomic<bool> live{true};
    std::atomic<std::uint64_t> made{0};
    std::atomic<std::uint64_t> cpuDone{0};

    std::optional<std::uint64_t> SubmissionsMade() const override {
        if (!live.load()) return std::nullopt;
        return made.load();
    }
    std::uint64_t CpuFramesCompleted() const override { return cpuDone.load(); }
};

void TestJoinWithoutGraph() {
    auto source = std::make_shared<FakeSource>();
    source->live = false;
    RetirementJoin join(source);
    Check(join.SubmittedPoint() == 0);
    join.Poll();
    Check(join.Clock()->Completed() == 0);
}

void TestJoinHostThenCpu() {
    auto source = std::make_shared<FakeSource>();
    auto clock = std::make_shared<RetirementClock>();
    RetirementJoin join(source, clock);
    Check(join.Clock() == clock);

    // Before any CPU frame the points are the host's counts.
    source->made = 3;
    Check(join.SubmittedPoint() == 3);
    join.NoteGraphSubmissionsRetired(2);
    Check(clock->Completed() == 2);
    join.NoteGraphSubmissionsRetired(3);
    Check(clock->Completed() == 3);

    // A point taken inside a CPU frame includes the frame's end signal.
    join.NoteCpuFrameOpened(1);
    source->made = 5;
    Check(join.SubmittedPoint() == 6);
    Check(join.SubmittedPoint() == 6);  // the same point is noted once
    join.NoteGraphSubmissionsRetired(5);
    Check(clock->Completed() == 3);  // the CPU frame is not done
    source->cpuDone = 1;
    join.Poll();
    Check(clock->Completed() == 6);

    // Points retire in order, each once both counts passed it.
    join.NoteCpuFrameSignalled(1);
    join.NoteCpuFrameOpened(2);
    source->made = 7;
    Check(join.SubmittedPoint() == 9);
    source->made = 8;
    Check(join.SubmittedPoint() == 10);
    join.NoteGraphSubmissionsRetired(7);
    Check(clock->Completed() == 6);
    source->cpuDone = 2;
    join.Poll();
    Check(clock->Completed() == 9);
    join.NoteGraphSubmissionsRetired(8);
    Check(clock->Completed() == 10);
    Check(!join.StateText().empty());
}

void TestJoinSeal() {
    auto source = std::make_shared<FakeSource>();
    RetirementJoin join(source);
    Check(join.CpuFramesSignalled() == 0 && join.CpuFramesSealed() == 0);
    join.NoteCpuFrameSignalled(1);
    join.SealCpuFrames();
    join.NoteCpuFrameSignalled(2);
    Check(join.CpuFramesSignalled() == 2 && join.CpuFramesSealed() == 1);
    join.SealCpuFrames();
    Check(join.CpuFramesSealed() == 2);
}

void TestJoinNeedsSource() {
    bool threw = false;
    try {
        RetirementJoin join(nullptr);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    Check(threw);
}

void TestJoinConcurrentRetire() {
    auto source = std::make_shared<FakeSource>();
    RetirementJoin join(source);
    constexpr std::uint64_t kSubmissions = 20000;
    std::atomic<bool> done{false};
    std::thread retirer([&] {
        // Retires what was made, from another thread, while points are taken.
        while (!done.load()) join.NoteGraphSubmissionsRetired(source->made.load());
    });
    std::thread poller([&] {
        while (!done.load()) join.Poll();
    });
    std::uint64_t last = 0;
    for (std::uint64_t i = 1; i <= kSubmissions; ++i) {
        source->made = i;
        last = join.SubmittedPoint();
    }
    done = true;
    retirer.join();
    poller.join();
    join.NoteGraphSubmissionsRetired(kSubmissions);
    Check(last == kSubmissions && join.Clock()->Completed() == kSubmissions);
}

void TestBumpArena() {
    BumpArena arena(1024);
    auto* first = arena.Make<std::uint32_t>(4);
    Check(reinterpret_cast<std::uintptr_t>(first) % alignof(std::uint32_t) == 0);
    auto* second = arena.Make<double>(2);
    Check(reinterpret_cast<std::uintptr_t>(second) % alignof(double) == 0);
    Check(arena.Chunks() == 1 && arena.BytesUsed() >= 16 + 16);
    // A request larger than a chunk gets a chunk of its own, and earlier ones stay put.
    first[0] = 7;
    auto* large = static_cast<std::byte*>(arena.Allocate(4096, 16));
    Check(large && arena.Chunks() == 2 && first[0] == 7);
    Check(reinterpret_cast<std::uintptr_t>(large) % 16 == 0);
    // Reset keeps the chunks and hands out the same memory again.
    arena.Reset();
    Check(arena.BytesUsed() == 0 && arena.Chunks() == 2);
    Check(arena.Make<std::uint32_t>(4) == first);
}

struct FakePool {
    std::size_t used = 0;
    int resets = 0;
    int id = 0;
    std::size_t Used() const { return used; }
    void Reset() {
        ++resets;
        used = 0;
    }
};

struct DeferredWork {
    std::vector<std::function<void()>> work;
    bool refuse = false;
    ResetPools<FakePool>::Defer Defer() {
        return [this](std::function<void()> body) {
            if (!refuse) work.push_back(std::move(body));
        };
    }
    void RunAll() {
        auto run = std::move(work);
        work.clear();
        for (auto& body : run) body();
    }
};

void TestResetPools() {
    DeferredWork deferred;
    ResetPools<FakePool> pools(deferred.Defer());

    // A pool with nothing used is ready at once.
    pools.ResetLater(FakePool{0, 0, 1});
    Check(deferred.work.empty());
    Check(pools.Take().id == 1);
    // None ready: a new pool.
    Check(pools.Take().id == 0);

    // A used pool is reset through the host's defer, then ready.
    pools.ResetLater(FakePool{3, 0, 2});
    Check(deferred.work.size() == 1);
    Check(pools.Take().id == 0);
    deferred.RunAll();
    const auto reset = pools.Take();
    Check(reset.id == 2 && reset.resets == 1 && reset.used == 0);

    // Recycle swaps a used pool for a ready one and keeps an unused one.
    FakePool slot{0, 0, 3};
    pools.Recycle(slot);
    Check(slot.id == 3 && deferred.work.empty());
    pools.ResetLater(FakePool{0, 0, 4});
    slot.used = 2;
    pools.Recycle(slot);
    Check(slot.id == 4 && deferred.work.size() == 1);
    deferred.RunAll();
    Check(pools.Take().id == 3);

    // A refused reset drops the pool.
    deferred.refuse = true;
    pools.ResetLater(FakePool{1, 0, 5});
    Check(pools.Take().id == 0);

    bool threw = false;
    try {
        ResetPools<FakePool> none(nullptr);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    Check(threw);
}

void TestRecordingSlot() {
    DeferredWork deferred;
    ResetPools<FakePool> pools(deferred.Defer());
    RecordingSlot<FakePool> slot(2, 512);
    Check(slot.Workers() == 2);
    auto* bytes = slot.Arena(1).Make<std::uint64_t>(8);
    Check(bytes && slot.Arena(1).BytesUsed() == 64 && slot.Arena(0).BytesUsed() == 0);
    slot.Lists().used = 1;
    slot.Lists().id = 9;
    slot.Retire(pools);
    Check(slot.Arena(1).BytesUsed() == 0 && slot.Arena(1).Chunks() == 1);
    Check(slot.Lists().Used() == 0 && slot.Lists().id == 0 && deferred.work.size() == 1);
    deferred.RunAll();
    Check(pools.Take().id == 9);
}

void TestPoolDefaults() {
    static_assert(PerFrameSpares(4) == 3 && PerFrameSpares(0) == 0);
    static_assert(PerFrameCopies(3, 4) == 3 && PerFrameCopies(0, 4) == 2 && PerFrameCopies(9, 4) == 4 && PerFrameCopies(3, 1) == 2);
    static_assert(!BackingShrinkPolicy(BackingShrink::Never).Shrinks());
    static_assert(BackingShrinkPolicy(BackingShrink::Settled) == ShrinkPolicy::WhenBelow(25, std::chrono::milliseconds(5000), 2));
    static_assert(BackingShrinkPolicy(BackingShrink::Eager) == ShrinkPolicy::WhenBelow(25, std::chrono::milliseconds(0), 1));
}

} // namespace

int main() {
    TestJoinWithoutGraph();
    TestJoinHostThenCpu();
    TestJoinSeal();
    TestJoinNeedsSource();
    TestJoinConcurrentRetire();
    TestBumpArena();
    TestResetPools();
    TestRecordingSlot();
    TestPoolDefaults();
    std::printf("ORGRetirementTests passed\n");
    return 0;
}
