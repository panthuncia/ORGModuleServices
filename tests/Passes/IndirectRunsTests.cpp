// Indirect run coalescing and slab ordering (ORGModuleServices/Passes/IndirectRuns.h). No device.
#include <ORGModuleServices/Passes/IndirectRuns.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); return __LINE__; } } while (false)

namespace {

using namespace org::services;

rhi::PipelineHandle Pipeline(std::uint32_t index) { return rhi::PipelineHandle{ index, 1 }; }

struct Lane {
    std::uint32_t part;
    std::uint32_t bucket;
    std::uint32_t draws;
};

// Buckets 0 and 1 share a pipeline, bucket 2 has its own with a tag, bucket 3 binds nothing.
IndirectRunBinding Bind(std::uint32_t bucket) {
    switch (bucket) {
    case 0:
    case 1:
        return { Pipeline(10), 0 };
    case 2:
        return { Pipeline(20), 7 };
    default:
        return {};
    }
}

int TestCoalescing() {
    const std::vector<Lane> lanes = {
        { 0, 0, 2 }, { 0, 1, 1 }, { 1, 2, 1 }, { 0, 2, 1 }, { 0, 3, 4 }, { 2, 0, 1 }, { 1, 2, 2 },
    };
    IndirectRunCounts<3> counts;
    for (const auto& lane : lanes) counts.Lane(lane.part, lane.bucket, lane.draws);
    CHECK(counts.Draws(0) == 8 && counts.Draws(1) == 3 && counts.Draws(2) == 1 && counts.Draws() == 12);
    // Bucket changes per part: part 0 sees 0, 1, 2, 3; part 1 sees 2 once; part 2 sees 0.
    CHECK(counts.Runs(0) == 4 && counts.Runs(1) == 1 && counts.Runs(2) == 1 && counts.Runs() == 6);

    std::atomic<std::uint32_t> cursor{ 100 };
    const auto first = ReserveIndirectDraws(cursor, 1000, counts.Draws());
    CHECK(first && *first == 100 && cursor.load() == 112);

    std::vector<IndirectRun> runs(counts.Runs());
    IndirectRunWriter<3> writer(counts, *first, runs.data());
    std::vector<std::uint32_t> slots;
    std::uint32_t binds = 0;
    const auto binder = [&](std::uint32_t bucket) {
        ++binds;
        return Bind(bucket);
    };
    for (const auto& lane : lanes) {
        if (!writer.Bucket(lane.part, lane.bucket, binder)) continue;
        for (std::uint32_t d = 0; d < lane.draws; ++d) slots.push_back(writer.Append(lane.part));
    }
    // The binder is asked once per bucket change in a part.
    CHECK(binds == 6);
    // Part 0's draws start at the reservation, part 1's after part 0's most draws, part 2's after part 1's.
    const std::vector<std::uint32_t> expected = { 100, 101, 102, 108, 103, 111, 109, 110 };
    CHECK(slots == expected);
    std::array<std::uint32_t, 4> runStart{};
    writer.Finish(runStart);
    // Part 0: buckets 0 and 1 share one run, bucket 2 another; bucket 3 made none. Part 1: one run. Part 2: one run.
    CHECK(runStart[0] == 0 && runStart[1] == 2 && runStart[2] == 3 && runStart[3] == 4);
    CHECK(SamePipeline(runs[0].pipeline, Pipeline(10)) && runs[0].first == 100 && runs[0].count == 3 && runs[0].tag == 0);
    CHECK(SamePipeline(runs[1].pipeline, Pipeline(20)) && runs[1].first == 103 && runs[1].count == 1 && runs[1].tag == 7);
    CHECK(SamePipeline(runs[2].pipeline, Pipeline(20)) && runs[2].first == 108 && runs[2].count == 3);
    CHECK(SamePipeline(runs[3].pipeline, Pipeline(10)) && runs[3].first == 111 && runs[3].count == 1);
    return 0;
}

int TestReserve() {
    std::atomic<std::uint32_t> cursor{ 0 };
    CHECK(!ReserveIndirectDraws(cursor, 10, 0) && cursor.load() == 0);
    CHECK(ReserveIndirectDraws(cursor, 10, 6) == 0u);
    // Past capacity: refused, and the cursor moved on.
    CHECK(!ReserveIndirectDraws(cursor, 10, 5) && cursor.load() == 11);
    CHECK(!ReserveIndirectDraws(cursor, 10, 1));
    return 0;
}

// A list that records what it was asked to do.
struct FakeList {
    std::vector<std::string> commands;
    void BindPipeline(rhi::PipelineHandle pipeline) { commands.push_back("bind " + std::to_string(pipeline.index)); }
    void ExecuteIndirect(rhi::CommandSignatureHandle, rhi::ResourceHandle, std::uint64_t offset, rhi::ResourceHandle, std::uint64_t, std::uint32_t count) {
        commands.push_back("draw " + std::to_string(offset) + " x" + std::to_string(count));
    }
};

int TestRecording() {
    const std::vector<IndirectRun> runs = {
        { Pipeline(10), 0, 0, 3 },
        { Pipeline(10), 0, 3, 1 },
        { Pipeline(20), 5, 4, 2 },
    };
    FakeList list;
    IndirectRunState state;
    state.bound = Pipeline(10);  // bound by a direct draw, whose words are not a run's
    const IndirectRunSource source{ .signature = {}, .buffer = {}, .stride = 32, .argumentsOffset = 4 };
    const auto push = [](FakeList& target, const IndirectRun& run) { target.commands.push_back("push " + std::to_string(run.tag)); };
    const std::uint32_t drawn = RecordIndirectRuns(list, runs, source, state, push);
    CHECK(drawn == 6);
    const std::vector<std::string> expected = { "push 0", "draw 4 x3", "draw 100 x1", "bind 20", "push 5", "draw 132 x2" };
    CHECK(list.commands == expected);
    CHECK(state.binds == 1 && state.pushes == 2 && state.indirects == 3 && state.wordsPushed && SamePipeline(state.bound, Pipeline(20)));
    // A second batch on the same list binds and pushes nothing new for the same pipeline.
    list.commands.clear();
    const std::vector<IndirectRun> more = { { Pipeline(20), 5, 6, 1 } };
    CHECK(RecordIndirectRuns(list, more, source, state, push) == 1);
    CHECK(list.commands == std::vector<std::string>{ "draw 196 x1" });
    return 0;
}

struct Item {
    float distance;
    std::uint32_t slab;
    std::uint32_t bucket;
    std::uint32_t id;
};

int TestSlabs() {
    std::vector<Item> items = {
        { 9.0f, 0, 1, 0 }, { 0.5f, 0, 2, 1 }, { 0.2f, 0, 1, 2 }, { 5.0f, 0, 0, 3 }, { 5.5f, 0, 0, 4 }, { 0.0f, 0, 2, 5 },
    };
    OrderBySlabs(items, 0.0f, 9.0f, 3);
    // Slabs of width 3: [0, 3) holds 1, 2, 5; [3, 6) holds 3, 4; the farthest is clamped into the last slab.
    // Within a slab by bucket, then distance.
    std::vector<std::uint32_t> order;
    for (const auto& item : items) order.push_back(item.id);
    CHECK((order == std::vector<std::uint32_t>{ 2, 5, 1, 3, 4, 0 }));
    CHECK(items[5].slab == 2 && items[0].slab == 0 && items[3].slab == 1);
    // One distance for all: by distance alone, stable.
    std::vector<Item> flat = { { 1.0f, 0, 3, 0 }, { 1.0f, 0, 1, 1 } };
    OrderBySlabs(flat, 1.0f, 1.0f, 8);
    CHECK(flat[0].id == 0 && flat[1].id == 1);
    return 0;
}

} // namespace

int main() {
    if (const int line = TestCoalescing()) return line;
    if (const int line = TestReserve()) return line;
    if (const int line = TestRecording()) return line;
    if (const int line = TestSlabs()) return line;
    std::printf("IndirectRuns tests passed\n");
    return 0;
}
