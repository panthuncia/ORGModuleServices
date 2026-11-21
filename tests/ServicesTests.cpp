#include <ORGModuleServices/PipelineRegistry.h>
#include <ORGModuleServices/CompileFlightRegistry.h>
#include <ORGModuleServices/Passes/GenerateMipsPass.h>
#include <ORGModuleServices/Passes/HzbPass.h>
#include <ORGModuleServices/Passes/PrefixSumPass.h>
#include <ORGModuleServices/Passes/SortScatter.h>
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
    // Mip steps halve, round down and stop at 1; the group counts cover each destination.
    const auto steps = org::services::PlanGenerateMips(5, 3, 3);
    CHECK(steps.size() == 2);
    CHECK(steps[0].sourceWidth == 5 && steps[0].sourceHeight == 3 && steps[0].destinationWidth == 2 && steps[0].destinationHeight == 1);
    CHECK(steps[1].destinationWidth == 1 && steps[1].destinationHeight == 1);
    CHECK(org::services::PlanGenerateMips(4, 4, 1).empty());
    const auto groups = org::services::PlanGenerateMipsGroups(64, 32, 2, 6);
    CHECK(groups.size() == 1 && groups[0].x == 4 && groups[0].y == 2 && groups[0].z == 6);
    // Prefix sum blocks round up.
    CHECK(org::services::PrefixSumBlocks(1) == 1 && org::services::PrefixSumBlocks(1024) == 1 && org::services::PrefixSumBlocks(1025) == 2);
    // The scatter's defines: the rank bits, and the range words only with ranges.
    CHECK(org::services::SortScatterDefines({}).size() == 1);
    const auto scatter = org::services::SortScatterDefines({.rankBits = 20, .ranges = true, .rangeSlotWord = 120, .rangeKeyWord = 132, .rangeSlotValue = 1});
    CHECK(scatter.size() == 5 && scatter[0].second == L"20" && scatter[1].second == L"120" && scatter[2].second == L"132" &&
        scatter[3].second == L"1" && scatter[4].second == L"0");
    // The HZB's mip 0 is half the next power of two, down to one texel.
    const auto hzb = org::services::PlanHzbLayout(1920, 1080);
    CHECK(hzb.width == 1024 && hzb.height == 1024 && hzb.mips == 11);
    CHECK(org::services::HzbTextureDescription(hzb).imageDimensions.size() == 11);
    // A 2048 domain builds every level in one dispatch from the depth's valid texels.
    const auto one = org::services::PlanHzbDispatches(hzb, 1920, 1080, 1.0f);
    CHECK(one.size() == 1 && one[0].level == 0 && one[0].constants.fromDepth == 1 && one[0].constants.mips == 11);
    CHECK(one[0].constants.validSize[0] == 1920 && one[0].constants.validSize[1] == 1080 && one[0].constants.domainSize[0] == 2048);
    CHECK(one[0].groupsX == 32 && one[0].groupsY == 32 && one[0].constants.workGroups == 1024 && one[0].constants.farValue == 1.0f);
    // An 8192 domain splits: six levels from the depth, then the rest from level 5.
    const auto large = org::services::PlanHzbLayout(5000, 5000);
    CHECK(large.width == 4096 && large.mips == 13);
    const auto two = org::services::PlanHzbDispatches(large, 5000, 5000, 0.0f);
    CHECK(two.size() == 2 && two[0].constants.mips == 6 && two[1].level == 6 && two[1].constants.fromDepth == 0);
    CHECK(two[1].constants.domainSize[0] == 128 && two[1].constants.mips == 7 && two[1].groupsX == 2 && two[1].constants.farValue == 0.0f);
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
