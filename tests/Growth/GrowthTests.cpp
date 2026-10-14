#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <ORGModuleServices/GrowthPolicy.h>
#include <ORGModuleServices/VersionFill.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {

using org::services::BackingAllocation;
using org::services::BackingLease;
using org::services::BackingPool;
using org::services::BackingPoolHooks;
using org::services::BackingRequest;
using org::services::BufferExtent;
using org::services::FilledContents;
using org::services::FilledVersion;
using org::services::GrowableRequirement;
using org::services::GrowthPolicy;
using org::services::LeasedVersion;
using org::services::NamedEntry;
using org::services::RetirementClock;

template <class F>
bool Throws(F&& f) {
    try {
        f();
    } catch (const std::length_error&) {
        return true;
    }
    return false;
}

void TestDoubling() {
    constexpr auto doubling = GrowthPolicy::Doubling();
    // Held: the current capacity (at least the minimum), never shrunk.
    static_assert(doubling.Next(8, 5) == 8);
    static_assert(doubling.Next(8, 8) == 8);
    static_assert(doubling.Next(0, 0) == 1);
    // Grown: doubled from the current capacity until it holds the need.
    static_assert(doubling.Next(8, 9) == 16);
    static_assert(doubling.Next(3, 13) == 24);
    static_assert(doubling.Next(0, 5) == 8);
    // The minimum: where doubling starts.
    constexpr auto from64 = GrowthPolicy::Doubling(64);
    static_assert(from64.Next(0, 1) == 64);
    static_assert(from64.Next(16, 65) == 128);
    static_assert(from64.Next(256, 65) == 256);
    static_assert(GrowthPolicy::Doubling(1024).Next(1500, 1501) == 3000);
    // Never less than the need, for every start.
    for (std::uint64_t current = 0; current < 70; ++current)
        for (std::uint64_t needed = 0; needed < 300; ++needed) {
            const auto next = from64.Next(current, needed);
            assert(next >= needed && next >= current && next >= 64);
        }
    // No clamp: past the type is a failure, not a smaller capacity.
    assert(Throws([] { (void)GrowthPolicy::Doubling().Next(std::uint64_t(1) << 63, (std::uint64_t(1) << 63) + 1); }));
    assert(GrowthPolicy::Doubling().NextAs<std::uint32_t>(1u << 30, 1u << 31) == 1u << 31);
    assert(Throws([] { (void)GrowthPolicy::Doubling().NextAs<std::uint32_t>(1u << 31, (std::uint64_t(1) << 31) + 1); }));
}

void TestHeadroom() {
    constexpr auto quarter = GrowthPolicy::Headroom(25);
    static_assert(quarter.Next(0, 100) == 125);
    static_assert(quarter.Next(0, 7) == 8);  // 7 + 7/4
    static_assert(quarter.Next(200, 100) == 200);
    static_assert(GrowthPolicy::Headroom(25, 64).Next(0, 4) == 64);
    static_assert(GrowthPolicy::Headroom(0).Next(3, 10) == 10);
    for (std::uint64_t needed = 1; needed < 1000; ++needed) assert(quarter.Next(0, needed) == needed + needed / 4);
    constexpr auto max = (std::numeric_limits<std::uint64_t>::max)();
    assert(Throws([&] { (void)GrowthPolicy::Headroom(50).Next(0, max - 1); }));
}

void TestExtent() {
    constexpr BufferExtent rows{0, 48, 48};
    static_assert(rows.BytesFor(10) == 480 && rows.UnitsIn(480) == 10 && rows.ElementsFor(10) == 10);
    constexpr BufferExtent words{8, 12, 4};  // a header, then 12-byte events, in 4-byte words
    static_assert(words.BytesFor(3) == 44 && words.ElementsFor(3) == 11 && words.UnitsIn(44) == 3);
    static_assert(words.UnitsIn(4) == 0 && words.UnitsIn(19) == 0 && words.UnitsIn(20) == 1);
    constexpr BufferExtent raw{0, 4, 0};
    static_assert(raw.ElementsFor(9) == 0 && raw.BytesFor(9) == 36);
    // A version made for a target holds at least it, read back from its size.
    for (std::uint64_t units = 0; units < 200; ++units) assert(words.UnitsIn(words.ElementsFor(units) * 4) >= units);
}

void TestRequirements() {
    GrowableRequirement asked;
    asked.Merge({10, 0});
    asked.Merge({4, 7});
    assert(asked.units == 10 && asked.newerThan == 7);
    assert(!asked.MetBy(10, 7));  // the relayout: not newer
    assert(!asked.MetBy(9, 8));   // too small
    assert(asked.MetBy(10, 8));
    const auto policy = GrowthPolicy::Doubling(4);
    assert(asked.GrowTo(policy, 6) == 12);
    // A relayout alone: a new version of the same size.
    assert((GrowableRequirement{0, 3}.GrowTo(policy, 16)) == 16);
    assert((GrowableRequirement{8, 0}.MetBy(8, 1)));
    // Nothing asked: met by any version.
    assert((GrowableRequirement{}.MetBy(0, 1)));
}

void TestFilledVersion() {
    FilledVersion filled;
    NamedEntry first;
    first.serial = 5;
    assert(!filled.Holds(first));  // nothing filled: written whole
    filled.Filled(first);
    assert(filled.Holds(first) && filled.Holds(5));
    NamedEntry grown;
    grown.serial = 9;
    assert(!filled.Holds(grown));  // another version: written whole again
    filled.Filled(grown);
    assert(filled.Holds(grown) && !filled.Holds(first));
}

void TestFilledContents() {
    FilledContents<> contents;
    NamedEntry fresh;
    fresh.serial = 3;
    assert(!contents.Since(fresh));  // none filled, nothing captured: whole
    contents.Filled(3, 40);
    assert(contents.Since(fresh) == 40u);
    // A grown version captured at stamp 37: changes since 37.
    NamedEntry captured;
    captured.serial = 4;
    captured.contentsStamp = 37;
    assert(contents.Since(captured) == 37u);
    // A grown version without contents: whole.
    NamedEntry empty;
    empty.serial = 6;
    assert(!contents.Since(empty));
    contents.Filled(6, 41);
    assert(contents.Since(empty) == 41u && contents.Since(captured) == 37u);
    // Any stamp type, with the capture's given.
    struct Position {
        std::uint32_t page = 0, at = 0;
    };
    FilledContents<Position> journal;
    assert(!journal.Since(2));
    journal.Filled(2, {1, 9});
    assert(journal.Since(2)->at == 9 && !journal.Since(3));
    assert(journal.Since(3, Position{0, 4})->at == 4);
}

void TestCapacityClass() {
    // A structured buffer's class is its elements, a raw one's its bytes: the size a growth's version is made at.
    constexpr BufferExtent rows{0, 48, 48};
    static_assert(rows.CapacityClassFor(16) == 16);
    constexpr BufferExtent words{8, 12, 4};
    static_assert(words.CapacityClassFor(3) == 11);
    constexpr BufferExtent raw{16, 4, 0};
    static_assert(raw.CapacityClassFor(10) == 56);
    // A policy's classes repeat: growths to needs within one class ask the same class, so a retired backing of it is reusable.
    const auto policy = GrowthPolicy::Doubling(4);
    assert(rows.CapacityClassFor(GrowableRequirement{10, 0}.GrowTo(policy, 0)) == rows.CapacityClassFor(GrowableRequirement{9, 0}.GrowTo(policy, 3)));
}

// A pool like the set's: no caps, its backings without an org::Buffer (each allocation a distinct address).
struct GrowthPool {
    std::shared_ptr<RetirementClock> clock = std::make_shared<RetirementClock>();
    std::shared_ptr<int> allocations = std::make_shared<int>(0);
    std::shared_ptr<BackingPool> pool;
    GrowthPool() {
        BackingPoolHooks hooks;
        hooks.allocate = [allocations = allocations](const BackingRequest&) {
            BackingAllocation allocation;
            allocation.address = 0x10000ull * static_cast<std::uint64_t>(++*allocations);
            return allocation;
        };
        pool = BackingPool::Create(std::move(hooks), clock, {.name = "growth", .maxPerClass = 0});
    }
    BackingLease Acquire(const BackingRequest& request, bool& allocated) {
        BackingPool::AcquireInfo info;
        auto lease = pool->Acquire(request, BackingPool::kAnyEpoch, &info);
        assert(lease);
        allocated = info.allocated;
        return lease;
    }
};

// What the set makes a version of stands in here: LeasedVersion's lifetime is the subject.
struct Version {
    std::uint64_t number = 0;
};

void TestGrowthFromPool() {
    GrowthPool growth;
    constexpr BufferExtent rows{0, 48, 48};
    BackingRequest request;
    request.capacityClass = rows.CapacityClassFor(GrowableRequirement{10, 0}.GrowTo(GrowthPolicy::Doubling(4), 0));
    request.stride = rows.structuredStride;
    request.compatibility = 7;  // the registration's key
    request.name = "rows";
    assert(request.capacityClass == 16);
    bool allocated = false;
    auto lease = growth.Acquire(request, allocated);
    assert(allocated && lease->byteCapacity == 16 * 48 && lease->capacityClass == 16);
    const std::uint64_t first = lease->generation;
    const std::uint64_t firstAddress = lease->address;

    // The version owns a lease; the set's entry holds a copy (NamedEntry::lease); the growth's own goes.
    auto version = LeasedVersion(Version{1}, lease);
    NamedEntry entry;
    entry.serial = 11;
    entry.lease = lease;
    lease.Reset();
    const auto backing = entry.lease.Get();  // observes; not a lease
    assert(backing->leases.load() == 2 && version->number == 1);
    // The frame that read the set binds it after its last submission (NamedVersions::Bind): point 3, max-merged.
    entry.lease.Bind(3);
    entry.lease.Bind(1);
    assert(backing->safeAt.load() == 3);

    // The set moves on (the entry goes) while a recording still holds the version: not reusable.
    entry = {};
    auto held = growth.Acquire(request, allocated);
    assert(allocated && held->generation != first);
    // The version's last reference goes: its backing returns, but the frame that read it has not retired.
    version.reset();
    assert(backing->leases.load() == 0);
    auto early = growth.Acquire(request, allocated);
    assert(allocated && early->generation != first);
    growth.clock->Advance(2);
    auto stillEarly = growth.Acquire(request, allocated);
    assert(allocated && stillEarly->generation != first);
    // Retired: the next growth of the class reuses it, at its address (holding its older version's contents).
    growth.clock->Advance(3);
    auto reused = growth.Acquire(request, allocated);
    assert(!allocated && reused->generation == first && reused->address == firstAddress);
    assert(*growth.allocations == 4);

    // Another registration's key never takes it, even free and retired.
    reused.Reset();
    BackingRequest other = request;
    other.compatibility = 8;
    auto foreign = growth.Acquire(other, allocated);
    assert(allocated && foreign->generation != first);
    // The same registration grown to the next class: a new backing of that class.
    BackingRequest grown = request;
    grown.capacityClass = 32;
    auto bigger = growth.Acquire(grown, allocated);
    assert(allocated && bigger->capacityClass == 32 && bigger->byteCapacity == 32 * 48);
}

void TestFillsIntoReusedBacking() {
    GrowthPool growth;
    BackingRequest request;
    request.capacityClass = 200000;  // raw bytes: not a whole number of zero pages
    request.compatibility = 1;
    bool allocated = false;
    auto lease = growth.Acquire(request, allocated);
    const std::uint64_t first = lease->generation;
    NamedEntry old;
    old.serial = 20;
    old.lease = lease;
    // A writer filled the old version (the contents rule), up to its stamp 40.
    FilledVersion filled;
    filled.Filled(old);
    FilledContents<> contents;
    contents.Filled(old, 40);
    lease.Bind(1);
    lease.Reset();
    old = {};
    growth.clock->Advance(1);
    auto again = growth.Acquire(request, allocated);
    assert(!allocated && again->generation == first);
    NamedEntry next;
    next.serial = 21;  // another version, the same backing
    next.lease = again;
    // Keyed on the version's serial, not the backing: the reused backing's older bytes are never taken as this version's.
    assert(!filled.Holds(next) && filled.Holds(20));
    assert(!contents.Since(next));  // Fresh: written whole
    next.contentsStamp = 37;        // Capture: what it laid out, not what the backing held
    assert(contents.Since(next) == 37u);
    // Zero: asked for the whole backing (its buffer's size), so none of the older bytes survive.
    const auto fill = org::services::ZeroFill(again->byteCapacity);
    std::uint64_t covered = 0;
    for (const auto& segment : fill.segments) {
        assert(segment.data && segment.size);
        for (std::size_t i = 0; i < segment.size; ++i) assert(static_cast<const std::byte*>(segment.data)[i] == std::byte{0});
        covered += segment.size;
    }
    assert(fill.offset == 0 && covered == again->byteCapacity);
    assert(org::services::ZeroFill(0).segments.empty());
}

} // namespace

int main() {
    TestDoubling();
    TestHeadroom();
    TestExtent();
    TestRequirements();
    TestFilledVersion();
    TestFilledContents();
    TestCapacityClass();
    TestGrowthFromPool();
    TestFillsIntoReusedBacking();
}
