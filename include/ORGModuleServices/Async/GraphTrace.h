#pragma once

#include <ORGModuleServices/Async/ArtifactIdentity.h>
#include <ORGModuleServices/Async/GraphDiagnostics.h>
#include <ORGModuleServices/Async/GraphScheduler.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace org::async {

enum class AsyncStateGraphTraceDetail : std::uint8_t {
	Summary,
	Lifecycle,
	FullDependencies
};

struct AsyncStateGraphTracePayload {
    std::array<std::uint64_t, 8> values{};
};

template <std::size_t KindCount>
struct AsyncStateGraphTraceConfig {
    std::size_t maximumEvents = 1'000'000;
	AsyncStateGraphTraceDetail detail = AsyncStateGraphTraceDetail::Lifecycle;
    bool includeDependencyEvents = true;
    bool includeRetentionEvents = true;
	// Empty/unfiltered traces retain the existing all-artifact behavior. Focused
	// captures can select kinds without adding work to the trace-off path.
	bool filterArtifactKinds = false;
	std::array<bool, KindCount> includedKinds{};
};


// Event vocabulary and report contents belong to the host; recording carries
// numeric payloads without inspecting renderer objects. The backend must quiesce
// all writers before Write() or destruction, including scheduler callbacks.
template <class Kind, class Event, std::size_t KindCount, class Report>
class GraphTraceSession {
public:
    using Key = ArtifactKey<Kind>;
    using ConfigType = AsyncStateGraphTraceConfig<KindCount>;
    virtual ~GraphTraceSession() = default;
    virtual void Record(Event event, Key key = {},
        std::uint64_t revision = 0, std::uint64_t generation = 0,
        ArtifactReadiness readiness = ArtifactReadiness::Missing,
        std::int64_t durationMicros = 0, AsyncStateGraphTracePayload payload = {},
        Key related = {}, std::uint64_t relatedRevision = 0) = 0;
    virtual void RecordSchedulerEvent(const TaskTraceEvent&) = 0;
    virtual void RecordMutex(GraphMutexPhase, std::uint64_t waitMicros,
        std::uint64_t holdMicros, std::uint64_t holdCpuMicros, const GraphMutexCounts&) = 0;
    virtual Report Write(const std::filesystem::path&) = 0;
    virtual const ConfigType& Config() const = 0;
};

} // namespace org::async
