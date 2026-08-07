#include <ORGModuleServices/PipelineService.h>
#include <ORGModuleServices/DescriptorViewCache.h>
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
#include <ORGModuleServices/ShaderCompiler.h>
#endif

#include <atomic>
#include <cassert>
#include <cstring>

int main() {
	org::services::DescriptorViewCache views;
	auto viewHandle = views.GetOrCreate({ 10, 20, 0 }, [] { return org::services::DescriptorView{ 1, 2, std::make_shared<int>(3) }; });
	assert(views.Resolve(viewHandle)); views.InvalidateResource(10); assert(!views.Resolve(viewHandle));
    org::services::PipelineService pipelines;
    std::atomic_uint32_t builds{};
    org::services::PipelineRecipe recipe{ "tests.compute", 1, 2, 3, 4, [&] {
        ++builds; return std::static_pointer_cast<void>(std::make_shared<uint32_t>(42));
    } };
    const auto key = pipelines.BuildKey(recipe);
    auto first = pipelines.Request(recipe);
    auto duplicate = pipelines.Request(recipe);
    assert(first.get()); assert(duplicate.get()); assert(builds == 1);
    pipelines.PublishReady(5); assert(pipelines.Find(key)); pipelines.Retire(5);
	recipe.shaderKey = 99;
	const auto replacementKey = pipelines.BuildKey(recipe);
	assert(pipelines.Request(recipe).get());
	pipelines.PublishReady(9);
	assert(!pipelines.Find(key)); assert(pipelines.Find(replacementKey));
	pipelines.Retire(8); pipelines.Retire(9);
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
    constexpr char shader[] = "[numthreads(1,1,1)] void main() {}";
    org::services::ShaderCompileRequest request{};
    request.sourceName = "inline-test.hlsl";
    request.source = { reinterpret_cast<const std::byte*>(shader), std::strlen(shader) };
    request.entryPoint = L"main"; request.target = L"cs_6_0";
    org::services::ShaderCompiler compiler;
    const auto keyA = compiler.BuildKey(request), keyB = compiler.BuildKey(request);
    assert(keyA != 0 && keyA == keyB);
    if (compiler.Available()) { auto artifact = compiler.Compile(request); assert(artifact); }
#endif
}
