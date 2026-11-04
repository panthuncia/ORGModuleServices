#include <ORGModuleServices/ShaderCompiler.h>
#include "CompletionFlight.h"
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
#include <unistd.h>
#endif

#include <fstream>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
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
// The compiler's identity, without reading its whole file. Paths and write times are left out: copying the
// same compiler somewhere else must not invalidate anything.
std::string LibraryIdentity(ModuleHandle module) {
    if (!module) return {};
#if defined(_WIN32)
    const auto* base = reinterpret_cast<const std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return {};
    char text[64]{};
    std::snprintf(text, sizeof(text), "%08lx:%08lx:%08lx", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
        static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage), static_cast<unsigned long>(nt->OptionalHeader.CheckSum));
    return text;
#else
    // ELF has no such header fields, so this hashes the file. It runs once per compiler.
    std::ifstream stream(LibraryPath(module), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return HashShaderText(bytes).ToHex();
#endif
}

// Reads includes through the store, so every path DXC asks for is recorded, including those that do not exist.
// DXC tries the including file's folder and then each -I folder, and moves on when this returns a failure.
class StoreIncludeHandler final : public IDxcIncludeHandler {
public:
    StoreIncludeHandler(IDxcUtils& utils, ShaderIncludeTracker& includes) : utils_(utils), includes_(includes) {}
    virtual ~StoreIncludeHandler() = default;
    // This object lives on the stack for one Compile call.
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        if (id == __uuidof(IDxcIncludeHandler) || id == __uuidof(IUnknown)) { *object = static_cast<IDxcIncludeHandler*>(this); return S_OK; }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR fileName, IDxcBlob** source) override {
        if (!source) return E_POINTER;
        *source = nullptr;
        const auto* bytes = includes_.Open(std::filesystem::path(fileName));
        if (!bytes) return E_FAIL;
        ComPtr<IDxcBlobEncoding> blob;
        const HRESULT result = utils_.CreateBlob(bytes->data(), static_cast<UINT32>(bytes->size()), DXC_CP_UTF8, &blob);
        if (FAILED(result)) return result;
        *source = blob.Detach();
        return S_OK;
    }
private:
    IDxcUtils& utils_;
    ShaderIncludeTracker& includes_;
};

// A compile's arguments, in the order DXC receives them. Only "shared" and "extra" must match for two variants to
// share an artifact: defines and include folders have already done their work once the text is preprocessed.
struct CompileArguments {
    std::vector<std::wstring> shared;
    std::vector<std::wstring> perVariant;
    std::vector<std::wstring> extra;
    std::wstring rootName;
};

CompileArguments BuildArguments(const ShaderCompileRequest& request) {
    CompileArguments arguments;
    auto& shared = arguments.shared;
    shared.emplace_back(L"-E"); shared.push_back(request.entryPoint); shared.emplace_back(L"-T"); shared.push_back(request.target);
    shared.emplace_back(L"-HV"); shared.push_back(request.languageVersion.empty() ? std::wstring(L"2021") : request.languageVersion);
    if (request.warningsAsErrors) shared.emplace_back(L"-WX");
    // SPIR-V gets -Zi's OpSource (every file's text embedded) and OpLine, which Nsight and RenderDoc
    // read; -Qembed_debug is the DXIL container's equivalent. Not -fspv-debug=vulkan[-with-source]:
    // its DebugValues keep dead loads alive, which changes the resources a stage uses, and DXC 1.9's
    // validator rejects some of its scopes against the embedded text.
    if (request.debugInfo) {
        shared.emplace_back(L"-Zi");
        if (request.format == ShaderBinaryFormat::Dxil) shared.emplace_back(L"-Qembed_debug");
    }
    // Same SPIR-V ABI as BasicRHI's Vulkan backend expects (descriptor-heap bindings, DX layout).
    if (request.format == ShaderBinaryFormat::Spirv) rhi::AppendVulkanDxcSpirvArguments(shared);
    for (const auto& define : request.defines) arguments.perVariant.push_back(L"-D" + define.name + (define.value.empty() ? L"" : L"=" + define.value));
    for (const auto& directory : request.includeDirectories) { arguments.perVariant.emplace_back(L"-I"); arguments.perVariant.push_back(directory.wstring()); }
    arguments.extra = request.arguments;
    // The positional argument names the main file for diagnostics, debug info and relative includes.
    arguments.rootName.assign(request.sourceName.begin(), request.sourceName.end());
    return arguments;
}

void AddList(ShaderKeyBuilder& key, const std::vector<std::wstring>& list) {
    key.Add(static_cast<uint64_t>(list.size()));
    for (const auto& item : list) key.Add(std::wstring_view(item));
}
}

class ShaderCompiler::Impl {
public:
    Impl(const std::filesystem::path& cacheDirectory, const std::filesystem::path& compilerDirectory, std::shared_ptr<ShaderStore> store) : store_(std::move(store)) {
        if (!store_) {
            ShaderStoreOptions options;
            options.sourceRoot = ".";
            options.cacheDirectory = cacheDirectory;
            options.persistent = !cacheDirectory.empty();
            store_ = std::make_shared<ShaderStore>(std::move(options));
        }
        if (!compilerDirectory.empty()) {
            // The validator first: dxcompiler resolves it by basename.
            validatorModule_ = OpenLibrary(compilerDirectory / kValidatorLibrary);
            module_ = OpenLibrary(compilerDirectory / kCompilerLibrary);
            if (!module_ && validatorModule_) { CloseLibrary(validatorModule_); validatorModule_ = nullptr; }
        }
        if (!module_) module_ = OpenLibrary(kCompilerLibrary);
        if (!module_) {
            if (const auto ownPath = OwnModulePath(reinterpret_cast<const void*>(&LibraryIdentity)); !ownPath.empty()) {
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
        compilerIdentity_ = LibraryIdentity(module_) + "|" + LibraryIdentity(validatorModule_);
    }
    ~Impl() {
		compiler_.Reset();
		utils_.Reset();
		CloseLibrary(module_);
		CloseLibrary(validatorModule_);
	}

    ShaderStoreHash VariantKey(const CompileArguments& arguments, ShaderBinaryFormat format) const noexcept {
        ShaderKeyBuilder key;
        key.Add(std::string_view("dxc")).Add(std::string_view(compilerIdentity_)).Add(static_cast<uint64_t>(format));
        AddList(key, arguments.shared);
        AddList(key, arguments.perVariant);
        AddList(key, arguments.extra);
        key.Add(std::wstring_view(arguments.rootName));
        return key.Finish();
    }

    ShaderStoreHash CompileConfigKey(const CompileArguments& arguments, ShaderBinaryFormat format) const noexcept {
        ShaderKeyBuilder key;
        key.Add(std::string_view("dxc")).Add(std::string_view(compilerIdentity_)).Add(static_cast<uint64_t>(format));
        AddList(key, arguments.shared);
        AddList(key, arguments.extra);
        return key.Finish();
    }

    // The in-memory key. It also covers the source bytes and the generation, because a shader reload
    // can change either one for the same arguments, and the old result must not be returned then.
    uint64_t Key(const ShaderCompileRequest& request) const noexcept {
        BT_ZONE_SCOPE("ORG.ShaderCompiler.BuildKey");
        ShaderKeyBuilder key;
        key.Add(VariantKey(BuildArguments(request), request.format)).Add(HashShaderBytes(request.source)).Add(store_->Generation());
        return key.Finish().low;
    }

    // Runs DXC. preprocess gives the -P text in output.text; otherwise the object in output.binary.
    struct Invocation {
        bool started{};  // False means DXC itself failed, not the shader.
        bool succeeded{};
        std::string text;
        std::vector<std::byte> binary;
        std::string diagnostics;
    };

    Invocation Invoke(const CompileArguments& arguments, const std::string* root, ShaderIncludeTracker& includes, bool preprocess) {
        Invocation invocation;
        const std::string_view source = root ? std::string_view(*root) : std::string_view{};
        ComPtr<IDxcBlobEncoding> sourceBlob;
        if (FAILED(utils_->CreateBlob(source.data(), static_cast<UINT32>(source.size()), DXC_CP_UTF8, &sourceBlob))) {
            invocation.diagnostics = "DXC failed to create the source blob";
            return invocation;
        }
        DxcBuffer buffer{ sourceBlob->GetBufferPointer(), sourceBlob->GetBufferSize(), DXC_CP_UTF8 };
        std::vector<const wchar_t*> raw;
        if (preprocess) raw.push_back(L"-P");
        for (const auto* list : { &arguments.shared, &arguments.perVariant, &arguments.extra })
            for (const auto& value : *list) raw.push_back(value.c_str());
        if (!arguments.rootName.empty()) raw.push_back(arguments.rootName.c_str());
        StoreIncludeHandler handler(*utils_.Get(), includes);
        ComPtr<IDxcResult> compileResult;
        if (FAILED(compiler_->Compile(&buffer, raw.data(), static_cast<UINT32>(raw.size()), &handler, IID_PPV_ARGS(&compileResult)))) {
            invocation.diagnostics = "DXC invocation failed";
            return invocation;
        }
        invocation.started = true;
        ComPtr<IDxcBlobUtf8> errors; compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (errors && errors->GetStringLength()) invocation.diagnostics.assign(errors->GetStringPointer(), errors->GetStringLength());
        HRESULT status{}; compileResult->GetStatus(&status);
        if (FAILED(status)) return invocation;
        if (preprocess) {
            ComPtr<IDxcBlobUtf8> text; compileResult->GetOutput(DXC_OUT_HLSL, IID_PPV_ARGS(&text), nullptr);
            if (text) invocation.text.assign(text->GetStringPointer(), text->GetStringLength());
            invocation.succeeded = !invocation.text.empty();
            return invocation;
        }
        ComPtr<IDxcBlob> object; compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
        if (!object) { invocation.diagnostics += "\nDXC produced no object"; return invocation; }
        const auto* first = static_cast<const std::byte*>(object->GetBufferPointer());
        invocation.binary.assign(first, first + object->GetBufferSize());
        invocation.succeeded = true;
        return invocation;
    }

    // What the store runs when it cannot use a saved artifact.
    class Build final : public ShaderVariantBuild {
    public:
        Build(Impl& impl, const CompileArguments& arguments, ShaderBinaryFormat format) : impl_(impl), arguments_(arguments), format_(format) {}

        ShaderPreprocessOutput Preprocess(const std::string* root, ShaderIncludeTracker& includes) override {
            auto invocation = impl_.Invoke(arguments_, root, includes, true);
            ShaderPreprocessOutput output;
            output.succeeded = invocation.succeeded;
            // Nothing from DXC means something is wrong with DXC, and may not happen next time. An error message is about the shader.
            output.transientFailure = !invocation.succeeded && (!invocation.started || invocation.diagnostics.empty());
            output.text = std::move(invocation.text);
            output.diagnostics = std::move(invocation.diagnostics);
            return output;
        }

        ShaderCompileOutput Compile(const std::string* root, const ShaderPreprocessOutput&, ShaderIncludeTracker& includes) override {
            auto invocation = impl_.Invoke(arguments_, root, includes, false);
            ShaderCompileOutput output;
            output.succeeded = invocation.succeeded;
            output.transientFailure = !invocation.succeeded && (!invocation.started || invocation.diagnostics.empty());
            output.binary = std::move(invocation.binary);
            output.diagnostics = std::move(invocation.diagnostics);
            if (output.succeeded && format_ == ShaderBinaryFormat::Dxil && output.binary.size() >= 20 && std::memcmp(output.binary.data(), "DXBC", 4) == 0) {
                bool unsignedContainer = true;
                for (size_t index = 4; index < 20; ++index)
                    unsignedContainer &= output.binary[index] == std::byte{};
                if (unsignedContainer) {
                    // A missing DLL, not a problem with the shader: installing dxil.dll must not need the cache cleared.
                    output.succeeded = false;
                    output.transientFailure = true;
                    output.binary.clear();
                    output.diagnostics += "\nDXC produced unsigned DXIL; ensure the matching dxil.dll validator is available beside dxcompiler.dll";
                }
            }
            return output;
        }

    private:
        Impl& impl_;
        const CompileArguments& arguments_;
        ShaderBinaryFormat format_;
    };

    ShaderArtifact CompileOwned(ShaderCompileRequest request, std::vector<std::byte> source, uint64_t key) {
        request.source = source;
        ShaderArtifact result{ key, request.format };
        { std::scoped_lock lock(mutex_); if (auto found = memory_.find(key); found != memory_.end()) { result = found->second; result.fromCache = true; return result; } }
        if (!compiler_ || !utils_) { result.diagnostics = "dxcompiler.dll is unavailable"; return result; }
#if !defined(ORG_MODULE_SERVICES_HAS_SPIRV)
        if (request.format == ShaderBinaryFormat::Spirv) { result.diagnostics = "SPIR-V support was not compiled into ORGModuleServices"; return result; }
#endif
        const auto arguments = BuildArguments(request);
        Build build(*this, arguments, request.format);
        ShaderVariantRequest variant;
        variant.variantKey = VariantKey(arguments, request.format);
        variant.compileConfigKey = CompileConfigKey(arguments, request.format);
        variant.debug = request.debugInfo;
        variant.format = static_cast<uint8_t>(request.format);
        variant.rootPath = request.sourceName;
        variant.rootBytes = source;
        variant.hasRootBytes = true;
        variant.build = &build;
        auto resolved = store_->Resolve(variant);
        result.variantKey = variant.variantKey;
        result.diagnostics = std::move(resolved.diagnostics);
        if (resolved.outcome == ShaderStoreOutcome::Failed) return result;
        result.binary = std::move(resolved.binary);
        result.fromCache = resolved.outcome != ShaderStoreOutcome::Compiled;
        if (request.keepInMemory) { std::scoped_lock lock(mutex_); memory_[key] = result; }
        return result;
    }

    std::shared_ptr<ShaderStore> store_;
    std::string compilerIdentity_;
    ModuleHandle module_{};
	ModuleHandle validatorModule_{};
    ComPtr<IDxcUtils> utils_;
    ComPtr<IDxcCompiler3> compiler_;
    using Flight = detail::CompletionFlight<ShaderArtifact>;

    // A compile task's body: on the submitter's thread (or the default thread).
    void RunFlight(const std::shared_ptr<Flight>& flight, ShaderCompileRequest request, std::vector<std::byte> source, uint64_t key) {
        const auto format = request.format;
        ShaderArtifact result;
        std::exception_ptr error;
        std::string message;
        try { result = CompileOwned(std::move(request), std::move(source), key); }
        catch (const std::exception& e) { error = std::current_exception(); message = e.what(); }
        catch (...) { error = std::current_exception(); message = "shader compilation failed"; }
        if (error) FinishFlight(flight, key, ShaderArtifact{ key, format, {}, std::move(message) }, error);
        else FinishFlight(flight, key, std::move(result), {});
    }
    // Leaves the flight table first, so a later request (also one a callback makes) finds the memory cache or starts
    // afresh, then completes the future and invokes the callbacks, outside mutex_.
    void FinishFlight(const std::shared_ptr<Flight>& flight, uint64_t key, ShaderArtifact result, std::exception_ptr error) {
        {
            std::scoped_lock lock(mutex_);
            if (auto found = flights_.find(key); found != flights_.end() && found->second == flight) flights_.erase(found);
        }
        if (error) flight->Fail(std::move(error), std::move(result));
        else flight->Complete(std::move(result));
    }

    std::mutex mutex_;
    std::unordered_map<uint64_t, ShaderArtifact> memory_;
    std::unordered_map<uint64_t, std::shared_ptr<Flight>> flights_;
    Submit submit_;
};

ShaderCompiler::ShaderCompiler(std::filesystem::path cacheDirectory, std::filesystem::path compilerDirectory, std::shared_ptr<ShaderStore> store) :
    impl_(std::make_shared<Impl>(cacheDirectory, compilerDirectory, std::move(store))) {}
ShaderCompiler::~ShaderCompiler() = default;
ShaderCompiler::ShaderCompiler(ShaderCompiler&&) noexcept = default;
ShaderCompiler& ShaderCompiler::operator=(ShaderCompiler&&) noexcept = default;
bool ShaderCompiler::Available() const noexcept { return impl_ && impl_->compiler_; }
uint64_t ShaderCompiler::BuildKey(const ShaderCompileRequest& request) const noexcept { return impl_->Key(request); }
ShaderStore& ShaderCompiler::Store() const noexcept { return *impl_->store_; }

std::shared_future<ShaderArtifact> ShaderCompiler::CompileAsync(ShaderCompileRequest request) { return CompileAsync(std::move(request), {}); }

std::shared_future<ShaderArtifact> ShaderCompiler::CompileAsync(ShaderCompileRequest request, CompletionCallback onComplete) {
    BT_ZONE_SCOPE("ORG.ShaderCompiler.CompileAsync");
    const uint64_t key = BuildKey(request);
    BT_ZONE_NAMED(enqueueZone, "ORG.ShaderCompiler.Enqueue");
    std::optional<ShaderArtifact> hit;
    std::shared_ptr<Impl::Flight> flight;
    Submit submit;
    bool owner = false;
    {
        std::scoped_lock lock(impl_->mutex_);
        if (auto found = impl_->memory_.find(key); found != impl_->memory_.end()) {
            hit = found->second; hit->fromCache = true;
        } else if (auto inFlight = impl_->flights_.find(key); inFlight != impl_->flights_.end()) {
            flight = inFlight->second;
        } else {
            flight = std::make_shared<Impl::Flight>();
            impl_->flights_.emplace(key, flight);
            submit = impl_->submit_;
            owner = true;
        }
    }
    if (hit) {
        auto future = Impl::Flight::Ready(std::move(*hit));
        if (onComplete) onComplete(future.get());
        return future;
    }
    // The owner attaches before submitting, so its callback is queued; a joiner may find the flight already done and
    // is then invoked here, on its own thread.
    flight->Attach(std::move(onComplete));
    auto future = flight->Future();
    if (!owner) return future;
    const auto format = request.format;
    std::vector<std::byte> source(request.source.begin(), request.source.end()); request.source = {};
    ServiceTask task = [impl = impl_, flight, request = std::move(request), source = std::move(source), key]() mutable {
        impl->RunFlight(flight, std::move(request), std::move(source), key);
    };
    if (!detail::LaunchServiceTask(submit, std::move(task)))
        impl_->FinishFlight(flight, key, ShaderArtifact{ key, format, {}, "the shader compile submitter refused the request" }, {});
    return future;
}
void ShaderCompiler::Configure(Submit submit) { std::scoped_lock lock(impl_->mutex_); impl_->submit_ = std::move(submit); }
ShaderArtifact ShaderCompiler::Compile(ShaderCompileRequest request) { return CompileAsync(std::move(request)).get(); }
void ShaderCompiler::ClearMemoryCache() { std::scoped_lock lock(impl_->mutex_); impl_->memory_.clear(); }
}
