#include <ORGModuleServices/PipelineRegistry.h>
#include <ORGModuleServices/PipelinePins.h>
#include <Render/PreparedPass.h>
#include <Render/RecordingTags.h>
#include <ORGModuleServices/CompileFlightRegistry.h>
#include <ORGModuleServices/Passes/GenerateMipsPass.h>
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
#include <ORGModuleServices/ShaderCompiler.h>
#endif

#include <atomic>
#include <cstring>
#include <memory>

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (false)

int main() {
	org::services::CompileFlightRegistry<int> flights;
	CHECK(flights.TryBecomeOwnerOrWait(1)); CHECK(flights.ActiveCount() == 1); flights.Complete(1);
    // A build reaches done with its payload, and a refused one with an error.
    std::atomic_uint32_t builds{};
    bool built{}, refused{};
    org::services::BuildPipeline([](org::services::ServiceTask task) { task(); return true; },
        [&] { ++builds; return std::static_pointer_cast<void>(std::make_shared<uint32_t>(42)); },
        [&](org::services::LiveBuild build) { built = build.payload && build.error.empty(); });
    CHECK(built); CHECK(builds == 1);
    org::services::BuildPipeline([](org::services::ServiceTask) { return false; }, [&] { ++builds; return org::services::PipelinePayload{}; },
        [&](org::services::LiveBuild build) { refused = !build.payload && !build.error.empty(); });
    CHECK(refused); CHECK(builds == 1);
    // A recording binds the version at its graph's pin, and the newest without one.
    {
        org::services::PipelineRegistry registry;
        registry.Configure([](org::services::ServiceTask task) { task(); return true; });
        const auto first = std::static_pointer_cast<void>(std::make_shared<uint32_t>(1));
        const auto second = std::static_pointer_cast<void>(std::make_shared<uint32_t>(2));
        org::services::LivePipelineDesc desc;
        desc.id = "pinned";
        desc.sources = { "Pinned.hlsl" };
        desc.rebuild = [second](const org::services::LiveRebuildRequest&, org::services::LiveRebuildDone done) {
            done(org::services::LiveBuild{ second, 2, {} });
        };
        const auto slot = registry.Register(std::move(desc), first, 1, registry.SourcesGeneration());
        const auto oldPin = registry.Pin();
        registry.RequestReload({}, true);
        CHECK(registry.Service().published == 1);
        const auto newPin = registry.Pin();
        CHECK(newPin->generation > oldPin->generation);
        auto recording = org::RecordingContext::ForSkipQuestions(org::RunRef{});
        CHECK(org::services::VersionFor(*slot, recording)->payload == second);
        recording.SetTags(std::make_shared<const org::RecordingTags>(org::RecordingTags{}.With(org::services::PipelinePinTag(), oldPin)));
        CHECK(org::services::VersionFor(*slot, recording)->payload == first);
        recording.SetTags(std::make_shared<const org::RecordingTags>(org::RecordingTags{}.With(org::services::PipelinePinTag(), newPin)));
        CHECK(org::services::VersionFor(*slot, recording)->payload == second);
        // Another extension's tag does not read as a pin.
        static const org::RecordingTagKey other{ "other" };
        recording.SetTags(std::make_shared<const org::RecordingTags>(org::RecordingTags{}.With(other, oldPin)));
        CHECK(org::services::VersionFor(*slot, recording)->payload == second);
    }
    // Mip steps halve, round down and stop at 1; the group counts cover each destination.
    const auto steps = org::services::PlanGenerateMips(5, 3, 3);
    CHECK(steps.size() == 2);
    CHECK(steps[0].sourceWidth == 5 && steps[0].sourceHeight == 3 && steps[0].destinationWidth == 2 && steps[0].destinationHeight == 1);
    CHECK(steps[1].destinationWidth == 1 && steps[1].destinationHeight == 1);
    CHECK(org::services::PlanGenerateMips(4, 4, 1).empty());
    const auto groups = org::services::PlanGenerateMipsGroups(64, 32, 2, 6);
    CHECK(groups.size() == 1 && groups[0].x == 4 && groups[0].y == 2 && groups[0].z == 6);
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
    constexpr char shader[] = "[numthreads(1,1,1)] void main() {}";
    org::services::ShaderCompileRequest request{};
    request.sourceName = "inline-test.hlsl";
    request.source = { reinterpret_cast<const std::byte*>(shader), std::strlen(shader) };
    request.entryPoint = L"main"; request.target = L"cs_6_0";
    org::services::ShaderCompiler compiler;
    const auto keyA = compiler.BuildKey(request), keyB = compiler.BuildKey(request);
    CHECK(keyA != 0 && keyA == keyB);
    if (compiler.Available()) { auto artifact = compiler.Compile(request); CHECK(artifact); }
#endif
}
