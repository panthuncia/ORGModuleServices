#include <ORGModuleServices/PipelineRegistry.h>
#include <ORGModuleServices/CompileFlightRegistry.h>
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
