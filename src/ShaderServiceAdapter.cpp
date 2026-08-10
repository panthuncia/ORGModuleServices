#include <ORGModuleServices/ShaderServiceAdapter.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace org::services {
namespace {
	std::wstring Widen(const char* value)
	{
		std::wstring result;
		while (value && *value) result.push_back(static_cast<unsigned char>(*value++));
		return result;
	}
}

struct ShaderServiceAdapter::Impl
{
	struct Job {
		std::shared_ptr<std::vector<std::byte>> source;
		std::shared_future<ShaderArtifact> future;
		std::shared_ptr<ShaderArtifact> artifact;
		bool released{};
	};

	explicit Impl(ShaderCompiler& value) : compiler(value) {}
	ShaderCompiler& compiler;
	std::mutex mutex;
	std::unordered_map<ORGShaderHandle, Job> jobs;
	std::atomic_uint64_t nextHandle{ 1 };
	bool open{ true };

	static ORGStatus ORG_RG_CALL Request(void* context, const ORGShaderRequest* request, ORGShaderHandle* out) noexcept
	{
		auto* self = static_cast<Impl*>(context);
		if (!self || !request || request->structSize < sizeof(*request) ||
			request->apiVersion != ORG_SHADER_COMPILER_SERVICE_VERSION_1 || !out ||
			!request->sourceName || !request->source || !request->sourceSize ||
			!request->entryPoint || !request->target || request->sourceSize > SIZE_MAX)
			return ORG_RG_E_INVALID_ARGUMENT;
		if (!self->compiler.Available()) return ORG_RG_E_UNSUPPORTED_CAPABILITY;
		try {
			auto source = std::make_shared<std::vector<std::byte>>(static_cast<size_t>(request->sourceSize));
			std::memcpy(source->data(), request->source, source->size());
			ShaderCompileRequest compile{};
			compile.sourceName = request->sourceName;
			compile.source = *source;
			compile.entryPoint = Widen(request->entryPoint);
			compile.target = Widen(request->target);
			std::scoped_lock lock(self->mutex);
			if (!self->open) return ORG_RG_E_CLOSED;
			const auto handle = self->nextHandle.fetch_add(1, std::memory_order_relaxed);
			Job job{ source, self->compiler.CompileAsync(std::move(compile)) };
			std::erase_if(self->jobs, [](const auto& item) {
				return item.second.released && item.second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
			});
			self->jobs.emplace(handle, std::move(job));
			*out = handle;
			return ORG_RG_OK;
		} catch (...) { return ORG_RG_E_INTERNAL; }
	}

	static ORGStatus ORG_RG_CALL Status(void* context, ORGShaderHandle handle, uint32_t* state,
		const void** bytes, uint64_t* size) noexcept
	{
		auto* self = static_cast<Impl*>(context);
		if (!self || !state || !bytes || !size) return ORG_RG_E_INVALID_ARGUMENT;
		std::scoped_lock lock(self->mutex);
		const auto found = self->jobs.find(handle);
		if (found == self->jobs.end()) return ORG_RG_E_STALE_HANDLE;
		auto& job = found->second;
		if (!job.artifact && job.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			try { job.artifact = std::make_shared<ShaderArtifact>(job.future.get()); }
			catch (...) { job.artifact = std::make_shared<ShaderArtifact>(); }
		}
		if (!job.artifact) { *state = ORG_SHADER_REQUEST_PENDING; *bytes = nullptr; *size = 0; return ORG_RG_OK; }
		*state = *job.artifact ? ORG_SHADER_REQUEST_READY : ORG_SHADER_REQUEST_FAILED;
		*bytes = job.artifact->binary.empty() ? nullptr : job.artifact->binary.data();
		*size = job.artifact->binary.size();
		return ORG_RG_OK;
	}

	static ORGStatus ORG_RG_CALL Release(void* context, ORGShaderHandle handle) noexcept
	{
		auto* self = static_cast<Impl*>(context);
		if (!self) return ORG_RG_E_INVALID_ARGUMENT;
		std::scoped_lock lock(self->mutex);
		const auto found = self->jobs.find(handle);
		if (found == self->jobs.end()) return ORG_RG_E_STALE_HANDLE;
		if (found->second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) self->jobs.erase(found);
		else found->second.released = true;
		return ORG_RG_OK;
	}
};

ShaderServiceAdapter::ShaderServiceAdapter(ShaderCompiler& compiler) : impl_(std::make_unique<Impl>(compiler)) {}
ShaderServiceAdapter::~ShaderServiceAdapter() { Shutdown(); }
ORGStatus ShaderServiceAdapter::Query(uint32_t version, void* out, uint32_t size) noexcept
{
	if (!out || size < sizeof(ORGShaderCompilerServiceAPI)) return ORG_RG_E_INVALID_ARGUMENT;
	if (version != ORG_SHADER_COMPILER_SERVICE_VERSION_1) return ORG_RG_E_UNSUPPORTED_VERSION;
	auto* table = static_cast<ORGShaderCompilerServiceAPI*>(out);
	if (table->structSize < sizeof(*table)) return ORG_RG_E_INVALID_ARGUMENT;
	*table = { sizeof(*table), version, impl_.get(), &Impl::Request, &Impl::Status, &Impl::Release };
	return impl_->compiler.Available() ? ORG_RG_OK : ORG_RG_E_UNSUPPORTED_CAPABILITY;
}
void ShaderServiceAdapter::Shutdown() noexcept
{
	if (!impl_) return;
	std::vector<std::shared_future<ShaderArtifact>> pending;
	{
		std::scoped_lock lock(impl_->mutex);
		if (!impl_->open && impl_->jobs.empty()) return;
		impl_->open = false;
		pending.reserve(impl_->jobs.size());
		for (const auto& [_, job] : impl_->jobs) pending.push_back(job.future);
	}
	for (auto& future : pending) try { future.wait(); } catch (...) {}
	{ std::scoped_lock lock(impl_->mutex); impl_->jobs.clear(); }
}

} // namespace org::services
