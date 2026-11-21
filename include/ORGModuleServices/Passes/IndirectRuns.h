#pragma once

#include <rhi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <vector>

// Indirect run coalescing for CPU-recorded draws. A recorder writes its draws' arguments into a mapped buffer, grouped by
// the bucket each draw's lane binds, and records one indirect draw per run of consecutive draws that bind the same
// pipeline. The host keeps its draw record's layout: it writes each draw at the slot a writer hands out and gives the
// buffer's stride when recording. What a bucket binds comes from the host's binder.
//
// A batch's draws may be split into parts, such as draws the host records at different points of a pass. Each part's
// draws are consecutive in the buffer and its runs are consecutive in the run list.
namespace org::services {

/** @brief What a bucket binds: a pipeline, and a host word bound with it (such as a vertex layout). A null pipeline skips the bucket's draws. */
struct IndirectRunBinding {
    rhi::PipelineHandle pipeline{};
    std::uint64_t tag = 0;
};

/** @brief Consecutive draws with one pipeline and tag, recorded as one indirect draw. */
struct IndirectRun {
    rhi::PipelineHandle pipeline{};
    std::uint64_t tag = 0;  // the binding's host word, pushed with the run
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

inline bool SamePipeline(rhi::PipelineHandle a, rhi::PipelineHandle b) { return a.index == b.index && a.generation == b.generation; }

/**
 * @brief A batch's first pass: an upper bound on its draws and runs per part, from each lane's part, bucket and most draws.
 * A bucket change within a part may start a run.
 */
template <std::uint32_t Parts>
class IndirectRunCounts {
public:
    void Lane(std::uint32_t part, std::uint32_t bucket, std::uint32_t draws) {
        m_draws[part] += draws;
        if (bucket != m_lastBucket[part]) {
            m_lastBucket[part] = bucket;
            ++m_runs[part];
        }
    }
    [[nodiscard]] std::uint32_t Draws(std::uint32_t part) const { return m_draws[part]; }
    [[nodiscard]] std::uint32_t Runs(std::uint32_t part) const { return m_runs[part]; }
    [[nodiscard]] std::uint32_t Draws() const {
        std::uint32_t total = 0;
        for (const auto draws : m_draws) total += draws;
        return total;
    }
    [[nodiscard]] std::uint32_t Runs() const {
        std::uint32_t total = 0;
        for (const auto runs : m_runs) total += runs;
        return total;
    }

private:
    std::array<std::uint32_t, Parts> m_draws{}, m_runs{};
    std::array<std::uint32_t, Parts> m_lastBucket = [] {
        std::array<std::uint32_t, Parts> buckets{};
        buckets.fill(~0u);
        return buckets;
    }();
};

/**
 * @brief Reserves `count` draws of a shared buffer, any thread. Null when count is 0 or the draws do not fit, and then
 * nothing may be written; the cursor still moves, so later reservations fail too until the buffer is replaced.
 */
inline std::optional<std::uint32_t> ReserveIndirectDraws(std::atomic<std::uint32_t>& cursor, std::uint32_t capacity, std::uint32_t count) {
    if (!count) return std::nullopt;
    const std::uint32_t first = cursor.fetch_add(count, std::memory_order_relaxed);
    if (first > capacity || count > capacity - first) return std::nullopt;
    return first;
}

/**
 * @brief A batch's second pass, in the same lane order as its counts. Hands out each draw's slot in the buffer and
 * coalesces consecutive draws of a part that bind the same pipeline and tag into one run. `runs` holds counts.Runs()
 * entries, uninitialised.
 */
template <std::uint32_t Parts>
class IndirectRunWriter {
public:
    IndirectRunWriter(const IndirectRunCounts<Parts>& counts, std::uint32_t first, IndirectRun* runs) : m_runs(runs) {
        std::uint32_t draw = first, run = 0;
        for (std::uint32_t part = 0; part < Parts; ++part) {
            m_cursor[part] = draw;
            m_runBase[part] = m_runEnd[part] = run;
            draw += counts.Draws(part);
            run += counts.Runs(part);
        }
        m_lastBucket.fill(~0u);
    }

    /**
     * @brief A lane's bucket in its part. Asks the binder (IndirectRunBinding(std::uint32_t bucket)) only when the part's
     * bucket changes. False when the bucket binds nothing, and then the lane's draws are skipped.
     */
    template <class Binder>
    bool Bucket(std::uint32_t part, std::uint32_t bucket, Binder&& binder) {
        if (bucket != m_lastBucket[part]) {
            m_lastBucket[part] = bucket;
            m_binding[part] = binder(bucket);
        }
        return m_binding[part].pipeline.valid();
    }

    /** @brief The slot of the part's next draw, after a Bucket that returned true. Buckets that bind the same pipeline and tag share a run. */
    std::uint32_t Append(std::uint32_t part) {
        const std::uint32_t at = m_cursor[part]++;
        const IndirectRunBinding& binding = m_binding[part];
        IndirectRun* open = m_runEnd[part] > m_runBase[part] ? &m_runs[m_runEnd[part] - 1] : nullptr;
        if (!open || !SamePipeline(open->pipeline, binding.pipeline) || open->tag != binding.tag)
            open = new (&m_runs[m_runEnd[part]++]) IndirectRun{ binding.pipeline, binding.tag, at, 0u };
        ++open->count;
        return at;
    }

    /** @brief Packs the parts' runs together. runStart[p] to runStart[p + 1] are part p's runs. */
    void Finish(std::array<std::uint32_t, Parts + 1>& runStart) {
        std::uint32_t written = m_runEnd[0];
        runStart[0] = 0;
        runStart[1] = written;
        for (std::uint32_t part = 1; part < Parts; ++part) {
            for (std::uint32_t r = m_runBase[part]; r < m_runEnd[part]; ++r) m_runs[written++] = m_runs[r];
            runStart[part + 1] = written;
        }
    }

private:
    IndirectRun* m_runs;
    std::array<std::uint32_t, Parts> m_cursor{}, m_runBase{}, m_runEnd{}, m_lastBucket{};
    std::array<IndirectRunBinding, Parts> m_binding{};
};

/** @brief What a list's run recording has bound so far, and its counts. The host keeps it across a list's recordings. */
struct IndirectRunState {
    rhi::PipelineHandle bound{};
    bool wordsPushed = false;  // whether the last pushed draw words are a run's
    std::uint32_t binds = 0, pushes = 0, indirects = 0;
};

/** @brief Where a batch's draws are: the signature, the buffer, its stride and where the arguments start in a record. */
struct IndirectRunSource {
    rhi::CommandSignatureHandle signature{};
    rhi::ResourceHandle buffer{};
    std::uint64_t stride = 0;
    std::uint64_t argumentsOffset = 0;
};

/**
 * @brief Records runs as indirect draws. Binds a run's pipeline unless bound, and pushes the run's words through
 * pushWords(list, run) when it bound or the last push was not a run's. Returns the draws recorded. List: anything with
 * BindPipeline(rhi::PipelineHandle) and ExecuteIndirect as rhi::CommandList has it.
 */
template <class List, class PushWords>
std::uint32_t RecordIndirectRuns(List& list, std::span<const IndirectRun> runs, const IndirectRunSource& source, IndirectRunState& state, PushWords&& pushWords) {
    std::uint32_t drawn = 0;
    for (const IndirectRun& run : runs) {
        const bool bind = !SamePipeline(run.pipeline, state.bound);
        if (bind) {
            list.BindPipeline(run.pipeline);
            ++state.binds;
        }
        if (bind || !state.wordsPushed) {
            pushWords(list, run);
            ++state.pushes;
            state.wordsPushed = true;
        }
        state.bound = run.pipeline;
        list.ExecuteIndirect(source.signature, source.buffer, std::uint64_t(run.first) * source.stride + source.argumentsOffset, {}, 0, run.count);
        ++state.indirects;
        drawn += run.count;
    }
    return drawn;
}

/**
 * @brief Orders items nearest first in `slabs` equal depth slabs between nearest and farthest, by bucket within a slab,
 * then by distance, for fewer binds. With every item at one distance, by distance alone. Stable. Item has float
 * `distance`, std::uint32_t `slab` (written here) and std::uint32_t `bucket`. Only for draws whose result does not
 * depend on order, such as depth-only draws.
 */
template <class Item>
void OrderBySlabs(std::vector<Item>& items, float nearest, float farthest, std::uint32_t slabs) {
    if (farthest > nearest) {
        const float scale = static_cast<float>(slabs) / (farthest - nearest);
        for (auto& item : items) item.slab = (std::min)(slabs - 1, static_cast<std::uint32_t>((item.distance - nearest) * scale));
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            return a.slab != b.slab ? a.slab < b.slab : a.bucket != b.bucket ? a.bucket < b.bucket : a.distance < b.distance;
        });
    } else {
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.distance < b.distance; });
    }
}

} // namespace org::services
