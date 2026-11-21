#pragma once

// Reflection of compiled shader stages: SPIR-V read word by word, DXIL read through the dxcompiler.dll already loaded in
// the process (the one a ShaderCompiler loads). Pure functions, any thread. Needs no device.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace org::services {

// How a DXIL stage's registers map to the binding numbers of the host's SPIR-V builds (DXC's -fvk-<b|t|s|u>-shift), so
// both formats reflect to the same numbers. Every DXIL binding is in set 0.
struct DxilBindingLayout {
    static constexpr uint32_t kNoRegister = ~0u;
    uint32_t constantBufferShift = 0;  // b registers
    uint32_t textureShift = 0;         // t registers: textures, texel buffers, structured and byte address buffers
    uint32_t samplerShift = 0;         // s registers
    uint32_t unorderedShift = 0;       // u registers and any other kind
    // A constant buffer register the host feeds as push data. It is left out of the bindings. kNoRegister keeps them all.
    uint32_t pushConstantRegister = kNoRegister;
};

// What a stage declares: inputs by semantic, the bindings its code uses, constant buffer members and its outputs.
// Written for DXC's output in both formats.
struct ShaderReflection {
    struct Input {
        std::string semantic;  // e.g. "TEXCOORD" from DXC's "in.var.TEXCOORD1"
        uint32_t semanticIndex;
        uint32_t location;     // the SPIR-V location, or the DXIL signature register
    };

    enum class BindingKind : uint32_t {
        ConstantBuffer,
        Texture,
        Sampler,
        StorageBuffer,  // structured or byte address buffers
        Other,
    };

    struct Binding {
        uint32_t set;
        uint32_t binding;
        BindingKind kind;
    };

    // A constant buffer member. DXC keeps every member of a kept cbuffer, used or not, with the cbuffer's layout.
    struct BlockMember {
        std::string block;
        std::string name;
        uint32_t offset;  // bytes from the block's start
    };

    std::vector<Input> inputs;
    std::vector<Binding> bindings;
    std::vector<BlockMember> blockMembers;
    // The stage outputs' locations (SPIR-V) or render target indices (DXIL), a bit each.
    uint32_t outputLocations = 0;

    // Parses a SPIR-V module, or a DXIL container through ReflectDxil with a_dxilLayout. False if it is neither, or if
    // DXIL cannot be read here.
    bool Parse(std::span<const std::byte> a_module, const DxilBindingLayout& a_dxilLayout = {});
};

// Whether a_binary is a DXIL container.
bool IsDxil(std::span<const std::byte> a_binary);

// A DXIL stage's reflection, with bindings numbered by a_layout. False when no dxcompiler.dll is loaded, when DXC
// cannot read it, or when the library was built without DXIL reflection.
bool ReflectDxil(std::span<const std::byte> a_dxil, ShaderReflection& a_out, const DxilBindingLayout& a_layout = {});

// A DXIL pixel stage's depth-related facts. A stage that cannot be read is treated as deferring depth.
struct DxilFragmentFacts {
    // Whether the stage defeats early depth testing: it discards, writes depth or the sample mask, or writes memory
    // (texture and typed buffer stores, atomics). Raw buffer stores are not counted.
    bool defersDepth = true;
    bool exportsDepth = false;
    bool exportsSampleMask = false;
};
DxilFragmentFacts DxilFragment(std::span<const std::byte> a_dxil);

}
