#include <ORGModuleServices/GrowableBufferTypes.h>
#include <ORGModuleServices/GrowthPolicy.h>

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {

using org::services::BufferExtent;
using org::services::FilledContents;
using org::services::FilledVersion;
using org::services::GrowableRequirement;
using org::services::GrowthPolicy;
using org::services::NamedEntry;

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

} // namespace

int main() {
    TestDoubling();
    TestHeadroom();
    TestExtent();
    TestRequirements();
    TestFilledVersion();
    TestFilledContents();
}
