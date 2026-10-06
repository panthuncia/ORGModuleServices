#include <ORGModuleServices/ShaderCompiler.h>
#include <rhi_shader_abi.h> // not rhi.h: on Linux its Win32 adapter clashes with DXC's
#include <BasicTelemetry/Tracy.h>

#if defined(_WIN32)
#include <Windows.h>
#include <unknwn.h>
#include <objidl.h>
#include <oaidl.h>
#include <dxcapi.h>
#include <wrl/client.h>
#else
#include <dlfcn.h>
#include <dxcapi.h>
#endif

#include <fstream>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace org::services {
namespace {
#if defined(_WIN32)
using Microsoft::WRL::ComPtr;
using ModuleHandle = HMODULE;
constexpr const wchar_t* kCompilerLibrary = L"dxcompiler.dll";
constexpr const wchar_t* kValidatorLibrary = L"dxil.dll";

ModuleHandle OpenLibrary(const std::filesystem::path& path) noexcept { return LoadLibraryW(path.c_str()); }
void CloseLibrary(ModuleHandle module) noexcept { if (module) FreeLibrary(module); }
void* LibrarySymbol(ModuleHandle module, const char* name) noexcept { return reinterpret_cast<void*>(GetProcAddress(module, name)); }
std::filesystem::path LibraryPath(ModuleHandle module) {
    wchar_t path[MAX_PATH]{};
    return module && GetModuleFileNameW(module, path, MAX_PATH) ? std::filesystem::path(path) : std::filesystem::path{};
}
// The module this library is linked into (a DLL host keeps DXC beside itself, not the executable).
std::filesystem::path OwnModulePath(const void* address) {
    HMODULE ownModule{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &ownModule)) return {};
    return LibraryPath(ownModule);
}
#else
// WinAdapter.h's CComPtr with the WRL ComPtr spelling this file uses.
template<class T> class ComPtr : public CComPtr<T> {
public:
    T* Get() const noexcept { return this->p; }
    void Reset() noexcept { this->Release(); }
};
using ModuleHandle = void*;
constexpr const char* kCompilerLibrary = "libdxcompiler.so";
// SPIR-V needs no validator, and DXIL is only consumed by D3D12.
constexpr const char* kValidatorLibrary = "libdxil.so";

ModuleHandle OpenLibrary(const std::filesystem::path& path) noexcept { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
void CloseLibrary(ModuleHandle module) noexcept { if (module) dlclose(module); }
void* LibrarySymbol(ModuleHandle module, const char* name) noexcept { return dlsym(module, name); }
std::filesystem::path AddressPath(const void* address) {
    Dl_info info{};
    return dladdr(address, &info) && info.dli_fname ? std::filesystem::path(info.dli_fname) : std::filesystem::path{};
}
std::filesystem::path LibraryPath(ModuleHandle module) {
    void* symbol = module ? dlsym(module, "DxcCreateInstance") : nullptr;
    return symbol ? AddressPath(symbol) : std::filesystem::path{};
}
std::filesystem::path OwnModulePath(const void* address) {
    // dladdr names the executable by argv[0]; /proc/self/exe is the real path.
    auto path = AddressPath(address);
    if (path.empty() || !path.has_parent_path()) {
        std::error_code ec;
        path = std::filesystem::read_symlink("/proc/self/exe", ec);
    }
    return path;
}
#endif
uint64_t HashBytes(uint64_t hash, const void* data, size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
    return hash;
}
template<class T> uint64_t HashValue(uint64_t hash, const T& value) noexcept { return HashBytes(hash, &value, sizeof(value)); }
uint64_t HashWide(uint64_t hash, std::wstring_view value) noexcept { return HashBytes(hash, value.data(), value.size() * sizeof(wchar_t)); }
uint64_t HashPath(uint64_t hash, const std::filesystem::path& value) noexcept {
    const auto& native = value.native();
    return HashBytes(hash, native.data(), native.size() * sizeof(native[0]));
}

uint64_t HashFileContents(const std::filesystem::path& path) noexcept {
	uint64_t hash = 1469598103934665603ULL;
	std::ifstream stream(path, std::ios::binary);
	if (!stream) return 0;
	std::array<char, 4096> bytes{};
	while (stream) {
		stream.read(bytes.data(), bytes.size());
		const auto count = stream.gcount();
		if (count > 0) hash = HashBytes(hash, bytes.data(), static_cast<std::size_t>(count));
	}
	return hash;
}

struct CacheHeader { uint32_t magic{ 0x5347524f }; uint16_t version{ 1 }; uint8_t format{}; uint8_t reserved{}; uint64_t key{}; uint64_t size{}; };
}

class ShaderCompiler::Impl {
public:
    Impl(std::filesystem::path cacheDirectory, const std::filesystem::path& compilerDirectory) : cacheDirectory_(std::move(cacheDirectory)) {
        if (!compilerDirectory.empty()) {
            // The validator first: dxcompiler resolves it by basename.
            validatorModule_ = OpenLibrary(compilerDirectory / kValidatorLibrary);
            module_ = OpenLibrary(compilerDirectory / kCompilerLibrary);
            if (!module_ && validatorModule_) { CloseLibrary(validatorModule_); validatorModule_ = nullptr; }
        }
        if (!module_) module_ = OpenLibrary(kCompilerLibrary);
        if (!module_) {
            if (const auto ownPath = OwnModulePath(reinterpret_cast<const void*>(&HashBytes)); !ownPath.empty()) {
                const auto directory = ownPath.parent_path();
                validatorModule_ = OpenLibrary(directory / kValidatorLibrary);
                module_ = OpenLibrary(directory / kCompilerLibrary);
            }
        }
        if (!module_) return;
		// dxcompiler loads the validator dynamically by basename. Applications such as
		// SKSE keep the compiler beside the plugin rather than beside the executable,
		// so explicitly preload the matching sibling validator before compiling.
		if (!validatorModule_) {
			if (const auto compilerPath = LibraryPath(module_); !compilerPath.empty())
				validatorModule_ = OpenLibrary(compilerPath.parent_path() / kValidatorLibrary);
		}
        const auto create = reinterpret_cast<DxcCreateInstanceProc>(LibrarySymbol(module_, "DxcCreateInstance"));
        if (!create || FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils_))) ||
            FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler_)))) {
            compiler_.Reset(); utils_.Reset(); CloseLibrary(module_); module_ = nullptr;
        }
        if (!cacheDirectory_.empty()) { std::error_code ec; std::filesystem::create_directories(cacheDirectory_, ec); }
		if (const auto path = module_ ? LibraryPath(module_) : std::filesystem::path{}; !path.empty()) {
			compilerFingerprint_ = HashPath(compilerFingerprint_, path);
			std::error_code ec; const auto size = std::filesystem::file_size(path, ec);
			if (!ec) compilerFingerprint_ = HashValue(compilerFingerprint_, size);
			ec.clear(); const auto stamp = std::filesystem::last_write_time(path, ec);
			if (!ec) { const auto ticks = stamp.time_since_epoch().count(); compilerFingerprint_ = HashValue(compilerFingerprint_, ticks); }
		}
    }
    ~Impl() {
		compiler_.Reset();
		utils_.Reset();
		CloseLibrary(module_);
		CloseLibrary(validatorModule_);
	}

    uint64_t Key(const ShaderCompileRequest& request) const noexcept {
        BT_ZONE_SCOPE("ORG.ShaderCompiler.BuildKey");
        uint64_t hash = request.inputsFingerprint
            ? HashValue(1469598103934665603ULL, request.inputsFingerprint)
            : HashBytes(1469598103934665603ULL, request.source.data(), request.source.size());
        hash = HashBytes(hash, request.sourceName.data(), request.sourceName.size());
        hash = HashWide(hash, request.entryPoint); hash = HashWide(hash, request.target); hash = HashWide(hash, request.languageVersion);
        hash = HashValue(hash, request.format); hash = HashValue(hash, request.debugInfo); hash = HashValue(hash, request.warningsAsErrors);
        for (const auto& define : request.defines) { hash = HashWide(hash, define.name); hash = HashWide(hash, define.value); }
        for (const auto& argument : request.arguments) hash = HashWide(hash, argument);
		for (const auto& directory : request.includeDirectories) hash = HashPath(hash, directory);
		if (!request.inputsFingerprint) {
            BT_ZONE_SCOPE("ORG.ShaderCompiler.DependencyKeys");
		    for (const auto& dependency : request.dependencyFiles) { hash = HashPath(hash, dependency); hash = HashValue(hash, DependencyHash(dependency)); }
        }
		constexpr uint32_t compilerArgumentsVersion = 6; hash = HashValue(hash, compilerArgumentsVersion);
		return HashValue(hash, compilerFingerprint_);
    }

    // Content hash of a dependency, re-read only when its size or write time changes: a permutation
    // set shares one include tree, and every key would otherwise re-hash all of it.
    uint64_t DependencyHash(const std::filesystem::path& path) const noexcept {
#if defined(_WIN32)
        // Both witnesses in one filesystem/VFS lookup, without changing cache invalidation.
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
            (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
        uint64_t size = (uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
        int64_t stamp = static_cast<int64_t>((uint64_t(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
            attributes.ftLastWriteTime.dwLowDateTime);
        if (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            // filesystem follows symlinks; attribute queries describe the link itself.
            std::error_code ec;
            size = std::filesystem::file_size(path, ec);
            if (ec) return 0;
            stamp = std::filesystem::last_write_time(path, ec).time_since_epoch().count();
            if (ec) return 0;
        }
#else
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) return 0;
        const uint64_t size = std::filesystem::file_size(path, ec);
        if (ec) return 0;
        const int64_t stamp = static_cast<int64_t>(std::filesystem::last_write_time(path, ec).time_since_epoch().count());
        if (ec) return 0;
#endif
        {
            std::scoped_lock lock(dependencyMutex_);
            if (const auto found = dependencies_.find(path.native()); found != dependencies_.end() && found->second.size == size && found->second.stamp == stamp)
                return found->second.hash;
        }
        const uint64_t hash = HashFileContents(path);
        std::scoped_lock lock(dependencyMutex_);
        dependencies_[path.native()] = { static_cast<uint64_t>(size), static_cast<int64_t>(stamp), hash };
        return hash;
    }

    std::filesystem::path CachePath(uint64_t key) const {
        char name[32]{}; std::snprintf(name, sizeof(name), "%016llx.orgshader", static_cast<unsigned long long>(key)); return cacheDirectory_ / name;
    }
    bool Load(uint64_t key, ShaderBinaryFormat format, ShaderArtifact& artifact) const {
        if (cacheDirectory_.empty()) return false;
        std::ifstream stream(CachePath(key), std::ios::binary); CacheHeader header{};
        if (!stream.read(reinterpret_cast<char*>(&header), sizeof(header)) || header.magic != CacheHeader{}.magic ||
            header.version != 1 || header.key != key || header.format != static_cast<uint8_t>(format) || header.size > (1ull << 32)) return false;
        artifact.binary.resize(static_cast<size_t>(header.size));
        if (!stream.read(reinterpret_cast<char*>(artifact.binary.data()), static_cast<std::streamsize>(artifact.binary.size()))) return false;
        artifact.key = key; artifact.format = format; artifact.fromCache = true; return true;
    }
    void Store(const ShaderArtifact& artifact) const {
        if (cacheDirectory_.empty() || !artifact) return;
        const auto destination = CachePath(artifact.key); auto temporary = destination; temporary += ".tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        CacheHeader header{}; header.format = static_cast<uint8_t>(artifact.format); header.key = artifact.key; header.size = artifact.binary.size();
        stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
        stream.write(reinterpret_cast<const char*>(artifact.binary.data()), static_cast<std::streamsize>(artifact.binary.size())); stream.close();
        std::error_code ec; std::filesystem::rename(temporary, destination, ec);
        if (ec) { std::filesystem::remove(destination, ec); ec.clear(); std::filesystem::rename(temporary, destination, ec); }
    }

    ShaderArtifact CompileOwned(ShaderCompileRequest request, std::vector<std::byte> source, uint64_t key) {
        request.source = source;
        ShaderArtifact result{ key, request.format };
        { std::scoped_lock lock(mutex_); if (auto found = memory_.find(key); found != memory_.end()) { result = found->second; result.fromCache = true; return result; } }
        if (Load(key, request.format, result)) { std::scoped_lock lock(mutex_); memory_[key] = result; return result; }
        if (!compiler_ || !utils_) { result.diagnostics = "dxcompiler.dll is unavailable"; return result; }
#if !defined(ORG_MODULE_SERVICES_HAS_SPIRV)
        if (request.format == ShaderBinaryFormat::Spirv) { result.diagnostics = "SPIR-V support was not compiled into ORGModuleServices"; return result; }
#endif
        ComPtr<IDxcBlobEncoding> sourceBlob;
        if (FAILED(utils_->CreateBlob(source.data(), static_cast<UINT32>(source.size()), DXC_CP_UTF8, &sourceBlob))) {
            result.diagnostics = "DXC failed to create the source blob"; return result;
        }
        DxcBuffer buffer{ sourceBlob->GetBufferPointer(), sourceBlob->GetBufferSize(), DXC_CP_UTF8 };
        std::vector<std::wstring> owned;
        owned.emplace_back(L"-E"); owned.push_back(request.entryPoint); owned.emplace_back(L"-T"); owned.push_back(request.target);
        owned.emplace_back(L"-HV"); owned.push_back(request.languageVersion.empty() ? std::wstring(L"2021") : request.languageVersion);
        if (request.warningsAsErrors) owned.emplace_back(L"-WX");
        // SPIR-V gets -Zi's OpSource (every file's text embedded) and OpLine, which Nsight and RenderDoc
        // read; -Qembed_debug is the DXIL container's equivalent. Not -fspv-debug=vulkan[-with-source]:
        // its DebugValues keep dead loads alive, which changes the resources a stage uses, and DXC 1.9's
        // validator rejects some of its scopes against the embedded text.
        if (request.debugInfo) {
            owned.emplace_back(L"-Zi");
            if (request.format == ShaderBinaryFormat::Dxil) owned.emplace_back(L"-Qembed_debug");
        }
        // Same SPIR-V ABI as BasicRHI's Vulkan backend expects (descriptor-heap bindings, DX layout).
        if (request.format == ShaderBinaryFormat::Spirv) rhi::AppendVulkanDxcSpirvArguments(owned);
        for (const auto& define : request.defines) owned.push_back(L"-D" + define.name + (define.value.empty() ? L"" : L"=" + define.value));
		for (const auto& directory : request.includeDirectories) { owned.emplace_back(L"-I"); owned.push_back(directory.wstring()); }
        owned.insert(owned.end(), request.arguments.begin(), request.arguments.end());
        // The positional argument names the main file for diagnostics, debug info and relative includes.
        if (!request.sourceName.empty()) owned.emplace_back(request.sourceName.begin(), request.sourceName.end());
        std::vector<const wchar_t*> arguments; arguments.reserve(owned.size()); for (const auto& value : owned) arguments.push_back(value.c_str());
        ComPtr<IDxcResult> compileResult;
		ComPtr<IDxcIncludeHandler> includeHandler;
		if ((!request.includeDirectories.empty() && FAILED(utils_->CreateDefaultIncludeHandler(&includeHandler))) ||
			FAILED(compiler_->Compile(&buffer, arguments.data(), static_cast<UINT32>(arguments.size()), includeHandler.Get(), IID_PPV_ARGS(&compileResult)))) {
            result.diagnostics = "DXC invocation failed"; return result;
        }
        ComPtr<IDxcBlobUtf8> errors; compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (errors && errors->GetStringLength()) result.diagnostics.assign(errors->GetStringPointer(), errors->GetStringLength());
        HRESULT status{}; compileResult->GetStatus(&status); if (FAILED(status)) return result;
        ComPtr<IDxcBlob> object; compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
        if (!object) { result.diagnostics += "\nDXC produced no object"; return result; }
		const auto* first = static_cast<const std::byte*>(object->GetBufferPointer());
		if (request.format == ShaderBinaryFormat::Dxil && object->GetBufferSize() >= 20 &&
			std::memcmp(first, "DXBC", 4) == 0) {
			bool unsignedContainer = true;
			for (size_t index = 4; index < 20; ++index)
				unsignedContainer &= first[index] == std::byte{};
			if (unsignedContainer) {
				result.diagnostics += "\nDXC produced unsigned DXIL; ensure the matching dxil.dll validator is available beside dxcompiler.dll";
				return result;
			}
		}
		result.binary.assign(first, first + object->GetBufferSize());
        Store(result); { std::scoped_lock lock(mutex_); memory_[key] = result; } return result;
    }

    std::filesystem::path cacheDirectory_;
    ModuleHandle module_{};
	ModuleHandle validatorModule_{};
    ComPtr<IDxcUtils> utils_;
    ComPtr<IDxcCompiler3> compiler_;
    std::mutex mutex_;
    std::unordered_map<uint64_t, ShaderArtifact> memory_;
    std::unordered_map<uint64_t, std::shared_future<ShaderArtifact>> flights_;
	uint64_t compilerFingerprint_{ 1469598103934665603ULL };
	struct DependencyEntry { uint64_t size{}; int64_t stamp{}; uint64_t hash{}; };
	mutable std::mutex dependencyMutex_;
	mutable std::unordered_map<std::filesystem::path::string_type, DependencyEntry> dependencies_;
};

ShaderCompiler::ShaderCompiler(std::filesystem::path path, std::filesystem::path compilerDirectory) : impl_(std::make_unique<Impl>(std::move(path), compilerDirectory)) {}
ShaderCompiler::~ShaderCompiler() = default;
ShaderCompiler::ShaderCompiler(ShaderCompiler&&) noexcept = default;
ShaderCompiler& ShaderCompiler::operator=(ShaderCompiler&&) noexcept = default;
bool ShaderCompiler::Available() const noexcept { return impl_ && impl_->compiler_; }
uint64_t ShaderCompiler::BuildKey(const ShaderCompileRequest& request) const noexcept { return impl_->Key(request); }
uint64_t ShaderCompiler::FingerprintInputs(std::span<const std::byte> source, const std::vector<std::filesystem::path>& dependencyFiles) const noexcept {
    BT_ZONE_SCOPE("ORG.ShaderCompiler.FingerprintInputs");
    uint64_t hash = HashBytes(1469598103934665603ULL, source.data(), source.size());
    for (const auto& dependency : dependencyFiles) { hash = HashPath(hash, dependency); hash = HashValue(hash, impl_->DependencyHash(dependency)); }
    // Never zero, which means "none" in a request.
    return hash ? hash : 1;
}

std::shared_future<ShaderArtifact> ShaderCompiler::CompileAsync(ShaderCompileRequest request) {
    BT_ZONE_SCOPE("ORG.ShaderCompiler.CompileAsync");
    const uint64_t key = BuildKey(request);
    BT_ZONE_NAMED(enqueueZone, "ORG.ShaderCompiler.Enqueue");
    std::scoped_lock lock(impl_->mutex_);
    if (auto found = impl_->flights_.find(key); found != impl_->flights_.end()) return found->second;
    std::vector<std::byte> source(request.source.begin(), request.source.end()); request.source = {};
    auto future = std::async(std::launch::async, [impl = impl_.get(), request = std::move(request), source = std::move(source), key]() mutable {
        auto result = impl->CompileOwned(std::move(request), std::move(source), key);
        std::scoped_lock lock(impl->mutex_); impl->flights_.erase(key); return result;
    }).share();
    impl_->flights_.emplace(key, future); return future;
}
ShaderArtifact ShaderCompiler::Compile(ShaderCompileRequest request) { return CompileAsync(std::move(request)).get(); }
void ShaderCompiler::ClearMemoryCache() { std::scoped_lock lock(impl_->mutex_); impl_->memory_.clear(); }
}
