#include <ORGModuleServices/RetirementChain.h>

#include <cassert>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace {
// Deferred reclamation by ownership: an item retired after publication p comes back only once every publication up to p is
// gone, whatever order they are dropped in, exactly once.
struct Retired {
    std::vector<std::pair<unsigned, unsigned>> items;  // (item, the newest publication made before it was retired)
    bool Empty() const { return items.empty(); }
};
void TestRetirementChain() {
    std::mt19937 random(1201);
    org::services::RetirementChain<Retired> chain;
    std::map<unsigned, std::shared_ptr<const void>> held;  // publication -> its hold
    unsigned publications = 0, next = 0, returnedCount = 0;
    std::set<unsigned> outstanding;
    auto check = [&](const Retired& a_batch) {
        for (const auto& [item, after] : a_batch.items) {
            for (const auto& [p, hold] : held)
                assert(p > after || after == 0);  // nothing up to 'after' still held (0: retired before any publication)
            assert(outstanding.erase(item) == 1);
            ++returnedCount;
        }
    };
    for (unsigned step = 0; step < 20000; ++step) {
        switch (random() % 4) {
        case 0:
            chain.Open().items.emplace_back(next, publications);
            outstanding.insert(next++);
            break;
        case 1:
            held.emplace(++publications, chain.Publish());
            break;
        default:
            if (!held.empty() && random() % 2) {
                auto it = held.begin();
                std::advance(it, random() % held.size());
                held.erase(it);
            }
            break;
        }
        chain.Drain(check);
    }
    held.clear();
    chain.Publish();  // the open node closed, nothing holding the one before it
    chain.Drain(check);
    assert(outstanding.empty() && returnedCount == next);
}

// Publications pushed from several threads are dropped only by the dropping thread, all of them.
void TestRetired() {
    struct Counted {
        std::shared_ptr<int> alive;
    };
    auto alive = std::make_shared<int>(0);
    org::services::Retired<Counted> retired;  // 64 cells, so most pushes spill
    std::vector<std::thread> pushers;
    for (int t = 0; t < 4; ++t)
        pushers.emplace_back([&] {
            for (int i = 0; i < 50; ++i)
                retired.Push(std::make_shared<const Counted>(Counted{alive}));
            retired.Push(nullptr);
        });
    for (auto& pusher : pushers)
        pusher.join();
    assert(alive.use_count() == 201);
    retired.Drop();
    assert(alive.use_count() == 1);
}
}  // namespace

int main() {
    TestRetirementChain();
    TestRetired();
}
