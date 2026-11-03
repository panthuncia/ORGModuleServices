#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A persistent shader cache that knows nothing about any compiler, graphics API or host.
//
// A shader variant is valid when the files it read last time still have the same contents.
// File times are never used: each file is hashed once per generation, which costs about the
// same as asking for its time and cannot be fooled by touched or restored files.
//
// The lookup for one variant:
//  1. Find the variant's record. If its root and its files are unchanged, read its artifact.
//  2. Otherwise preprocess. If the preprocessed text gives a known artifact, use it. This covers
//     comment edits, inactive code, reverted edits and variants that compile to the same thing.
//  3. Otherwise compile, and save the result.
//
// The index only speeds up step 1. Losing it costs preprocessing, never a compile.
namespace org::services {

struct ShaderStoreHash {
    uint64_t high{};
    uint64_t low{};
    bool operator==(const ShaderStoreHash&) const = default;
    explicit operator bool() const noexcept { return high || low; }
    // 32 lowercase hex digits, high word first.
    std::string ToHex() const;
};

ShaderStoreHash HashShaderBytes(std::span<const std::byte> bytes) noexcept;
ShaderStoreHash HashShaderText(std::string_view text) noexcept;

// Builds a key from a list of fields. Each field is stored with its length, so
// {"A", "BC"} and {"AB", "C"} give different keys.
class ShaderKeyBuilder {
public:
    ShaderKeyBuilder();
    ~ShaderKeyBuilder();
    ShaderKeyBuilder(const ShaderKeyBuilder&) = delete;
    ShaderKeyBuilder& operator=(const ShaderKeyBuilder&) = delete;

    ShaderKeyBuilder& Add(std::string_view text) noexcept;
    ShaderKeyBuilder& Add(std::wstring_view text) noexcept;
    ShaderKeyBuilder& Add(uint64_t value) noexcept;
    ShaderKeyBuilder& Add(const ShaderStoreHash& value) noexcept;
    ShaderStoreHash Finish() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// The text that decides whether two compiles match. Removes blank lines and line markers, in both
// the "#line N" form and the "# N" form. A debug build must not use this: its output embeds line numbers.
std::string CanonicalizePreprocessedText(std::string_view text);

// One source file as this generation sees it.
struct ShaderSourceFile {
    std::string logicalPath;
    bool exists{};
    ShaderStoreHash hash;
    std::shared_ptr<const std::string> bytes;
};

// What the compiler's include callback uses to read files. It records every path it is asked for,
// including paths that do not exist: adding such a file later can change the include's result.
class ShaderIncludeTracker {
public:
    virtual ~ShaderIncludeTracker() = default;
    // Returns the file's bytes, or null if the file does not exist. The bytes stay valid for the whole build.
    virtual const std::string* Open(const std::filesystem::path& path) = 0;
};

struct ShaderPreprocessOutput {
    bool succeeded{};
    bool transientFailure{};  // Out of memory and the like: do not remember this failure.
    std::string text;
    std::string diagnostics;
};

struct ShaderCompileOutput {
    bool succeeded{};
    bool transientFailure{};  // Out of memory and the like: do not remember this failure.
    std::vector<std::byte> binary;
    std::string diagnostics;
};

// What a host provides for one variant.
class ShaderVariantBuild {
public:
    virtual ~ShaderVariantBuild() = default;
    // root is the root file's bytes, or null if it does not exist.
    virtual ShaderPreprocessOutput Preprocess(const std::string* root, ShaderIncludeTracker& includes) = 0;
    virtual ShaderCompileOutput Compile(const std::string* root, const ShaderPreprocessOutput& preprocessed, ShaderIncludeTracker& includes) = 0;
};

struct ShaderVariantRequest {
    // Everything that identifies the variant, as the host builds it: backend, root name, entry point,
    // target, defines in order, final compiler arguments and flags, compiler identity.
    ShaderStoreHash variantKey;
    // The inputs that two variants must share to share an artifact: backend, entry point, target,
    // final arguments and flags except defines, include directories and the root name, compiler identity.
    ShaderStoreHash compileConfigKey;
    // Debug builds embed source text and line numbers, so every source change rebuilds them and they share nothing.
    bool debug{};
    uint8_t format{};  // Stored with the artifact and checked when it is read.
    // The root file. It is read through the snapshot unless rootBytes is given.
    std::filesystem::path rootPath;
    // For a root that was generated in memory (a rewritten source). Must stay valid during the call.
    std::span<const std::byte> rootBytes;
    bool hasRootBytes{};
    ShaderVariantBuild* build{};
};

enum class ShaderStoreOutcome : uint8_t {
    Hit,       // The record was valid and its artifact was read.
    Absorbed,  // Inputs changed but an artifact for the same preprocessed text already existed.
    Compiled,
    Failed,
};

enum class ShaderStoreMiss : uint8_t {
    None,
    NoRecord,
    RootChanged,
    DependencyChanged,
    ArtifactInvalid,  // The artifact is missing, corrupt or has the wrong format.
    Disabled,         // Persistence is off.
    Count,
};

struct ShaderStoreResult {
    ShaderStoreOutcome outcome{ ShaderStoreOutcome::Failed };
    ShaderStoreMiss miss{ ShaderStoreMiss::None };
    bool failureFromCache{};  // A remembered failure: nothing ran.
    std::string changedFile;  // The first changed file, for ShaderStoreMiss::DependencyChanged.
    ShaderStoreHash compileKey;
    std::vector<std::byte> binary;
    std::string diagnostics;
};

struct ShaderStoreStats {
    uint64_t requests{}, hits{}, absorbed{}, compiled{}, failed{}, failuresFromCache{}, notSaved{}, preprocessed{};
    uint64_t sourceFilesRead{};
    std::array<uint64_t, static_cast<size_t>(ShaderStoreMiss::Count)> misses{};
    std::string firstChangedFile;
};

struct ShaderStoreIndexInfo {
    uint64_t variants{}, dependencySets{}, dependencyEntries{}, files{}, savedBytes{};
    // What startup found and did. overCapBytes is what stayed over maxBytes because recent starts used it.
    uint64_t artifactsAtStart{}, artifactBytesAtStart{}, artifactsEvicted{}, overCapBytes{};
};

struct ShaderStoreOptions {
    // Include paths are recorded relative to this folder, so moving the folder invalidates nothing.
    std::filesystem::path sourceRoot;
    std::filesystem::path cacheDirectory;
    // Evict down to this size at startup. Anything used in the last keepGenerations starts is kept even over this size:
    // evicting a live shader would rebuild it on every start.
    uint64_t maxBytes{ 4ull << 30 };
    uint32_t keepGenerations{ 3 };
    // How often a changed index is saved. Zero saves only on Flush and in the destructor, for tests.
    std::chrono::milliseconds saveInterval{ 5000 };
    // False reads and writes nothing on disk except source files. Requests always build.
    bool persistent{ true };
};

class ShaderStore {
public:
    explicit ShaderStore(ShaderStoreOptions options);
    ~ShaderStore();
    ShaderStore(const ShaderStore&) = delete;
    ShaderStore& operator=(const ShaderStore&) = delete;

    // Safe to call from many threads.
    ShaderStoreResult Resolve(const ShaderVariantRequest& request);

    // Source files may have changed: forget the snapshot. Later requests validate against the new contents.
    void BeginGeneration();
    // Moves with BeginGeneration. A host's in-memory results are only good for one generation.
    uint64_t Generation() const;
    // Saves the index now if it changed. Call this when a batch of compiles ends.
    void Flush();

    // Calls fn with the absolute path of each file the variant read, for hot reload. False if there is no record.
    bool ForEachDependency(const ShaderStoreHash& variantKey, const std::function<void(const std::filesystem::path&)>& fn) const;

    ShaderStoreStats Stats() const;
    ShaderStoreIndexInfo IndexInfo() const;
    // One line for the log: what happened, and why anything was not a plain hit.
    std::string Summary() const;
    // False when another process owns this cache folder. Requests still work, but the index is not saved.
    bool OwnsIndex() const noexcept;

    // How many artifacts are on disk and how many bytes they take. This walks the folder, so call it away from the frame.
    struct ArtifactUsage { uint64_t count{}, bytes{}; };
    ArtifactUsage MeasureArtifacts() const;
    // Removes one artifact, for one the host could not use (a device rejected it). The next request builds it again.
    void RemoveArtifact(const ShaderStoreHash& compileKey);
    // Removes every artifact and the index.
    void Clear();
    // Removes only the index. The next start preprocesses but does not compile.
    void ClearIndex();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace org::services
