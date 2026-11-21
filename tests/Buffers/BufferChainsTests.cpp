// BufferChains, standalone (MS headers only; backings have no org::Buffer). The post hook stands in for the graph: it keeps each
// build and the test finishes it, as BuildVersionedBuffer would, by leasing a backing and running built, then done.
#include <ORGModuleServices/BufferChains.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <tuple>
#include <vector>

using namespace org::services;

namespace {

void Check(bool condition, const std::source_location location = std::source_location::current()) {
    if (condition) return;
    std::fprintf(stderr, "check failed at %s:%u\n", location.file_name(), static_cast<unsigned>(location.line()));
    std::fflush(stderr);
    std::abort();
}

constexpr std::size_t kChains = 2;  // 0: journalled, no tails; 1: journalled with tails

struct Source final : BufferChainSource {
    std::uint64_t serial = 0;
    std::array<bool, kChains> present{true, true};
    std::array<std::uint64_t, kChains> units{}, epochs{}, tails{kNoTail, kNoTail};
    std::vector<BufferByteRange> changes;  // what ChangedSince returns for any epoch at or above `floor`
    std::uint64_t floor = 1;
    struct Emitted {
        std::size_t chain;
        std::optional<std::uint64_t> from;
        std::uint64_t version, previous;
    };
    mutable std::vector<Emitted> emitted;

    std::uint64_t Serial() const override { return serial; }
    bool Present(std::size_t chain) const override { return present[chain]; }
    std::uint64_t Units(std::size_t chain) const override { return units[chain]; }
    std::uint64_t Elements(std::size_t chain) const override { return units[chain] * 2; }
    std::uint64_t ContentEpoch(std::size_t chain) const override { return epochs[chain]; }
    std::uint64_t TailStart(std::size_t chain) const override { return tails[chain]; }
    std::optional<std::vector<BufferByteRange>> ChangedSince(std::size_t, std::uint64_t epoch) const override {
        if (epoch < floor) return std::nullopt;
        return changes;
    }
    void EmitDelta(std::size_t chain, std::optional<std::uint64_t> from, std::uint64_t version, std::uint64_t previous,
        const ChainDeltaWrite& write) const override {
        emitted.push_back({chain, from, version, previous});
        const std::uint32_t word = 7;
        write(&word, sizeof(word), 0);
    }
};

struct Posted {
    VersionedBufferBuildInput input;
    BufferChainBuilt built;
    BufferChainDone done;
};

struct Harness {
    std::shared_ptr<RetirementClock> clock = std::make_shared<RetirementClock>();
    std::vector<Posted> posted;
    std::vector<std::tuple<std::size_t, std::uint64_t, std::string>> failures;
    std::size_t pools = 0, wakes = 0;
    bool accept = true;
    std::unique_ptr<BufferChains> chains;

    Harness() {
        BufferChainsDesc desc;
        desc.chains.push_back({.name = "test.plain", .stride = 4});
        desc.chains.push_back({.name = "test.tailed", .stride = 4, .deviceAddress = true, .tailed = true});
        desc.makePool = [this](std::size_t chain, std::function<void(BackingPoolEvent, std::string_view, std::uint64_t)> telemetry) {
            ++pools;
            BackingPoolHooks hooks;
            hooks.allocate = [count = std::make_shared<std::uint64_t>(0)](const BackingRequest&) {
                BackingAllocation allocation;
                allocation.srvIndex = static_cast<std::uint32_t>(++*count);
                return allocation;
            };
            hooks.telemetry = std::move(telemetry);
            return BackingPool::Create(std::move(hooks), clock, {.name = chain ? "tailed" : "plain"});
        };
        desc.post = [this](VersionedBufferBuildInput input, BufferChainBuilt built, BufferChainDone done) {
            if (!accept) return false;
            posted.push_back({std::move(input), std::move(built), std::move(done)});
            return true;
        };
        desc.wake = [this] { ++wakes; };
        desc.failed = [this](std::size_t chain, std::uint64_t version, std::string_view error) { failures.emplace_back(chain, version, std::string(error)); };
        desc.growth = GrowthPolicy::Headroom(25, 64);
        chains = std::make_unique<BufferChains>(std::move(desc));
    }

    // Finishes a posted build as BuildVersionedBuffer does: a lease at the version's sequence, built, then done.
    std::shared_ptr<const PublishedBufferVersion> Complete(Posted& build, bool ok = true) {
        if (!ok) {
            build.done(nullptr, "boom");
            return nullptr;
        }
        BackingRequest request;
        request.capacityClass = build.input.capacity;
        request.stride = build.input.elementStride;
        request.name = build.input.debugName;
        auto lease = build.input.backingPool->Acquire(request);
        Check(static_cast<bool>(lease));
        std::uint64_t bytes = 0;
        build.input.emitDelta([&](const void*, std::size_t size, std::uint64_t) { bytes += size; });
        lease->contentEpoch.store(build.input.writeSequence);
        auto version = std::make_shared<PublishedBufferVersion>();
        version->writeSequence = build.input.writeSequence;
        version->elementCount = build.input.elementCount;
        version->lease = std::move(lease);
        version->uploadedBytes = bytes;
        build.built(*version);
        build.done(version, {});
        return version;
    }
};

std::shared_ptr<Source> MakeSource(std::uint64_t serial, std::uint64_t epoch0, std::uint64_t epoch1) {
    auto source = std::make_shared<Source>();
    source->serial = serial;
    source->units = {4, 8};
    source->epochs = {epoch0, epoch1};
    source->tails = {BufferChainSource::kNoTail, 64};
    source->changes = {{0, 16}};
    return source;
}

void TestFirstVersions() {
    Harness h;
    const auto source = MakeSource(1, 5, 3);
    std::array<std::shared_ptr<const PublishedBufferVersion>, kChains> out;
    Check(!h.chains->Finish(*source, out));
    Check(h.chains->Post(source, nullptr));
    Check(h.posted.size() == 2 && h.pools == 2);
    Check(h.posted[0].input.debugName == "test.plain" && h.posted[1].input.flags.deviceAddress);
    Check(h.posted[0].input.writeSequence == 1 && h.posted[0].input.elementCount == 8 && h.posted[0].input.capacity == 8);
    Check(!h.posted[0].input.previous && h.posted[0].input.keep == source);
    Check(h.posted[0].input.growth && h.posted[0].input.growth->percent == 25);
    Check(!h.chains->Settle());
    Check(h.chains->Pending().size() == 2 && !h.chains->Pending()[0].done);
    const auto first = h.Complete(h.posted[0]);
    const auto second = h.Complete(h.posted[1]);
    Check(h.wakes == 2);
    // The first versions have no predecessor position: everything is sent.
    Check(source->emitted.size() == 2 && !source->emitted[0].from && !source->emitted[1].from && source->emitted[0].previous == 0);
    Check(h.chains->Settle() && h.chains->Pending().empty());
    Check(h.chains->Finish(*source, out) && out[0] == first && out[1] == second);
    // Each pool's oldest backing holds version 1, at the source's journal position.
    Check(h.chains->OldestContentEpoch(0) == 5u && h.chains->OldestContentEpoch(1) == 3u);
    const auto report = h.chains->TakeReport();
    Check(report.size() == 2 && report[0].built == 1 && report[0].uploadedBytes == 4 && report[0].liveBackings == 1 && report[0].floor == 5u);
    Check(h.chains->TakeReport()[0].built == 0);
}

void TestOnlyChangedChainsBuild() {
    Harness h;
    const auto first = MakeSource(1, 5, 3);
    Check(h.chains->Post(first, nullptr));
    const auto plain = h.Complete(h.posted[0]);
    const auto tailed = h.Complete(h.posted[1]);
    Check(h.chains->Settle());
    // The same journal positions and units from a new source: the plain chain is unchanged, the tailed one changes with the source.
    const auto next = MakeSource(2, 5, 3);
    Check(h.chains->Post(next, nullptr));
    Check(h.posted.size() == 3);
    auto& build = h.posted[2];
    Check(build.input.debugName == "test.tailed" && build.input.writeSequence == 2 && build.input.previous == tailed);
    // A backing at version 1 lacks the journal's runs since its position, plus everything from the tail start on.
    const auto ranges = build.input.changedSince(1);
    Check(ranges && ranges->size() == 2 && (*ranges)[0].offset == 0 && (*ranges)[1].offset == 64 && (*ranges)[1].size == ~0ull - 64);
    // An unknown version, or a journal that does not reach back, copies whole.
    Check(!build.input.changedSince(99));
    next->floor = 4;
    Check(!build.input.changedSince(1));
    const auto version = h.Complete(build);
    Check(next->emitted.size() == 1 && next->emitted[0].from == 3u && next->emitted[0].previous == 1 && next->emitted[0].version == 2);
    Check(h.chains->Settle());
    std::array<std::shared_ptr<const PublishedBufferVersion>, kChains> out;
    Check(h.chains->Finish(*next, out) && out[0] == plain && out[1] == version);
    // A new journal position rebuilds the plain chain from its predecessor.
    const auto moved = MakeSource(3, 6, 3);
    Check(h.chains->Post(moved, nullptr));
    Check(h.posted.size() == 5 && h.posted[3].input.previous == plain);
}

void TestFailureAndAbsence() {
    Harness h;
    const auto first = MakeSource(1, 5, 3);
    Check(h.chains->Post(first, nullptr));
    h.Complete(h.posted[0]);
    h.Complete(h.posted[1]);
    Check(h.chains->Settle());
    // A failed build: reported, and the source has no versions. The chain keeps its last version.
    const auto failing = MakeSource(2, 6, 3);
    failing->present[1] = false;
    Check(h.chains->Post(failing, nullptr));
    Check(h.posted.size() == 3);
    h.Complete(h.posted[2], false);
    Check(h.chains->Settle());
    Check(h.failures.size() == 1 && std::get<0>(h.failures[0]) == 0 && std::get<1>(h.failures[0]) == 2 && std::get<2>(h.failures[0]) == "boom");
    std::array<std::shared_ptr<const PublishedBufferVersion>, kChains> out;
    Check(!h.chains->Finish(*failing, out));
    Check(h.chains->TakeReport()[0].failures == 1);
    // The next source builds it again. An absent chain is left out of the result.
    const auto retry = MakeSource(3, 6, 3);
    retry->present[1] = false;
    Check(h.chains->Post(retry, nullptr));
    const auto version = h.Complete(h.posted[3]);
    Check(h.chains->Settle());
    Check(h.chains->Finish(*retry, out) && out[0] == version && !out[1]);
    // A refused post fails the source's chains that were refused.
    h.accept = false;
    const auto refused = MakeSource(4, 7, 3);
    refused->present[1] = false;
    Check(!h.chains->Post(refused, nullptr));
    Check(!h.chains->Finish(*refused, out));
}

} // namespace

int main() {
    TestFirstVersions();
    TestOnlyChangedChainsBuild();
    TestFailureAndAbsence();
    std::puts("PASS: BufferChainsTests");
    return 0;
}
