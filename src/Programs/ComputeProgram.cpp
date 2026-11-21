#include <ORGModuleServices/Programs/ComputeProgram.h>

#include <ORGModuleServices/Passes/GenerateMipsPass.h>
#include <ORGModuleServices/PipelineRegistry.h>
#include <ORGModuleServices/Programs/ProgramBuild.h>
#include <ORGModuleServices/Programs/ShaderTarget.h>
#include <ORGModuleServices/ShaderCompiler.h>

#include "Render/LatchBlock.h"
#include "Render/PreparedPass.h"
#include "Resources/Buffers/Buffer.h"

#include <filesystem>
#include <format>
#include <span>

namespace org::services {

namespace {

namespace build = program_build;

const rhi::PipelinePtr& ComputePipelineOf(const PipelineVersion& version) {
    return *static_cast<const rhi::PipelinePtr*>(version.payload.get());
}

#if defined(ORG_MODULE_SERVICES_HAS_DXC)
ShaderCompileRequest MakeComputeRequest(const ComputeProgram::Desc& desc, const std::filesystem::path& path, std::span<const std::byte> source) {
    return build::MakeRequest(path, source, desc.entry, L"cs_6_6", desc.defines);
}

// source labels the pipeline's debugging dumps.
rhi::PipelinePtr CreateComputePipeline(rhi::Device device, rhi::PipelineLayoutHandle layoutHandle, const std::vector<std::byte>& binary,
    const std::string& entry, const std::filesystem::path& source) {
    rhi::PipelinePtr pipeline;
    const rhi::SubobjLayout layout{ layoutHandle };
    // DXC keeps the HLSL entry point's name in the SPIR-V.
    const rhi::SubobjShader shader{ rhi::ShaderStage::Compute, { binary.data(), static_cast<std::uint32_t>(binary.size()) }, entry.c_str() };
    const rhi::PipelineStreamItem items[] = { rhi::Make(layout), rhi::Make(shader) };
    const std::string label = source.stem().string();
    const auto& labelPipelines = CurrentProgramEnvironment().labelPipelines;
    if (labelPipelines)
        labelPipelines(label.c_str());
    const auto result = device.CreatePipeline(items, 2, pipeline);
    if (labelPipelines)
        labelPipelines(nullptr);
    if (result != rhi::Result::Ok)
        return {};
    return pipeline;
}

// Recompiles the source off the owner thread and makes a new pipeline under the program's own layout. The program is held
// weakly, so one released meanwhile is skipped.
LiveRebuild RebuildCompute(std::weak_ptr<const ComputeProgram> weakProgram, rhi::Device device, ComputeProgram::Desc desc) {
    return [weakProgram = std::move(weakProgram), device, desc = std::move(desc)](const LiveRebuildRequest& request, LiveRebuildDone done) {
        auto* compiler = build::Compiler();
        const std::filesystem::path path = build::SourcePath(desc.source);
        const auto source = build::ReadSource(path);
        if (!compiler || source.empty())
            return done({ .error = compiler ? std::format("missing shader source {}", path.string()) : std::string("no runtime shader compiler") });
        // A memory-cache hit completes here on this thread.
        (void)compiler->CompileAsync(MakeComputeRequest(desc, path, source),
            [weakProgram, device, request, done = std::move(done), entry = build::Narrow(desc.entry), sourcePath = std::filesystem::path(desc.source)](
                const ShaderArtifact& artifact) {
                if (!artifact)
                    return done({ .error = std::format("{} build ({}) failed:\n{}", ShaderFormatName(build::Format()), entry, artifact.diagnostics.substr(0, 2000)) });
                const std::uint64_t fingerprint = build::HashBinary(artifact.binary);
                if (fingerprint == request.fingerprint && !request.force)
                    return done({ .fingerprint = fingerprint });
                const auto program = weakProgram.lock();
                if (!program)
                    return done({ .fingerprint = request.fingerprint });  // released, nothing binds it
                auto pipeline = CreateComputePipeline(device, program->layout->GetHandle(), artifact.binary, entry, sourcePath);
                if (!pipeline)
                    return done({ .error = std::format("the compute pipeline ({}) could not be created", entry) });
                done({ .payload = std::make_shared<rhi::PipelinePtr>(std::move(pipeline)), .fingerprint = fingerprint });
            });
    };
}
#endif

} // namespace

rhi::PipelineHandle ComputeProgram::Pipeline(const org::RecordingContext& recording) const {
    // Non-owning. The recording's pin keeps the version.
    const auto& versionFor = CurrentProgramEnvironment().versionFor;
    return ComputePipelineOf(*(versionFor ? versionFor(*slot, recording) : slot->Current()))->GetHandle();
}

rhi::PipelineHandle ComputeProgram::CurrentPipeline() const {
    return ComputePipelineOf(*slot->Current())->GetHandle();
}

void ComputeProgram::Dispatch(org::RecordingContext& recording, std::span<const std::uint32_t> constants, std::uint32_t x, std::uint32_t y,
    std::uint32_t z) const {
    if (constants.size() > constantWords || !x || !y || !z || !slot)
        return;
    auto& commands = recording.Commands();
    commands.BindLayout(layout->GetHandle());
    commands.BindPipeline(Pipeline(recording));
    if (!constants.empty())
        commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, static_cast<std::uint32_t>(constants.size()), constants.data());
    commands.Dispatch(x, y, z);
}

void ComputeProgram::DispatchIndirect(org::RecordingContext& recording, std::span<const std::uint32_t> constants, rhi::ResourceHandle arguments,
    std::uint64_t offset) const {
    if (constants.size() > constantWords || !slot || !dispatchSignature)
        return;
    auto& commands = recording.Commands();
    commands.BindLayout(layout->GetHandle());
    commands.BindPipeline(Pipeline(recording));
    if (!constants.empty())
        commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, static_cast<std::uint32_t>(constants.size()), constants.data());
    commands.ExecuteIndirect(dispatchSignature->GetHandle(), arguments, offset, {}, 0, 1);
}

void ComputeProgram::DispatchIndirect(org::RecordingContext& recording, std::span<const std::uint32_t> constants, const org::LatchBlock& latch,
    std::uint32_t offset) const {
    const auto region = latch.At(recording.Run());
    DispatchIndirect(recording, constants, region.buffer->GetAPIResource().GetHandle(), region.offset + offset);
}

std::shared_ptr<const ComputeProgram> ComputeProgram::Load(rhi::Device device, const Desc& source) {
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
    // Resolved once here, so a hot reload compiles the same permutation.
    Desc desc = source;
    if (desc.commonDefines)
        desc.defines = build::WithCommonDefines(source.defines, L"cs_6_6");
    desc.commonDefines = false;
    auto* compiler = build::Compiler();
    if (!compiler) {
        build::Log(ProgramLogLevel::Warning, std::format("{}: no runtime shader compiler", desc.source));
        return {};
    }
    const auto& registryOf = CurrentProgramEnvironment().registry;
    auto* registry = registryOf ? registryOf() : nullptr;
    if (!registry) {
        build::Log(ProgramLogLevel::Error, std::format("{}: no pipeline registry", desc.source));
        return {};
    }
    // Read before the source, so a reload that starts after rebuilds it.
    const std::uint64_t builtFor = registry->SourcesGeneration();
    const std::filesystem::path path = build::SourcePath(desc.source);
    const auto bytes = build::ReadSource(path);
    if (bytes.empty()) {
        build::Log(ProgramLogLevel::Error, std::format("Missing shader source {}", path.string()));
        return {};
    }
    const std::string entry = build::Narrow(desc.entry);
    const auto artifact = compiler->Compile(MakeComputeRequest(desc, path, bytes));
    if (!artifact) {
        build::Log(ProgramLogLevel::Error,
            std::format("{} build of {} ({}) failed:\n{}", ShaderFormatName(build::Format()), desc.source, entry, artifact.diagnostics.substr(0, 2000)));
        return {};
    }

    auto program = std::make_shared<ComputeProgram>();
    program->constantWords = desc.constantWords;
    rhi::PushConstantRangeDesc constants{};
    constants.visibility = rhi::ShaderStage::Compute;
    constants.num32BitValues = desc.constantWords;
    if (device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, 1 }, .flags = rhi::PipelineLayoutFlags::PF_None }, program->layout) !=
        rhi::Result::Ok) {
        build::Log(ProgramLogLevel::Error, std::format("{}: the pipeline layout could not be created", desc.source));
        return {};
    }
    auto pipeline = CreateComputePipeline(device, program->layout->GetHandle(), artifact.binary, entry, desc.source);
    if (!pipeline) {
        build::Log(ProgramLogLevel::Error, std::format("{}: the compute pipeline could not be created", desc.source));
        return {};
    }
    rhi::IndirectArg dispatch[] = { { .kind = rhi::IndirectArgKind::Dispatch } };
    if (device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatch, 1), sizeof(std::uint32_t) * 3 },
            program->layout->GetHandle(), program->dispatchSignature) != rhi::Result::Ok) {
        build::Log(ProgramLogLevel::Error, std::format("{}: the dispatch signature could not be created", desc.source));
        return {};
    }
    LivePipelineDesc live;
    live.id = std::format("{} ({})", desc.source, entry);
    live.sources = { desc.source };
    live.rebuild = RebuildCompute(program, device, desc);
    program->slot = registry->Register(std::move(live), std::make_shared<rhi::PipelinePtr>(std::move(pipeline)), build::HashBinary(artifact.binary), builtFor);
    return program;
#else
    (void)device;
    build::Log(ProgramLogLevel::Warning, std::format("{}: built without the module's shader compiler", source.source));
    return {};
#endif
}

ComputeProgram::Desc ComputeProgram::GenerateMips(bool array) {
    // The module's shader has no other build, so it takes none of the host's defines.
    Desc desc{ .source = kGenerateMipsShader, .constantWords = kGenerateMipsConstantWords, .commonDefines = false };
    if (array)
        desc.defines.push_back({ kGenerateMipsArrayDefine, L"1" });
    return desc;
}

} // namespace org::services
