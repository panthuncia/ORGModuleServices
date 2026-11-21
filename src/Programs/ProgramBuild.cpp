#include <ORGModuleServices/Programs/ProgramBuild.h>

#include <ORGModuleServices/Programs/ShaderTarget.h>
#include <ORGModuleServices/ShaderStore.h>

#include <cstring>
#include <fstream>
#include <iterator>

namespace org::services {

namespace {

ProgramEnvironment& EnvironmentStorage() {
    static ProgramEnvironment environment;
    return environment;
}

uint64_t Fold(const ShaderStoreHash& hash) { return hash.high ^ hash.low; }

} // namespace

void SetProgramEnvironment(ProgramEnvironment environment) { EnvironmentStorage() = std::move(environment); }

const ProgramEnvironment& CurrentProgramEnvironment() { return EnvironmentStorage(); }

namespace program_build {

ShaderBinaryFormat Format() {
    const auto& backend = CurrentProgramEnvironment().backend;
    return ShaderFormatFor(backend ? backend() : rhi::Backend::Vulkan);
}

ShaderCompiler* Compiler() {
    const auto& compiler = CurrentProgramEnvironment().compiler;
    return compiler ? compiler() : nullptr;
}

void Log(ProgramLogLevel level, std::string_view message) {
    if (const auto& log = CurrentProgramEnvironment().log)
        log(level, message);
}

std::filesystem::path SourcePath(std::string_view source) {
    return std::filesystem::path(CurrentProgramEnvironment().shaderRoot) / source;
}

std::vector<std::byte> ReadSource(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> chars((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    if (!chars.empty())
        std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
}

uint64_t HashBinary(std::span<const std::byte> bytes) { return Fold(HashShaderBytes(bytes)); }

uint64_t HashBinaries(std::span<const std::byte> first, std::span<const std::byte> second) {
    ShaderKeyBuilder key;
    key.Add(HashShaderBytes(first)).Add(HashShaderBytes(second));
    return Fold(key.Finish());
}

ProgramDefines WithCommonDefines(const ProgramDefines& own, const wchar_t* target) {
    const auto& common = CurrentProgramEnvironment().commonDefines;
    return common ? common(own, target ? std::wstring_view(target) : std::wstring_view()) : own;
}

ShaderCompileRequest MakeRequest(const std::filesystem::path& path, std::span<const std::byte> source, const std::wstring& entry,
    const wchar_t* target, const ProgramDefines& defines) {
    const auto& environment = CurrentProgramEnvironment();
    ShaderCompileRequest request{};
    request.sourceName = path.generic_string();
    request.source = source;
    request.entryPoint = entry;
    request.target = target;
    // SPIR-V on Vulkan, DXIL on D3D12, from the same HLSL.
    request.format = Format();
    request.warningsAsErrors = false;
    // Source-level debug info for graphics debuggers. Drivers ignore it and optimization is unchanged.
    request.debugInfo = true;
    request.includeDirectories = { environment.shaderRoot };
    // Storage images take their view's format. Without it DXC types every float4 RWTexture as Rgba32f, which must not be
    // written through an RGBA16F view. D3D12 always works this way.
    AddSpirvArgument(request, L"-fspv-use-unknown-image-format");
    request.defines.push_back({ ShaderApiDefine(request.format), L"1" });
    if (environment.requestDefines)
        for (auto& [name, value] : environment.requestDefines())
            request.defines.push_back({ std::move(name), std::move(value) });
    for (const auto& [name, value] : defines)
        request.defines.push_back({ name, value });
    return request;
}

std::string Narrow(std::wstring_view text) {
    std::string narrow;
    narrow.reserve(text.size());
    for (const wchar_t c : text)
        narrow.push_back(static_cast<char>(c));
    return narrow;
}

} // namespace program_build

} // namespace org::services
