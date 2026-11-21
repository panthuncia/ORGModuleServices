// DXIL reflection through the dxcompiler.dll in the process. Windows only, built with DXC.

#include <ORGModuleServices/ShaderReflection.h>

#include <Windows.h>
#include <unknwn.h>
#include <objidl.h>
#include <oaidl.h>
#include <dxcapi.h>
#include <d3d12shader.h>
#include <wrl/client.h>

#include <string_view>

namespace org::services {
namespace {

using Microsoft::WRL::ComPtr;

// Spelled out so no GUID definition is needed.
constexpr GUID kClsidDxcCompiler = { 0x73e22d93, 0xe6ce, 0x47f3, { 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0 } };
constexpr GUID kClsidDxcUtils = { 0x6245d6af, 0x66e0, 0x48fd, { 0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c } };

// The loaded dxcompiler.dll's DxcCreateInstance, or null.
DxcCreateInstanceProc CreateInstance()
{
    const HMODULE dxc = ::GetModuleHandleW(L"dxcompiler.dll");
    return dxc ? reinterpret_cast<DxcCreateInstanceProc>(::GetProcAddress(dxc, "DxcCreateInstance")) : nullptr;
}

// One utils and one compiler per thread, because DXC's objects are not shared between threads.
IDxcUtils* Utils()
{
    thread_local ComPtr<IDxcUtils> utils;
    if (!utils)
        if (const auto create = CreateInstance())
            (void)create(kClsidDxcUtils, __uuidof(IDxcUtils), reinterpret_cast<void**>(utils.GetAddressOf()));
    return utils.Get();
}

IDxcCompiler3* Compiler()
{
    thread_local ComPtr<IDxcCompiler3> compiler;
    if (!compiler)
        if (const auto create = CreateInstance())
            (void)create(kClsidDxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void**>(compiler.GetAddressOf()));
    return compiler.Get();
}

ComPtr<ID3D12ShaderReflection> Reflector(std::span<const std::byte> a_dxil)
{
    auto* utils = Utils();
    if (!utils || !IsDxil(a_dxil))
        return nullptr;
    const DxcBuffer buffer{ a_dxil.data(), a_dxil.size(), 0 };
    ComPtr<ID3D12ShaderReflection> reflector;
    if (FAILED(utils->CreateReflection(&buffer, __uuidof(ID3D12ShaderReflection), reinterpret_cast<void**>(reflector.GetAddressOf()))))
        return nullptr;
    return reflector;
}

}

bool ReflectDxil(std::span<const std::byte> a_dxil, ShaderReflection& a_out, const DxilBindingLayout& a_layout)
{
    a_out.inputs.clear();
    a_out.bindings.clear();
    a_out.blockMembers.clear();
    a_out.outputLocations = 0;
    const auto reflector = Reflector(a_dxil);
    D3D12_SHADER_DESC desc{};
    if (!reflector || FAILED(reflector->GetDesc(&desc)))
        return false;
    for (UINT i = 0; i < desc.InputParameters; ++i) {
        D3D12_SIGNATURE_PARAMETER_DESC input{};
        if (SUCCEEDED(reflector->GetInputParameterDesc(i, &input)) && input.SemanticName)
            a_out.inputs.push_back({ input.SemanticName, input.SemanticIndex, input.Register });
    }
    for (UINT i = 0; i < desc.OutputParameters; ++i) {
        D3D12_SIGNATURE_PARAMETER_DESC output{};
        if (SUCCEEDED(reflector->GetOutputParameterDesc(i, &output)) && output.SystemValueType == D3D_NAME_TARGET && output.SemanticIndex < 32)
            a_out.outputLocations |= 1u << output.SemanticIndex;
    }
    using Kind = ShaderReflection::BindingKind;
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D12_SHADER_INPUT_BIND_DESC bind{};
        if (FAILED(reflector->GetResourceBindingDesc(i, &bind)))
            continue;
        switch (bind.Type) {
        case D3D_SIT_CBUFFER:
            // Push data, not a binding.
            if (bind.BindPoint == a_layout.pushConstantRegister)
                continue;
            a_out.bindings.push_back({ 0, a_layout.constantBufferShift + bind.BindPoint, Kind::ConstantBuffer });
            break;
        case D3D_SIT_TEXTURE:
        case D3D_SIT_TBUFFER:
            a_out.bindings.push_back({ 0, a_layout.textureShift + bind.BindPoint, Kind::Texture });
            break;
        case D3D_SIT_STRUCTURED:
        case D3D_SIT_BYTEADDRESS:
            a_out.bindings.push_back({ 0, a_layout.textureShift + bind.BindPoint, Kind::StorageBuffer });
            break;
        case D3D_SIT_SAMPLER:
            a_out.bindings.push_back({ 0, a_layout.samplerShift + bind.BindPoint, Kind::Sampler });
            break;
        default:
            a_out.bindings.push_back({ 0, a_layout.unorderedShift + bind.BindPoint, Kind::Other });
            break;
        }
    }
    for (UINT b = 0; b < desc.ConstantBuffers; ++b) {
        auto* buffer = reflector->GetConstantBufferByIndex(b);
        D3D12_SHADER_BUFFER_DESC bufferDesc{};
        if (!buffer || FAILED(buffer->GetDesc(&bufferDesc)) || bufferDesc.Type != D3D_CT_CBUFFER || !bufferDesc.Name)
            continue;
        for (UINT v = 0; v < bufferDesc.Variables; ++v) {
            auto* variable = buffer->GetVariableByIndex(v);
            D3D12_SHADER_VARIABLE_DESC variableDesc{};
            if (variable && SUCCEEDED(variable->GetDesc(&variableDesc)) && variableDesc.Name)
                a_out.blockMembers.push_back({ bufferDesc.Name, variableDesc.Name, variableDesc.StartOffset });
        }
    }
    return true;
}

DxilFragmentFacts DxilFragment(std::span<const std::byte> a_dxil)
{
    DxilFragmentFacts facts;
    const auto reflector = Reflector(a_dxil);
    D3D12_SHADER_DESC desc{};
    auto* compiler = Compiler();
    if (!reflector || !compiler || FAILED(reflector->GetDesc(&desc)))
        return facts;
    for (UINT i = 0; i < desc.OutputParameters; ++i) {
        D3D12_SIGNATURE_PARAMETER_DESC output{};
        if (FAILED(reflector->GetOutputParameterDesc(i, &output)))
            continue;
        facts.exportsDepth = facts.exportsDepth || output.SystemValueType == D3D_NAME_DEPTH || output.SystemValueType == D3D_NAME_DEPTH_GREATER_EQUAL ||
                             output.SystemValueType == D3D_NAME_DEPTH_LESS_EQUAL;
        facts.exportsSampleMask = facts.exportsSampleMask || output.SystemValueType == D3D_NAME_COVERAGE;
    }
    // Discards, texture and typed buffer stores and atomics are found by name in the disassembly. Raw buffer stores are not
    // counted, matching what a word scan of SPIR-V finds.
    const DxcBuffer buffer{ a_dxil.data(), a_dxil.size(), 0 };
    ComPtr<IDxcResult> result;
    ComPtr<IDxcBlobUtf8> text;
    if (FAILED(compiler->Disassemble(&buffer, __uuidof(IDxcResult), reinterpret_cast<void**>(result.GetAddressOf()))) || !result ||
        FAILED(result->GetOutput(DXC_OUT_DISASSEMBLY, __uuidof(IDxcBlobUtf8), reinterpret_cast<void**>(text.GetAddressOf()), nullptr)) || !text)
        return facts;
    const std::string_view disassembly(text->GetStringPointer(), text->GetStringLength());
    bool writes = false;
    for (const std::string_view op : { "@dx.op.discard", "@dx.op.textureStore", "@dx.op.bufferStore", "@dx.op.atomicBinOp", "@dx.op.atomicCompareExchange" })
        writes = writes || disassembly.find(op) != std::string_view::npos;
    facts.defersDepth = writes || facts.exportsDepth || facts.exportsSampleMask;
    return facts;
}

}
