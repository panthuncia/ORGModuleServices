#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <system_error>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <ORGModuleServices/ServiceExecutor.h>

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
    // When nonzero: ShaderCompiler::FingerprintInputs of this source and these dependency files, taken by the caller while
    // they were current. The key uses it instead of hashing the source and checking every dependency file again: a
    // permutation set shares both, and a key otherwise costs a read of every dependency's attributes (slow through a
    // virtual file system), per permutation.
    uint64_t inputsFingerprint{};
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
    using Submit = ServiceSubmit;
    // Receives the artifact of a CompileAsync call. A failure is an artifact without a binary whose diagnostics say why
    // (an exception thrown by the compile is also delivered this way, while the future rethrows it).
    using CompletionCallback = std::function<void(const ShaderArtifact&)>;

    // compilerDirectory: load dxcompiler.dll and dxil.dll from there. Otherwise the loader's search
    // order applies, then the directory of the module this library is linked into. In-process
    // hosts (game plugins) should pass it: another dxcompiler.dll, possibly without SPIR-V support,
    // may already be loaded by basename.
    // Renames a written cache file onto its key's (replacing it). A host whose other code renames files too passes its own, so
    // that a file system hook unsafe under concurrent renames (Mod Organizer's usvfs) sees one at a time across the process.
    // Without one, the compiler's renames are serialized among themselves.
    using RenameFile = std::function<void(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec)>;

    explicit ShaderCompiler(std::filesystem::path cacheDirectory = {}, std::filesystem::path compilerDirectory = {}, RenameFile rename = {});
    ~ShaderCompiler();
    ShaderCompiler(ShaderCompiler&&) noexcept;
    ShaderCompiler& operator=(ShaderCompiler&&) noexcept;
    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;

    bool Available() const noexcept;
    uint64_t BuildKey(const ShaderCompileRequest& request) const noexcept;
    // The source's contents and every dependency's, for ShaderCompileRequest::inputsFingerprint. A dependency's contents are
    // re-read only when its size or write time changed since the compiler last hashed it.
    uint64_t FingerprintInputs(std::span<const std::byte> source, const std::vector<std::filesystem::path>& dependencyFiles) const noexcept;
    std::shared_future<ShaderArtifact> CompileAsync(ShaderCompileRequest request);
    // As above, and invokes onComplete exactly once with the artifact, after it is available through the returned
    // future, never while the compiler holds a lock of its own (it may call back into the compiler):
    // - a memory-cache hit: at once, on the calling thread, and no work is submitted;
    // - joining a request already in flight for the same key: on the thread that completes that request, or at once on
    //   the calling thread when it has already completed;
    // - otherwise: on the thread that runs the compile (the submitter's, see Configure); a refused submission
    //   completes at once on the calling thread with an error artifact.
    std::shared_future<ShaderArtifact> CompileAsync(ShaderCompileRequest request, CompletionCallback onComplete);
    // Synchronous: blocks on CompileAsync. Never call it from a thread the configured submitter needs in order to run
    // the compile (for example the thread that drains a manual queue).
    ShaderArtifact Compile(ShaderCompileRequest request);
    // Installs the executor every compile is submitted to; empty restores the default (one new thread per compile).
    // Setup: call before the compiler is shared with other threads. Requests submitted earlier keep their executor.
    // Queued compiles keep the compiler's internals alive, so destroying the ShaderCompiler before they run is safe.
    void Configure(Submit submit);
    void ClearMemoryCache();

private:
    class Impl;
    std::shared_ptr<Impl> impl_; // shared with the compiles in flight
};

}
