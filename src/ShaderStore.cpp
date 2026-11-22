#include <ORGModuleServices/ShaderStore.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <zstd.h>

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
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace org::services {

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kIndexMagic = 0x58495353;   // "SSIX"
constexpr uint32_t kPackMagic = 0x4B505353;    // "SSPK"
constexpr uint32_t kRecordMagic = 0x52435353;  // "SSCR"
// Bump when the meaning of a key or of any file format changes. 2: pack files of zstd records.
constexpr uint32_t kSchema = 2;
constexpr const char* kIndexSlots[2] = { "index.a", "index.b" };
constexpr const char* kPackFolder = "packs";
// A remembered failure keeps the start of its message. One broken header can fail thousands of variants.
constexpr size_t kMaxRememberedDiagnostics = 4096;

enum class Codec : uint8_t { Raw = 0, Zstd = 1 };

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
// The start of every pack file.
struct PackHeader {
    uint32_t magic{ kPackMagic };
    uint32_t schema{ kSchema };
    uint32_t packId{};
    uint32_t reserved{};
};
// The start of every record in a pack. The stored bytes follow it.
struct RecordHeader {
    uint32_t magic{ kRecordMagic };
    uint8_t format{};
    uint8_t codec{};
    uint16_t reserved{};
    ShaderStoreHash key;
    uint32_t storedSize{};
    uint32_t rawSize{};
    // Covers the fields above and the stored bytes, so a torn or stale record never passes.
    ShaderStoreHash hash;
};
#pragma pack(pop)
static_assert(sizeof(PackHeader) == 16 && sizeof(RecordHeader) == 48);

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

ShaderStoreHash RecordHash(const RecordHeader& header, std::span<const std::byte> stored) noexcept {
    XXH3_state_t state;
    XXH3_128bits_reset(&state);
    XXH3_128bits_update(&state, &header, sizeof(RecordHeader) - sizeof(ShaderStoreHash));
    XXH3_128bits_update(&state, stored.data(), stored.size());
    return ToHash(XXH3_128bits_digest(&state));
}

// One zstd context of each kind per thread, made on first use.
struct ZstdContexts {
    ZSTD_CCtx* compress{};
    ZSTD_DCtx* decompress{};
    ZstdContexts() = default;
    ZstdContexts(const ZstdContexts&) = delete;
    ZstdContexts& operator=(const ZstdContexts&) = delete;
    ~ZstdContexts() {
        ZSTD_freeCCtx(compress);
        ZSTD_freeDCtx(decompress);
    }
};
ZstdContexts& Zstd() {
    thread_local ZstdContexts contexts;
    return contexts;
}

// Fills header and stored for one artifact: zstd at the given level, or the raw bytes when that is no smaller.
void EncodeRecord(const ShaderStoreHash& key, uint8_t format, std::span<const std::byte> binary, int level, RecordHeader& header, std::vector<std::byte>& stored) {
    header = {};
    header.format = format;
    header.key = key;
    header.rawSize = static_cast<uint32_t>(binary.size());
    header.codec = static_cast<uint8_t>(Codec::Raw);
    stored.clear();
    if (level > 0) {
        auto& contexts = Zstd();
        if (!contexts.compress) contexts.compress = ZSTD_createCCtx();
        if (contexts.compress) {
            stored.resize(ZSTD_compressBound(binary.size()));
            ZSTD_CCtx_reset(contexts.compress, ZSTD_reset_session_and_parameters);
            ZSTD_CCtx_setParameter(contexts.compress, ZSTD_c_compressionLevel, level);
            const size_t size = ZSTD_compress2(contexts.compress, stored.data(), stored.size(), binary.data(), binary.size());
            if (!ZSTD_isError(size) && size < binary.size()) {
                stored.resize(size);
                header.codec = static_cast<uint8_t>(Codec::Zstd);
            } else {
                stored.clear();
            }
        }
    }
    if (header.codec == static_cast<uint8_t>(Codec::Raw)) stored.assign(binary.begin(), binary.end());
    header.storedSize = static_cast<uint32_t>(stored.size());
    header.hash = RecordHash(header, stored);
}

// Turns a record's stored bytes back into the artifact. False if they do not decode to exactly rawSize bytes.
bool DecodeRecord(const RecordHeader& header, std::span<const std::byte> stored, std::vector<std::byte>& binary) {
    if (header.codec == static_cast<uint8_t>(Codec::Raw)) {
        if (header.storedSize != header.rawSize || stored.empty()) return false;
        binary.assign(stored.begin(), stored.end());
        return true;
    }
    if (header.codec != static_cast<uint8_t>(Codec::Zstd) || header.rawSize == 0) return false;
    auto& contexts = Zstd();
    if (!contexts.decompress) contexts.decompress = ZSTD_createDCtx();
    if (!contexts.decompress) return false;
    binary.resize(header.rawSize);
    const size_t size = ZSTD_decompressDCtx(contexts.decompress, binary.data(), binary.size(), stored.data(), stored.size());
    if (ZSTD_isError(size) || size != header.rawSize) {
        binary.clear();
        return false;
    }
    return true;
}

// A pack file, for positional reads and, in the process that owns the cache, writes. Many threads read at once.
// Every handle shares delete, so a pack can be removed while another process still reads it.
class PackFile {
public:
    // Opens an existing file, or creates a new one when create is set. Null on failure.
    static std::unique_ptr<PackFile> Open(const fs::path& path, bool write, bool create) {
        std::unique_ptr<PackFile> file(new PackFile());
#if defined(_WIN32)
        // Overlapped, so the system does not serialize the threads' reads on the handle.
        file->handle_ = CreateFileW(path.c_str(), write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, create ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (file->handle_ == INVALID_HANDLE_VALUE) return nullptr;
#else
        file->fd_ = ::open(path.c_str(), (write ? O_RDWR : O_RDONLY) | (create ? (O_CREAT | O_EXCL) : 0), 0644);
        if (file->fd_ < 0) return nullptr;
#endif
        return file;
    }

    ~PackFile() {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    PackFile(const PackFile&) = delete;
    PackFile& operator=(const PackFile&) = delete;

    std::optional<uint64_t> Size() const {
#if defined(_WIN32)
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle_, &size)) return std::nullopt;
        return static_cast<uint64_t>(size.QuadPart);
#else
        struct stat info {};
        if (::fstat(fd_, &info) != 0) return std::nullopt;
        return static_cast<uint64_t>(info.st_size);
#endif
    }

    // True only when all size bytes were read.
    bool ReadAt(uint64_t offset, void* data, size_t size) const { return Transfer(offset, static_cast<std::byte*>(data), size, false); }
    // True only when all size bytes were written.
    bool WriteAt(uint64_t offset, const void* data, size_t size) { return Transfer(offset, const_cast<std::byte*>(static_cast<const std::byte*>(data)), size, true); }

private:
    PackFile() = default;

    bool Transfer(uint64_t offset, std::byte* bytes, size_t size, bool write) const {
        while (size > 0) {
            const auto chunk = static_cast<uint32_t>(std::min<size_t>(size, size_t{ 1 } << 30));
#if defined(_WIN32)
            // Each thread waits on its own event. Without one, every caller would wait on the handle's single signal.
            struct Event {
                HANDLE handle{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
                Event() = default;
                Event(const Event&) = delete;
                Event& operator=(const Event&) = delete;
                ~Event() {
                    if (handle) CloseHandle(handle);
                }
            };
            thread_local Event event;
            if (!event.handle) return false;
            OVERLAPPED overlapped{};
            overlapped.Offset = static_cast<DWORD>(offset);
            overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
            overlapped.hEvent = event.handle;
            const BOOL started = write ? WriteFile(handle_, bytes, chunk, nullptr, &overlapped) : ReadFile(handle_, bytes, chunk, nullptr, &overlapped);
            if (!started && GetLastError() != ERROR_IO_PENDING) return false;
            DWORD done = 0;
            if (!GetOverlappedResult(handle_, &overlapped, &done, TRUE) || done != chunk) return false;
#else
            const auto done = write ? ::pwrite(fd_, bytes, chunk, static_cast<off_t>(offset)) : ::pread(fd_, bytes, chunk, static_cast<off_t>(offset));
            if (done != static_cast<ssize_t>(chunk)) return false;
#endif
            bytes += chunk;
            offset += chunk;
            size -= chunk;
        }
        return true;
    }

#if defined(_WIN32)
    HANDLE handle_{ INVALID_HANDLE_VALUE };
#else
    int fd_{ -1 };
#endif
};

// One pack file. Records are only ever appended, at end.
struct Pack {
    uint32_t id{};
    fs::path path;
    std::unique_ptr<PackFile> file;
    // Where the next record goes. Everything before it is whole records. Changed only with appendMutex and tableMutex both held.
    uint64_t end{};
};

// Where one artifact is.
struct ArtifactEntry {
    uint32_t pack{};
    uint64_t offset{};
    uint32_t storedSize{};
    uint32_t rawSize{};
    uint8_t format{};
    uint8_t codec{};
    uint64_t RecordBytes() const noexcept { return sizeof(RecordHeader) + storedSize; }
};

// What the saved index said about a pack: how far its records were indexed, and whether it was to be deleted.
struct IndexedPack {
    uint64_t end{};
    bool retired{};
};

fs::path PackPath(const fs::path& cacheDirectory, uint32_t id) {
    char name[24]{};
    std::snprintf(name, sizeof(name), "%08x.pack", id);
    return cacheDirectory / kPackFolder / name;
}

// The id in a pack file's name, or nothing for any other file.
std::optional<uint32_t> PackIdOf(const fs::path& path) {
    if (path.extension() != ".pack") return std::nullopt;
    const auto stem = path.stem().generic_string();
    if (stem.size() != 8 || !std::all_of(stem.begin(), stem.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) return std::nullopt;
    return static_cast<uint32_t>(std::strtoul(stem.c_str(), nullptr, 16));
}

uint64_t MicrosecondsSince(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
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
            if (!this->options.readOnly) fs::create_directories(this->options.cacheDirectory, ec);
            ClaimIndex();
            auto start = std::chrono::steady_clock::now();
            LoadIndex();
            indexLoadMicroseconds = MicrosecondsSince(start);
            start = std::chrono::steady_clock::now();
            OpenPacks();
            packScanMicroseconds = MicrosecondsSince(start);
            ++startCount;
            start = std::chrono::steady_clock::now();
            Evict();
            Compact();
            compactMicroseconds = MicrosecondsSince(start);
            dirty = true;
            // Compaction deleted packs, so save where their records went now.
            if (packsCompacted) Save();
            if (this->options.saveInterval.count() > 0 && Writable()) saver = std::thread([this] { SaveLoop(); });
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

    // Whether this process may change the cache on disk.
    bool Writable() const noexcept { return options.persistent && ownsIndex && !options.readOnly; }

    // --- Index ownership ---------------------------------------------------------------------------------------

    void ClaimIndex() {
        // A read-only tool must not take the cache from a game that starts while it runs.
        if (options.readOnly) return;
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
        const auto packCount = reader.Count(sizeof(uint32_t) + sizeof(uint64_t) + 1);
        for (uint32_t i = 0; i < packCount && !reader.failed; ++i) {
            const auto id = reader.Value<uint32_t>();
            auto& pack = indexedPacks[id];
            pack.end = reader.Value<uint64_t>();
            pack.retired = reader.Value<uint8_t>() != 0;
        }
        const auto artifactCount = reader.Count(sizeof(ShaderStoreHash) + sizeof(uint32_t) + sizeof(uint64_t) + 2 * sizeof(uint32_t) + 2);
        for (uint32_t i = 0; i < artifactCount && !reader.failed; ++i) {
            const auto key = reader.Value<ShaderStoreHash>();
            ArtifactEntry entry;
            entry.pack = reader.Value<uint32_t>();
            entry.offset = reader.Value<uint64_t>();
            entry.storedSize = reader.Value<uint32_t>();
            entry.rawSize = reader.Value<uint32_t>();
            entry.format = reader.Value<uint8_t>();
            entry.codec = reader.Value<uint8_t>();
            artifacts[key] = entry;
        }
        return !reader.failed;
    }

    // The variant records only. Artifacts stay findable by their compile key.
    void ResetVariantTables() {
        paths.clear(); pathIds.clear(); files.clear(); fileIds.clear(); sets.clear(); setIds.clear(); variants.clear();
        startCount = 0;
    }

    void ResetTables() {
        ResetVariantTables();
        artifacts.clear();
        indexedPacks.clear();
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

    // Holds tableMutex shared.
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
        // Each pack's end as of this save. A start reads only what lies past it.
        writer.Value(static_cast<uint32_t>(packs.size() + retiredPacks.size()));
        for (const auto& [id, pack] : packs) {
            writer.Value(id);
            writer.Value(pack->end);
            writer.Value(uint8_t{ 0 });
        }
        for (const auto id : retiredPacks) {
            writer.Value(id);
            writer.Value(uint64_t{ 0 });
            writer.Value(uint8_t{ 1 });
        }
        writer.Value(static_cast<uint32_t>(artifacts.size()));
        for (const auto& [key, entry] : artifacts) {
            writer.Value(key);
            writer.Value(entry.pack);
            writer.Value(entry.offset);
            writer.Value(entry.storedSize);
            writer.Value(entry.rawSize);
            writer.Value(entry.format);
            writer.Value(entry.codec);
        }
        return std::move(writer.bytes);
    }

    // Writes over the older slot. There is no rename. A torn write fails the hash check on load, and the other slot is used.
    void Save() {
        if (!Writable()) return;
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

    // --- Packs -------------------------------------------------------------------------------------------------

    // Reads the records in [from, size) of a pack and adds each whole one to the table. Returns where the whole
    // records end: a torn record from a killed process ends the scan, and the next append writes over it.
    // Only called at startup, with tableMutex held.
    uint64_t ScanRecords(const Pack& pack, uint64_t from, uint64_t size) {
        if (size <= from) return from;
        std::vector<std::byte> region(static_cast<size_t>(size - from));
        if (!pack.file->ReadAt(from, region.data(), region.size())) return from;
        size_t at = 0;
        while (region.size() - at >= sizeof(RecordHeader)) {
            RecordHeader header{};
            std::memcpy(&header, region.data() + at, sizeof(header));
            const size_t left = region.size() - at - sizeof(header);
            if (header.magic != kRecordMagic || header.storedSize == 0 || header.storedSize > left) break;
            const std::span<const std::byte> stored(region.data() + at + sizeof(header), header.storedSize);
            if (!(RecordHash(header, stored) == header.hash)) break;
            // A later record for the same key is newer: a rebuild after a bad read, or a compaction copy.
            const auto [found, added] = artifacts.insert_or_assign(header.key,
                ArtifactEntry{ pack.id, from + at, header.storedSize, header.rawSize, header.format, header.codec });
            if (added) ++recoveredAtStart;
            at += sizeof(header) + header.storedSize;
        }
        return from + at;
    }

    // Opens every pack and reads what was appended after the saved index. Runs once at startup, before any request.
    void OpenPacks() {
        const bool write = Writable();
        std::map<uint32_t, fs::path> found;
        std::error_code ec;
        for (fs::directory_iterator it(options.cacheDirectory / kPackFolder, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entryError;
            if (!it->is_regular_file(entryError)) continue;
            if (const auto id = PackIdOf(it->path())) found.emplace(*id, it->path());
        }
        std::unique_lock lock(tableMutex);
        for (const auto& [id, path] : found) {
            nextPackId = std::max(nextPackId, id + 1);
            const auto known = indexedPacks.find(id);
            if (known != indexedPacks.end() && known->second.retired) {
                // Compaction emptied it, but could not delete it then.
                std::error_code removeError;
                if (!write || !fs::remove(path, removeError)) retiredPacks.push_back(id);
                continue;
            }
            auto pack = std::make_shared<Pack>();
            pack->id = id;
            pack->path = path;
            pack->file = PackFile::Open(path, write, false);
            if (!pack->file) continue;
            const auto size = pack->file->Size();
            PackHeader header{};
            if (!size || *size < sizeof(header) || !pack->file->ReadAt(0, &header, sizeof(header)) || header.magic != kPackMagic ||
                header.schema != kSchema || header.packId != id) {
                // Not a pack this version can read, or one whose header was never written.
                pack->file.reset();
                std::error_code removeError;
                if (write) fs::remove(path, removeError);
                continue;
            }
            // A pack the index knows is read from its saved end. Any other is read whole: it was made after the save.
            uint64_t from = sizeof(PackHeader);
            if (known != indexedPacks.end() && known->second.end >= sizeof(PackHeader) && known->second.end <= *size) from = known->second.end;
            pack->end = ScanRecords(*pack, from, *size);
            packBytesAtStart += *size;
            packs.emplace(id, std::move(pack));
        }
        // A retired pack that is gone needs nothing more.
        indexedPacks.clear();
        // Entries in packs that are gone, or past a pack's whole records, cannot be read.
        std::erase_if(artifacts, [&](const auto& item) {
            const auto pack = packs.find(item.second.pack);
            return pack == packs.end() || item.second.offset < sizeof(PackHeader) || item.second.offset + item.second.RecordBytes() > pack->second->end;
        });
        for (const auto& [key, entry] : artifacts) artifactBytesAtStart += entry.RecordBytes();
        artifactsAtStart = artifacts.size();
        packsAtStart = packs.size();
        // Keep appending to the newest pack while it has room.
        if (write && !packs.empty() && packs.rbegin()->second->end < options.packBytes) activePack = packs.rbegin()->second;
    }

    // Makes a new, empty pack to append to. Holds appendMutex.
    std::shared_ptr<Pack> CreatePack() {
        std::error_code ec;
        fs::create_directories(options.cacheDirectory / kPackFolder, ec);
        auto pack = std::make_shared<Pack>();
        pack->id = nextPackId++;
        pack->path = PackPath(options.cacheDirectory, pack->id);
        pack->file = PackFile::Open(pack->path, true, true);
        if (!pack->file) return nullptr;
        PackHeader header{};
        header.packId = pack->id;
        if (!pack->file->WriteAt(0, &header, sizeof(header))) {
            pack->file.reset();
            std::error_code removeError;
            fs::remove(pack->path, removeError);
            return nullptr;
        }
        pack->end = sizeof(header);
        std::unique_lock lock(tableMutex);
        packs.emplace(pack->id, pack);
        dirty = true;
        return pack;
    }

    // Appends one whole record (header and stored bytes) and points the key at it. The table changes only after
    // the write, so a reader never finds a record that is still being written.
    bool Append(const ShaderStoreHash& key, std::span<const std::byte> record) {
        if (!Writable() || record.size() < sizeof(RecordHeader)) return false;
        RecordHeader header{};
        std::memcpy(&header, record.data(), sizeof(header));
        std::scoped_lock appending(appendMutex);
        // Roll over when this record would take the pack past its size. An empty pack takes any record.
        if (activePack && activePack->end > sizeof(PackHeader) && activePack->end + record.size() > options.packBytes) activePack.reset();
        if (!activePack) activePack = CreatePack();
        if (!activePack) return false;
        // Only appenders change end, and they hold appendMutex, so it can be read here without tableMutex.
        const uint64_t offset = activePack->end;
        // A failed write leaves end where it was, so the next record writes over what it left.
        if (!activePack->file->WriteAt(offset, record.data(), record.size())) return false;
        std::unique_lock lock(tableMutex);
        activePack->end = offset + record.size();
        artifacts.insert_or_assign(key, ArtifactEntry{ activePack->id, offset, header.storedSize, header.rawSize, header.format, header.codec });
        dirty = true;
        return true;
    }

    // Reads one record and checks it is the one the entry names. False for a short read or any mismatch.
    static bool ReadRecord(const Pack& pack, const ArtifactEntry& entry, const ShaderStoreHash& key, std::vector<std::byte>& record) {
        if (!pack.file) return false;
        record.resize(static_cast<size_t>(entry.RecordBytes()));
        if (!pack.file->ReadAt(entry.offset, record.data(), record.size())) return false;
        RecordHeader header{};
        std::memcpy(&header, record.data(), sizeof(header));
        const std::span<const std::byte> stored(record.data() + sizeof(header), entry.storedSize);
        return header.magic == kRecordMagic && header.key == key && header.format == entry.format && header.codec == entry.codec &&
            header.storedSize == entry.storedSize && header.rawSize == entry.rawSize && RecordHash(header, stored) == header.hash;
    }

    // --- Artifacts ---------------------------------------------------------------------------------------------

    // True only for a whole artifact with the right key, format and contents.
    bool ReadArtifact(const ShaderStoreHash& key, uint8_t format, std::vector<std::byte>& binary) const {
        if (!options.persistent) return false;
        ArtifactEntry entry;
        std::shared_ptr<const Pack> pack;
        {
            std::shared_lock lock(tableMutex);
            const auto found = artifacts.find(key);
            if (found == artifacts.end() || found->second.format != format) return false;
            entry = found->second;
            const auto owner = packs.find(entry.pack);
            if (owner == packs.end()) return false;
            // The pointer keeps the file open even if Clear drops the pack meanwhile.
            pack = owner->second;
        }
        std::vector<std::byte> record;
        if (!ReadRecord(*pack, entry, key, record)) return false;
        RecordHeader header{};
        std::memcpy(&header, record.data(), sizeof(header));
        return DecodeRecord(header, std::span<const std::byte>(record).subspan(sizeof(header)), binary);
    }

    // Compresses outside any lock, then appends. A record never names an artifact that was not written.
    bool WriteArtifact(const ShaderStoreHash& key, uint8_t format, std::span<const std::byte> binary) {
        if (!options.persistent) return true;
        if (!Writable() || binary.empty() || binary.size() > std::numeric_limits<uint32_t>::max()) return false;
        RecordHeader header{};
        std::vector<std::byte> stored;
        EncodeRecord(key, format, binary, options.compressionLevel, header, stored);
        std::vector<std::byte> record(sizeof(header) + stored.size());
        std::memcpy(record.data(), &header, sizeof(header));
        std::memcpy(record.data() + sizeof(header), stored.data(), stored.size());
        return Append(key, record);
    }

    // Runs once at startup, before any request. Drops entries from the table; Compact then frees their bytes.
    void Evict() {
        if (!Writable()) return;
        struct Candidate { ShaderStoreHash key; uint64_t bytes; uint32_t lastUsedStart; bool referenced; uint32_t pack; uint64_t offset; };
        std::unordered_map<ShaderStoreHash, uint32_t, HashOf> lastUsed;
        for (const auto& [key, variant] : variants) {
            if (variant.status != VariantStatus::Ok) continue;
            auto& newest = lastUsed[variant.compileKey];
            newest = std::max(newest, variant.lastUsedStart.load(std::memory_order_relaxed));
        }
        std::vector<Candidate> candidates;
        candidates.reserve(artifacts.size());
        uint64_t total = 0;
        for (const auto& [key, entry] : artifacts) {
            const auto found = lastUsed.find(key);
            candidates.push_back({ key, entry.RecordBytes(), found == lastUsed.end() ? 0u : found->second, found != lastUsed.end(), entry.pack, entry.offset });
            total += entry.RecordBytes();
        }
        if (total <= options.maxBytes) return;
        // Unreferenced artifacts go first, oldest first. They only serve reverted edits. Older packs hold older records.
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.referenced != b.referenced) return !a.referenced;
            if (a.lastUsedStart != b.lastUsedStart) return a.lastUsedStart < b.lastUsedStart;
            if (a.pack != b.pack) return a.pack < b.pack;
            return a.offset < b.offset;
        });
        // startCount already counts this start, so this keeps what the last keepGenerations starts used.
        const uint32_t keepFrom = startCount > options.keepGenerations ? startCount - options.keepGenerations : 0;
        std::unordered_set<ShaderStoreHash, HashOf> removed;
        for (const auto& candidate : candidates) {
            if (total <= options.maxBytes) break;
            if (candidate.referenced && candidate.lastUsedStart >= keepFrom) break;
            artifacts.erase(candidate.key);
            total -= candidate.bytes;
            removed.insert(candidate.key);
            ++artifactsEvicted;
        }
        if (total > options.maxBytes) overCapBytes = total - options.maxBytes;
        std::erase_if(variants, [&](const auto& entry) { return removed.contains(entry.second.compileKey); });
    }

    // Runs once at startup, after Evict. A pack that is more than half dead bytes (removed, evicted or replaced
    // records, or a torn tail) has its live records copied to the newest pack, and is deleted. Over the size limit,
    // the packs with the most dead bytes go too, until what is left fits.
    void Compact() {
        if (!Writable()) return;
        std::map<uint32_t, uint64_t> live;
        for (const auto& [key, entry] : artifacts) live[entry.pack] += entry.RecordBytes();
        struct Candidate { uint32_t id; uint64_t used; uint64_t dead; };
        std::vector<Candidate> candidates;
        uint64_t disk = 0;
        for (const auto& [id, pack] : packs) {
            const auto size = pack->file->Size().value_or(pack->end);
            const uint64_t used = std::max(size, pack->end) - sizeof(PackHeader);
            const uint64_t liveBytes = live[id];
            disk += used;
            candidates.push_back({ id, used, used > liveBytes ? used - liveBytes : 0 });
        }
        std::vector<uint32_t> victims;
        uint64_t left = disk;
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.dead > b.dead; });
        for (const auto& candidate : candidates) {
            const bool mostlyDead = candidate.dead > 0 && candidate.dead * 2 > candidate.used;
            const bool overLimit = candidate.dead > 0 && left > options.maxBytes;
            if (!mostlyDead && !overLimit) continue;
            victims.push_back(candidate.id);
            left -= candidate.dead;
        }
        if (victims.empty()) return;
        // Never copy into a pack that is about to be deleted.
        if (activePack && std::find(victims.begin(), victims.end(), activePack->id) != victims.end()) activePack.reset();
        std::vector<std::byte> record;
        for (const auto id : victims) {
            const auto pack = packs.at(id);
            std::vector<std::pair<ShaderStoreHash, ArtifactEntry>> moving;
            for (const auto& [key, entry] : artifacts)
                if (entry.pack == id) moving.emplace_back(key, entry);
            std::sort(moving.begin(), moving.end(), [](const auto& a, const auto& b) { return a.second.offset < b.second.offset; });
            bool copied = true;
            for (const auto& [key, entry] : moving) {
                // A record that fails its check is dropped: it would cost a rebuild anyway.
                if (!ReadRecord(*pack, entry, key, record)) { artifacts.erase(key); continue; }
                if (!Append(key, record)) { copied = false; break; }
                compactedBytes += record.size();
            }
            // Out of disk space or the like: keep the pack, and what was copied is only a duplicate.
            if (!copied) break;
            const auto size = pack->file->Size().value_or(pack->end);
            {
                std::unique_lock lock(tableMutex);
                packs.erase(id);
                dirty = true;
            }
            pack->file.reset();
            std::error_code removeError;
            if (!fs::remove(pack->path, removeError)) retiredPacks.push_back(id);
            reclaimedBytes += size;
            ++packsCompacted;
        }
        reclaimedBytes = reclaimedBytes > compactedBytes ? reclaimedBytes - compactedBytes : 0;
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
    std::unordered_map<ShaderStoreHash, ArtifactEntry, HashOf> artifacts;
    std::map<uint32_t, std::shared_ptr<Pack>> packs;
    std::vector<uint32_t> retiredPacks;  // Emptied by compaction but not yet deleted.
    std::map<uint32_t, IndexedPack> indexedPacks;  // From the index, until OpenPacks reads it.
    uint32_t startCount{};
    uint64_t artifactsAtStart{}, artifactBytesAtStart{}, artifactsEvicted{}, overCapBytes{};
    uint64_t packsAtStart{}, packBytesAtStart{}, recoveredAtStart{};
    uint64_t packsCompacted{}, compactedBytes{}, reclaimedBytes{};
    uint64_t indexLoadMicroseconds{}, packScanMicroseconds{}, compactMicroseconds{};

    std::mutex appendMutex;
    std::shared_ptr<Pack> activePack;  // The pack new records go to. Guarded by appendMutex.
    uint32_t nextPackId{ 1 };          // Guarded by appendMutex after startup.

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
bool ShaderStore::OwnsIndex() const noexcept { return impl_->Writable(); }

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
    info.packsAtStart = impl_->packsAtStart;
    info.packBytesAtStart = impl_->packBytesAtStart;
    info.recoveredAtStart = impl_->recoveredAtStart;
    info.packsCompacted = impl_->packsCompacted;
    info.compactedBytes = impl_->compactedBytes;
    info.reclaimedBytes = impl_->reclaimedBytes;
    info.indexLoadMicroseconds = impl_->indexLoadMicroseconds;
    info.packScanMicroseconds = impl_->packScanMicroseconds;
    info.compactMicroseconds = impl_->compactMicroseconds;
    info.artifacts = impl_->artifacts.size();
    for (const auto& [key, entry] : impl_->artifacts) {
        info.storedBytes += entry.storedSize;
        info.rawBytes += entry.rawSize;
    }
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
    std::shared_lock lock(impl_->tableMutex);
    ArtifactUsage usage;
    usage.count = impl_->artifacts.size();
    for (const auto& [id, pack] : impl_->packs) usage.bytes += pack->file ? pack->file->Size().value_or(pack->end) : 0;
    return usage;
}

bool ShaderStore::ReadArtifact(const ShaderStoreHash& compileKey, uint8_t format, std::vector<std::byte>& binary) const {
    return impl_->ReadArtifact(compileKey, format, binary);
}

void ShaderStore::ForEachArtifact(const std::function<void(const ShaderStoreHash&, uint8_t, std::span<const std::byte>)>& fn) const {
    struct Item { ShaderStoreHash key; ArtifactEntry entry; std::shared_ptr<const Pack> pack; };
    std::vector<Item> items;
    {
        std::shared_lock lock(impl_->tableMutex);
        items.reserve(impl_->artifacts.size());
        for (const auto& [key, entry] : impl_->artifacts)
            if (const auto pack = impl_->packs.find(entry.pack); pack != impl_->packs.end()) items.push_back({ key, entry, pack->second });
    }
    // In file order, so the packs are read front to back.
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.entry.pack != b.entry.pack ? a.entry.pack < b.entry.pack : a.entry.offset < b.entry.offset;
    });
    std::vector<std::byte> record, binary;
    for (const auto& item : items) {
        if (!Impl::ReadRecord(*item.pack, item.entry, item.key, record)) continue;
        RecordHeader header{};
        std::memcpy(&header, record.data(), sizeof(header));
        if (!DecodeRecord(header, std::span<const std::byte>(record).subspan(sizeof(header)), binary)) continue;
        fn(item.key, item.entry.format, binary);
    }
}

void ShaderStore::RemoveArtifact(const ShaderStoreHash& compileKey) {
    std::unique_lock lock(impl_->tableMutex);
    if (impl_->artifacts.erase(compileKey)) impl_->dirty = true;
}

void ShaderStore::ClearIndex() {
    {
        std::unique_lock lock(impl_->tableMutex);
        impl_->ResetVariantTables();
        impl_->startCount = 1;
        impl_->dirty = true;
    }
    impl_->Save();
}

void ShaderStore::Clear() {
    std::vector<fs::path> remove;
    {
        std::scoped_lock appending(impl_->appendMutex);
        std::unique_lock lock(impl_->tableMutex);
        impl_->ResetTables();
        impl_->startCount = 1;
        for (const auto& [id, pack] : impl_->packs) remove.push_back(pack->path);
        for (const auto id : impl_->retiredPacks) remove.push_back(PackPath(impl_->options.cacheDirectory, id));
        // Readers still holding a pack keep its file open until they finish.
        impl_->packs.clear();
        impl_->retiredPacks.clear();
        impl_->activePack.reset();
        impl_->dirty = true;
    }
    // Another process's packs are not this one's to delete.
    if (!impl_->Writable()) return;
    std::error_code ec;
    for (const auto& path : remove) fs::remove(path, ec);
    for (const auto* slot : kIndexSlots) fs::remove(impl_->options.cacheDirectory / slot, ec);
}

}  // namespace org::services
