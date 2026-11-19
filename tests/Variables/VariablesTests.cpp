#include <ORGModuleServices/Variables.h>

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using org::services::RequireSizes;
using org::services::Variable;
using org::services::VariableIndex;
using org::services::VariableSet;
using org::services::VariableSnapshot;
using org::services::VariableTraits;

template <class E, class F>
bool Throws(F&& f) {
    try {
        f();
    } catch (const E&) {
        return true;
    }
    return false;
}

struct Extent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    friend bool operator==(const Extent&, const Extent&) = default;
};

enum class Method : std::uint8_t { None, Fsr, Dlss };

void TestEquality() {
    int wakes = 0;
    VariableSet set([&] { ++wakes; });
    const auto size = set.Declare("Size", Extent{ 1920, 1080 });
    const auto ratio = set.Declare("Ratio", 1.0f, VariableTraits{ .sizes = false });
    const auto name = set.Declare<std::string>("Name", "a");
    assert(wakes == 3);  // Declarations wake too.
    wakes = 0;

    // The same value is no change and no wake.
    assert(!set.Set(size, Extent{ 1920, 1080 }));
    assert(!set.Set(ratio, 1.0f));
    assert(!set.Set(name, std::string("a")));
    assert(wakes == 0);
    assert(set.Serial(size.index) == 1);

    assert(set.Set(size, Extent{ 1280, 720 }));
    assert(set.Set(ratio, 0.5f));
    assert(set.Set(name, std::string("b")));
    assert(wakes == 3);
    assert(set.Serial(size.index) == 2);
    assert(set.Get(size) == (Extent{ 1280, 720 }));
    assert(set.Get(ratio) == 0.5f);
    assert(set.Get(name) == "b");

    // Floats compare exactly.
    assert(set.Set(ratio, 0.5f + 1e-7f));
}

void TestSnapshots() {
    VariableSet set;
    const auto a = set.Declare("A", 1);
    const auto b = set.Declare("B", Method::None);

    const auto first = set.Take();
    assert(first->Size() == 2);
    assert(first->Get(a) == 1);
    assert(first->Get(b) == Method::None);
    assert(first->Serial(a.index) == 1);
    assert(first->Name(b.index) == "B");
    assert(first->Traits(a.index).sizes);

    // Nothing changed, so the same object.
    assert(set.Take() == first);
    (void)set.Set(a, 1);
    assert(set.Take() == first);

    (void)set.Set(a, 2);
    const auto second = set.Take();
    assert(second != first);
    assert(second->Serial() != first->Serial());
    assert(second->Get(a) == 2);
    assert(first->Get(a) == 1);  // Snapshots are immutable.
    assert(second->Serial(a.index) == 2);
    assert(second->Serial(b.index) == 1);
    assert((second->ChangedSince(*first) == std::vector<VariableIndex>{ a.index }));
    assert(second->ChangedSince(*second).empty());

    // A variable declared after the older snapshot counts as changed.
    const auto c = set.Declare("C", 0u);
    const auto third = set.Take();
    assert(third->Size() == 3);
    assert((third->ChangedSince(*second) == std::vector<VariableIndex>{ c.index }));
    assert((third->ChangedSince(*first) == std::vector<VariableIndex>{ a.index, c.index }));

    // Changed and changed back. The serial still moved.
    (void)set.Set(a, 5);
    (void)set.Set(a, 2);
    const auto fourth = set.Take();
    assert(fourth->Get(a) == 2);
    assert(fourth->Serial(a.index) == 4);
    assert((fourth->ChangedSince(*third) == std::vector<VariableIndex>{ a.index }));

    assert(fourth->Describe(a.index) == "2");
    assert(fourth->Describe(b.index) == "0");  // An enum prints its number.
    assert(fourth->Describe(c.index) == "0");
    const auto e = set.Declare("E", Extent{ 4, 4 });
    assert(set.Take()->Describe(e.index) == "<value>");
}

void TestDerived() {
    VariableSet set;
    const auto display = set.Declare("Display", Extent{ 1920, 1080 });
    const auto scale = set.Declare("Scale", 2u);
    const auto other = set.Declare("Other", 0);
    int halfRuns = 0;
    const auto half = set.Derive<Extent>("Half", { display.index, scale.index }, [&](const VariableSnapshot& s) {
        ++halfRuns;
        const Extent d = s.Get(display);
        return Extent{ d.width / s.Get(scale), d.height / s.Get(scale) };
    });
    assert(halfRuns == 1);  // Evaluated once at declaration.
    assert(set.Get(half) == (Extent{ 960, 540 }));
    int areaRuns = 0;
    const auto area = set.Derive<std::uint64_t>("Area", { half.index }, [&](const VariableSnapshot& s) {
        ++areaRuns;
        const Extent h = s.Get(half);
        return std::uint64_t(h.width) * h.height;
    });
    assert(set.Get(area) == 960ull * 540);
    assert(Throws<std::invalid_argument>([&] { (void)set.Set(half, Extent{}); }));

    const auto first = set.Take();
    assert(first->Get(half) == (Extent{ 960, 540 }));
    assert(first->Get(area) == 960ull * 540);
    assert(first->IsDerived(half.index) && !first->IsDerived(display.index));
    assert(halfRuns == 1 && areaRuns == 1);

    // A change to an unrelated variable evaluates nothing.
    (void)set.Set(other, 1);
    const auto second = set.Take();
    assert(halfRuns == 1 && areaRuns == 1);
    assert((second->ChangedSince(*first) == std::vector<VariableIndex>{ other.index }));

    // A source change that leaves the derived value equal is no change, and the chain stops there.
    (void)set.Set(display, Extent{ 1921, 1081 });
    const auto third = set.Take();
    assert(halfRuns == 2 && areaRuns == 1);
    assert(third->Serial(half.index) == first->Serial(half.index));
    assert((third->ChangedSince(*second) == std::vector<VariableIndex>{ display.index }));

    // A real change moves the whole chain.
    (void)set.Set(scale, 4u);
    const auto fourth = set.Take();
    assert(halfRuns == 3 && areaRuns == 2);
    assert(fourth->Get(half) == (Extent{ 480, 270 }));
    assert(fourth->Get(area) == 480ull * 270);
    assert((fourth->ChangedSince(*third) == std::vector<VariableIndex>{ scale.index, half.index, area.index }));
    assert(set.Get(area) == 480ull * 270);

    // Dependencies must be declared, and constants may not feed a size.
    const auto ratio = set.Declare("Ratio", 0.5f, VariableTraits{ .sizes = false });
    assert(Throws<std::invalid_argument>([&] {
        (void)set.Derive<int>("Bad", { 999 }, [](const VariableSnapshot&) { return 0; });
    }));
    assert(Throws<std::invalid_argument>([&] {
        (void)set.Derive<float>("Sized", { ratio.index }, [&](const VariableSnapshot& s) { return s.Get(ratio); });
    }));
    const auto constant = set.Derive<float>("Twice", { ratio.index },
        [&](const VariableSnapshot& s) { return s.Get(ratio) * 2; }, VariableTraits{ .sizes = false });
    assert(set.Take()->Get(constant) == 1.0f);
}

void TestErrors() {
    VariableSet set;
    const auto a = set.Declare("A", 1);
    assert(Throws<std::invalid_argument>([&] { (void)set.Declare("A", 2); }));
    assert(Throws<std::invalid_argument>([&] { (void)set.Declare("", 2); }));
    assert(set.Find("A") == a.index);
    assert(set.Find("Missing") == org::services::kNoVariable);

    // The wrong type through a forged handle.
    const Variable<float> wrong{ a.index };
    assert(Throws<std::invalid_argument>([&] { (void)set.Get(wrong); }));
    assert(Throws<std::invalid_argument>([&] { (void)set.Set(wrong, 1.0f); }));
    const auto snapshot = set.Take();
    assert(Throws<std::invalid_argument>([&] { (void)snapshot->Get(wrong); }));
    const Variable<int> missing{ 42 };
    assert(Throws<std::out_of_range>([&] { (void)snapshot->Get(missing); }));
    assert(Throws<std::out_of_range>([&] { (void)set.Set(missing, 1); }));
    assert(!Variable<int>{});

    // A throwing derived function propagates and leaves the previous snapshot newest.
    bool fail = false;
    const auto d = set.Derive<int>("D", { a.index }, [&](const VariableSnapshot& s) {
        if (fail) throw std::runtime_error("derive failed");
        return s.Get(a) + 1;
    });
    const auto before = set.Take();
    fail = true;
    (void)set.Set(a, 7);
    assert(Throws<std::runtime_error>([&] { (void)set.Take(); }));
    fail = false;
    const auto after = set.Take();
    assert(after != before && after->Get(d) == 8);
}

void TestRequireSizes() {
    VariableSet set;
    const auto display = set.Declare("Display", Extent{ 1, 1 });
    const auto render = set.Declare("RenderSize", Extent{ 1, 1 }, VariableTraits{ .sizes = false });
    const auto snapshot = set.Take();
    const std::array ok{ display.index };
    RequireSizes(*snapshot, ok, "Texture");
    const std::array bad{ display.index, render.index };
    assert(Throws<std::invalid_argument>([&] { RequireSizes(*snapshot, bad, "Texture"); }));
    const std::array undeclared{ VariableIndex{ 9 } };
    assert(Throws<std::invalid_argument>([&] { RequireSizes(*snapshot, undeclared, "Texture"); }));
}

// Several writers hammer several variables while the consumer takes snapshots and another thread declares.
void TestConcurrentWriters() {
    constexpr int kWriters = 6;
    constexpr int kWrites = 20000;
    std::atomic<std::uint64_t> wakes{ 0 };
    VariableSet set([&] { wakes.fetch_add(1, std::memory_order_relaxed); });
    std::vector<Variable<int>> own;
    for (int w = 0; w < kWriters; ++w) own.push_back(set.Declare("Own" + std::to_string(w), 0));
    const auto shared = set.Declare("Shared", std::string("start"));
    const auto sum = set.Derive<long long>("Sum", { own[0].index, own[1].index }, [&](const VariableSnapshot& s) {
        return static_cast<long long>(s.Get(own[0])) + s.Get(own[1]);
    });
    const std::uint64_t declaredWakes = wakes.load();

    std::atomic<int> running{ kWriters };
    std::atomic<std::uint64_t> changes{ 0 };
    std::vector<std::thread> threads;
    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            std::uint64_t mine = 0;
            for (int i = 1; i <= kWrites; ++i) {
                // Each write to the own variable is a change. The repeat is not.
                mine += set.Set(own[w], i);
                mine += set.Set(own[w], i);
                if (i % 7 == 0) mine += set.Set(shared, std::to_string(w) + ":" + std::to_string(i % 3));
            }
            changes.fetch_add(mine);
            running.fetch_sub(1);
        });
    }
    std::thread declarer([&] {
        for (int i = 0; i < 300; ++i) (void)set.Declare("Late" + std::to_string(i), i);
    });

    // The consumer. Each own variable's value is its serial less one, in every snapshot.
    std::shared_ptr<const VariableSnapshot> previous;
    std::uint64_t takes = 0;
    while (running.load() > 0) {
        const auto snapshot = set.Take();
        ++takes;
        for (int w = 0; w < kWriters; ++w) {
            const int value = snapshot->Get(own[w]);
            assert(static_cast<std::uint64_t>(value) + 1 == snapshot->Serial(own[w].index));
        }
        assert(snapshot->Get(sum) == static_cast<long long>(snapshot->Get(own[0])) + snapshot->Get(own[1]));
        if (previous) {
            assert(snapshot->Serial() >= previous->Serial());
            assert(snapshot->Size() >= previous->Size());
            for (std::size_t i = 0; i < previous->Size(); ++i)
                assert(snapshot->Serial(static_cast<VariableIndex>(i)) >= previous->Serial(static_cast<VariableIndex>(i)));
            if (snapshot->Serial() == previous->Serial()) assert(snapshot == previous);
        }
        previous = snapshot;
    }
    for (auto& thread : threads) thread.join();
    declarer.join();

    const auto last = set.Take();
    for (int w = 0; w < kWriters; ++w) {
        assert(last->Get(own[w]) == kWrites);
        assert(last->Serial(own[w].index) == kWrites + 1);
    }
    assert(last->Get(sum) == 2ll * kWrites);
    assert(last->Size() == kWriters + 2 + 300);
    assert(changes.load() >= kWriters * std::uint64_t(kWrites));
    // One wake per change and per declaration.
    assert(wakes.load() == declaredWakes + changes.load() + 300);
    assert(takes > 0);
    assert(set.Take() == last);
}

} // namespace

int main() {
    TestEquality();
    TestSnapshots();
    TestDerived();
    TestErrors();
    TestRequireSizes();
    TestConcurrentWriters();
    return 0;
}
