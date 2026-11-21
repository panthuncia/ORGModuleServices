#include <ORGModuleServices/GraphHostRetirement.h>

#include <algorithm>
#include <utility>

namespace org::services {

/** @brief The join's source: the host's submission count and the CPU frame timeline. */
class GraphHostRetirement::Source final : public RetirementJoin::Source {
public:
    explicit Source(GraphHostAccess host) : host(std::move(host)) {}

    std::optional<std::uint64_t> SubmissionsMade() const override {
        const auto* graph = Host();
        if (!graph) return std::nullopt;
        return graph->SubmissionsMade();
    }

    std::uint64_t CpuFramesCompleted() const override {
        return timeline && *timeline ? (*timeline)->GetCompletedValue() : 0;
    }

    const GraphHost* Host() const { return host ? host() : nullptr; }

    GraphHostAccess host;
    // Set once before the first CPU frame signals. Read after an acquire of the join's CPU counts.
    std::shared_ptr<rhi::TimelinePtr> timeline;
};

GraphHostRetirement::GraphHostRetirement(GraphHostAccess host, std::shared_ptr<RetirementClock> clock)
    : m_source(std::make_shared<Source>(std::move(host))), m_join(m_source, std::move(clock)) {}

GraphHostRetirement::~GraphHostRetirement() = default;

void GraphHostRetirement::SetCpuFrameTimeline(std::shared_ptr<rhi::TimelinePtr> timeline) {
    m_source->timeline = std::move(timeline);
}

GraphHost::GpuPoint GraphHostRetirement::SubmittedGpuPoint() const {
    const auto* host = m_source->Host();
    GraphHost::GpuPoint point = host ? host->SubmittedPoint() : GraphHost::GpuPoint{};
    if (const std::uint64_t cpu = m_join.CpuFramesSignalled(); cpu && m_source->timeline) point.points.emplace_back(m_source->timeline, cpu);
    return point;
}

GraphHost::GpuPoint GraphHostRetirement::SealedGpuPoint() const {
    const auto* host = m_source->Host();
    GraphHost::GpuPoint point = host ? host->SealedPoint() : GraphHost::GpuPoint{};
    if (const std::uint64_t cpu = m_join.CpuFramesSealed(); cpu && m_source->timeline) point.points.emplace_back(m_source->timeline, cpu);
    return point;
}

bool GraphHostRetirement::Sealed(const GraphHost::GpuPoint& point) const {
    const auto sealed = SealedGpuPoint();
    // A seal still queued on the submission thread has no values yet, so nothing can be said until the thread passes it.
    if (!sealed.Passed()) return true;
    // Nothing is sealed before the first present.
    if (sealed.points.empty()) return true;
    // A point still in the submission thread's queue names work that is not on the device yet.
    if (!point.Passed()) return false;
    for (const auto& [timeline, value] : point.Resolve()) {
        if (!timeline || !*timeline || (*timeline)->GetCompletedValue() >= value) continue;
        const auto known = std::ranges::find_if(sealed.points, [&](const auto& entry) { return entry.first.get() == timeline.get(); });
        if (known == sealed.points.end() || known->second < value) return false;
    }
    return true;
}

} // namespace org::services
