#include <ORGModuleServices/Programs/RasterProgram.h>

#include <ORGModuleServices/PipelineRegistry.h>
#include <ORGModuleServices/Programs/ProgramBuild.h>
#include <ORGModuleServices/Programs/ShaderTarget.h>
#include <ORGModuleServices/ShaderCompiler.h>

#include "Render/LatchBlock.h"
#include "Render/PreparedPass.h"
#include "Render/ResourceUseDeclaration.h"
#include "Resources/Buffers/Buffer.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <string_view>

namespace org::services {

namespace {

namespace build = program_build;

// Logs each reason a draw was skipped once.
class SkipLog {
public:
    void Report(std::string_view reason, std::string_view detail) {
        {
            const std::scoped_lock lock(mutex);
            if (const auto found = counts.find(reason); found != counts.end()) {
                ++found->second;
                return;
            }
            counts.emplace(std::string(reason), 1);
        }
        build::Log(ProgramLogLevel::Warning, detail.empty() ? std::format("raster program: a draw was not recorded: {}", reason)
                                                            : std::format("raster program: a draw was not recorded: {}, {}", reason, detail));
    }

private:
    std::mutex mutex;
    std::map<std::string, std::uint64_t, std::less<>> counts;
};

#if defined(ORG_MODULE_SERVICES_HAS_DXC)
// Kept for the registry's rebuilds.
struct PipelineState {
    rhi::RasterState raster{};
    rhi::DepthStencilState depth{};
    rhi::BlendState blend{};
    rhi::RenderTargets targets{};
    rhi::Format depthFormat = rhi::Format::Unknown;
    rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::TriangleList;
};

PipelineState StateOf(const RasterProgram::Desc& desc) {
    PipelineState state{};
    state.raster = desc.raster;
    state.depth = desc.depth;
    state.depthFormat = desc.depthFormat;
    state.topology = desc.topology;
    state.blend.alphaToCoverage = desc.alphaToCoverage;
    state.blend.numAttachments = static_cast<std::uint32_t>(desc.targets.size());
    state.targets.count = static_cast<std::uint32_t>(desc.targets.size());
    for (std::size_t i = 0; i < desc.targets.size(); ++i) {
        state.targets.formats[i] = desc.targets[i].format;
        state.blend.attachments[i] = desc.targets[i].blend;
        // Independent blending only when the targets differ, since one state for all works on every backend.
        const auto& a = state.blend.attachments[i];
        const auto& b = state.blend.attachments[0];
        if (a.enable != b.enable || a.srcColor != b.srcColor || a.dstColor != b.dstColor || a.colorOp != b.colorOp || a.srcAlpha != b.srcAlpha ||
            a.dstAlpha != b.dstAlpha || a.alphaOp != b.alphaOp || a.writeMask != b.writeMask)
            state.blend.independentBlend = true;
    }
    return state;
}

// label names the pipeline's debugging dumps.
rhi::PipelinePtr CreateRasterPipeline(rhi::Device device, rhi::PipelineLayoutHandle layoutHandle, const PipelineState& state,
    const std::vector<std::byte>& vertexBinary, const std::string& vertexEntry, const std::vector<std::byte>& pixelBinary, const std::string& pixelEntry,
    const std::string& label) {
    const rhi::SubobjLayout layout{ layoutHandle };
    // DXC keeps the HLSL entry points' names in the SPIR-V.
    const rhi::SubobjShader vertex{ rhi::ShaderStage::Vertex, { vertexBinary.data(), static_cast<std::uint32_t>(vertexBinary.size()) }, vertexEntry };
    const rhi::SubobjShader pixel{ rhi::ShaderStage::Pixel, { pixelBinary.data(), static_cast<std::uint32_t>(pixelBinary.size()) }, pixelEntry };
    const rhi::SubobjRaster raster{ state.raster };
    const rhi::SubobjDepth depth{ state.depth };
    const rhi::SubobjBlend blend{ state.blend };
    const rhi::SubobjRTVs targets{ state.targets };
    const rhi::SubobjDSV depthFormat{ state.depthFormat };
    const rhi::SubobjPrimitiveTopology topology{ state.topology };
    const rhi::SubobjInputLayout input{};  // no vertex buffers, positions come from SV_VertexID
    const rhi::PipelineStreamItem items[] = {
        rhi::Make(layout),
        rhi::Make(vertex),
        rhi::Make(pixel),
        rhi::Make(raster),
        rhi::Make(depth),
        rhi::Make(blend),
        rhi::Make(targets),
        rhi::Make(depthFormat),
        rhi::Make(topology),
        rhi::Make(input),
    };
    const auto& labelPipelines = CurrentProgramEnvironment().labelPipelines;
    if (labelPipelines)
        labelPipelines(label.c_str());
    rhi::PipelinePtr pipeline;
    const auto result = device.CreatePipeline(items, static_cast<std::uint32_t>(std::size(items)), pipeline);
    if (labelPipelines)
        labelPipelines(nullptr);
    if (result != rhi::Result::Ok)
        return {};
    return pipeline;
}

struct Stages {
    std::string vertexSource, pixelSource;
    std::wstring vertexEntry, pixelEntry;
    ProgramDefines vertexDefines, pixelDefines;
    std::string label;
    std::string vertexText, pixelText;
    std::wstring languageVersion = L"2021";
};

// A stage's generated text when it has one, else its file.
std::vector<std::byte> StageBytes(const std::filesystem::path& path, const std::string& text) {
    if (text.empty())
        return build::ReadSource(path);
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return { data, data + text.size() };
}

ShaderCompileRequest StageRequest(const std::filesystem::path& path, std::span<const std::byte> source, const std::wstring& entry, const wchar_t* target,
    const Stages& stages) {
    const bool vertex = std::wstring_view(target).starts_with(L"vs_");
    auto request = build::MakeRequest(path, source, entry, target, vertex ? stages.vertexDefines : stages.pixelDefines);
    request.languageVersion = stages.languageVersion;
    return request;
}

// Recompiles both stages off the owner thread and makes a new pipeline under the program's own layout. The program is held
// weakly, so one released meanwhile is skipped.
LiveRebuild RebuildRaster(std::weak_ptr<const RasterProgram> weakProgram, rhi::Device device, PipelineState state, Stages stages) {
    return [weakProgram = std::move(weakProgram), device, state, stages = std::move(stages)](const LiveRebuildRequest& request, LiveRebuildDone done) {
        auto* compiler = build::Compiler();
        const auto vertexPath = build::SourcePath(stages.vertexSource);
        const auto pixelPath = build::SourcePath(stages.pixelSource);
        auto vertexSource = std::make_shared<std::vector<std::byte>>(StageBytes(vertexPath, stages.vertexText));
        auto pixelSource = std::make_shared<std::vector<std::byte>>(StageBytes(pixelPath, stages.pixelText));
        if (!compiler || vertexSource->empty() || pixelSource->empty())
            return done({ .error = compiler ? std::format("missing shader source for {}", stages.label) : std::string("no runtime shader compiler") });
        // A memory-cache hit completes inline on this thread.
        (void)compiler->CompileAsync(StageRequest(vertexPath, *vertexSource, stages.vertexEntry, L"vs_6_6", stages),
            [compiler, weakProgram, device, state, stages, request, done, pixelPath, pixelSource](const ShaderArtifact& vertex) {
                if (!vertex)
                    return done({ .error = std::format("{} build of {}'s vertex stage failed:\n{}", ShaderFormatName(build::Format()), stages.label,
                                      vertex.diagnostics.substr(0, 2000)) });
                (void)compiler->CompileAsync(StageRequest(pixelPath, *pixelSource, stages.pixelEntry, L"ps_6_6", stages),
                    [weakProgram, device, state, stages, request, done, vertexBinary = vertex.binary](const ShaderArtifact& pixel) {
                        if (!pixel)
                            return done({ .error = std::format("{} build of {}'s pixel stage failed:\n{}", ShaderFormatName(build::Format()), stages.label,
                                              pixel.diagnostics.substr(0, 2000)) });
                        const std::uint64_t fingerprint = build::HashBinaries(vertexBinary, pixel.binary);
                        if (fingerprint == request.fingerprint && !request.force)
                            return done({ .fingerprint = fingerprint });
                        const auto program = weakProgram.lock();
                        if (!program)
                            return done({ .fingerprint = request.fingerprint });  // released, nothing binds it
                        auto pipeline = CreateRasterPipeline(device, program->layout->GetHandle(), state, vertexBinary, build::Narrow(stages.vertexEntry),
                            pixel.binary, build::Narrow(stages.pixelEntry), stages.label);
                        if (!pipeline)
                            return done({ .error = std::format("the raster pipeline ({}) could not be created", stages.label) });
                        done({ .payload = std::make_shared<rhi::PipelinePtr>(std::move(pipeline)), .fingerprint = fingerprint });
                    });
            });
    };
}

// Compiles one stage now, blocking. Empty and logged on failure.
std::vector<std::byte> CompileStage(ShaderCompiler& compiler, const std::string& source, const std::string& text, const std::wstring& entry,
    const wchar_t* target, const Stages& stages, ProgramLogLevel failureLevel = ProgramLogLevel::Error) {
    const auto path = build::SourcePath(source);
    const auto bytes = StageBytes(path, text);
    if (bytes.empty()) {
        build::Log(ProgramLogLevel::Error, std::format("Missing shader source {}", path.string()));
        return {};
    }
    const auto artifact = compiler.Compile(StageRequest(path, bytes, entry, target, stages));
    if (!artifact) {
        build::Log(failureLevel,
            std::format("{} build of {} ({}) failed:\n{}", ShaderFormatName(build::Format()), source, build::Narrow(entry), artifact.diagnostics.substr(0, 2000)));
        return {};
    }
    return artifact.binary;
}

// Resolves the desc's defines now, unless they were resolved where it was queued.
Stages StagesOf(const RasterProgram::Desc& desc) {
    return { desc.vertexSource, desc.pixelSource, desc.vertexEntry, desc.pixelEntry,
        !desc.vertexStageDefines.empty() ? desc.vertexStageDefines :
        desc.commonDefines               ? build::WithCommonDefines(desc.defines, L"vs_6_6") :
                                           desc.defines,
        !desc.pixelStageDefines.empty() ? desc.pixelStageDefines :
        desc.commonDefines              ? build::WithCommonDefines(desc.defines, L"ps_6_6") :
                                          desc.defines,
        std::format("{} ({})", std::filesystem::path(desc.pixelSource).stem().string(), build::Narrow(desc.pixelEntry)), desc.vertexText, desc.pixelText,
        desc.languageVersion };
}
#endif

const rhi::PipelinePtr& RasterPipelineOf(const PipelineVersion& version) {
    return *static_cast<const rhi::PipelinePtr*>(version.payload.get());
}

} // namespace

bool RasterProgram::CompileStages(const Desc& desc) {
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
    auto* compiler = build::Compiler();
    if (!compiler)
        return false;
    const Stages stages = StagesOf(desc);
    // A precompile failure is only a warning. A Load that needs it logs its own error.
    return !CompileStage(*compiler, stages.vertexSource, stages.vertexText, stages.vertexEntry, L"vs_6_6", stages, ProgramLogLevel::Warning).empty() &&
           !CompileStage(*compiler, stages.pixelSource, stages.pixelText, stages.pixelEntry, L"ps_6_6", stages, ProgramLogLevel::Warning).empty();
#else
    (void)desc;
    return false;
#endif
}

rhi::PipelineHandle RasterProgram::Pipeline(const org::RecordingContext& recording) const {
    // Non-owning. The recording's pin keeps the version.
    const auto& versionFor = CurrentProgramEnvironment().versionFor;
    return RasterPipelineOf(*(versionFor ? versionFor(*slot, recording) : slot->Current()))->GetHandle();
}

std::shared_ptr<const RasterProgram> RasterProgram::Load(rhi::Device device, const Desc& desc) {
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
    // A depth-only program is allowed, for depth clears drawn as a program.
    if ((desc.targets.empty() && desc.depthFormat == rhi::Format::Unknown) || desc.targets.size() > 8) {
        build::Log(ProgramLogLevel::Error, std::format("{}: a raster program needs 1 to 8 colour targets, or none with a depth format, not {}",
                                               desc.pixelSource, desc.targets.size()));
        return {};
    }
    auto* compiler = build::Compiler();
    if (!compiler) {
        build::Log(ProgramLogLevel::Warning, std::format("{}: no runtime shader compiler", desc.pixelSource));
        return {};
    }
    const auto& registryOf = CurrentProgramEnvironment().registry;
    auto* registry = registryOf ? registryOf() : nullptr;
    if (!registry) {
        build::Log(ProgramLogLevel::Error, std::format("{}: no pipeline registry", desc.pixelSource));
        return {};
    }
    // Read before the sources, so a reload that starts after rebuilds it.
    const std::uint64_t builtFor = registry->SourcesGeneration();
    // Resolved once here, so a hot reload compiles the same permutation.
    const Stages stages = StagesOf(desc);
    const auto vertex = CompileStage(*compiler, stages.vertexSource, stages.vertexText, stages.vertexEntry, L"vs_6_6", stages);
    const auto pixel = vertex.empty() ? std::vector<std::byte>{} : CompileStage(*compiler, stages.pixelSource, stages.pixelText, stages.pixelEntry, L"ps_6_6", stages);
    if (vertex.empty() || pixel.empty())
        return {};

    auto program = std::make_shared<RasterProgram>();
    program->constantWords = desc.constantWords;
    program->targetCount = static_cast<std::uint32_t>(desc.targets.size());
    program->hasDepth = desc.depthFormat != rhi::Format::Unknown;
    rhi::PushConstantRangeDesc constants{};
    constants.visibility = rhi::ShaderStage::AllGraphics;
    constants.num32BitValues = desc.constantWords;
    if (device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, desc.constantWords ? 1u : 0u }, .flags = rhi::PipelineLayoutFlags::PF_None },
            program->layout) != rhi::Result::Ok) {
        build::Log(ProgramLogLevel::Error, std::format("{}: the pipeline layout could not be created", stages.label));
        return {};
    }
    const auto state = StateOf(desc);
    auto pipeline = CreateRasterPipeline(device, program->layout->GetHandle(), state, vertex, build::Narrow(stages.vertexEntry), pixel,
        build::Narrow(stages.pixelEntry), stages.label);
    if (!pipeline) {
        build::Log(ProgramLogLevel::Error, std::format("{}: the raster pipeline could not be created", stages.label));
        return {};
    }
    rhi::IndirectArg draw[] = { { .kind = rhi::IndirectArgKind::Draw } };
    if (device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(draw, 1), sizeof(std::uint32_t) * 4 },
            program->layout->GetHandle(), program->drawSignature) != rhi::Result::Ok) {
        build::Log(ProgramLogLevel::Error, std::format("{}: the draw signature could not be created", stages.label));
        return {};
    }
    LivePipelineDesc live;
    live.id = std::format("{} + {}", desc.vertexSource, stages.label);
    // Generated stages have no file a reload could name.
    if (desc.vertexText.empty())
        live.sources.push_back(desc.vertexSource);
    if (desc.pixelText.empty())
        live.sources.push_back(desc.pixelSource);
    live.rebuild = RebuildRaster(program, device, state, stages);
    program->slot = registry->Register(std::move(live), std::make_shared<rhi::PipelinePtr>(std::move(pipeline)), build::HashBinaries(vertex, pixel), builtFor);
    return program;
#else
    (void)device;
    build::Log(ProgramLogLevel::Warning, std::format("{}: built without the module's shader compiler", desc.pixelSource));
    return {};
#endif
}

bool RasterProgram::Draw(org::RecordingContext& recording, const DrawDesc& draw) const {
    // A skipped draw leaves its targets as they were, and its reason is logged once.
    static SkipLog skipped;
    const auto skip = [&](std::string_view reason) {
        skipped.Report(reason, draw.debugName ? draw.debugName : "");
        return false;
    };
    if (draw.colours.size() != targetCount)
        return skip(std::format("{} colour targets given, the program has {}", draw.colours.size(), targetCount));
    if ((draw.depth != nullptr) != hasDepth)
        return skip(hasDepth ? "no depth target given, the program has one" : "a depth target given, the program has none");
    if (draw.constants.size() > constantWords)
        return skip(std::format("{} constant words given, the program takes {}", draw.constants.size(), constantWords));
    if (!draw.viewport.width || !draw.viewport.height)
        return skip("an empty viewport");
    if (!slot)
        return skip("no pipeline slot");
    if (draw.indirectBuffer.valid() && !drawSignature)
        return skip("an indirect buffer without the program's draw signature");
    std::array<rhi::ColorAttachment, 8> colours{};
    for (std::uint32_t i = 0; i < targetCount; ++i) {
        const auto& colour = draw.colours[i];
        if (!colour.view)
            return skip("a colour target without a view");
        auto& attachment = colours[i];
        attachment.rtv = recording.View(*colour.view);
        attachment.resource = recording.Resolve(*colour.view).GetHandle();
        attachment.loadOp = colour.load;
        attachment.storeOp = rhi::StoreOp::Store;
        attachment.clear.type = rhi::ClearValueType::Color;
        std::copy(colour.clear.begin(), colour.clear.end(), attachment.clear.rgba);
    }
    rhi::DepthAttachment depth{};
    if (draw.depth) {
        if (!draw.depth->view)
            return skip("a depth target without a view");
        depth.dsv = recording.View(*draw.depth->view);
        depth.resource = recording.Resolve(*draw.depth->view).GetHandle();
        depth.readOnly = draw.depth->readOnly;
        depth.depthLoad = draw.depth->load;
        depth.depthStore = draw.depth->readOnly ? rhi::StoreOp::DontCare : rhi::StoreOp::Store;
        // The stencil is loaded and stored so a program that writes it keeps what it wrote. clearStencil clears it to 0.
        depth.stencilLoad = draw.depth->load == rhi::LoadOp::Clear && draw.depth->clearStencil ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
        depth.stencilStore = rhi::StoreOp::Store;
        depth.clear.type = rhi::ClearValueType::DepthStencil;
        depth.clear.depthStencil.depth = draw.depth->clear;
        depth.clear.depthStencil.stencil = 0;
    }
    rhi::PassBeginInfo begin{};
    begin.colors = { colours.data(), targetCount };
    begin.depth = draw.depth ? &depth : nullptr;
    begin.x = draw.viewport.x;
    begin.y = draw.viewport.y;
    begin.width = draw.viewport.width;
    begin.height = draw.viewport.height;
    begin.minDepth = draw.viewport.minDepth;
    begin.maxDepth = draw.viewport.maxDepth;
    begin.debugName = draw.debugName;
    auto& commands = recording.Commands();
    commands.BeginPass(begin);
    commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
    commands.BindLayout(layout->GetHandle());
    commands.BindPipeline(Pipeline(recording));
    // Set every draw, since the reference holds across the list's passes.
    commands.SetStencilRef(draw.stencilReference);
    // BeginPass set viewport 0 and the scissor to the render area. A draw's own replace them inside this pass.
    if (draw.dynamicViewport)
        commands.SetViewports({ &*draw.dynamicViewport, 1 });
    if (draw.scissor) {
        // Clipped to the render area, which would clip it anyway.
        const auto& area = draw.viewport;
        const auto& rect = *draw.scissor;
        const std::int64_t left = (std::max)(static_cast<std::int64_t>(rect.x), static_cast<std::int64_t>(area.x));
        const std::int64_t top = (std::max)(static_cast<std::int64_t>(rect.y), static_cast<std::int64_t>(area.y));
        const std::int64_t right = (std::min)(static_cast<std::int64_t>(rect.x) + rect.width, static_cast<std::int64_t>(area.x) + area.width);
        const std::int64_t bottom = (std::min)(static_cast<std::int64_t>(rect.y) + rect.height, static_cast<std::int64_t>(area.y) + area.height);
        const rhi::ScissorRect clipped{ .x = static_cast<std::int32_t>(left),
            .y = static_cast<std::int32_t>(top),
            .width = static_cast<std::uint32_t>((std::max)(right - left, std::int64_t(0))),
            .height = static_cast<std::uint32_t>((std::max)(bottom - top, std::int64_t(0))) };
        commands.SetScissors({ &clipped, 1 });
    }
    if (!draw.constants.empty())
        commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, static_cast<std::uint32_t>(draw.constants.size()), draw.constants.data());
    // Vulkan ends a query begun inside a pass in that same pass, so the count is this draw's alone.
    const bool counted = draw.occlusionPool.valid();
    if (counted)
        commands.BeginQuery(draw.occlusionPool, draw.occlusionIndex);
    if (draw.indirect && drawSignature) {
        const auto region = draw.indirect->At(recording.Run());
        commands.ExecuteIndirect(drawSignature->GetHandle(), region.buffer->GetAPIResource().GetHandle(), region.offset + draw.indirectOffset, {}, 0, 1);
    } else if (draw.indirectBuffer.valid() && drawSignature)
        commands.ExecuteIndirect(drawSignature->GetHandle(), draw.indirectBuffer, draw.indirectBufferOffset, {}, 0, 1);
    else
        commands.Draw(draw.vertexCount, draw.instanceCount, 0, 0);
    if (counted)
        commands.EndQuery(draw.occlusionPool, draw.occlusionIndex);
    commands.EndPass();
    return true;
}

} // namespace org::services
