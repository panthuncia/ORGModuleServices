#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace org::services {

enum class ShaderBinaryFormat : uint8_t { Dxil, Spirv };

struct ShaderDefine { std::wstring name; std::wstring value; };

struct ShaderCompileRequest {
    std::string sourceName;
    std::span<const std::byte> source;
    std::wstring entryPoint;
    std::wstring target;
    std::vector<ShaderDefine> defines;
    std::vector<std::wstring> arguments;
	std::vector<std::filesystem::path> includeDirectories;
	std::vector<std::filesystem::path> dependencyFiles;
    ShaderBinaryFormat format{ ShaderBinaryFormat::Dxil };
    // DXC -HV: 2018 keeps the pre-2021 semantics (vector ternaries, ...) that FXC-era sources rely on.
    std::wstring languageVersion{ L"2021" };
    // Source-level debug info: embedded PDB for DXIL, OpSource/OpLine for SPIR-V, with
    // every source file's text embedded. sourceName is the file name the debug info and diagnostics refer to.
    bool debugInfo{};
    bool warningsAsErrors{ true };
};

struct ShaderArtifact {
    uint64_t key{};
    ShaderBinaryFormat format{};
    std::vector<std::byte> binary;
    std::string diagnostics;
    bool fromCache{};
    explicit operator bool() const noexcept { return !binary.empty(); }
};

class ShaderCompiler {
public:
    // compilerDirectory: load dxcompiler.dll and dxil.dll from there. Otherwise the loader's search
    // order applies, then the directory of the module this library is linked into. In-process
    // hosts (game plugins) should pass it: another dxcompiler.dll, possibly without SPIR-V support,
    // may already be loaded by basename.
    explicit ShaderCompiler(std::filesystem::path cacheDirectory = {}, std::filesystem::path compilerDirectory = {});
    ~ShaderCompiler();
    ShaderCompiler(ShaderCompiler&&) noexcept;
    ShaderCompiler& operator=(ShaderCompiler&&) noexcept;
    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;

    bool Available() const noexcept;
    uint64_t BuildKey(const ShaderCompileRequest& request) const noexcept;
    std::shared_future<ShaderArtifact> CompileAsync(ShaderCompileRequest request);
    ShaderArtifact Compile(ShaderCompileRequest request);
    void ClearMemoryCache();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
