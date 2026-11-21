#pragma once

#include <ORGModuleServices/RetirementJoin.h>

#include <OpenRenderGraph/GraphHost.h>

#include <functional>
#include <memory>

namespace org::services {

/** @brief How a service reaches the graph host now: null while there is none (before it is made, or after it went). */
using GraphHostAccess = std::function<const GraphHost*()>;

/**
 * @brief A RetirementJoin whose source is a graph host, plus the GPU points behind its points.
 *
 * The host's submissions are one source. The host picks the CPU pipeline's frame timeline (SetCpuFrameTimeline) and calls the
 * join's CPU frame hooks and seal hook. Without a host, points are 0 and the GPU points are empty.
 */
class GraphHostRetirement {
public:
    explicit GraphHostRetirement(GraphHostAccess host, std::shared_ptr<RetirementClock> clock = std::make_shared<RetirementClock>());
    ~GraphHostRetirement();
    GraphHostRetirement(const GraphHostRetirement&) = delete;
    GraphHostRetirement& operator=(const GraphHostRetirement&) = delete;

    [[nodiscard]] RetirementJoin& Join() noexcept { return m_join; }
    [[nodiscard]] const RetirementJoin& Join() const noexcept { return m_join; }

    /** @brief CPU frame thread. The CPU pipeline's frame timeline, set once before its first frame signals. */
    void SetCpuFrameTimeline(std::shared_ptr<rhi::TimelinePtr> timeline);

    /** @brief Submitting thread. The GPU points behind RetirementJoin::SubmittedPoint, for a GPU wait on it. */
    [[nodiscard]] GraphHost::GpuPoint SubmittedGpuPoint() const;

    /**
     * @brief Any thread. The GPU points of the work the last present sent to the device: the host's sealed point plus the CPU
     * frames sealed with it. A producer whose signal later work of the frame waits for may wait only on these. Work submitted
     * since may only reach the device at the frame's present, which waits for that very signal. Use SubmittedGpuPoint for binds,
     * not for those waits.
     */
    [[nodiscard]] GraphHost::GpuPoint SealedGpuPoint() const;

    /** @brief Any thread. Whether every point of `point` is reached or covered by SealedGpuPoint. */
    [[nodiscard]] bool Sealed(const GraphHost::GpuPoint& point) const;

private:
    class Source;
    std::shared_ptr<Source> m_source;
    RetirementJoin m_join;
};

} // namespace org::services
