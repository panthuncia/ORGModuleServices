#pragma once

#include <ORGModuleServices/ShaderCompiler.h>

#include <rhi.h>

#include <string>
#include <utility>

namespace org::services {

// The shader binary format a backend takes. Vulkan takes SPIR-V. D3D12 takes DXIL, where the same HLSL indexes the SM 6.6
// descriptor heaps.
inline ShaderBinaryFormat ShaderFormatFor(rhi::Backend backend) {
    return backend == rhi::Backend::D3D12 ? ShaderBinaryFormat::Dxil : ShaderBinaryFormat::Spirv;
}

inline const char* ShaderFormatName(ShaderBinaryFormat format) {
    return format == ShaderBinaryFormat::Spirv ? "SPIR-V" : "DXIL";
}

// The define that names the shader API to BasicRHI's shared HLSL.
inline const wchar_t* ShaderApiDefine(ShaderBinaryFormat format) {
    return format == ShaderBinaryFormat::Spirv ? L"BASICRHI_SHADER_API_VULKAN" : L"BASICRHI_SHADER_API_D3D12";
}

// Adds a SPIR-V-only DXC argument (-fvk-*, -fspv-*) to a request for SPIR-V. Nothing for DXIL. Set the request's format first.
inline void AddSpirvArgument(ShaderCompileRequest& request, std::wstring argument) {
    if (request.format == ShaderBinaryFormat::Spirv)
        request.arguments.push_back(std::move(argument));
}

} // namespace org::services
