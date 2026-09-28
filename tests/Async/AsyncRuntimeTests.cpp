#include <ORGModuleServices/Async/GraphTrace.h>
#include <ORGModuleServices/Async/SuspensionIdentity.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <thread>
#include <type_traits>
#include <vector>

std::uint64_t AllocateFromOtherTranslationUnit();

namespace {
enum class Kind : std::uint16_t { Actor, Geometry };
enum class Event : std::uint16_t { Requested = 17, Published = 31 };
struct Report {
    std::filesystem::path destination;
    unsigned events = 0;
};
using Trace = org::async::GraphTraceSession<Kind, Event, 2, Report>;

class TestTrace final : public Trace {
public:
    void Record(Event event, Key key, std::uint64_t revision, std::uint64_t generation,
        org::async::ArtifactReadiness readiness, std::int64_t duration,
        org::async::AsyncStateGraphTracePayload payload, Key related, std::uint64_t relatedRevision) override {
        lastEvent = event;
        lastKey = key;
        lastRevision = revision;
        lastGeneration = generation;
        lastReadiness = readiness;
        lastDuration = duration;
        lastPayload = payload;
        lastRelated = related;
        lastRelatedRevision = relatedRevision;
        ++events;
    }
    void RecordSchedulerEvent(const org::async::TaskTraceEvent& event) override {
        schedulerEvent = event;
    }
    void RecordMutex(org::async::GraphMutexPhase phase, std::uint64_t wait, std::uint64_t hold,
        std::uint64_t cpu, const org::async::GraphMutexCounts& counts) override {
        mutexPhase = phase;
        mutexTimes = {wait, hold, cpu};
        mutexCounts = counts;
    }
    Report Write(const std::filesystem::path& path) override { return {path, events}; }
    const ConfigType& Config() const override { return config; }

    ConfigType config;
    Event lastEvent{};
    Key lastKey{}, lastRelated{};
    std::uint64_t lastRevision = 0, lastGeneration = 0, lastRelatedRevision = 0;
    org::async::ArtifactReadiness lastReadiness{};
    std::int64_t lastDuration = 0;
    org::async::AsyncStateGraphTracePayload lastPayload;
    org::async::TaskTraceEvent schedulerEvent;
    org::async::GraphMutexPhase mutexPhase{};
    std::array<std::uint64_t, 3> mutexTimes{};
    org::async::GraphMutexCounts mutexCounts;
    unsigned events = 0;
};

void TestTraceContract() {
    using namespace org::async;
    static_assert(std::is_abstract_v<Trace>);
    static_assert(std::has_virtual_destructor_v<Trace>);
    static_assert(std::is_trivially_copyable_v<AsyncStateGraphTracePayload>);
    TestTrace implementation;
    Trace& trace = implementation;
    assert(trace.Config().maximumEvents == 1'000'000);
    assert(trace.Config().detail == AsyncStateGraphTraceDetail::Lifecycle);
    assert(trace.Config().includeDependencyEvents && trace.Config().includeRetentionEvents);
    assert(!trace.Config().filterArtifactKinds && trace.Config().includedKinds.size() == 2);
    assert(!trace.Config().includedKinds[0] && !trace.Config().includedKinds[1]);
    const Trace::Key actor{Kind::Actor, 4, 8}, geometry{Kind::Geometry, 9, 2};
    trace.Record(Event::Requested, actor, 5, 6, ArtifactReadiness::GpuReady, 13,
        {{1, 2, 3, 4, 5, 6, 7, 8}}, geometry, 10);
    assert(implementation.lastEvent == Event::Requested && implementation.lastKey == actor);
    assert(implementation.lastRevision == 5 && implementation.lastGeneration == 6);
    assert(implementation.lastReadiness == ArtifactReadiness::GpuReady && implementation.lastDuration == 13);
    assert(implementation.lastPayload.values[7] == 8);
    assert(implementation.lastRelated == geometry && implementation.lastRelatedRevision == 10);
    trace.Record(Event::Published); // Shared interface retains optional argument defaults.
    assert(implementation.lastKey == Trace::Key{} && implementation.lastRevision == 0);
    assert(implementation.lastReadiness == ArtifactReadiness::Missing);
    TaskTraceEvent scheduler;
    scheduler.taskID = 72;
    scheduler.metadata.correlationID = 88;
    trace.RecordSchedulerEvent(scheduler);
    assert(implementation.schedulerEvent.taskID == 72 && implementation.schedulerEvent.metadata.correlationID == 88);
    trace.RecordMutex(GraphMutexPhase::Reclaim, 3, 4, 5, {1, 2, 3, 4, 5, 6});
    assert(implementation.mutexPhase == GraphMutexPhase::Reclaim);
    assert((implementation.mutexTimes == std::array<std::uint64_t, 3>{3, 4, 5}));
    assert(implementation.mutexCounts.waiters == 6);
    const auto report = trace.Write("host-report");
    assert(report.destination == "host-report" && report.events == 2);

    assert(KeyString(actor) == "0:4:8");
    assert(ReadinessName(ArtifactReadiness::GpuReady) == "GpuReady");
    assert(ReadinessName(static_cast<ArtifactReadiness>(255)) == "Unknown");
    assert(RequestStatusName(ArtifactRequestStatus::TypeMismatch) == "TypeMismatch");
    assert(RequestStatusName(static_cast<ArtifactRequestStatus>(255)) == "Unknown");
    for (unsigned phase = 0; phase != static_cast<unsigned>(GraphMutexPhase::Count); ++phase)
        assert(GraphMutexPhaseName(static_cast<GraphMutexPhase>(phase)) != "Unknown");
    assert(GraphMutexPhaseName(GraphMutexPhase::Count) == "Unknown");
    assert(StableTraceID("") != 0 && StableTraceID("build") != StableTraceID("request"));
    assert(TraceCorrelationID(actor, 5, 6) != TraceCorrelationID(actor, 5, 7));
    assert(TraceCorrelationID(actor, 5, 6) != TraceCorrelationID(geometry, 5, 6));
}

void TestSharedSuspensionNamespace() {
    constexpr unsigned threadCount = 8, perThread = 4096;
    std::array<std::vector<std::uint64_t>, threadCount> values;
    std::vector<std::thread> workers;
    for (unsigned lane = 0; lane != threadCount; ++lane) {
        workers.emplace_back([&, lane] {
            auto& local = values[lane];
            local.reserve(perThread);
            for (unsigned index = 0; index != perThread; ++index)
                local.push_back(index & 1 ? AllocateFromOtherTranslationUnit()
                    : org::async::AllocateArtifactSuspensionIdentity());
        });
    }
    for (auto& worker : workers) worker.join();
    std::vector<std::uint64_t> merged;
    for (const auto& local : values) merged.insert(merged.end(), local.begin(), local.end());
    std::sort(merged.begin(), merged.end());
    assert(merged.size() == threadCount * perThread && merged.front() != 0);
    assert(std::adjacent_find(merged.begin(), merged.end()) == merged.end());
    const auto first = org::async::AllocateArtifactSuspensionIdentity();
    const auto second = AllocateFromOtherTranslationUnit();
    assert(second == first + 1);
}
}

int main() {
    TestTraceContract();
    TestSharedSuspensionNamespace();
}
