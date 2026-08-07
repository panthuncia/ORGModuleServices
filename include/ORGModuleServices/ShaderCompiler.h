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
    ShaderBinaryFormat format{ ShaderBinaryFormat::Dxil };
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
    explicit ShaderCompiler(std::filesystem::path cacheDirectory = {});
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
