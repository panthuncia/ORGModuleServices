#include <ORGModuleServices/ShaderStore.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace org::services {

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kIndexMagic = 0x58495353;     // "SSIX"
constexpr uint32_t kArtifactMagic = 0x41435353;  // "SSCA"
// Bump when the meaning of a key or of either file format changes.
constexpr uint32_t kSchema = 1;
constexpr const char* kIndexSlots[2] = { "index.a", "index.b" };
// A remembered failure keeps the start of its message. One broken header can fail thousands of variants.
constexpr size_t kMaxRememberedDiagnostics = 4096;

struct HashOf {
    size_t operator()(const ShaderStoreHash& hash) const noexcept { return static_cast<size_t>(hash.low ^ (hash.high * 0x9E3779B97F4A7C15ull)); }
};

ShaderStoreHash ToHash(XXH128_hash_t hash) noexcept { return { hash.high64, hash.low64 }; }

#pragma pack(push, 1)
struct IndexHeader {
    uint32_t magic{ kIndexMagic };
    uint32_t schema{ kSchema };
    uint64_t saveCount{};
    uint64_t payloadSize{};
    ShaderStoreHash payloadHash;
};
struct ArtifactHeader {
    uint32_t magic{ kArtifactMagic };
    uint16_t schema{ static_cast<uint16_t>(kSchema) };
    uint8_t format{};
    uint8_t reserved{};
    ShaderStoreHash key;
    uint64_t size{};
    ShaderStoreHash payloadHash;
};
#pragma pack(pop)

// Writes the index into a byte buffer.
class Writer {
public:
    template <typename T> void Value(const T& value) {
        const auto* first = reinterpret_cast<const char*>(&value);
        bytes.insert(bytes.end(), first, first + sizeof(T));
    }
    void Text(std::string_view text) {
        Value(static_cast<uint32_t>(text.size()));
        bytes.insert(bytes.end(), text.begin(), text.end());
    }
    std::string bytes;
};

// Reads the index from a byte buffer. Any read past the end marks it as failed.
class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}
    template <typename T> T Value() {
        T value{};
        if (data_.size() - position_ < sizeof(T)) { failed = true; return value; }
        std::memcpy(&value, data_.data() + position_, sizeof(T));
        position_ += sizeof(T);
        return value;
    }
    std::string Text() {
        const auto size = Value<uint32_t>();
        if (failed || data_.size() - position_ < size) { failed = true; return {}; }
        std::string text(data_.substr(position_, size));
        position_ += size;
        return text;
    }
    // A count of items that take at least minimumItemSize bytes. Stops a damaged count from causing a huge allocation.
    uint32_t Count(size_t minimumItemSize) {
        const auto count = Value<uint32_t>();
        if (failed || count > (data_.size() - position_) / minimumItemSize) { failed = true; return 0; }
        return count;
    }
    bool failed{};

private:
    std::string_view data_;
    size_t position_{};
};

bool ReadWholeFile(const fs::path& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return false;
    const auto size = stream.tellg();
    if (size < 0) return false;
    out.resize(static_cast<size_t>(size));
    stream.seekg(0);
    return out.empty() || static_cast<bool>(stream.read(out.data(), static_cast<std::streamsize>(out.size())));
}

// Case differs between include lines on Windows, so paths are compared in lower case there.
std::string LogicalText(const fs::path& path) {
    // UTF-8 explicitly: narrow path conversions throw on names outside the ANSI code page.
    const auto utf8 = path.lexically_normal().generic_u8string();
    std::string text(reinterpret_cast<const char*>(utf8.data()), utf8.size());
#if defined(_WIN32)
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(c < 0x80 ? std::tolower(c) : c); });
#endif
    return text;
}

// Holds each source file's contents for one generation. A file is read once, on first use.
class Snapshot {
public:
    explicit Snapshot(fs::path root) {
        std::error_code ec;
        root_ = fs::absolute(root, ec);
        if (ec) root_ = std::move(root);
        root_ = root_.lexically_normal();
    }

    std::string LogicalPath(const fs::path& path) const {
        std::error_code ec;
        const auto absolute = path.is_absolute() ? path : fs::absolute(path, ec);
        const auto relative = (ec ? path : absolute).lexically_normal().lexically_relative(root_);
        // A file outside the root keeps its absolute path.
        if (relative.empty() || *relative.begin() == "..") return LogicalText(ec ? path : absolute);
        return LogicalText(relative);
    }

    fs::path PhysicalPath(std::string_view logical) const {
        const fs::path path(std::u8string_view(reinterpret_cast<const char8_t*>(logical.data()), logical.size()));
        return path.is_absolute() ? path : root_ / path;
    }

    std::shared_ptr<const ShaderSourceFile> Open(const std::string& logical) {
        std::shared_ptr<Slot> slot;
        {
            std::shared_lock lock(mutex_);
            if (const auto found = files_.find(logical); found != files_.end()) slot = found->second;
        }
        if (!slot) {
            std::unique_lock lock(mutex_);
            slot = files_.try_emplace(logical, std::make_shared<Slot>()).first->second;
        }
        // Many threads ask for the same files at once. One reads, outside the map's lock so a slow disk
        // stalls only those waiting for this file, and the others wait for its result.
        std::call_once(slot->once, [&] {
            auto file = std::make_shared<ShaderSourceFile>();
            file->logicalPath = logical;
            auto bytes = std::make_shared<std::string>();
            file->exists = ReadWholeFile(PhysicalPath(logical), *bytes);
            if (file->exists) {
                file->hash = HashShaderText(*bytes);
                file->bytes = std::move(bytes);
            }
            slot->file = std::move(file);
            reads_.fetch_add(1, std::memory_order_relaxed);
        });
        return slot->file;
    }

    void BeginGeneration() {
        std::unique_lock lock(mutex_);
        files_.clear();
        ++generation_;
    }

    uint64_t Generation() const {
        std::shared_lock lock(mutex_);
        return generation_;
    }

    uint64_t Reads() const noexcept { return reads_.load(std::memory_order_relaxed); }

private:
    struct Slot {
        std::once_flag once;
        std::shared_ptr<const ShaderSourceFile> file;
    };
    fs::path root_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Slot>> files_;
    uint64_t generation_{ 1 };
    std::atomic<uint64_t> reads_{};
};

// One path at one version. Dependency sets refer to these.
struct FileRecord {
    uint32_t pathId{};
    bool exists{};
    ShaderStoreHash hash;
};

struct DependencySet {
    std::vector<uint32_t> files;  // Sorted ids into files_.
    // The generation this was last checked in, and the result. Holds (generation << 1) | clean.
    mutable std::atomic<uint64_t> checked{};
};

enum class VariantStatus : uint8_t { Ok, Failed };

struct VariantRecord {
    ShaderStoreHash rootHash;
    ShaderStoreHash compileKey;
    uint32_t dependencySet{};
    VariantStatus status{ VariantStatus::Ok };
    uint8_t format{};
    std::string diagnostics;
    mutable std::atomic<uint32_t> lastUsedStart{};
};

class Tracker final : public ShaderIncludeTracker {
public:
    explicit Tracker(Snapshot& snapshot) : snapshot_(snapshot) {}

    const std::string* Open(const fs::path& path) override {
        auto file = snapshot_.Open(snapshot_.LogicalPath(path));
        const auto* bytes = file->bytes.get();
        // Snapshot files live in shared pointers, so a hot reload cannot free them during this build.
        seen_.try_emplace(file->logicalPath, std::move(file));
        return bytes;
    }

    std::vector<std::shared_ptr<const ShaderSourceFile>> Seen() const {
        std::vector<std::shared_ptr<const ShaderSourceFile>> files;
        files.reserve(seen_.size());
        for (const auto& [path, file] : seen_) files.push_back(file);
        return files;
    }

private:
    Snapshot& snapshot_;
    std::unordered_map<std::string, std::shared_ptr<const ShaderSourceFile>> seen_;
};

}  // namespace

std::string ShaderStoreHash::ToHex() const {
    char text[33]{};
    std::snprintf(text, sizeof(text), "%016llx%016llx", static_cast<unsigned long long>(high), static_cast<unsigned long long>(low));
    return text;
}

ShaderStoreHash HashShaderBytes(std::span<const std::byte> bytes) noexcept { return ToHash(XXH3_128bits(bytes.data(), bytes.size())); }
ShaderStoreHash HashShaderText(std::string_view text) noexcept { return ToHash(XXH3_128bits(text.data(), text.size())); }

struct ShaderKeyBuilder::State {
    XXH3_state_t hash;
};

ShaderKeyBuilder::ShaderKeyBuilder() : state_(std::make_unique<State>()) { XXH3_128bits_reset(&state_->hash); }
ShaderKeyBuilder::~ShaderKeyBuilder() = default;

ShaderKeyBuilder& ShaderKeyBuilder::Add(uint64_t value) noexcept {
    XXH3_128bits_update(&state_->hash, &value, sizeof(value));
    return *this;
}
ShaderKeyBuilder& ShaderKeyBuilder::Add(std::string_view text) noexcept {
    Add(static_cast<uint64_t>(text.size()));
    XXH3_128bits_update(&state_->hash, text.data(), text.size());
    return *this;
}
ShaderKeyBuilder& ShaderKeyBuilder::Add(std::wstring_view text) noexcept {
    Add(static_cast<uint64_t>(text.size()));
    XXH3_128bits_update(&state_->hash, text.data(), text.size() * sizeof(wchar_t));
    return *this;
}
ShaderKeyBuilder& ShaderKeyBuilder::Add(const ShaderStoreHash& value) noexcept {
    Add(value.high);
    return Add(value.low);
}
ShaderStoreHash ShaderKeyBuilder::Finish() const noexcept { return ToHash(XXH3_128bits_digest(&state_->hash)); }

std::string CanonicalizePreprocessedText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t position = 0;
    while (position < text.size()) {
        const size_t end = text.find('\n', position);
        const size_t next = end == std::string_view::npos ? text.size() : end + 1;
        const auto line = text.substr(position, next - position);
        position = next;
        const size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) continue;
        const auto body = line.substr(first);
        if (body.starts_with("#line")) continue;
        // Clang-style preprocessors, DXC among them, write markers as: # 12 "file"
        if (body[0] == '#') {
            const size_t digit = body.find_first_not_of(" \t", 1);
            if (digit != std::string_view::npos && body[digit] >= '0' && body[digit] <= '9') continue;
        }
        out.append(line);
    }
    return out;
}

struct ShaderStore::Impl {
    explicit Impl(ShaderStoreOptions options) : options(std::move(options)), snapshot(this->options.sourceRoot) {
        if (this->options.persistent) {
            std::error_code ec;
            fs::create_directories(this->options.cacheDirectory, ec);
            ClaimIndex();
            LoadIndex();
            ++startCount;
            Evict();
            dirty = true;
            if (this->options.saveInterval.count() > 0 && ownsIndex) saver = std::thread([this] { SaveLoop(); });
        }
    }

    ~Impl() {
        if (saver.joinable()) {
            { std::scoped_lock lock(saverMutex); stopping = true; }
            saverWake.notify_all();
            saver.join();
        }
        Save();
#if defined(_WIN32)
        if (indexMutex) CloseHandle(indexMutex);
#endif
    }

    // --- Index ownership ---------------------------------------------------------------------------------------

    void ClaimIndex() {
#if defined(_WIN32)
        // The name comes from the folder, so two game instances sharing a cache find each other. Windows
        // releases the mutex if a process dies, so a crash cannot leave the cache locked.
        std::error_code ec;
        const auto folder = LogicalText(fs::absolute(options.cacheDirectory, ec));
        const auto hex = HashShaderText(folder).ToHex();
        const std::wstring name = L"ORGShaderStore-" + std::wstring(hex.begin(), hex.end());
        indexMutex = CreateMutexW(nullptr, TRUE, name.c_str());
        ownsIndex = indexMutex && GetLastError() != ERROR_ALREADY_EXISTS;
#else
        ownsIndex = true;
#endif
    }

    // --- Index tables ------------------------------------------------------------------------------------------

    uint32_t InternPath(const std::string& path) {
        const auto [found, added] = pathIds.try_emplace(path, static_cast<uint32_t>(paths.size()));
        if (added) paths.push_back(path);
        return found->second;
    }

    uint32_t InternFile(uint32_t pathId, bool exists, const ShaderStoreHash& hash) {
        ShaderKeyBuilder key;
        key.Add(static_cast<uint64_t>(pathId)).Add(static_cast<uint64_t>(exists)).Add(hash);
        const auto [found, added] = fileIds.try_emplace(key.Finish(), static_cast<uint32_t>(files.size()));
        if (added) files.push_back({ pathId, exists, hash });
        return found->second;
    }

    uint32_t InternSet(std::vector<uint32_t> ids) {
        std::sort(ids.begin(), ids.end());
        const auto key = HashShaderBytes(std::as_bytes(std::span(ids)));
        const auto [found, added] = setIds.try_emplace(key, static_cast<uint32_t>(sets.size()));
        if (added) {
            auto& set = sets.emplace_back();
            set.files = std::move(ids);
        }
        return found->second;
    }

    void Record(const ShaderStoreHash& variantKey, const ShaderStoreHash& rootHash, const std::vector<std::shared_ptr<const ShaderSourceFile>>& seen,
        const ShaderStoreHash& compileKey, VariantStatus status, uint8_t format, std::string_view diagnostics) {
        std::unique_lock lock(tableMutex);
        std::vector<uint32_t> ids;
        ids.reserve(seen.size());
        for (const auto& file : seen) ids.push_back(InternFile(InternPath(file->logicalPath), file->exists, file->hash));
        const auto setId = InternSet(std::move(ids));
        auto& variant = variants[variantKey];
        variant.rootHash = rootHash;
        variant.compileKey = compileKey;
        variant.dependencySet = setId;
        variant.status = status;
        variant.format = format;
        variant.diagnostics = diagnostics.substr(0, kMaxRememberedDiagnostics);
        variant.lastUsedStart.store(startCount, std::memory_order_relaxed);
        dirty = true;
    }

    // True if every file in the set still matches. The result is remembered until the next generation, so each
    // set does its comparisons once and every variant using it costs one atomic load. Holds tableMutex shared.
    bool SetIsClean(const DependencySet& set) {
        const uint64_t generation = snapshot.Generation();
        const uint64_t checked = set.checked.load(std::memory_order_acquire);
        if ((checked >> 1) == generation) return checked & 1;
        bool clean = true;
        for (const auto id : set.files) {
            if (!FileIsCurrent(files[id])) { clean = false; break; }
        }
        set.checked.store((generation << 1) | (clean ? 1 : 0), std::memory_order_release);
        return clean;
    }

    bool FileIsCurrent(const FileRecord& record) {
        const auto file = snapshot.Open(paths[record.pathId]);
        return file->exists == record.exists && (!file->exists || file->hash == record.hash);
    }

    std::string FirstChangedFile(const DependencySet& set) {
        for (const auto id : set.files)
            if (!FileIsCurrent(files[id])) return paths[files[id].pathId];
        return {};
    }

    // --- Index files -------------------------------------------------------------------------------------------

    // Only called while tableMutex is held. Damaged or foreign input leaves the tables empty.
    bool ParseIndex(std::string_view payload) {
        Reader reader(payload);
        startCount = reader.Value<uint32_t>();
        const auto pathCount = reader.Count(sizeof(uint32_t));
        for (uint32_t i = 0; i < pathCount && !reader.failed; ++i) InternPath(reader.Text());
        const auto fileCount = reader.Count(sizeof(uint32_t) + 1 + sizeof(ShaderStoreHash));
        for (uint32_t i = 0; i < fileCount && !reader.failed; ++i) {
            const auto pathId = reader.Value<uint32_t>();
            const auto exists = reader.Value<uint8_t>() != 0;
            const auto hash = reader.Value<ShaderStoreHash>();
            if (pathId >= paths.size()) reader.failed = true;
            else InternFile(pathId, exists, hash);
        }
        const auto setCount = reader.Count(sizeof(uint32_t));
        for (uint32_t i = 0; i < setCount && !reader.failed; ++i) {
            const auto count = reader.Count(sizeof(uint32_t));
            std::vector<uint32_t> ids(count);
            for (auto& id : ids) {
                id = reader.Value<uint32_t>();
                if (id >= files.size()) reader.failed = true;
            }
            if (!reader.failed) InternSet(std::move(ids));
        }
        const auto variantCount = reader.Count(sizeof(ShaderStoreHash));
        for (uint32_t i = 0; i < variantCount && !reader.failed; ++i) {
            const auto key = reader.Value<ShaderStoreHash>();
            auto& variant = variants[key];
            variant.rootHash = reader.Value<ShaderStoreHash>();
            variant.compileKey = reader.Value<ShaderStoreHash>();
            variant.dependencySet = reader.Value<uint32_t>();
            variant.status = static_cast<VariantStatus>(reader.Value<uint8_t>());
            variant.format = reader.Value<uint8_t>();
            variant.lastUsedStart.store(reader.Value<uint32_t>(), std::memory_order_relaxed);
            variant.diagnostics = reader.Text();
            if (variant.dependencySet >= sets.size()) reader.failed = true;
        }
        return !reader.failed;
    }

    void ResetTables() {
        paths.clear(); pathIds.clear(); files.clear(); fileIds.clear(); sets.clear(); setIds.clear(); variants.clear();
        startCount = 0;
    }

    void LoadIndex() {
        // Try the newest slot first. The other slot is the fallback if the newest is damaged.
        struct Slot { int index; std::string data; uint64_t saveCount; };
        std::vector<Slot> slots;
        for (int slot = 0; slot < 2; ++slot) {
            std::string data;
            if (!ReadWholeFile(options.cacheDirectory / kIndexSlots[slot], data) || data.size() < sizeof(IndexHeader)) continue;
            IndexHeader header{};
            std::memcpy(&header, data.data(), sizeof(header));
            const std::string_view payload(data.data() + sizeof(header), data.size() - sizeof(header));
            if (header.magic != kIndexMagic || header.schema != kSchema || header.payloadSize != payload.size() || !(HashShaderText(payload) == header.payloadHash)) continue;
            slots.push_back({ slot, std::move(data), header.saveCount });
        }
        std::sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) { return a.saveCount > b.saveCount; });
        std::unique_lock lock(tableMutex);
        for (const auto& slot : slots) {
            if (ParseIndex(std::string_view(slot.data).substr(sizeof(IndexHeader)))) {
                nextSlot = 1 - slot.index;
                saveCount = slot.saveCount;
                return;
            }
            ResetTables();
        }
    }

    std::string SerializeIndex() const {
        Writer writer;
        writer.Value(startCount);
        writer.Value(static_cast<uint32_t>(paths.size()));
        for (const auto& path : paths) writer.Text(path);
        writer.Value(static_cast<uint32_t>(files.size()));
        for (const auto& file : files) {
            writer.Value(file.pathId);
            writer.Value(static_cast<uint8_t>(file.exists));
            writer.Value(file.hash);
        }
        writer.Value(static_cast<uint32_t>(sets.size()));
        for (const auto& set : sets) {
            writer.Value(static_cast<uint32_t>(set.files.size()));
            for (const auto id : set.files) writer.Value(id);
        }
        writer.Value(static_cast<uint32_t>(variants.size()));
        for (const auto& [key, variant] : variants) {
            writer.Value(key);
            writer.Value(variant.rootHash);
            writer.Value(variant.compileKey);
            writer.Value(variant.dependencySet);
            writer.Value(static_cast<uint8_t>(variant.status));
            writer.Value(variant.format);
            writer.Value(variant.lastUsedStart.load(std::memory_order_relaxed));
            writer.Text(variant.diagnostics);
        }
        return std::move(writer.bytes);
    }

    // Writes over the older slot. There is no rename. A torn write fails the hash check on load, and the other slot is used.
    void Save() {
        if (!options.persistent || !ownsIndex) return;
        std::scoped_lock saving(saveMutex);
        std::string payload;
        {
            std::shared_lock lock(tableMutex);
            if (!dirty.exchange(false)) return;
            payload = SerializeIndex();
        }
        IndexHeader header{};
        header.saveCount = ++saveCount;
        header.payloadSize = payload.size();
        header.payloadHash = HashShaderText(payload);
        const auto path = options.cacheDirectory / kIndexSlots[nextSlot];
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
        stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        stream.close();
        if (stream) {
            nextSlot = 1 - nextSlot;
            savedBytes.store(sizeof(header) + payload.size(), std::memory_order_relaxed);
        } else {
            dirty = true;  // Try again next time. The other slot is still good.
        }
    }

    void SaveLoop() {
        std::unique_lock lock(saverMutex);
        while (!saverWake.wait_for(lock, options.saveInterval, [this] { return stopping; })) {
            lock.unlock();
            Save();
            lock.lock();
        }
    }

    // --- Artifacts ---------------------------------------------------------------------------------------------

    fs::path ArtifactPath(const ShaderStoreHash& key) const {
        const auto hex = key.ToHex();
        return options.cacheDirectory / "cas" / hex.substr(0, 2) / (hex + ".bin");
    }

    // True only for a whole artifact with the right key, format and contents.
    bool ReadArtifact(const ShaderStoreHash& key, uint8_t format, std::vector<std::byte>& binary) const {
        if (!options.persistent) return false;
        std::string data;
        if (!ReadWholeFile(ArtifactPath(key), data) || data.size() < sizeof(ArtifactHeader)) return false;
        ArtifactHeader header{};
        std::memcpy(&header, data.data(), sizeof(header));
        const std::string_view payload(data.data() + sizeof(header), data.size() - sizeof(header));
        if (header.magic != kArtifactMagic || header.schema != kSchema || header.format != format || !(header.key == key) || header.size != payload.size() ||
            !(HashShaderText(payload) == header.payloadHash) || payload.empty()) return false;
        const auto* first = reinterpret_cast<const std::byte*>(payload.data());
        binary.assign(first, first + payload.size());
        return true;
    }

    // Creates the file at its final name, with no rename. Only a record in the index makes it reachable from a
    // variant, and a reader checks the hash, so a torn write costs one rebuild.
    bool WriteArtifact(const ShaderStoreHash& key, uint8_t format, std::span<const std::byte> binary) const {
        if (!options.persistent) return true;
        ArtifactHeader header{};
        header.format = format;
        header.key = key;
        header.size = binary.size();
        header.payloadHash = HashShaderBytes(binary);
        const auto path = ArtifactPath(key);
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
        stream.write(reinterpret_cast<const char*>(binary.data()), static_cast<std::streamsize>(binary.size()));
        stream.close();
        return static_cast<bool>(stream);
    }

    // Runs once at startup, before any request.
    void Evict() {
        if (!ownsIndex) return;
        struct Artifact { fs::path path; uint64_t size; fs::file_time_type time; ShaderStoreHash key; uint32_t lastUsedStart; bool referenced; };
        std::unordered_map<ShaderStoreHash, uint32_t, HashOf> lastUsed;
        for (const auto& [key, variant] : variants) {
            if (variant.status != VariantStatus::Ok) continue;
            auto& newest = lastUsed[variant.compileKey];
            newest = std::max(newest, variant.lastUsedStart.load(std::memory_order_relaxed));
        }
        std::vector<Artifact> artifacts;
        uint64_t total = 0;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(options.cacheDirectory / "cas", ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entryError;
            if (!it->is_regular_file(entryError) || it->path().extension() != ".bin") continue;
            const auto size = it->file_size(entryError);
            const auto time = it->last_write_time(entryError);
            if (entryError) continue;
            const auto stem = it->path().stem().generic_string();
            if (stem.size() != 32) continue;
            ShaderStoreHash key{ std::strtoull(stem.substr(0, 16).c_str(), nullptr, 16), std::strtoull(stem.substr(16).c_str(), nullptr, 16) };
            const auto found = lastUsed.find(key);
            artifacts.push_back({ it->path(), size, time, key, found == lastUsed.end() ? 0u : found->second, found != lastUsed.end() });
            total += size;
        }
        artifactsAtStart = artifacts.size();
        artifactBytesAtStart = total;
        if (total <= options.maxBytes) return;
        // Unreferenced artifacts go first, oldest first. They only serve reverted edits.
        std::sort(artifacts.begin(), artifacts.end(), [](const Artifact& a, const Artifact& b) {
            if (a.referenced != b.referenced) return !a.referenced;
            if (a.lastUsedStart != b.lastUsedStart) return a.lastUsedStart < b.lastUsedStart;
            return a.time < b.time;
        });
        // startCount already counts this start, so this keeps what the last keepGenerations starts used.
        const uint32_t keepFrom = startCount > options.keepGenerations ? startCount - options.keepGenerations : 0;
        std::unordered_set<ShaderStoreHash, HashOf> removed;
        for (const auto& artifact : artifacts) {
            if (total <= options.maxBytes) break;
            if (artifact.referenced && artifact.lastUsedStart >= keepFrom) break;
            std::error_code removeError;
            fs::remove(artifact.path, removeError);
            if (removeError) continue;
            total -= artifact.size;
            removed.insert(artifact.key);
            ++artifactsEvicted;
        }
        if (total > options.maxBytes) overCapBytes = total - options.maxBytes;
        std::erase_if(variants, [&](const auto& entry) { return removed.contains(entry.second.compileKey); });
    }

    // --- Lookup ------------------------------------------------------------------------------------------------

    void Count(ShaderStoreOutcome outcome, ShaderStoreMiss miss, const std::string& changedFile) {
        std::scoped_lock lock(statsMutex);
        ++stats.requests;
        switch (outcome) {
        case ShaderStoreOutcome::Hit: ++stats.hits; break;
        case ShaderStoreOutcome::Absorbed: ++stats.absorbed; break;
        case ShaderStoreOutcome::Compiled: ++stats.compiled; break;
        case ShaderStoreOutcome::Failed: ++stats.failed; break;
        }
        ++stats.misses[static_cast<size_t>(miss)];
        if (stats.firstChangedFile.empty()) stats.firstChangedFile = changedFile;
    }

    void CountFinished(const ShaderStoreResult& result) { Count(result.outcome, result.miss, result.changedFile); }

    // Only one thread compiles a given text. The others wait, then find its artifact.
    class CompileGate {
    public:
        CompileGate(Impl& impl, const ShaderStoreHash& key) : impl_(impl), key_(key) {
            std::unique_lock lock(impl_.compilingMutex);
            impl_.compilingDone.wait(lock, [&] { return !impl_.compiling.contains(key_); });
            impl_.compiling.insert(key_);
        }
        ~CompileGate() {
            { std::scoped_lock lock(impl_.compilingMutex); impl_.compiling.erase(key_); }
            impl_.compilingDone.notify_all();
        }
    private:
        Impl& impl_;
        ShaderStoreHash key_;
    };

    ShaderStoreResult Resolve(const ShaderVariantRequest& request) {
        ShaderStoreResult result;
        // The root: generated bytes from the caller, or the file as the snapshot sees it.
        std::shared_ptr<const ShaderSourceFile> rootFile;
        std::string generatedRoot;
        const std::string* root = nullptr;
        if (request.hasRootBytes) {
            generatedRoot.assign(reinterpret_cast<const char*>(request.rootBytes.data()), request.rootBytes.size());
            root = &generatedRoot;
        } else {
            rootFile = snapshot.Open(snapshot.LogicalPath(request.rootPath));
            root = rootFile->bytes.get();
        }
        const ShaderStoreHash rootHash = root ? HashShaderText(*root) : ShaderStoreHash{};

        result.miss = ShaderStoreMiss::Disabled;
        if (options.persistent) {
            result.miss = ShaderStoreMiss::NoRecord;
            std::optional<ShaderStoreHash> compileKey;
            bool failed = false;
            {
                std::shared_lock lock(tableMutex);
                if (const auto found = variants.find(request.variantKey); found != variants.end()) {
                    const auto& variant = found->second;
                    if (!(variant.rootHash == rootHash)) {
                        result.miss = ShaderStoreMiss::RootChanged;
                    } else if (const auto& set = sets[variant.dependencySet]; !SetIsClean(set)) {
                        result.miss = ShaderStoreMiss::DependencyChanged;
                        result.changedFile = FirstChangedFile(set);
                    } else {
                        if (variant.lastUsedStart.exchange(startCount, std::memory_order_relaxed) != startCount) dirty = true;
                        compileKey = variant.compileKey;
                        failed = variant.status == VariantStatus::Failed;
                        if (failed) result.diagnostics = variant.diagnostics;
                    }
                }
            }
            if (failed) {
                result.outcome = ShaderStoreOutcome::Failed;
                result.miss = ShaderStoreMiss::None;
                result.failureFromCache = true;
                { std::scoped_lock lock(statsMutex); ++stats.failuresFromCache; }
                CountFinished(result);
                return result;
            }
            if (compileKey) {
                if (ReadArtifact(*compileKey, request.format, result.binary)) {
                    result.outcome = ShaderStoreOutcome::Hit;
                    result.miss = ShaderStoreMiss::None;
                    result.compileKey = *compileKey;
                    CountFinished(result);
                    return result;
                }
                result.miss = ShaderStoreMiss::ArtifactInvalid;
            }
        }

        // The inputs changed or are unknown. Preprocess to learn what they mean now.
        Tracker tracker(snapshot);
        { std::scoped_lock lock(statsMutex); ++stats.preprocessed; }
        const auto preprocessed = request.build->Preprocess(root, tracker);
        if (!preprocessed.succeeded) {
            result.outcome = ShaderStoreOutcome::Failed;
            result.diagnostics = preprocessed.diagnostics;
            if (options.persistent && !preprocessed.transientFailure)
                Record(request.variantKey, rootHash, tracker.Seen(), {}, VariantStatus::Failed, request.format, preprocessed.diagnostics);
            CountFinished(result);
            return result;
        }

        ShaderKeyBuilder keyBuilder;
        keyBuilder.Add(static_cast<uint64_t>(kSchema)).Add(request.compileConfigKey).Add(static_cast<uint64_t>(request.format));
        if (request.debug) {
            // Debug output embeds the source, so any change to any input must rebuild.
            keyBuilder.Add(request.variantKey).Add(rootHash).Add(std::string_view(preprocessed.text));
            auto seen = tracker.Seen();
            std::sort(seen.begin(), seen.end(), [](const auto& a, const auto& b) { return a->logicalPath < b->logicalPath; });
            for (const auto& file : seen) keyBuilder.Add(std::string_view(file->logicalPath)).Add(file->hash);
        } else {
            keyBuilder.Add(std::string_view(CanonicalizePreprocessedText(preprocessed.text)));
        }
        result.compileKey = keyBuilder.Finish();

        if (ReadArtifact(result.compileKey, request.format, result.binary)) {
            result.outcome = ShaderStoreOutcome::Absorbed;
            Record(request.variantKey, rootHash, tracker.Seen(), result.compileKey, VariantStatus::Ok, request.format, {});
            CountFinished(result);
            return result;
        }

        CompileGate gate(*this, result.compileKey);
        // Another thread may have finished this text while this one waited.
        if (ReadArtifact(result.compileKey, request.format, result.binary)) {
            result.outcome = ShaderStoreOutcome::Absorbed;
            Record(request.variantKey, rootHash, tracker.Seen(), result.compileKey, VariantStatus::Ok, request.format, {});
            CountFinished(result);
            return result;
        }

        auto compiled = request.build->Compile(root, preprocessed, tracker);
        result.diagnostics = std::move(compiled.diagnostics);
        if (!compiled.succeeded || compiled.binary.empty()) {
            result.outcome = ShaderStoreOutcome::Failed;
            if (options.persistent && !compiled.transientFailure)
                Record(request.variantKey, rootHash, tracker.Seen(), result.compileKey, VariantStatus::Failed, request.format, result.diagnostics);
            CountFinished(result);
            return result;
        }
        result.binary = std::move(compiled.binary);
        result.outcome = ShaderStoreOutcome::Compiled;
        if (options.persistent) {
            // Write the artifact before the record, so a record never names an artifact that was never written.
            if (!WriteArtifact(result.compileKey, request.format, result.binary)) {
                std::scoped_lock lock(statsMutex);
                ++stats.notSaved;
            }
            Record(request.variantKey, rootHash, tracker.Seen(), result.compileKey, VariantStatus::Ok, request.format, {});
        }
        CountFinished(result);
        return result;
    }

    // --- Members -----------------------------------------------------------------------------------------------

    ShaderStoreOptions options;
    Snapshot snapshot;

    mutable std::shared_mutex tableMutex;
    std::vector<std::string> paths;
    std::unordered_map<std::string, uint32_t> pathIds;
    std::vector<FileRecord> files;
    std::unordered_map<ShaderStoreHash, uint32_t, HashOf> fileIds;
    std::deque<DependencySet> sets;  // A deque, so a set stays in place while others are added.
    std::unordered_map<ShaderStoreHash, uint32_t, HashOf> setIds;
    std::unordered_map<ShaderStoreHash, VariantRecord, HashOf> variants;
    uint32_t startCount{};
    uint64_t artifactsAtStart{}, artifactBytesAtStart{}, artifactsEvicted{}, overCapBytes{};

    std::atomic<bool> dirty{};
    std::mutex saveMutex;
    int nextSlot{};
    uint64_t saveCount{};
    std::atomic<uint64_t> savedBytes{};
    bool ownsIndex{};
#if defined(_WIN32)
    HANDLE indexMutex{};
#endif

    std::mutex saverMutex;
    std::condition_variable saverWake;
    bool stopping{};
    std::thread saver;

    std::mutex compilingMutex;
    std::condition_variable compilingDone;
    std::unordered_set<ShaderStoreHash, HashOf> compiling;

    mutable std::mutex statsMutex;
    ShaderStoreStats stats;
};

ShaderStore::ShaderStore(ShaderStoreOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
ShaderStore::~ShaderStore() = default;

ShaderStoreResult ShaderStore::Resolve(const ShaderVariantRequest& request) { return impl_->Resolve(request); }
void ShaderStore::BeginGeneration() { impl_->snapshot.BeginGeneration(); }
uint64_t ShaderStore::Generation() const { return impl_->snapshot.Generation(); }
void ShaderStore::Flush() { impl_->Save(); }
bool ShaderStore::OwnsIndex() const noexcept { return impl_->ownsIndex; }

bool ShaderStore::ForEachDependency(const ShaderStoreHash& variantKey, const std::function<void(const fs::path&)>& fn) const {
    std::shared_lock lock(impl_->tableMutex);
    const auto found = impl_->variants.find(variantKey);
    if (found == impl_->variants.end()) return false;
    for (const auto id : impl_->sets[found->second.dependencySet].files) {
        const auto& file = impl_->files[id];
        if (file.exists) fn(impl_->snapshot.PhysicalPath(impl_->paths[file.pathId]));
    }
    return true;
}

ShaderStoreStats ShaderStore::Stats() const {
    std::scoped_lock lock(impl_->statsMutex);
    auto stats = impl_->stats;
    stats.sourceFilesRead = impl_->snapshot.Reads();
    return stats;
}

ShaderStoreIndexInfo ShaderStore::IndexInfo() const {
    std::shared_lock lock(impl_->tableMutex);
    ShaderStoreIndexInfo info;
    info.variants = impl_->variants.size();
    info.dependencySets = impl_->sets.size();
    for (const auto& set : impl_->sets) info.dependencyEntries += set.files.size();
    info.files = impl_->files.size();
    info.savedBytes = impl_->savedBytes.load(std::memory_order_relaxed);
    info.artifactsAtStart = impl_->artifactsAtStart;
    info.artifactBytesAtStart = impl_->artifactBytesAtStart;
    info.artifactsEvicted = impl_->artifactsEvicted;
    info.overCapBytes = impl_->overCapBytes;
    return info;
}

std::string ShaderStore::Summary() const {
    const auto stats = Stats();
    const auto miss = [&](ShaderStoreMiss reason) { return std::to_string(stats.misses[static_cast<size_t>(reason)]); };
    std::string text = std::to_string(stats.requests) + " requests: " + std::to_string(stats.hits) + " hits, " + std::to_string(stats.absorbed) + " absorbed, " +
        std::to_string(stats.compiled) + " compiled, " + std::to_string(stats.failed) + " failed (" + std::to_string(stats.failuresFromCache) + " remembered), " +
        std::to_string(stats.notSaved) + " not saved. " + std::to_string(stats.preprocessed) + " preprocessed, " + std::to_string(stats.sourceFilesRead) +
        " source files read. Misses: " + miss(ShaderStoreMiss::NoRecord) + " no record, " + miss(ShaderStoreMiss::RootChanged) + " root changed, " +
        miss(ShaderStoreMiss::DependencyChanged) + " dependency changed, " + miss(ShaderStoreMiss::ArtifactInvalid) + " artifact invalid.";
    if (!stats.firstChangedFile.empty()) text += " First changed file: " + stats.firstChangedFile;
    return text;
}

ShaderStore::ArtifactUsage ShaderStore::MeasureArtifacts() const {
    ArtifactUsage usage;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(impl_->options.cacheDirectory / "cas", ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryError;
        if (!it->is_regular_file(entryError) || it->path().extension() != ".bin") continue;
        const auto size = it->file_size(entryError);
        if (entryError) continue;
        ++usage.count;
        usage.bytes += size;
    }
    return usage;
}

void ShaderStore::RemoveArtifact(const ShaderStoreHash& compileKey) {
    std::error_code ec;
    fs::remove(impl_->ArtifactPath(compileKey), ec);
}

void ShaderStore::ClearIndex() {
    std::unique_lock lock(impl_->tableMutex);
    impl_->ResetTables();
    impl_->startCount = 1;
    std::error_code ec;
    for (const auto* slot : kIndexSlots) fs::remove(impl_->options.cacheDirectory / slot, ec);
    impl_->dirty = true;
}

void ShaderStore::Clear() {
    ClearIndex();
    std::error_code ec;
    fs::remove_all(impl_->options.cacheDirectory / "cas", ec);
}

}  // namespace org::services
