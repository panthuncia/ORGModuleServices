#include <ORGModuleServices/ViolationPolicy.h>

#include <string>
#include <thread>
#include <vector>

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (false)

namespace {

int TestCounted() {
    using Outcome = org::services::ViolationPolicy::Outcome;
    org::services::ViolationPolicy policy({.kinds = 3, .fatal = false});
    CHECK(!policy.Fatal());
    // The first of each kind logs; later ones only count.
    CHECK(policy.Note(1, "first of kind 1") == Outcome::First);
    CHECK(policy.Note(1, "second of kind 1") == Outcome::Counted);
    CHECK(policy.Note(2, "first of kind 2") == Outcome::First);
    CHECK(policy.Note(3, "out of range") == Outcome::Invalid);
    CHECK(policy.Count(0) == 0 && policy.Count(1) == 2 && policy.Count(2) == 1 && policy.Count(3) == 0);
    CHECK(policy.Total() == 4);
    // Warnings last the session, once each, in the order they were raised.
    CHECK(policy.RaiseWarning("b"));
    CHECK(policy.RaiseWarning("a"));
    CHECK(!policy.RaiseWarning("b"));
    const auto warnings = policy.Warnings();
    CHECK(warnings.size() == 2 && warnings[0] == "b" && warnings[1] == "a");
    return 0;
}

int TestFatal() {
    using Outcome = org::services::ViolationPolicy::Outcome;
    std::vector<std::string> fatal;
    org::services::ViolationPolicy policy(
        {.kinds = 2, .fatal = true, .onFatal = [&](std::string_view message) { fatal.emplace_back(message); }});
    CHECK(policy.Fatal());
    // Every violation goes to the callback, the first and the later ones, and none counts by kind.
    CHECK(policy.Note(0, "one") == Outcome::Fatal);
    CHECK(policy.Note(0, "two") == Outcome::Fatal);
    CHECK(fatal.size() == 2 && fatal[0] == "one" && fatal[1] == "two");
    CHECK(policy.Count(0) == 0 && policy.Total() == 2);
    return 0;
}

int TestConcurrent() {
    using Outcome = org::services::ViolationPolicy::Outcome;
    org::services::ViolationPolicy policy({.kinds = 1});
    constexpr unsigned kThreads = 4, kEach = 500;
    std::vector<unsigned> firsts(kThreads, 0);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            for (unsigned i = 0; i < kEach; ++i) {
                if (policy.Note(0, "concurrent") == Outcome::First) ++firsts[t];
                policy.RaiseWarning("shared");
            }
        });
    for (auto& thread : threads) thread.join();
    unsigned first = 0;
    for (const auto count : firsts) first += count;
    CHECK(first == 1);
    CHECK(policy.Count(0) == kThreads * kEach && policy.Warnings().size() == 1);
    return 0;
}

} // namespace

int main() {
    if (const int line = TestCounted()) return line;
    if (const int line = TestFatal()) return line;
    if (const int line = TestConcurrent()) return line;
    return 0;
}
