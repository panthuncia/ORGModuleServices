#pragma once

// What ComputeProgram and RasterProgram share to build their stages: the host's environment, and the request for one stage.

#include <ORGModuleServices/ShaderCompiler.h>

#include <rhi.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace org {
class RecordingContext;
}

namespace org::services {

class LivePipelineSlot;
class PipelineRegistry;
struct PipelineVersion;

// A program's defines, in order: name, value.
using ProgramDefines = std::vector<std::pair<std::wstring, std::wstring>>;

enum class ProgramLogLevel : uint8_t { Info, Warning, Error };

// What the programs read from their host. The host sets it once at setup, before the first program loads
// (SetProgramEnvironment). Every callback but commonDefines may be called on any thread.
struct ProgramEnvironment {
    // The folder program sources are relative to. Also the compiler's include directory.
    std::string shaderRoot;
    // The device's backend, which picks the binary format (ShaderFormatFor). Empty: Vulkan.
    std::function<rhi::Backend()> backend;
    // The compiler, or null while there is none.
    std::function<ShaderCompiler*()> compiler;
    // The registry live pipelines are registered in. Needed once a compiler is there.
    std::function<PipelineRegistry*()> registry;
    // The host's defines for a program that asks for them (its desc's commonDefines): a_own plus the host's, for a stage
    // target such as "cs_6_6". Called where a program is queued or resolved (ResolveProgramDefines), never on a build
    // thread, so it may read state only that thread may read. Empty: the program's own defines alone.
    std::function<ProgramDefines(const ProgramDefines& own, std::wstring_view target)> commonDefines;
    // Defines every request gets after the API define and before the program's own.
    std::function<ProgramDefines()> requestDefines;
    // Record time. The version of a slot a recording binds. Empty: the slot's newest version.
    std::function<std::shared_ptr<const PipelineVersion>(const LivePipelineSlot&, const org::RecordingContext&)> versionFor;
    // Labels the pipelines the calling thread creates next, for debugging dumps. Null clears it. Optional.
    std::function<void(const char*)> labelPipelines;
    // The programs' messages. Optional.
    std::function<void(ProgramLogLevel, std::string_view)> log;
    // Whether SPIR-V builds take bound push tables: the Vulkan device binds PushIndex constant buffers as real uniform buffers
    // (rhi::vulkan::BoundUniformBuffers). Empty: they do not. DXIL builds always take them.
    std::function<bool()> spirvBoundPushTables;
    // Whether stages carry source-level debug info for graphics debuggers. Empty: they do.
    std::function<bool()> debugInfo;
};

// Setup, once, before any program loads and before any other thread reads it.
void SetProgramEnvironment(ProgramEnvironment environment);

// Any thread. The environment set at setup, or an empty one.
const ProgramEnvironment& CurrentProgramEnvironment();

namespace program_build {

// The environment's binary format.
ShaderBinaryFormat Format();

// The environment's compiler, or null.
ShaderCompiler* Compiler();

// Logs through the environment, when it has a log.
void Log(ProgramLogLevel level, std::string_view message);

// source is relative to the environment's shader root.
std::filesystem::path SourcePath(std::string_view source);

// The file's bytes, empty when it is missing.
std::vector<std::byte> ReadSource(const std::filesystem::path& path);

// The content hash of a binary, folded to 64 bits, used as a live pipeline's fingerprint.
uint64_t HashBinary(std::span<const std::byte> bytes);

// The fingerprint of two binaries together, such as a vertex and a pixel stage.
uint64_t HashBinaries(std::span<const std::byte> first, std::span<const std::byte> second);

// own plus the environment's common defines for target. Call it where programs are queued, not from a build thread.
ProgramDefines WithCommonDefines(const ProgramDefines& own, const wchar_t* target);

// The compiler request for one stage, in the environment's format. source must outlive the request.
ShaderCompileRequest MakeRequest(const std::filesystem::path& path, std::span<const std::byte> source, const std::wstring& entry,
    const wchar_t* target, const ProgramDefines& defines);

// Narrows an ASCII name, such as an entry point.
std::string Narrow(std::wstring_view text);

// Bound push tables. A program build gets ORG_BOUND_PUSH_TABLES defined: DXIL always, SPIR-V when the environment's
// spirvBoundPushTables says the device binds them. A constant buffer a stage then declares at register(b<w>, space
// kBoundPushTableSpace) is bound at the heap slot pushed in word w of the program's push constants: a descriptor table on D3D12,
// a push descriptor uniform buffer on Vulkan (DXC puts it at set kBoundPushTableSpace, binding w). On NVIDIA its values are free
// operands, where a heap-indexed constant buffer's cost a register each. Other SPIR-V builds read every constant buffer from the
// heap.
inline constexpr uint32_t kBoundPushTableSpace = 11;

// Whether this environment's builds take bound push tables.
bool BoundPushTables();

// The layout ranges for the bound push tables a_stages declare (DXIL or SPIR-V reflection), at most a_constantWords of them.
std::vector<rhi::LayoutBindingRange> BoundPushTableRanges(std::initializer_list<std::span<const std::byte>> a_stages, uint32_t a_constantWords,
    rhi::ShaderStage a_visibility);

} // namespace program_build

} // namespace org::services
