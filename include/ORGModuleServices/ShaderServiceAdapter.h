#pragma once

#include <OpenRenderGraph/ShaderCompilerService.h>
#include <ORGModuleServices/ShaderCompiler.h>

#include <memory>

namespace org::services {

class ShaderServiceAdapter
{
public:
	explicit ShaderServiceAdapter(ShaderCompiler& compiler);
	~ShaderServiceAdapter();
	ShaderServiceAdapter(const ShaderServiceAdapter&) = delete;
	ShaderServiceAdapter& operator=(const ShaderServiceAdapter&) = delete;

	ORGStatus Query(uint32_t version, void* outTable, uint32_t outTableSize) noexcept;
	void Shutdown() noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace org::services
