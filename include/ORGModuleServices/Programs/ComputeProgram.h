#pragma once

#include <ORGModuleServices/Passes/ComputeKernel.h>

#include <rhi.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace org {
class LatchBlock;
class RecordingContext;
}

namespace org::services {

class LivePipelineSlot;

// A compute shader, its pipeline and its push-constant layout.
//
// Compiled from HLSL at runtime through the environment's compiler (ProgramBuild.h), which keeps it in its shader store.
// Shaders fetch everything else by heap index. For hot reload the pipeline lives in a registry slot: a recompiled pipeline is
// a new version, which graphs built after it bind. A program that fails to recompile keeps its pipeline.
// It is a ComputeKernel, so the module's passes record through it.
struct ComputeProgram final : ComputeKernel {
    rhi::PipelineLayoutPtr layout;
    // Bind Pipeline(recording) at record time, never a handle kept across recordings.
    std::shared_ptr<LivePipelineSlot> slot;
    std::uint32_t constantWords = 0;
    // Three uints of group counts. The one owner for indirect dispatches.
    rhi::CommandSignaturePtr dispatchSignature;

    // The pipeline a recording binds: the slot's version the environment's versionFor gives (the version at its graph's
    // pin, say). A shader reload never changes it.
    rhi::PipelineHandle Pipeline(const org::RecordingContext& recording) const;
    // The slot's newest pipeline, for a list recorded outside a graph and submitted at once. The registry keeps a version it
    // drops until every submission made before has completed.
    rhi::PipelineHandle CurrentPipeline() const;

    struct Desc {
        std::string source;  // relative to the environment's shader root
        std::wstring entry = L"main";
        std::uint32_t constantWords = 0;  // in 32-bit words
        std::vector<std::pair<std::wstring, std::wstring>> defines;
        // Also take the environment's common defines (ProgramEnvironment::commonDefines). Off for a shader that must not
        // see the host's defines.
        bool commonDefines = true;
    };

    // Null and logged on failure. A compile the cache does not have runs on the calling thread (ShaderCompiler::CompileHere),
    // so call it at setup or on a build thread, not per frame.
    static std::shared_ptr<const ComputeProgram> Load(rhi::Device device, const Desc& desc);

    // Records nothing when the constants are longer than constantWords or a group count is zero.
    void Dispatch(org::RecordingContext& recording, std::span<const std::uint32_t> constants, std::uint32_t x, std::uint32_t y,
        std::uint32_t z) const override;

    // Dispatches the group counts at offset in arguments, three 4-byte aligned uints. A zero count dispatches nothing, so a
    // kept recording can skip work by its data alone.
    void DispatchIndirect(org::RecordingContext& recording, std::span<const std::uint32_t> constants, rhi::ResourceHandle arguments,
        std::uint64_t offset) const;

    // DispatchIndirect with the group counts in the latch region of the recording's run, which the commit writes each run.
    void DispatchIndirect(org::RecordingContext& recording, std::span<const std::uint32_t> constants, const org::LatchBlock& latch,
        std::uint32_t offset) const override;

    // The module's GenerateMips shader (kGenerateMipsShader), for 2D textures or for arrays and cubes.
    static Desc GenerateMips(bool array);
};

} // namespace org::services
