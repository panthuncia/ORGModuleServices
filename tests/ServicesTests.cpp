#include <ORGModuleServices/PipelineService.h>
#include <ORGModuleServices/CompileFlightRegistry.h>
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
#include <ORGModuleServices/ShaderCompiler.h>
#include <ORGModuleServices/ShaderServiceAdapter.h>
#endif

#include <atomic>
#include <cstring>

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (false)

int main() {
	org::services::CompileFlightRegistry<int> flights;
	CHECK(flights.TryBecomeOwnerOrWait(1)); CHECK(flights.ActiveCount() == 1); flights.Complete(1);
    org::services::PipelineService pipelines;
    std::atomic_uint32_t builds{};
    org::services::PipelineRecipe recipe{ "tests.compute", 1, 2, 3, 4, [&] {
        ++builds; return std::static_pointer_cast<void>(std::make_shared<uint32_t>(42));
    } };
    const auto key = pipelines.BuildKey(recipe);
    auto first = pipelines.Request(recipe);
    auto duplicate = pipelines.Request(recipe);
    CHECK(first.get()); CHECK(duplicate.get()); CHECK(builds == 1);
    pipelines.PublishReady(5); CHECK(pipelines.Find(key)); pipelines.Retire(5);
	recipe.shaderKey = 99;
	const auto replacementKey = pipelines.BuildKey(recipe);
	CHECK(pipelines.Request(recipe).get());
	pipelines.PublishReady(9);
	CHECK(!pipelines.Find(key)); CHECK(pipelines.Find(replacementKey));
	pipelines.Retire(8); pipelines.Retire(9);
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
	org::services::ShaderServiceAdapter service(compiler);
	ORGShaderCompilerServiceAPI table{ sizeof(table), ORG_SHADER_COMPILER_SERVICE_VERSION_1 };
	CHECK(service.Query(2, &table, sizeof(table)) == ORG_RG_E_UNSUPPORTED_VERSION);
	const auto queryStatus = service.Query(ORG_SHADER_COMPILER_SERVICE_VERSION_1, &table, sizeof(table));
	CHECK(queryStatus == (compiler.Available() ? ORG_RG_OK : ORG_RG_E_UNSUPPORTED_CAPABILITY));
	service.Shutdown();
#endif
}
