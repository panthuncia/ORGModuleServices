// The build context and the build inputs, without a graph host.

#include <ORGModuleServices/BuildContext.h>
#include <ORGModuleServices/BuildInputs.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace services = org::services;

// Outside a build nothing is deferred. Inside one the calls collect in order, empty ones are skipped, and nested scopes
// give the outer build back.
void TestContext() {
    assert(!services::CurrentBuild());
    assert(!services::DeferToAttach([] {}));
    assert(!services::DeferToStaging([] {}));

    services::BuildContext outer;
    outer.input = std::make_shared<const int>(7);
    std::vector<int> order;
    {
        services::BuildScope scope(outer);
        assert(services::CurrentBuild() == &outer);
        assert(services::DeferToAttach([&] { order.push_back(1); }, [&](const std::string&) { order.push_back(-1); }));
        assert(services::DeferToAttach([&] { order.push_back(2); }));
        assert(services::DeferToAttach({}, {}));
        assert(services::DeferToStaging([&] { order.push_back(0); }));
        assert(services::DeferToStaging({}));
        {
            services::BuildContext inner;
            services::BuildScope nested(inner);
            assert(services::CurrentBuild() == &inner);
            assert(services::DeferToAttach([] {}));
            assert(inner.attached.size() == 1 && !inner.input);
        }
        assert(services::CurrentBuild() == &outer);
        // Another thread is outside any build.
        bool other = true;
        std::thread([&] { other = services::CurrentBuild() != nullptr; }).join();
        assert(!other);
    }
    assert(!services::CurrentBuild());
    assert(outer.attached.size() == 2 && outer.failed.size() == 1 && outer.staged.size() == 1);
    for (auto& fn : outer.staged)
        fn();
    for (auto& fn : outer.attached)
        fn();
    outer.failed[0]("x");
    assert((order == std::vector<int>{0, 1, 2, -1}));
    assert(*std::static_pointer_cast<const int>(outer.input) == 7);
}

struct State {
    std::uint64_t serial = 0;
    int value = 0;
};

bool Same(const State& left, const State& right) { return left.value == right.value; }

// The serial moves only when the value changes.
void TestNext() {
    using Inputs = services::BuildInputs<State>;
    Inputs::Ptr current;
    current = Inputs::Next(current, State{0, 5}, Same);
    assert(current && current->serial == 1 && current->value == 5);
    const auto kept = Inputs::Next(current, State{99, 5}, Same);
    assert(kept == current && kept->serial == 1);
    const auto next = Inputs::Next(current, State{0, 6}, Same);
    assert(next != current && next->serial == 2 && next->value == 6);
}

// Settle publishes the newest serial as built once no build is pending, and only once. While no epoch has a graph,
// the value built next is attached too.
void TestSettle() {
    using Inputs = services::BuildInputs<State>;
    Inputs inputs;
    assert(inputs.BuiltSerial() == 0 && inputs.AttachedSerial() == 0 && !inputs.Attached());

    int asked = 0;
    const auto pending = [&] {
        ++asked;
        return false;
    };
    const auto settled = [&] {
        ++asked;
        return true;
    };
    // No graph yet and a build pending: nothing.
    assert(inputs.Settle(1, false, pending) == Inputs::Settled::None);
    assert(asked == 1 && inputs.BuiltSerial() == 0);
    // The first builds staged, so they attached: built and attached.
    assert(inputs.Settle(1, true, settled) == Inputs::Settled::BuiltAndAttached);
    assert(inputs.BuiltSerial() == 1);
    // Built already: settled() is not asked.
    asked = 0;
    assert(inputs.Settle(1, true, settled) == Inputs::Settled::None);
    assert(asked == 0);
    // A later value with graphs to keep: built only.
    assert(inputs.Settle(2, true, pending) == Inputs::Settled::None);
    assert(inputs.Settle(2, true, settled) == Inputs::Settled::Built);
    assert(inputs.BuiltSerial() == 2);
    // Every graph went: the next value built attaches too.
    assert(inputs.Settle(2, false, settled) == Inputs::Settled::None);
    assert(inputs.Settle(3, true, settled) == Inputs::Settled::BuiltAndAttached);

    auto state = std::make_shared<const State>(State{3, 1});
    inputs.PublishAttached(nullptr);
    assert(!inputs.Attached() && inputs.AttachedSerial() == 0);
    inputs.PublishAttached(state);
    assert(inputs.Attached() == state && inputs.AttachedSerial() == 3);
}
} // namespace

int main() {
    TestContext();
    TestNext();
    TestSettle();
}
