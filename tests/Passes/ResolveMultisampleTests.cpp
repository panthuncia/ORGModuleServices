// Multisampled textures end to end, on WARP (D3D12) by default.
//
// A 4x texture is made through ORG, so its views come from the descriptor heap manager: a Texture2DMS SRV, a Texture2DMS
// RTV, no UAV. A render pass clears it through its RTV. ResolveMultisamplePass reads it through its default SRV and writes a
// single-sampled texture, which a copy pass reads back. Every texel must be the clear colour. A Texture2D SRV of the
// multisampled texture (what ORG made before) is invalid on D3D12, and the shader would not read the clear.
// Also: a multisampled texture with a UAV or more than one mip is refused.
//
// --vulkan runs on the default Vulkan device with validation, --hardware on the first hardware D3D12 adapter.
// Exit code 77: skipped (no shader compiler, or the device lacks shader model 6.6).

#include <ORGModuleServices/Passes/ResolveMultisamplePass.h>
#include <ORGModuleServices/PipelineRegistry.h>
#include <ORGModuleServices/Programs/ComputeProgram.h>
#include <ORGModuleServices/Programs/ProgramBuild.h>
#include <ORGModuleServices/ShaderCompiler.h>

#include "OpenRenderGraph/GraphHost.h"
#include "OpenRenderGraph/Runtime.h"
#include "Render/PassBuilders.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/GloballyIndexedResource.h"
#include "Resources/PixelBuffer.h"

#include <rhi.h>
#include <rhi_helpers.h>
#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ORG's test assertions and task service (OpenRenderGraph/tests).
#include "TestSupport.h"
#include "TestTaskService.h"

#ifndef ORG_SERVICES_TEST_SHADER_DIR
#error "ORG_SERVICES_TEST_SHADER_DIR must name ORGModuleServices/shaders"
#endif

namespace {

constexpr int kSkipped = 77;
constexpr uint32_t kSize = 16;
constexpr uint32_t kSamples = 4;
constexpr rhi::Format kFormat = rhi::Format::R8G8B8A8_UNorm;
// Exact in 8-bit UNORM: 51, 102, 153, 255.
constexpr float kClear[4] = {0.2f, 0.4f, 0.6f, 1.0f};

[[maybe_unused]] std::filesystem::path CompilerDirectory() {
    if (const char* directory = std::getenv("ORG_SERVICES_TEST_DXC_DIR")) return directory;
    return {};
}

org::TextureDescription Describe(uint32_t samples, bool target, bool unordered, uint32_t mips = 1) {
    org::TextureDescription description{};
    description.format = kFormat;
    description.channels = 4;
    description.sampleCount = static_cast<uint8_t>(samples);
    description.hasSRV = true;
    description.srvFormat = kFormat;
    description.hasRTV = target;
    description.rtvFormat = kFormat;
    description.hasUAV = unordered;
    description.uavFormat = kFormat;
    for (uint32_t i = 0; i < 4; ++i) description.clearColor[i] = kClear[i];
    for (uint32_t mip = 0; mip < mips; ++mip) description.imageDimensions.push_back({(std::max)(1u, kSize >> mip), (std::max)(1u, kSize >> mip), 0, 0});
    return description;
}

// Clears a render target through its RTV, in an empty render pass.
struct ClearBindings { org::DeclaredViewToken target; };
struct ClearFrame { uint32_t width = 0, height = 0; };
class ClearPass final : public org::TypedRenderGraphPass<ClearPass, ClearFrame, ClearBindings> {
public:
    explicit ClearPass(std::shared_ptr<org::Resource> target) : m_target(std::move(target)) {}
    ClearBindings Declare(org::PassBuilder& builder) {
        return {builder.RenderTargetClear(m_target).View()};
    }
    ClearFrame Prepare(const ClearBindings& bindings, const org::PassPrepareContext& preparation) const {
        const auto& texture = preparation.Describe(bindings.target).texture;
        return {texture.width, texture.height};
    }
    static void Record(const ClearBindings& bindings, const ClearFrame& frame, org::PassRecordContext& recording) {
        rhi::ColorAttachment color{};
        color.rtv = recording.View(bindings.target);
        color.loadOp = rhi::LoadOp::Clear;
        color.storeOp = rhi::StoreOp::Store;
        color.clear.type = rhi::ClearValueType::Color;
        color.clear.format = kFormat;
        for (uint32_t i = 0; i < 4; ++i) color.clear.rgba[i] = kClear[i];
        rhi::PassBeginInfo begin{};
        begin.colors = {&color, 1u};
        begin.width = frame.width;
        begin.height = frame.height;
        begin.debugName = "ResolveMultisampleTests clear";
        recording.Commands().BeginPass(begin);
        recording.Commands().EndPass();
    }
private:
    std::shared_ptr<org::Resource> m_target;
};

// Copies mip 0 of a texture into a readback buffer at the footprint the test measured.
struct ReadbackBindings { org::ResourceBindingToken texture, buffer; };
struct ReadbackFrame { rhi::CopyableFootprint footprint{}; };
class ReadbackPass final : public org::TypedRenderGraphPass<ReadbackPass, ReadbackFrame, ReadbackBindings> {
public:
    ReadbackPass(std::shared_ptr<org::Resource> texture, std::shared_ptr<org::Resource> buffer, rhi::CopyableFootprint footprint)
        : m_texture(std::move(texture)), m_buffer(std::move(buffer)), m_footprint(footprint) {}
    ReadbackBindings Declare(org::PassBuilder& builder) {
        return {builder.CopySource(m_texture), builder.CopyDestination(m_buffer)};
    }
    ReadbackFrame Prepare(const ReadbackBindings&, const org::PassPrepareContext&) const { return {m_footprint}; }
    static void Record(const ReadbackBindings& bindings, const ReadbackFrame& frame, org::PassRecordContext& recording) {
        rhi::BufferTextureCopyFootprint copy{};
        copy.texture = recording.Resolve(bindings.texture).GetHandle();
        copy.buffer = recording.Resolve(bindings.buffer).GetHandle();
        copy.mip = 0;
        copy.arraySlice = 0;
        copy.footprint = frame.footprint;
        recording.Commands().CopyTextureToBuffer(copy);
    }
private:
    std::shared_ptr<org::Resource> m_texture, m_buffer;
    rhi::CopyableFootprint m_footprint{};
};

class PassesExtension final : public org::RenderGraph::IRenderGraphExtension {
public:
    explicit PassesExtension(std::vector<std::pair<const char*, std::shared_ptr<org::RenderPass>>> passes) : m_passes(std::move(passes)) {}
    void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& out) override {
        for (const auto& [name, pass] : m_passes)
            out.push_back(org::RenderGraph::ExternalPassDesc::Render(name, pass).PreferQueue(org::QueueKind::Graphics));
    }
private:
    std::vector<std::pair<const char*, std::shared_ptr<org::RenderPass>>> m_passes;
};

// The views ORG made for a multisampled texture: an MS SRV as the default, one mip, an RTV, no UAV.
int TestViews(const org::PixelBuffer& texture) {
    CHECK(texture.HasSRV() && texture.HasRTV());
    CHECK(!texture.HasUAVShaderVisible() && !texture.HasUAVNonShaderVisible());
    CHECK(texture.GetNumSRVMipLevels() == 1 && texture.GetNumSRVSlices() == 1);
    CHECK(texture.GetNumSRVSlices(org::SRVViewType::Texture2DMS) == 1);
    CHECK(texture.GetSRVInfo(0).slot.index == texture.GetSRVInfo(org::SRVViewType::Texture2DMS, 0).slot.index);
    CHECK(texture.GetNumSRVSlices(org::SRVViewType::Texture2D) == 0);
    rhi::ResourceDesc description{};
    CHECK(texture.TryGetRHIResourceDesc(description) && description.texture.sampleCount == kSamples);
    return 0;
}

// A multisampled texture with a UAV, or with more than one mip, is refused when it is made.
int TestRefusals() {
    const auto refused = [](const org::TextureDescription& description) {
        try {
            (void)org::PixelBuffer::CreateShared(description);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CHECK(refused(Describe(kSamples, true, true)));
    CHECK(refused(Describe(kSamples, true, false, 2)));
    return 0;
}

[[maybe_unused]] int TestResolve(rhi::Device device, const std::shared_ptr<org::Runtime>& runtime, std::shared_ptr<const org::services::ComputeProgram> kernel) {
    auto multisampled = org::PixelBuffer::CreateShared(Describe(kSamples, true, false));
    multisampled->SetName("ResolveMultisampleTests source");
    if (const int line = TestViews(*multisampled)) return line;
    auto resolved = org::PixelBuffer::CreateShared(Describe(1, false, true));
    resolved->SetName("ResolveMultisampleTests destination");

    rhi::CopyableFootprint footprint{};
    rhi::FootprintRangeDesc range{};
    range.texture = resolved->GetAPIResource().GetHandle();
    const uint64_t bytes = device.GetCopyableFootprints(range, &footprint, 1).totalBytes;
    CHECK(bytes && footprint.width == kSize && footprint.height == kSize);
    auto readback = org::Buffer::CreateShared(rhi::HeapType::Readback, bytes);
    readback->SetName("ResolveMultisampleTests readback");

    auto resolve = org::services::MakeResolveMultisamplePass(kernel, multisampled, resolved);
    CHECK(resolve);
    {
        org::GraphHost host({.runtime = runtime});
        host.AddExtension("test.resolve", [&] {
            return std::make_unique<PassesExtension>(std::vector<std::pair<const char*, std::shared_ptr<org::RenderPass>>>{
                {"resolve.clear", std::make_shared<ClearPass>(multisampled)},
                {"resolve.resolve", resolve},
                {"resolve.readback", std::make_shared<ReadbackPass>(resolved, readback, footprint)}});
        });
        host.ExecuteFrame();
        CHECK(device.WaitIdle() == rhi::Result::Ok);
    }

    void* mapped = nullptr;
    readback->GetAPIResource().Map(&mapped, 0, bytes);
    CHECK(mapped);
    uint32_t wrong = 0;
    uint8_t first[4]{};
    for (uint32_t y = 0; y < kSize; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped) + footprint.offset + uint64_t{y} * footprint.rowPitch;
        for (uint32_t x = 0; x < kSize; ++x) {
            const uint8_t* texel = row + x * 4u;
            if (x == 0 && y == 0) std::memcpy(first, texel, 4);
            for (uint32_t c = 0; c < 4; ++c) {
                const int expected = static_cast<int>(std::lround(kClear[c] * 255.0f));
                if (std::abs(static_cast<int>(texel[c]) - expected) > 1) ++wrong;
            }
        }
    }
    readback->GetAPIResource().Unmap(0, 0);
    if (wrong) std::fprintf(stderr, "texel (0, 0) reads %u %u %u %u\n", first[0], first[1], first[2], first[3]);
    CHECK(wrong == 0);
    return 0;
}

int Run(int argc, char** argv) {
    bool vulkan = false, hardware = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vulkan") == 0) vulkan = true;
        else if (std::strcmp(argv[i], "--hardware") == 0) hardware = true;
        else if (std::strcmp(argv[i], "--warp") != 0) return 2;
    }
    rhi::DeviceCreateInfo create{};
    create.framesInFlight = 2;
    rhi::DevicePtr device;
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (vulkan) {
        create.backend = rhi::Backend::Vulkan;
        create.enableDebug = true;
        CHECK(!rhi::Failed(rhi::CreateVulkanDevice(create, device)) && device);
    } else {
        CHECK(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
        if (hardware) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
            for (UINT index = 0; SUCCEEDED(factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)));
                 ++index) {
                DXGI_ADAPTER_DESC1 description{};
                CHECK(SUCCEEDED(candidate->GetDesc1(&description)));
                if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    adapter = candidate;
                    break;
                }
                candidate.Reset();
            }
            if (!adapter) {
                std::puts("SKIP: no hardware adapter");
                return kSkipped;
            }
        } else {
            CHECK(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))));
        }
        create.backend = rhi::Backend::D3D12;
        create.nativeAdapter = adapter.Get();
        CHECK(!rhi::Failed(rhi::CreateD3D12Device(create, device)) && device);
    }
    // The resolve shader indexes the descriptor heap directly.
    ShaderFeatureInfo shaders{};
    if (device->QueryFeatureInfo(&shaders.header) != rhi::Result::Ok || shaders.maxShaderModel < ShaderModel::SM_6_6) {
        std::puts("SKIP: the device lacks shader model 6.6");
        return kSkipped;
    }
    auto runtime = org::Runtime::Create({.device = device.Get(), .backend = create.backend, .tasks = std::make_shared<org::test::TestTaskService>(2)});
    CHECK(runtime);

    if (const int line = TestRefusals()) return line;

#if !defined(ORG_MODULE_SERVICES_HAS_DXC)
    std::puts("SKIP: built without the module's shader compiler");
    return kSkipped;
#else
    org::services::ShaderCompiler compiler({}, CompilerDirectory());
    if (!compiler.Available()) {
        std::puts("SKIP: no shader compiler");
        return kSkipped;
    }
    org::services::PipelineRegistry registry;
    {
        org::services::ProgramEnvironment environment;
        environment.shaderRoot = ORG_SERVICES_TEST_SHADER_DIR;
        const auto backend = create.backend;
        environment.backend = [backend] { return backend; };
        environment.compiler = [&compiler] { return &compiler; };
        environment.registry = [&registry] { return &registry; };
        environment.log = [](org::services::ProgramLogLevel, std::string_view message) {
            std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
        };
        org::services::SetProgramEnvironment(std::move(environment));
    }
    int result = 0;
    {
        // The module's shader, from its own folder: it has no other build, so it takes no host defines.
        const org::services::ComputeProgram::Desc desc{
            .source = "ResolveMultisample.hlsl", .constantWords = org::services::kResolveMultisampleConstantWords, .commonDefines = false};
        auto kernel = org::services::ComputeProgram::Load(device.Get(), desc);
        if (!kernel) {
            ORG_TEST_REPORT("the resolve shader did not build");
            result = 1;
        } else {
            result = TestResolve(device.Get(), runtime, kernel);
        }
    }
    org::services::SetProgramEnvironment({});
    runtime.reset();
    if (result == 0) std::puts(vulkan ? "PASS: ResolveMultisampleTests (Vulkan)" : "PASS: ResolveMultisampleTests (D3D12)");
    return result;
#endif
}

} // namespace

int main(int argc, char** argv) {
    try {
        return Run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
