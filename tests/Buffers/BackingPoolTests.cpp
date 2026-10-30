// RetirementClock, BackingPool and BufferJournal, standalone (MS headers only; backings have no org::Buffer).
#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/BufferImage.h>
#include <ORGModuleServices/BufferJournal.h>
#include <ORGModuleServices/VersionedBufferBuild.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <source_location>
#include <span>
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

struct Harness {
    std::shared_ptr<RetirementClock> clock = std::make_shared<RetirementClock>();
    std::shared_ptr<std::atomic<std::uint64_t>> allocations = std::make_shared<std::atomic<std::uint64_t>>(0);
    std::shared_ptr<BackingPool> pool;

    explicit Harness(BackingPool::Config config = {}, BackingPoolHooks extra = {}) {
        BackingPoolHooks hooks = std::move(extra);
        hooks.allocate = [allocations = allocations](const BackingRequest& request) {
            const auto index = allocations->fetch_add(1) + 1;
            BackingAllocation allocation;
            allocation.address = 0x10000ull * index;
            allocation.srvIndex = static_cast<std::uint32_t>(index);
            (void)request;
            return allocation;
        };
        pool = BackingPool::Create(std::move(hooks), clock, std::move(config));
    }
};

BackingRequest Request(std::uint64_t capacityClass, std::uint32_t stride = 16, std::uint64_t compatibility = 0) {
    BackingRequest request;
    request.capacityClass = capacityClass;
    request.stride = stride;
    request.compatibility = compatibility;
    request.name = "test";
    return request;
}

void TestRetirementClock() {
    RetirementClock clock;
    Check(clock.Completed() == 0 && clock.Reached(0) && !clock.Reached(1));
    clock.Advance(3);
    clock.Advance(2);  // never backwards
    Check(clock.Completed() == 3);

    int inlineWakes = 0;
    clock.Subscribe(3, [&](std::uint64_t completed) {
        Check(completed == 3);
        ++inlineWakes;
    });
    Check(inlineWakes == 1 && clock.Waiting() == 0);

    int early = 0, late = 0;
    clock.Subscribe(5, [&](std::uint64_t completed) {
        Check(completed >= 5);
        ++early;
    });
    clock.Subscribe(9, [&](std::uint64_t) { ++late; });
    Check(clock.Waiting() == 2);
    clock.Advance(4);
    Check(early == 0 && late == 0);
    clock.Advance(6);
    Check(early == 1 && late == 0 && clock.Waiting() == 1);
    clock.Advance(6);
    Check(early == 1);
    clock.Advance(100);
    Check(early == 1 && late == 1 && clock.Waiting() == 0);

    // Concurrent advances and subscriptions: every waiter runs exactly once.
    RetirementClock shared;
    constexpr int kSubscribers = 4, kEach = 2000;
    std::atomic<std::uint64_t> wakes{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kSubscribers; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < kEach; ++i) shared.Subscribe(static_cast<std::uint64_t>(i + t), [&](std::uint64_t) { wakes.fetch_add(1); });
        });
    threads.emplace_back([&] {
        for (std::uint64_t point = 1; point <= kEach + kSubscribers; ++point) shared.Advance(point);
    });
    for (auto& thread : threads) thread.join();
    shared.Advance(kEach + kSubscribers + 1);
    Check(wakes.load() == static_cast<std::uint64_t>(kSubscribers) * kEach && shared.Waiting() == 0);
}

void TestAcquireReuseRules() {
    Harness h;
    BackingPool::AcquireInfo info;
    auto lease = h.pool->Acquire(Request(4), BackingPool::kAnyEpoch, &info);
    Check(lease && info.allocated && lease->capacityClass == 4 && lease->byteCapacity == 64 && lease->leases.load() == 1);
    Check(lease->generation == 1 && lease->address == 0x10000 && lease->srvIndex == 1);
    const auto* first = lease.Get().get();
    {
        auto copy = lease;  // copies count
        Check(first->leases.load() == 2);
    }
    Check(first->leases.load() == 1);

    // Leased: never reused.
    auto other = h.pool->Acquire(Request(4), BackingPool::kAnyEpoch, &info);
    Check(info.allocated && other.Get().get() != first);
    other.Reset();

    // Bound past the clock: not reused until the clock reaches it.
    lease.Bind(5);
    lease.Bind(3);  // max-merged
    Check(first->safeAt.load() == 5);
    lease.Reset();
    h.clock->Advance(4);
    auto blocked = h.pool->Acquire(Request(4), BackingPool::kAnyEpoch, &info);
    Check(blocked.Get().get() != first);  // `other`'s backing (safeAt 0) instead
    Check(!info.allocated);
    blocked.Reset();
    h.clock->Advance(5);
    // Both free and reusable now; equal contentEpoch (0): either may come back, but nothing new is made.
    auto again = h.pool->Acquire(Request(4), BackingPool::kAnyEpoch, &info);
    Check(!info.allocated);
    // A reused backing is a fresh fill: no published version names it, so its sealed mark is gone.
    again->sealedBytes.store(48);
    auto* sealed = again.Get().get();
    again.Reset();
    {
        // The two free backings come back (either order), nothing new made.
        auto a = h.pool->Acquire(Request(4), BackingPool::kAnyEpoch, &info);
        Check(!info.allocated);
        auto b = h.pool->Acquire(Request(4), BackingPool::kAnyEpoch, &info);
        Check(!info.allocated && (a.Get().get() == sealed || b.Get().get() == sealed));
        Check((a.Get().get() == sealed ? a : b)->sealedBytes.load() == 0);
    }

    // A readback pin and a binding consumer each keep a free backing from reuse.
    Harness pins({.name = "pins", .maxPerClass = 0, .framesInFlight = 3, .spareFree = 4});
    auto pinned = pins.pool->Acquire(Request(8));
    const auto pinnedBacking = pinned.Get();
    pinned.Reset();
    pinnedBacking->readbackPins.fetch_add(1);
    auto fresh = pins.pool->Acquire(Request(8), BackingPool::kAnyEpoch, &info);
    Check(info.allocated && fresh.Get() != pinnedBacking);
    fresh.Reset();
    pinnedBacking->readbackPins.fetch_sub(1);
    auto consumer = pins.pool->PinBindingConsumer(pinnedBacking);
    Check(pinnedBacking->bindingConsumers.load() == 1);
    auto avoided = pins.pool->Acquire(Request(8), BackingPool::kAnyEpoch, &info);
    Check(!info.allocated && avoided.Get() != pinnedBacking);
    consumer.reset();
    Check(pinnedBacking->bindingConsumers.load() == 0);
    auto reused = pins.pool->Acquire(Request(8), BackingPool::kAnyEpoch, &info);
    Check(!info.allocated && reused.Get() == pinnedBacking);
    Check(!pinnedBacking->retired.load());

    // Another stride or compatibility key is never handed out for a request.
    auto keyed = pins.pool->Acquire(Request(8, 16, 7), BackingPool::kAnyEpoch, &info);
    Check(info.allocated && keyed->compatibility == 7);
    auto strided = pins.pool->Acquire(Request(8, 32, 0), BackingPool::kAnyEpoch, &info);
    Check(info.allocated && strided->stride == 32 && strided->byteCapacity == 256);
}

void TestPreferEpochAndSpares() {
    Harness h({.name = "epochs", .maxPerClass = 0, .framesInFlight = 3, .spareFree = 8});
    std::vector<BackingLease> leases;
    for (const std::uint64_t epoch : {3u, 7u, 12u}) {
        leases.push_back(h.pool->Acquire(Request(4)));
        leases.back()->contentEpoch.store(epoch);
    }
    leases.clear();
    auto pick = [&](std::uint64_t prefer) {
        auto lease = h.pool->Acquire(Request(4), prefer);
        const auto epoch = lease->contentEpoch.load();
        return epoch;
    };
    Check(pick(10) == 7);                      // the highest at or below
    Check(pick(BackingPool::kAnyEpoch) == 12); // any: the newest
    Check(pick(2) == 3);                       // none at or below: the nearest past it
    Check(h.allocations->load() == 3);

    // spareFree: the reusable free backings past the chosen one and spareFree spares are released.
    Harness s({.name = "spares", .maxPerClass = 0, .framesInFlight = 3, .spareFree = 1});
    leases.clear();
    for (int i = 0; i < 4; ++i) leases.push_back(s.pool->Acquire(Request(4)));
    leases.clear();
    BackingPool::AcquireInfo info;
    auto kept = s.pool->Acquire(Request(4), BackingPool::kAnyEpoch, &info);
    Check(!info.allocated && info.released == 2);
    auto census = s.pool->Count();
    Check(census.live == 2 && census.leased == 1 && census.free == 1 && census.released == 2);

    // A superseded class of the same key goes once reusable; another key's free backing stays.
    Harness c({.name = "classes", .maxPerClass = 0, .framesInFlight = 3, .spareFree = 1});
    auto small = c.pool->Acquire(Request(4));
    auto otherKey = c.pool->Acquire(Request(4, 16, 9));
    small.Bind(2);
    small.Reset();
    otherKey.Reset();
    auto grown = c.pool->Acquire(Request(8), BackingPool::kAnyEpoch, &info);
    Check(info.allocated && info.released == 0);  // the small one is not reusable yet (clock 0 < 2)
    c.clock->Advance(2);
    auto grownAgain = c.pool->Acquire(Request(8), BackingPool::kAnyEpoch, &info);
    Check(info.allocated && info.released == 1);
    census = c.pool->Count();
    Check(census.live == 3);  // grown, grownAgain, and the other key's free backing
    std::uint64_t otherKeyLive = 0;
    c.pool->Visit([&](const Backing& backing) { otherKeyLive += backing.compatibility == 9; });
    Check(otherKeyLive == 1);

    // Trim: releases what Acquire would, without acquiring.
    grown.Reset();
    grownAgain.Reset();
    Harness t({.name = "trim", .maxPerClass = 0, .framesInFlight = 3, .spareFree = 0});
    auto a = t.pool->Acquire(Request(4));
    auto b = t.pool->Acquire(Request(4));
    a.Reset();
    b.Bind(1);
    b.Reset();
    t.pool->Trim();
    Check(t.pool->Count().live == 1);  // b waits for the clock
    t.clock->Advance(1);
    t.pool->Trim();
    Check(t.pool->Count().live == 0 && t.pool->Count().released == 2);
}

void TestMaxPerClassAndAvailability() {
    // BasicRenderer's configuration: a cap; exhaustion returns no lease and availability wakes the caller.
    Harness h({.name = "capped", .maxPerClass = 2, .framesInFlight = 3, .spareFree = 1});
    auto a = h.pool->Acquire(Request(4));
    auto b = h.pool->Acquire(Request(4));
    auto none = h.pool->Acquire(Request(4));
    Check(a && b && !none && h.pool->Count().exhausted == 1);
    std::atomic<int> wakes{0};
    std::atomic<std::uint64_t> woken{0};
    h.pool->SubscribeAvailability(77, [&](std::uint64_t identity) {
        woken.store(identity);
        wakes.fetch_add(1);
    });
    Check(wakes.load() == 0);
    a.Reset();  // a lease returns: availability wakes, once
    Check(wakes.load() == 1 && woken.load() == 77);
    b.Reset();
    Check(wakes.load() == 1);
    auto c = h.pool->Acquire(Request(4));
    Check(c && h.allocations->load() == 2);

    // Held back only by the clock: the wake comes when the clock reaches the free backing's safeAt.
    Harness k({.name = "clocked", .maxPerClass = 1, .framesInFlight = 3, .spareFree = 1});
    auto only = k.pool->Acquire(Request(4));
    only.Bind(10);
    only.Reset();
    Check(!k.pool->Acquire(Request(4)));
    int clockWakes = 0;
    k.pool->SubscribeAvailability(5, [&](std::uint64_t identity) {
        Check(identity == 5);
        ++clockWakes;
    });
    Check(clockWakes == 0);
    k.clock->Advance(9);
    Check(clockWakes == 0);
    k.clock->Advance(10);
    Check(clockWakes == 1);
    Check(static_cast<bool>(k.pool->Acquire(Request(4))));

    // An event between the failed Acquire and the subscription is not lost: the subscription wakes at once.
    Harness r({.name = "race", .maxPerClass = 1, .framesInFlight = 3, .spareFree = 1});
    auto held = r.pool->Acquire(Request(4));
    Check(!r.pool->Acquire(Request(4)));
    held.Reset();
    int raced = 0;
    r.pool->SubscribeAvailability(1, [&](std::uint64_t) { ++raced; });
    Check(raced == 1);

    // A subscription with nothing happening since the last look stays asleep.
    auto back = r.pool->Acquire(Request(4));
    Check(back && !r.pool->Acquire(Request(4)));
    int idle = 0;
    r.pool->SubscribeAvailability(2, [&](std::uint64_t) { ++idle; });
    Check(idle == 0);
    r.clock->Advance(1);
    Check(idle == 0);
    back.Reset();
    Check(idle == 1);
}

void TestNoCap() {
    Harness h({.name = "uncapped", .maxPerClass = 0, .framesInFlight = 3, .spareFree = 1});
    std::vector<BackingLease> leases;
    for (int i = 0; i < 100; ++i) {
        leases.push_back(h.pool->Acquire(Request(4)));
        Check(static_cast<bool>(leases.back()));
    }
    const auto census = h.pool->Count();
    Check(census.live == 100 && census.leased == 100 && census.exhausted == 0 && h.allocations->load() == 100);
}

void TestLeaseReturnFromOtherThreads() {
    Harness h({.name = "threads", .maxPerClass = 0, .framesInFlight = 3, .spareFree = 64});
    constexpr int kLeases = 32;
    std::vector<BackingLease> leases;
    for (int i = 0; i < kLeases; ++i) leases.push_back(h.pool->Acquire(Request(4)));
    // Each lease is copied and released many times on several threads; only the very last release returns the backing.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([copies = leases] {
            for (int round = 0; round < 1000; ++round)
                for (const auto& lease : copies) {
                    BackingLease copy = lease;
                    copy.Bind(static_cast<std::uint64_t>(round % 3));
                }
        });
    leases.clear();
    for (auto& thread : threads) thread.join();
    Check(h.pool->Count().free == kLeases);
    h.clock->Advance(2);  // past every Bind above
    std::set<std::uint64_t> generations;
    for (int i = 0; i < kLeases; ++i) {
        auto lease = h.pool->Acquire(Request(4), BackingPool::kAnyEpoch);
        Check(lease && generations.insert(lease->generation).second);
        leases.push_back(std::move(lease));
    }
    Check(h.allocations->load() == kLeases);

    // The acquirer keeps acquiring and trimming while other threads release: no backing is lost or handed out twice.
    std::atomic<bool> stop{false};
    std::vector<BackingLease> pending = std::move(leases);
    std::thread releaser([&] {
        for (auto& lease : pending) {
            BackingLease copy = std::move(lease);
            copy.Reset();
        }
        stop.store(true);
    });
    std::vector<BackingLease> held;
    while (!stop.load() || held.size() < 8) {
        auto lease = h.pool->Acquire(Request(4));
        Check(lease && lease->leases.load() == 1);
        held.push_back(std::move(lease));
        if (held.size() > 8) held.erase(held.begin());
        h.pool->Trim();
    }
    releaser.join();
    held.clear();
    h.pool->Trim();
    std::uint64_t leased = 0;
    h.pool->Visit([&](const Backing& backing) { leased += backing.leases.load() != 0; });
    Check(leased == 0);
}

void TestOldestContentEpoch() {
    Harness h({.name = "floor", .maxPerClass = 0, .framesInFlight = 3, .spareFree = 0});
    Check(!h.pool->OldestContentEpoch());
    auto a = h.pool->Acquire(Request(4));
    auto b = h.pool->Acquire(Request(4));
    auto c = h.pool->Acquire(Request(4));
    Check(!h.pool->OldestContentEpoch());  // all hold nothing (written whole)
    a->contentEpoch.store(5);
    b->contentEpoch.store(3);
    Check(h.pool->OldestContentEpoch() == 3u);
    b.Bind(1);
    b.Reset();  // free but kept (clock 0 < 1): still a floor
    h.pool->Trim();
    Check(h.pool->OldestContentEpoch() == 3u);
    h.clock->Advance(1);
    h.pool->Trim();  // spareFree 0: released
    Check(h.pool->OldestContentEpoch() == 5u);
    a.Reset();
    h.pool->Trim();
    Check(!h.pool->OldestContentEpoch());
    (void)c;
}

void TestPoolOutlivedAndHooks() {
    BackingLease survivor;
    std::atomic<int> exclusive{0}, retired{0};
    std::atomic<int> allocated{0};
    {
        BackingPoolHooks hooks;
        hooks.exclusive = [&](const std::function<void()>& body) {
            exclusive.fetch_add(1);
            body();
        };
        hooks.retire = [&](std::shared_ptr<org::Buffer>) { retired.fetch_add(1); };
        hooks.telemetry = [&](BackingPoolEvent event, std::string_view name, std::uint64_t) {
            Check(name == "hooks");
            if (event == BackingPoolEvent::Allocated) allocated.fetch_add(1);
        };
        Harness h({.name = "hooks", .maxPerClass = 0, .framesInFlight = 3, .spareFree = 0}, std::move(hooks));
        survivor = h.pool->Acquire(Request(4));
        auto other = h.pool->Acquire(Request(4));
        other.Reset();
        h.pool->Trim();
        (void)h.pool->OldestContentEpoch();
        Check(exclusive.load() == 4 && allocated.load() == 2);
        Check(retired.load() == 0);  // the backings have no buffer: nothing to hand over
    }
    // The pool went first: a leased backing is not claimed (nothing can reuse it, and its holder may still publish it), and the
    // last lease still releases safely.
    Check(!survivor->retired.load());
    survivor.Reset();
}

void TestJournal() {
    BufferJournal journal(sizeof(std::uint32_t));
    const std::uint32_t initialRows[]{10, 20, 30};
    journal.Initialize(std::as_bytes(std::span(initialRows)), 3, 4);
    auto capture = journal.CaptureDesired();
    Check(capture.writeSequence == 1 && capture.elementCount == 3 && capture.capacity == 4 && capture.image &&
        capture.image->ByteSize() == sizeof(initialRows));
    const auto initialDesired = capture.image->Materialize();

    auto published = std::make_shared<PublishedBufferVersion>();
    published->writeSequence = capture.writeSequence;
    published->elementCount = capture.elementCount;
    published->capacity = capture.capacity;
    published->elementStride = sizeof(std::uint32_t);
    published->image = capture.image;
    journal.Acknowledge(published);
    Check(!journal.HasUnpublishedChanges());

    journal.RequestCapacity(8);
    const std::uint32_t replacement = 200;
    journal.AppendWrite(1, std::as_bytes(std::span(&replacement, 1)), 4);
    capture = journal.CaptureDesired();
    Check(capture.previous == published && capture.writes.size() == 2 && capture.capacity == 8 && capture.elementCount == 4 &&
        capture.journalBaseSequence == published->writeSequence && capture.image && journal.HasUnpublishedChanges());
    Check(std::memcmp(initialDesired->data(), initialRows, sizeof(initialRows)) == 0);
    const auto capturedImage = capture.image;
    const auto capturedDesired = capturedImage->Materialize();
    const auto* desiredRows = reinterpret_cast<const std::uint32_t*>(capturedDesired->data());
    Check(desiredRows[0] == 10 && desiredRows[1] == 200 && desiredRows[2] == 30 && desiredRows[3] == 0);
    const std::uint32_t overlap[]{300, 400};
    journal.AppendWrite(1, std::as_bytes(std::span(overlap)), 4);
    // Captures are immutable while the journal's image advances.
    const auto recaptured = capturedImage->Materialize();
    desiredRows = reinterpret_cast<const std::uint32_t*>(recaptured->data());
    Check(desiredRows[1] == 200 && desiredRows[2] == 30);
    const auto repeatedCapture = journal.CaptureDesired();
    Check(repeatedCapture.writes.size() == capture.writes.size() + 1u && repeatedCapture.image != capturedImage);
    const std::uint32_t lowRangeReplacement = 11;
    journal.AppendWrite(0, std::as_bytes(std::span(&lowRangeReplacement, 1)), 1);
    const auto sparseCapture = journal.CaptureDesired();
    Check(sparseCapture.elementCount == 4 && sparseCapture.image && sparseCapture.image->ByteSize() == 4 * sizeof(std::uint32_t));
    const auto sparseBytes = sparseCapture.image->Materialize();
    const auto* sparseRows = reinterpret_cast<const std::uint32_t*>(sparseBytes->data());
    Check(sparseRows[0] == 11 && sparseRows[1] == 300 && sparseRows[2] == 400 && sparseRows[3] == 0);

    // Sparse growth across a page: the former tail page grows, zero-filled.
    BufferJournal crossPage(1);
    const std::byte first{0x2a};
    crossPage.Initialize(std::span(&first, 1), 1, BufferImage::PageBytes + 8u);
    const std::byte distant{0x5b};
    crossPage.AppendWrite(BufferImage::PageBytes + 5u, std::span(&distant, 1), BufferImage::PageBytes + 6u);
    const auto crossPageCapture = crossPage.CaptureDesired();
    const auto crossPageBytes = crossPageCapture.image->Materialize();
    Check(crossPageBytes->size() == BufferImage::PageBytes + 6u && (*crossPageBytes)[0] == first &&
        (*crossPageBytes)[BufferImage::PageBytes - 1u] == std::byte{} && (*crossPageBytes)[BufferImage::PageBytes + 5u] == distant);

    const std::uint32_t compactedRows[]{7, 8};
    const auto replacementSequence = journal.ReplaceImage(std::as_bytes(std::span(compactedRows)), 2, 8);
    const auto compacted = journal.CaptureDesired();
    Check(compacted.writeSequence == replacementSequence && compacted.elementCount == 2 && compacted.capacity == 8 && compacted.image &&
        compacted.image->ByteSize() == sizeof(compactedRows));
    const auto compactedBytes = compacted.image->Materialize();
    const auto* compactedImage = reinterpret_cast<const std::uint32_t*>(compactedBytes->data());
    Check(compactedImage[0] == 7 && compactedImage[1] == 8);

    // Acknowledge drops the writes the version holds, and ignores an older version.
    auto newer = std::make_shared<PublishedBufferVersion>();
    newer->writeSequence = compacted.writeSequence;
    journal.Acknowledge(newer);
    Check(journal.CaptureDesired().writes.empty() && !journal.HasUnpublishedChanges());
    journal.Acknowledge(published);
    Check(journal.CaptureDesired().previous == newer);

    bool threw = false;
    try {
        journal.AppendWrite(0, std::as_bytes(std::span(compactedRows)), 9);  // past the desired capacity
    } catch (const std::out_of_range&) {
        threw = true;
    }
    Check(threw);
}

// Acknowledge trims to min(acknowledged, the pool's OldestContentEpoch()): writes an older backing still needs stay.
void TestJournalTrimFloor() {
    Harness h({.name = "journal", .maxPerClass = 0, .framesInFlight = 3, .spareFree = 4});
    BufferJournal journal(sizeof(std::uint32_t));
    journal.SetBackingPool(h.pool);
    const std::uint32_t rows[]{1, 2, 3, 4};
    journal.Initialize(std::as_bytes(std::span(rows)), 4, 4);
    const auto version = [](std::uint64_t sequence) {
        auto published = std::make_shared<PublishedBufferVersion>();
        published->writeSequence = sequence;
        return std::shared_ptr<const PublishedBufferVersion>(std::move(published));
    };
    for (std::uint32_t i = 0; i < 4; ++i) journal.AppendWrite(i, std::as_bytes(std::span(&rows[i], 1)), 4);  // sequences 2..5
    auto older = h.pool->Acquire(Request(4));
    auto newer = h.pool->Acquire(Request(4));
    older->contentEpoch.store(3);
    newer->contentEpoch.store(5);
    journal.Acknowledge(version(5));
    auto capture = journal.CaptureDesired();
    Check(capture.journalBaseSequence == 3 && capture.writes.size() == 2 && capture.writes[0].sequence == 4);
    older->contentEpoch.store(0);  // holds nothing: no floor
    journal.AppendWrite(0, std::as_bytes(std::span(&rows[0], 1)), 4);  // 6
    journal.Acknowledge(version(6));
    capture = journal.CaptureDesired();
    Check(capture.journalBaseSequence == 5 && capture.writes.size() == 1 && capture.writes[0].sequence == 6);
    older->contentEpoch.store(2);  // a floor below the base never brings dropped writes back
    journal.AppendWrite(0, std::as_bytes(std::span(&rows[0], 1)), 4);  // 7
    journal.Acknowledge(version(7));
    Check(journal.CaptureDesired().journalBaseSequence == 5);
    older.Reset();
    newer.Reset();
}

} // namespace

int main() {
    TestRetirementClock();
    TestAcquireReuseRules();
    TestPreferEpochAndSpares();
    TestMaxPerClassAndAvailability();
    TestNoCap();
    TestLeaseReturnFromOtherThreads();
    TestOldestContentEpoch();
    TestPoolOutlivedAndHooks();
    TestJournal();
    TestJournalTrimFloor();
    std::puts("ORGBufferPoolTests: all passed");
    return 0;
}
