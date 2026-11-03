// Tests for ShaderStore, using real files and a fake compiler that counts its calls.
//
// The fake preprocessor understands just enough:
//  - `#include "x"` tries each search directory in order, and inserts the file.
//  - `//` comments are removed.
//  - Each include writes a `#line` marker, as a real preprocessor does.
//  - `#error` fails preprocessing, and a line `compile_error` fails the compile.
// The fake compiler's output is the text it was given plus the build's flags, so a stale result is visible.

#include <ORGModuleServices/ShaderStore.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace org::services;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            std::abort();                                                                \
        }                                                                                \
    } while (0)

std::atomic<int> g_preprocesses{};
std::atomic<int> g_compiles{};

void ResetCounts() { g_preprocesses = 0; g_compiles = 0; }

void WriteFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::stringstream text;
    text << stream.rdbuf();
    return text.str();
}

class FakeBuild final : public ShaderVariantBuild {
public:
    FakeBuild(std::vector<fs::path> searchDirectories, std::string defines) :
        search_(std::move(searchDirectories)), defines_(std::move(defines)) {}

    ShaderPreprocessOutput Preprocess(const std::string* root, ShaderIncludeTracker& includes) override {
        ++g_preprocesses;
        ShaderPreprocessOutput output;
        if (!root) { output.diagnostics = "no root"; return output; }
        output.succeeded = Expand(*root, includes, output);
        return output;
    }

    ShaderCompileOutput Compile(const std::string*, const ShaderPreprocessOutput& preprocessed, ShaderIncludeTracker&) override {
        ++g_compiles;
        ShaderCompileOutput output;
        if (preprocessed.text.find("compile_error") != std::string::npos) { output.diagnostics = "compile error"; return output; }
        if (transient) { output.transientFailure = true; output.diagnostics = "out of memory"; return output; }
        const auto text = "compiled(" + flags + "): " + preprocessed.text;
        const auto* first = reinterpret_cast<const std::byte*>(text.data());
        output.binary.assign(first, first + text.size());
        output.succeeded = true;
        return output;
    }

    std::string flags;
    bool transient{};

private:
    bool Expand(const std::string& source, ShaderIncludeTracker& includes, ShaderPreprocessOutput& output) {
        std::istringstream lines(source);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.starts_with("#include \"")) {
                const auto name = line.substr(10, line.size() - 11);
                const std::string* found = nullptr;
                for (const auto& directory : search_)
                    if ((found = includes.Open(directory / name))) break;
                if (!found) { output.diagnostics = "cannot open " + name; return false; }
                output.text += "#line 1 \"" + name + "\"\n";
                if (!Expand(*found, includes, output)) return false;
                continue;
            }
            if (line.starts_with("#error")) { output.diagnostics = line; return false; }
            if (const auto comment = line.find("//"); comment != std::string::npos) line.resize(comment);
            output.text += line + "\n";
        }
        // A define only reaches the text when the source asks for it, as with macro expansion.
        if (source.find("USE_DEFINES") != std::string::npos) output.text += defines_ + "\n";
        return true;
    }

    std::vector<fs::path> search_;
    std::string defines_;
};

// A temporary game folder: Data/Shaders plus a cache folder.
struct Fixture {
    Fixture() {
        static std::atomic<int> counter{};
        folder = fs::temp_directory_path() / ("org_shader_store_test_" + std::to_string(++counter));
        fs::remove_all(folder);
        fs::create_directories(shaders());
        ResetCounts();
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(folder, ec);
    }
    fs::path shaders() const { return folder / "Data" / "Shaders"; }
    fs::path cache() const { return folder / "Cache"; }

    ShaderStoreOptions Options() const {
        ShaderStoreOptions options;
        options.sourceRoot = folder / "Data";
        options.cacheDirectory = cache();
        options.saveInterval = std::chrono::milliseconds(0);
        return options;
    }
    fs::path folder;
};

struct Variant {
    std::string rootName{ "Root.hlsl" };
    std::string defines;
    std::string flags;
    bool debug{};
    uint8_t format{ 1 };
};

ShaderStoreHash ConfigKeyOf(const Variant& variant) {
    ShaderKeyBuilder config;
    config.Add(std::string_view("fake")).Add(std::string_view(variant.flags));
    return config.Finish();
}

ShaderStoreHash VariantKeyOf(const Variant& variant) {
    ShaderKeyBuilder key;
    key.Add(ConfigKeyOf(variant)).Add(std::string_view(variant.rootName)).Add(std::string_view(variant.defines)).Add(static_cast<uint64_t>(variant.debug));
    return key.Finish();
}

ShaderStoreResult Run(ShaderStore& store, const Fixture& fixture, const Variant& variant = {}, std::vector<fs::path> search = {}, const std::string* generatedRoot = nullptr, bool transient = false) {
    if (search.empty()) search.push_back(fixture.shaders());
    FakeBuild build(std::move(search), variant.defines);
    build.flags = variant.flags;
    build.transient = transient;
    ShaderVariantRequest request;
    request.compileConfigKey = ConfigKeyOf(variant);
    request.variantKey = VariantKeyOf(variant);
    request.debug = variant.debug;
    request.format = variant.format;
    request.rootPath = fixture.shaders() / variant.rootName;
    if (generatedRoot) {
        request.rootBytes = std::as_bytes(std::span(*generatedRoot));
        request.hasRootBytes = true;
    }
    request.build = &build;
    return store.Resolve(request);
}

std::string Text(const ShaderStoreResult& result) { return std::string(reinterpret_cast<const char*>(result.binary.data()), result.binary.size()); }

// One "boot": a fresh store over the same folder, as a new game process would make.
template <typename Body> void Boot(ShaderStoreOptions options, Body&& body) {
    ResetCounts();
    ShaderStore store(std::move(options));
    body(store);
}
template <typename Body> void Boot(const Fixture& fixture, Body&& body) { Boot(fixture.Options(), std::forward<Body>(body)); }

void WriteBasicShaders(const Fixture& fixture) {
    WriteFile(fixture.shaders() / "Root.hlsl", "#include \"Common.hlsli\"\nmain code\n");
    WriteFile(fixture.shaders() / "Common.hlsli", "common code\n");
}

void TestSecondBootDoesNoWork() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    std::string first;
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.outcome == ShaderStoreOutcome::Compiled);
        CHECK(result.miss == ShaderStoreMiss::NoRecord);
        first = Text(result);
        CHECK(g_compiles == 1);
        CHECK(g_preprocesses == 1);
    });
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.outcome == ShaderStoreOutcome::Hit);
        CHECK(Text(result) == first);
        CHECK(g_compiles == 0);
        CHECK(g_preprocesses == 0);
        CHECK(store.Stats().hits == 1);
    });
}

void TestTouchedAndFutureTimesDoNoWork() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    Boot(fixture, [&](ShaderStore& store) { Run(store, fixture); });
    // Far in the future, then far in the past: file times must not matter at all.
    for (const auto offset : { std::chrono::hours(24 * 365), std::chrono::hours(-24 * 365 * 5) }) {
        for (const auto& entry : fs::directory_iterator(fixture.shaders()))
            fs::last_write_time(entry.path(), fs::file_time_type::clock::now() + offset);
        Boot(fixture, [&](ShaderStore& store) {
            CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Hit);
            CHECK(g_compiles == 0);
            CHECK(g_preprocesses == 0);
        });
    }
}

void TestLosingTheIndexNeverCompiles() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    Boot(fixture, [&](ShaderStore& store) { Run(store, fixture); });
    std::error_code ec;
    fs::remove(fixture.cache() / "index.a", ec);
    fs::remove(fixture.cache() / "index.b", ec);
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Absorbed);
        CHECK(g_preprocesses == 1);
        CHECK(g_compiles == 0);
    });
    // The index was rebuilt, so the next boot does nothing.
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Hit);
        CHECK(g_preprocesses == 0);
    });
}

void TestCommentEditIsAbsorbedAndRevertCompilesNothing() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    Boot(fixture, [&](ShaderStore& store) { Run(store, fixture); });
    WriteFile(fixture.shaders() / "Common.hlsli", "// a comment\n\n\ncommon code\n");
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.outcome == ShaderStoreOutcome::Absorbed);
        CHECK(result.miss == ShaderStoreMiss::DependencyChanged);
        CHECK(result.changedFile == "shaders/common.hlsli");
        CHECK(g_preprocesses == 1);
        CHECK(g_compiles == 0);
    });
    // A real edit compiles, and the output really changes.
    WriteFile(fixture.shaders() / "Common.hlsli", "new code\n");
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.outcome == ShaderStoreOutcome::Compiled);
        CHECK(Text(result).find("new code") != std::string::npos);
        CHECK(g_compiles == 1);
    });
    // Reverting finds the old artifact.
    WriteFile(fixture.shaders() / "Common.hlsli", "common code\n");
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.outcome == ShaderStoreOutcome::Absorbed);
        CHECK(Text(result).find("common code") != std::string::npos);
        CHECK(g_compiles == 0);
    });
}

void TestChangesThatTimestampsMissAreDetected() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    Boot(fixture, [&](ShaderStore& store) { Run(store, fixture); });
    const auto path = fixture.shaders() / "Common.hlsli";
    const auto time = fs::last_write_time(path);
    // Same size and same time, different content. Only a content hash can see this.
    WriteFile(path, "COMMON CODE\n");
    fs::last_write_time(path, time);
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.outcome == ShaderStoreOutcome::Compiled);
        CHECK(Text(result).find("COMMON CODE") != std::string::npos);
    });
    // A file older than the cache, as a mod install gives: also detected.
    WriteFile(path, "older mod\n");
    fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(24 * 365 * 3));
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.outcome == ShaderStoreOutcome::Compiled);
        CHECK(Text(result).find("older mod") != std::string::npos);
    });
}

void TestOnlyDependentVariantsRebuild() {
    Fixture fixture;
    WriteFile(fixture.shaders() / "A.hlsl", "#include \"OnlyA.hlsli\"\na\n");
    WriteFile(fixture.shaders() / "B.hlsl", "b\n");
    WriteFile(fixture.shaders() / "OnlyA.hlsli", "only a\n");
    const Variant a{ "A.hlsl" }, b{ "B.hlsl" };
    Boot(fixture, [&](ShaderStore& store) { Run(store, fixture, a); Run(store, fixture, b); CHECK(g_compiles == 2); });
    WriteFile(fixture.shaders() / "OnlyA.hlsli", "changed\n");
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture, b).outcome == ShaderStoreOutcome::Hit);
        CHECK(g_preprocesses == 0);
        CHECK(Run(store, fixture, a).outcome == ShaderStoreOutcome::Compiled);
        CHECK(g_compiles == 1);
    });
}

void TestFilesNeverOpenedDoNotCount() {
    // A file this variant never opened cannot invalidate it. A regex scan of the tree would count it.
    Fixture fixture;
    WriteBasicShaders(fixture);
    WriteFile(fixture.shaders() / "Unrelated.hlsli", "unrelated\n");
    Boot(fixture, [&](ShaderStore& store) { Run(store, fixture); });
    WriteFile(fixture.shaders() / "Unrelated.hlsli", "edited\n");
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Hit);
        CHECK(g_preprocesses == 0);
    });
}

void TestNewFileShadowingAnIncludeIsDetected() {
    Fixture fixture;
    const auto first = fixture.shaders() / "First";
    const auto second = fixture.shaders() / "Second";
    WriteFile(fixture.shaders() / "Root.hlsl", "#include \"X.hlsli\"\n");
    WriteFile(second / "X.hlsli", "from second\n");
    const std::vector<fs::path> search{ first, second };
    Boot(fixture, [&](ShaderStore& store) { CHECK(Text(Run(store, fixture, {}, search)).find("from second") != std::string::npos); });
    // No recorded file changed. Only the recorded absence shows that this matters.
    WriteFile(first / "X.hlsli", "from first\n");
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture, {}, search);
        CHECK(result.miss == ShaderStoreMiss::DependencyChanged);
        CHECK(result.outcome == ShaderStoreOutcome::Compiled);
        CHECK(Text(result).find("from first") != std::string::npos);
    });
    // Removing it again goes back without compiling.
    fs::remove(first / "X.hlsli");
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Text(Run(store, fixture, {}, search)).find("from second") != std::string::npos);
        CHECK(g_compiles == 0);
    });
}

void TestFlagsSeparateVariants() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    const Variant base, flagged{ "Root.hlsl", "", "partial_precision" };
    Boot(fixture, [&](ShaderStore& store) { Run(store, fixture, base); });
    // V1 reused the old file here: its path does not contain the flags.
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture, flagged);
        CHECK(result.outcome == ShaderStoreOutcome::Compiled);
        CHECK(Text(result).find("partial_precision") != std::string::npos);
    });
    // Toggling back needs nothing, and neither does toggling forward again.
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture, base).outcome == ShaderStoreOutcome::Hit);
        CHECK(Run(store, fixture, flagged).outcome == ShaderStoreOutcome::Hit);
        CHECK(g_preprocesses == 0);
    });
}

void TestVariantsWithTheSameTextShareOneArtifact() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    // This shader never looks at the define, so both variants compile to the same thing.
    const Variant one{ "Root.hlsl", "UNUSED_A" }, two{ "Root.hlsl", "UNUSED_B" };
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture, one).outcome == ShaderStoreOutcome::Compiled);
        CHECK(Run(store, fixture, two).outcome == ShaderStoreOutcome::Absorbed);
        CHECK(g_compiles == 1);
    });
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture, two).outcome == ShaderStoreOutcome::Hit);
        CHECK(Run(store, fixture, one).outcome == ShaderStoreOutcome::Hit);
    });
}

void TestDefinesThatChangeTextDoNotShare() {
    Fixture fixture;
    WriteFile(fixture.shaders() / "Root.hlsl", "USE_DEFINES\n");
    const Variant one{ "Root.hlsl", "ONE" }, two{ "Root.hlsl", "TWO" };
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Text(Run(store, fixture, one)).find("ONE") != std::string::npos);
        const auto result = Run(store, fixture, two);
        CHECK(result.outcome == ShaderStoreOutcome::Compiled);
        CHECK(Text(result).find("TWO") != std::string::npos);
    });
}

void TestCorruptArtifactCompilesAgain() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    fs::path artifact;
    Boot(fixture, [&](ShaderStore& store) {
        const auto hex = Run(store, fixture).compileKey.ToHex();
        artifact = fixture.cache() / "cas" / hex.substr(0, 2) / (hex + ".bin");
    });
    CHECK(fs::exists(artifact));
    // Flip a byte, keeping the length. A length check alone would accept this.
    auto bytes = ReadFile(artifact);
    const auto intact = bytes;
    bytes.back() ^= 0x55;
    WriteFile(artifact, bytes);
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.miss == ShaderStoreMiss::ArtifactInvalid);
        CHECK(result.outcome == ShaderStoreOutcome::Compiled);
        CHECK(!result.binary.empty());
        CHECK(g_compiles == 1);
    });
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Hit); });
    CHECK(ReadFile(artifact) == intact);
    // A truncated file and a deleted file behave the same way.
    WriteFile(artifact, intact.substr(0, 10));
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Compiled); });
    fs::remove(artifact);
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.outcome == ShaderStoreOutcome::Compiled);
        CHECK(!result.binary.empty());
    });
}

void TestCorruptIndexSlotFallsBack() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    Boot(fixture, [&](ShaderStore& store) { Run(store, fixture); });
    Boot(fixture, [&](ShaderStore& store) { Run(store, fixture); });
    // Both slots now hold Root's record. Damage one, then the other: either alone is enough.
    for (const auto* slot : { "index.a", "index.b" }) {
        const auto path = fixture.cache() / slot;
        CHECK(fs::exists(path));
        const auto intact = ReadFile(path);
        auto bytes = intact;
        bytes[bytes.size() / 2] ^= 0x55;
        WriteFile(path, bytes);
        {
            ResetCounts();
            ShaderStore store(fixture.Options());
            CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Hit);
            CHECK(g_preprocesses == 0);
        }
        // Build both slots again for the next round.
        std::error_code ec;
        fs::remove(fixture.cache() / "index.a", ec);
        fs::remove(fixture.cache() / "index.b", ec);
        Boot(fixture, [&](ShaderStore& store) { Run(store, fixture); });
        Boot(fixture, [&](ShaderStore& store) { Run(store, fixture); });
    }
    // Garbage in both slots: no crash, and still no compile.
    WriteFile(fixture.cache() / "index.a", "garbage");
    WriteFile(fixture.cache() / "index.b", std::string(100000, 'x'));
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Absorbed);
        CHECK(g_compiles == 0);
    });
}

void TestRememberedCompileFailures() {
    Fixture fixture;
    WriteFile(fixture.shaders() / "Root.hlsl", "#include \"Common.hlsli\"\ncompile_error\n");
    WriteFile(fixture.shaders() / "Common.hlsli", "common\n");
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.outcome == ShaderStoreOutcome::Failed);
        CHECK(result.diagnostics == "compile error");
        CHECK(g_compiles == 1);
    });
    // The same inputs fail the same way. Nothing runs and the message is kept.
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.outcome == ShaderStoreOutcome::Failed);
        CHECK(result.failureFromCache);
        CHECK(result.diagnostics == "compile error");
        CHECK(g_compiles == 0);
        CHECK(g_preprocesses == 0);
    });
    // Fixing a file clears it.
    WriteFile(fixture.shaders() / "Root.hlsl", "#include \"Common.hlsli\"\nfine\n");
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Compiled); });
}

void TestRememberedPreprocessFailures() {
    Fixture fixture;
    WriteFile(fixture.shaders() / "Root.hlsl", "#include \"Missing.hlsli\"\n");
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Failed);
        CHECK(g_preprocesses == 1);
    });
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture).failureFromCache);
        CHECK(g_preprocesses == 0);
    });
    // Adding the file that was missing retries, because its absence was recorded.
    WriteFile(fixture.shaders() / "Missing.hlsli", "now here\n");
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Compiled); });
    // An #error is remembered too.
    WriteFile(fixture.shaders() / "Root.hlsl", "#error nope\n");
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Failed); });
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture).failureFromCache); });
}

void TestTransientFailureIsNotRemembered() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture, {}, {}, nullptr, true).outcome == ShaderStoreOutcome::Failed); });
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Compiled); });
}

void TestDebugBuildsDoNotAbsorbOrShare() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    const Variant debug{ "Root.hlsl", "", "", true };
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture, debug).outcome == ShaderStoreOutcome::Compiled); });
    // A comment edit changes the embedded source, so a debug build must rebuild.
    WriteFile(fixture.shaders() / "Common.hlsli", "common code // comment\n");
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture, debug).outcome == ShaderStoreOutcome::Compiled);
        CHECK(g_compiles == 1);
    });
    // Reverting finds the old artifact.
    WriteFile(fixture.shaders() / "Common.hlsli", "common code\n");
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture, debug).outcome == ShaderStoreOutcome::Absorbed);
        CHECK(g_compiles == 0);
    });
    // Two defines with the same text share nothing in debug.
    const Variant debugTwo{ "Root.hlsl", "UNUSED", "", true };
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture, debugTwo).outcome == ShaderStoreOutcome::Compiled); });
}

void TestGeneratedRootIsTrackedByItsBytes() {
    Fixture fixture;
    WriteFile(fixture.shaders() / "Common.hlsli", "common\n");
    const std::string plain = "#include \"Common.hlsli\"\nrewrite one\n";
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture, {}, {}, &plain).outcome == ShaderStoreOutcome::Compiled); });
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture, {}, {}, &plain).outcome == ShaderStoreOutcome::Hit);
        CHECK(g_preprocesses == 0);
    });
    const std::string other = "#include \"Common.hlsli\"\nrewrite two\n";
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture, {}, {}, &other);
        CHECK(result.miss == ShaderStoreMiss::RootChanged);
        CHECK(result.outcome == ShaderStoreOutcome::Compiled);
        CHECK(Text(result).find("rewrite two") != std::string::npos);
    });
}

void TestMovingTheFolderInvalidatesNothing() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    Boot(fixture, [&](ShaderStore& store) { Run(store, fixture); });
    Fixture moved;
    fs::remove_all(moved.folder);
    fs::rename(fixture.folder, moved.folder);
    Boot(moved, [&](ShaderStore& store) {
        CHECK(Run(store, moved).outcome == ShaderStoreOutcome::Hit);
        CHECK(g_preprocesses == 0);
    });
}

void TestHotReloadSeesEditsInTheNextGeneration() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Compiled);
        CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Hit);
        // The snapshot holds this generation's view, so an edit waits for the next generation.
        WriteFile(fixture.shaders() / "Common.hlsli", "edited\n");
        CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Hit);
        store.BeginGeneration();
        const auto result = Run(store, fixture);
        CHECK(result.outcome == ShaderStoreOutcome::Compiled);
        CHECK(Text(result).find("edited") != std::string::npos);
    });
}

void TestForEachDependencyWorksAfterHits() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    Boot(fixture, [&](ShaderStore& store) { Run(store, fixture); });
    // After a boot served entirely by hits, hot reload must still know the dependencies.
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Hit);
        std::vector<fs::path> files;
        CHECK(store.ForEachDependency(VariantKeyOf({}), [&](const fs::path& path) { files.push_back(path); }));
        CHECK(files.size() == 1);
        CHECK(fs::exists(files[0]));
        CHECK(!store.ForEachDependency(ShaderStoreHash{ 1, 2 }, [](const fs::path&) {}));
    });
}

void TestDependencySetsAreShared() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    Boot(fixture, [&](ShaderStore& store) {
        for (int i = 0; i < 20; ++i) Run(store, fixture, Variant{ "Root.hlsl", "D" + std::to_string(i) });
        const auto info = store.IndexInfo();
        CHECK(info.variants == 20);
        CHECK(info.dependencySets == 1);
        CHECK(info.files == 1);
        // All twenty read each file only once.
        CHECK(store.Stats().sourceFilesRead == 2);
    });
    // A boot of hits reads each source once and preprocesses nothing.
    Boot(fixture, [&](ShaderStore& store) {
        for (int i = 0; i < 20; ++i) CHECK(Run(store, fixture, Variant{ "Root.hlsl", "D" + std::to_string(i) }).outcome == ShaderStoreOutcome::Hit);
        CHECK(store.Stats().sourceFilesRead == 2);
        CHECK(g_preprocesses == 0);
    });
}

void TestEvictionKeepsWhatIsInUse() {
    Fixture fixture;
    const auto write = [&](const std::string& name) { WriteFile(fixture.shaders() / (name + ".hlsl"), name + std::string(4000, 'x') + "\n"); };
    write("Old");
    write("Live");
    auto options = fixture.Options();
    options.maxBytes = 1;  // Everything is over the cap.
    options.keepGenerations = 2;
    const Variant old{ "Old.hlsl" }, live{ "Live.hlsl" };
    Boot(options, [&](ShaderStore& store) { Run(store, fixture, old); Run(store, fixture, live); });
    // Live is used in every start after this, so it must never rebuild, even over the cap.
    for (int start = 0; start < 5; ++start) {
        Boot(options, [&](ShaderStore& store) {
            CHECK(Run(store, fixture, live).outcome == ShaderStoreOutcome::Hit);
            CHECK(g_compiles == 0);
        });
    }
    // Old went unused for longer than keepGenerations, so it is gone.
    Boot(options, [&](ShaderStore& store) {
        CHECK(Run(store, fixture, old).outcome == ShaderStoreOutcome::Compiled);
        CHECK(Run(store, fixture, live).outcome == ShaderStoreOutcome::Hit);
    });
}

void TestNotPersistentTouchesNoDisk() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    auto options = fixture.Options();
    options.persistent = false;
    Boot(options, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.outcome == ShaderStoreOutcome::Compiled);
        CHECK(result.miss == ShaderStoreMiss::Disabled);
        CHECK(!result.binary.empty());
    });
    CHECK(!fs::exists(fixture.cache()));
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Compiled); });
    // A session with the setting off deletes nothing.
    Boot(options, [&](ShaderStore& store) { Run(store, fixture); });
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Hit); });
}

void TestSecondInstanceDoesNotSaveTheIndex() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    ShaderStore owner(fixture.Options());
    CHECK(owner.OwnsIndex());
    Run(owner, fixture);
    owner.Flush();
#if defined(_WIN32)
    const auto before = ReadFile(fixture.cache() / "index.a");
    {
        ShaderStore second(fixture.Options());
        CHECK(!second.OwnsIndex());
        // It can still use the shared artifacts.
        CHECK(Run(second, fixture).outcome == ShaderStoreOutcome::Hit);
        second.Flush();
    }
    CHECK(ReadFile(fixture.cache() / "index.a") == before);
    CHECK(!fs::exists(fixture.cache() / "index.b"));
#endif
}

void TestClearKeepsOrDropsArtifacts() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    Boot(fixture, [&](ShaderStore& store) { Run(store, fixture); store.ClearIndex(); });
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Absorbed);
        store.Clear();
    });
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Compiled); });
}

void TestConcurrentRequestsCompileOnce() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    ResetCounts();
    ShaderStore store(fixture.Options());
    std::vector<std::thread> threads;
    std::atomic<int> empty{};
    // Eight variants, all with the same text, across many threads.
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 25; ++i)
                if (Run(store, fixture, Variant{ "Root.hlsl", "D" + std::to_string((t + i) % 8) }).binary.empty()) ++empty;
        });
    }
    for (auto& thread : threads) thread.join();
    CHECK(empty == 0);
    CHECK(g_compiles == 1);
    CHECK(store.Stats().requests == 200);
    CHECK(store.IndexInfo().variants == 8);
}

void TestSaverRunsInTheBackground() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    auto options = fixture.Options();
    options.saveInterval = std::chrono::milliseconds(10);
    ShaderStore store(options);
    Run(store, fixture);
    // A process killed now keeps its work, because the index was saved without Flush or the destructor.
    for (int i = 0; i < 500 && store.IndexInfo().savedBytes == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(store.IndexInfo().savedBytes > 0);
}

void TestRememberedDiagnosticsAreCapped() {
    Fixture fixture;
    // A long message, as a broken header gives. The first run reports it all; what is remembered is cut short.
    std::string source;
    for (int i = 0; i < 2000; ++i) source += "#include \"Missing" + std::to_string(i) + ".hlsli\"\n";
    WriteFile(fixture.shaders() / "Root.hlsl", "#error " + std::string(100000, 'x') + "\n");
    size_t firstSize = 0;
    Boot(fixture, [&](ShaderStore& store) { firstSize = Run(store, fixture).diagnostics.size(); });
    CHECK(firstSize > 100000);
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.failureFromCache);
        CHECK(!result.diagnostics.empty());
        CHECK(result.diagnostics.size() <= 4096);
    });
    CHECK(fs::file_size(fixture.cache() / "index.a") < 20000);
}

void TestStartupReportsWhatItFound() {
    Fixture fixture;
    WriteFile(fixture.shaders() / "A.hlsl", "a" + std::string(4000, 'x') + "\n");
    WriteFile(fixture.shaders() / "B.hlsl", "b" + std::string(4000, 'x') + "\n");
    const Variant a{ "A.hlsl" }, b{ "B.hlsl" };
    Boot(fixture, [&](ShaderStore& store) {
        CHECK(store.MeasureArtifacts().count == 0);
        Run(store, fixture, a);
        Run(store, fixture, b);
        CHECK(store.MeasureArtifacts().count == 2);
        CHECK(store.MeasureArtifacts().bytes > 8000);
    });
    Boot(fixture, [&](ShaderStore& store) {
        const auto info = store.IndexInfo();
        CHECK(info.artifactsAtStart == 2);
        CHECK(info.artifactBytesAtStart > 8000);
        CHECK(info.artifactsEvicted == 0);
        CHECK(info.overCapBytes == 0);
    });
    // Over the cap, but used by the last start: kept, and reported so the host can warn.
    auto options = fixture.Options();
    options.maxBytes = 1;
    Boot(options, [&](ShaderStore& store) {
        const auto info = store.IndexInfo();
        CHECK(info.artifactsEvicted == 0);
        CHECK(info.overCapBytes > 8000);
        CHECK(Run(store, fixture, a).outcome == ShaderStoreOutcome::Hit);
    });
}

void TestEachFileIsReadOncePerGeneration() {
    Fixture fixture;
    WriteFile(fixture.shaders() / "Root.hlsl", "#include \"A.hlsli\"\n#include \"B.hlsli\"\n");
    WriteFile(fixture.shaders() / "A.hlsli", "a\n");
    WriteFile(fixture.shaders() / "B.hlsli", "b\n");
    constexpr uint64_t kFiles = 3;
    const auto hammer = [&](ShaderStore& store) {
        std::vector<std::thread> threads;
        std::atomic<bool> go{};
        for (int t = 0; t < 16; ++t) {
            threads.emplace_back([&, t] {
                while (!go) std::this_thread::yield();
                for (int i = 0; i < 10; ++i) Run(store, fixture, Variant{ "Root.hlsl", "D" + std::to_string((t + i) % 16) });
            });
        }
        go = true;
        for (auto& thread : threads) thread.join();
    };
    // Threads race for the same files on a cold boot and on a boot of hits. Neither may read a file twice.
    Boot(fixture, [&](ShaderStore& store) { hammer(store); CHECK(store.Stats().sourceFilesRead == kFiles); });
    Boot(fixture, [&](ShaderStore& store) {
        hammer(store);
        CHECK(store.Stats().sourceFilesRead == kFiles);
        CHECK(store.Stats().hits == 160);
        // A new generation reads each file once more.
        store.BeginGeneration();
        hammer(store);
        CHECK(store.Stats().sourceFilesRead == kFiles * 2);
    });
}

void TestRemoveArtifactRebuildsOnce() {
    Fixture fixture;
    WriteBasicShaders(fixture);
    Boot(fixture, [&](ShaderStore& store) {
        const auto first = Run(store, fixture);
        CHECK(first.outcome == ShaderStoreOutcome::Compiled);
        // A host found the bytes unusable, so it asks for them to be dropped.
        store.RemoveArtifact(first.compileKey);
        const auto second = Run(store, fixture);
        CHECK(second.miss == ShaderStoreMiss::ArtifactInvalid);
        CHECK(second.outcome == ShaderStoreOutcome::Compiled);
        CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Hit);
        CHECK(g_compiles == 2);
    });
}

void TestGenerationMoves() {
    Fixture fixture;
    ShaderStore store(fixture.Options());
    const auto before = store.Generation();
    store.BeginGeneration();
    CHECK(store.Generation() != before);
}

void TestSummaryNamesTheCause() {
    // The summary is what a user's log shows, so it must say what happened and why.
    Fixture fixture;
    WriteBasicShaders(fixture);
    Boot(fixture, [&](ShaderStore& store) { Run(store, fixture); });
    WriteFile(fixture.shaders() / "Common.hlsli", "changed\n");
    Boot(fixture, [&](ShaderStore& store) {
        Run(store, fixture);
        const auto summary = store.Summary();
        CHECK(summary.find("1 compiled") != std::string::npos);
        CHECK(summary.find("1 dependency changed") != std::string::npos);
        CHECK(summary.find("shaders/common.hlsli") != std::string::npos);
    });
}

void TestPathsOutsideTheRootStillWork() {
    Fixture fixture;
    WriteFile(fixture.shaders() / "Root.hlsl", "#include \"Outside.hlsli\"\n");
    const auto outside = fixture.folder / "Elsewhere";
    WriteFile(outside / "Outside.hlsli", "outside one\n");
    const std::vector<fs::path> search{ outside };
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture, {}, search).outcome == ShaderStoreOutcome::Compiled); });
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture, {}, search).outcome == ShaderStoreOutcome::Hit); });
    WriteFile(outside / "Outside.hlsli", "outside two\n");
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture, {}, search).outcome == ShaderStoreOutcome::Compiled); });
}

void TestNonAsciiPaths() {
    Fixture fixture;
    const fs::path name = fs::path(u8"R\u00f6\u00f6t\u65e5\u672c.hlsl");
    WriteFile(fixture.shaders() / name, "#include \"Common.hlsli\"\nmain\n");
    WriteFile(fixture.shaders() / "Common.hlsli", "common\n");
    Variant variant;
    variant.rootName = reinterpret_cast<const char*>(name.u8string().c_str());
    FakeBuild build({ fixture.shaders() }, "");
    ShaderVariantRequest request;
    request.compileConfigKey = ConfigKeyOf(variant);
    request.variantKey = VariantKeyOf(variant);
    request.format = 1;
    request.rootPath = fixture.shaders() / name;
    request.build = &build;
    Boot(fixture, [&](ShaderStore& store) { CHECK(store.Resolve(request).outcome == ShaderStoreOutcome::Compiled); });
    Boot(fixture, [&](ShaderStore& store) { CHECK(store.Resolve(request).outcome == ShaderStoreOutcome::Hit); });
    // A non-ASCII include is recorded, read back and invalidated like any other.
    WriteFile(fixture.shaders() / name, "#include \"Common.hlsli\"\nedited\n");
    Boot(fixture, [&](ShaderStore& store) { CHECK(store.Resolve(request).outcome == ShaderStoreOutcome::Compiled); });
}

void TestEmptyIncludeIsNotMissing() {
    Fixture fixture;
    WriteFile(fixture.shaders() / "Root.hlsl", "#include \"Empty.hlsli\"\nmain\n");
    WriteFile(fixture.shaders() / "Empty.hlsli", "");
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Compiled); });
    Boot(fixture, [&](ShaderStore& store) { CHECK(Run(store, fixture).outcome == ShaderStoreOutcome::Hit); });
    fs::remove(fixture.shaders() / "Empty.hlsli");
    Boot(fixture, [&](ShaderStore& store) {
        const auto result = Run(store, fixture);
        CHECK(result.miss == ShaderStoreMiss::DependencyChanged);
        CHECK(result.outcome == ShaderStoreOutcome::Failed);
    });
}

void TestKeysSeparateFields() {
    ShaderKeyBuilder one, two, three;
    one.Add(std::string_view("A")).Add(std::string_view("BC"));
    two.Add(std::string_view("AB")).Add(std::string_view("C"));
    three.Add(std::string_view("A")).Add(std::string_view("BC"));
    CHECK(!(one.Finish() == two.Finish()));
    CHECK(one.Finish() == three.Finish());
    CHECK(one.Finish().ToHex().size() == 32);
}

void TestCanonicalText() {
    const std::string a = "#line 1 \"a.hlsl\"\nfloat x;\n\n   \n# 12 \"b.hlsl\"\nfloat y;\n";
    const std::string b = "float x;\n#line 99 \"c.hlsl\"\nfloat y;\n";
    CHECK(CanonicalizePreprocessedText(a) == CanonicalizePreprocessedText(b));
    CHECK(CanonicalizePreprocessedText(a) == "float x;\nfloat y;\n");
    // Other directives are real input and must stay.
    CHECK(CanonicalizePreprocessedText("#pragma warning(disable: 1)\nfloat x;\n") == "#pragma warning(disable: 1)\nfloat x;\n");
}

}  // namespace

int main() {
    TestKeysSeparateFields();
    TestCanonicalText();
    TestSecondBootDoesNoWork();
    TestTouchedAndFutureTimesDoNoWork();
    TestLosingTheIndexNeverCompiles();
    TestCommentEditIsAbsorbedAndRevertCompilesNothing();
    TestChangesThatTimestampsMissAreDetected();
    TestOnlyDependentVariantsRebuild();
    TestFilesNeverOpenedDoNotCount();
    TestNewFileShadowingAnIncludeIsDetected();
    TestFlagsSeparateVariants();
    TestVariantsWithTheSameTextShareOneArtifact();
    TestDefinesThatChangeTextDoNotShare();
    TestCorruptArtifactCompilesAgain();
    TestCorruptIndexSlotFallsBack();
    TestRememberedCompileFailures();
    TestRememberedPreprocessFailures();
    TestTransientFailureIsNotRemembered();
    TestDebugBuildsDoNotAbsorbOrShare();
    TestGeneratedRootIsTrackedByItsBytes();
    TestMovingTheFolderInvalidatesNothing();
    TestHotReloadSeesEditsInTheNextGeneration();
    TestForEachDependencyWorksAfterHits();
    TestDependencySetsAreShared();
    TestEvictionKeepsWhatIsInUse();
    TestNotPersistentTouchesNoDisk();
    TestSecondInstanceDoesNotSaveTheIndex();
    TestClearKeepsOrDropsArtifacts();
    TestConcurrentRequestsCompileOnce();
    TestSaverRunsInTheBackground();
    TestRememberedDiagnosticsAreCapped();
    TestStartupReportsWhatItFound();
    TestEachFileIsReadOncePerGeneration();
    TestRemoveArtifactRebuildsOnce();
    TestGenerationMoves();
    TestSummaryNamesTheCause();
    TestPathsOutsideTheRootStillWork();
    TestNonAsciiPaths();
    TestEmptyIncludeIsNotMissing();
    std::puts("ShaderStore tests passed");
    return 0;
}
