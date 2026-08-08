#include <ORGModuleServices/ShaderCompiler.h>

#include <Windows.h>
#include <unknwn.h>
#include <objidl.h>
#include <oaidl.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <fstream>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace org::services {
namespace {
using Microsoft::WRL::ComPtr;
uint64_t HashBytes(uint64_t hash, const void* data, size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
    return hash;
}
template<class T> uint64_t HashValue(uint64_t hash, const T& value) noexcept { return HashBytes(hash, &value, sizeof(value)); }
uint64_t HashWide(uint64_t hash, std::wstring_view value) noexcept { return HashBytes(hash, value.data(), value.size() * sizeof(wchar_t)); }

uint64_t HashFile(uint64_t hash, const std::filesystem::path& path) noexcept {
	hash = HashWide(hash, path.native());
	std::ifstream stream(path, std::ios::binary);
	if (!stream) return HashValue(hash, std::uint64_t{});
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
    explicit Impl(std::filesystem::path cacheDirectory) : cacheDirectory_(std::move(cacheDirectory)) {
        module_ = LoadLibraryW(L"dxcompiler.dll");
        if (!module_) {
            HMODULE ownModule{};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&HashBytes), &ownModule)) {
                wchar_t modulePath[MAX_PATH]{};
                if (GetModuleFileNameW(ownModule, modulePath, MAX_PATH)) {
					const auto directory = std::filesystem::path(modulePath).parent_path();
					validatorModule_ = LoadLibraryW((directory / L"dxil.dll").c_str());
					module_ = LoadLibraryW((directory / L"dxcompiler.dll").c_str());
				}
            }
        }
        if (!module_) return;
		// dxcompiler loads the validator dynamically by basename. Applications such as
		// SKSE keep the compiler beside the plugin rather than beside the executable,
		// so explicitly preload the matching sibling validator before compiling.
		if (!validatorModule_) {
			wchar_t compilerPath[MAX_PATH]{};
			if (GetModuleFileNameW(module_, compilerPath, MAX_PATH))
				validatorModule_ = LoadLibraryW((std::filesystem::path(compilerPath).parent_path() / L"dxil.dll").c_str());
		}
        const auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module_, "DxcCreateInstance"));
        if (!create || FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils_))) ||
            FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler_)))) {
            compiler_.Reset(); utils_.Reset(); FreeLibrary(module_); module_ = nullptr;
        }
        if (!cacheDirectory_.empty()) { std::error_code ec; std::filesystem::create_directories(cacheDirectory_, ec); }
		wchar_t compilerPath[MAX_PATH]{};
		if (module_ && GetModuleFileNameW(module_, compilerPath, MAX_PATH)) {
			const std::filesystem::path path(compilerPath); compilerFingerprint_ = HashWide(compilerFingerprint_, path.native());
			std::error_code ec; const auto size = std::filesystem::file_size(path, ec);
			if (!ec) compilerFingerprint_ = HashValue(compilerFingerprint_, size);
			ec.clear(); const auto stamp = std::filesystem::last_write_time(path, ec);
			if (!ec) { const auto ticks = stamp.time_since_epoch().count(); compilerFingerprint_ = HashValue(compilerFingerprint_, ticks); }
		}
    }
    ~Impl() {
		compiler_.Reset();
		utils_.Reset();
		if (module_) FreeLibrary(module_);
		if (validatorModule_) FreeLibrary(validatorModule_);
	}

    uint64_t Key(const ShaderCompileRequest& request) const noexcept {
        uint64_t hash = HashBytes(1469598103934665603ULL, request.source.data(), request.source.size());
        hash = HashBytes(hash, request.sourceName.data(), request.sourceName.size());
        hash = HashWide(hash, request.entryPoint); hash = HashWide(hash, request.target);
        hash = HashValue(hash, request.format); hash = HashValue(hash, request.debugInfo); hash = HashValue(hash, request.warningsAsErrors);
        for (const auto& define : request.defines) { hash = HashWide(hash, define.name); hash = HashWide(hash, define.value); }
        for (const auto& argument : request.arguments) hash = HashWide(hash, argument);
		for (const auto& directory : request.includeDirectories) hash = HashWide(hash, directory.native());
		for (const auto& dependency : request.dependencyFiles) hash = HashFile(hash, dependency);
		constexpr uint32_t compilerArgumentsVersion = 3; hash = HashValue(hash, compilerArgumentsVersion);
		return HashValue(hash, compilerFingerprint_);
    }

    std::filesystem::path CachePath(uint64_t key) const {
        wchar_t name[32]{}; swprintf_s(name, L"%016llx.orgshader", static_cast<unsigned long long>(key)); return cacheDirectory_ / name;
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
        const auto destination = CachePath(artifact.key); auto temporary = destination; temporary += L".tmp";
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
        owned.emplace_back(L"-HV"); owned.emplace_back(L"2021");
        if (request.warningsAsErrors) owned.emplace_back(L"-WX");
        if (request.debugInfo) { owned.emplace_back(L"-Zi"); owned.emplace_back(L"-Qembed_debug"); }
        if (request.format == ShaderBinaryFormat::Spirv) owned.emplace_back(L"-spirv");
        for (const auto& define : request.defines) owned.push_back(L"-D" + define.name + (define.value.empty() ? L"" : L"=" + define.value));
		for (const auto& directory : request.includeDirectories) { owned.emplace_back(L"-I"); owned.push_back(directory.native()); }
        owned.insert(owned.end(), request.arguments.begin(), request.arguments.end());
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
    HMODULE module_{};
	HMODULE validatorModule_{};
    ComPtr<IDxcUtils> utils_;
    ComPtr<IDxcCompiler3> compiler_;
    std::mutex mutex_;
    std::unordered_map<uint64_t, ShaderArtifact> memory_;
    std::unordered_map<uint64_t, std::shared_future<ShaderArtifact>> flights_;
	uint64_t compilerFingerprint_{ 1469598103934665603ULL };
};

ShaderCompiler::ShaderCompiler(std::filesystem::path path) : impl_(std::make_unique<Impl>(std::move(path))) {}
ShaderCompiler::~ShaderCompiler() = default;
ShaderCompiler::ShaderCompiler(ShaderCompiler&&) noexcept = default;
ShaderCompiler& ShaderCompiler::operator=(ShaderCompiler&&) noexcept = default;
bool ShaderCompiler::Available() const noexcept { return impl_ && impl_->compiler_; }
uint64_t ShaderCompiler::BuildKey(const ShaderCompileRequest& request) const noexcept { return impl_->Key(request); }

std::shared_future<ShaderArtifact> ShaderCompiler::CompileAsync(ShaderCompileRequest request) {
    const uint64_t key = BuildKey(request);
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
