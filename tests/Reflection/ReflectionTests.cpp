// ShaderReflection: a hand-written SPIR-V module, then DXC's output in both formats when DXC can compile here.
#include <ORGModuleServices/ShaderReflection.h>
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
#include <ORGModuleServices/ShaderCompiler.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;
#define CHECK(expression) do { if (!(expression)) { std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } } while (false)

using org::services::ShaderReflection;
using Kind = ShaderReflection::BindingKind;

std::span<const std::byte> Bytes(const std::vector<uint32_t>& words) { return std::as_bytes(std::span(words)); }

bool HasBinding(const ShaderReflection& reflection, uint32_t set, uint32_t binding, Kind kind) {
    return std::ranges::any_of(reflection.bindings, [&](const ShaderReflection::Binding& b) { return b.set == set && b.binding == binding && b.kind == kind; });
}

bool HasBindingNumber(const ShaderReflection& reflection, uint32_t binding) {
    return std::ranges::any_of(reflection.bindings, [&](const ShaderReflection::Binding& b) { return b.binding == binding; });
}

const ShaderReflection::Input* FindInput(const ShaderReflection& reflection, std::string_view semantic, uint32_t index) {
    for (const auto& input : reflection.inputs)
        if (input.semantic == semantic && input.semanticIndex == index) return &input;
    return nullptr;
}

bool HasMember(const ShaderReflection& reflection, std::string_view name, uint32_t offset) {
    return std::ranges::any_of(reflection.blockMembers, [&](const ShaderReflection::BlockMember& m) { return m.name == name && m.offset == offset; });
}

// Builds a module word by word.
class Module {
public:
    explicit Module(uint32_t bound) : words_{ 0x07230203u, 0x00010000u, 0u, bound, 0u } {}
    void Op(uint16_t opcode, std::initializer_list<uint32_t> operands) {
        words_.push_back((uint32_t(operands.size() + 1) << 16) | opcode);
        words_.insert(words_.end(), operands);
    }
    // OpName: the id, then a null-terminated string padded to words.
    void Name(uint32_t id, std::string_view name) {
        std::vector<uint32_t> text((name.size() + 4) / 4, 0u);
        std::memcpy(text.data(), name.data(), name.size());
        words_.push_back((uint32_t(text.size() + 2) << 16) | 5u);
        words_.push_back(id);
        words_.insert(words_.end(), text.begin(), text.end());
    }
    const std::vector<uint32_t>& Words() const { return words_; }
private:
    std::vector<uint32_t> words_;
};

constexpr uint16_t kOpTypeFloat = 22, kOpTypeSampler = 26, kOpTypePointer = 32, kOpVariable = 59, kOpDecorate = 71;
constexpr uint16_t kOpFunction = 54, kOpFunctionEnd = 56, kOpLoad = 61;
constexpr uint32_t kInput = 1, kOutput = 3, kUniformConstant = 0;
constexpr uint32_t kBuiltIn = 11, kLocation = 30, kBinding = 33, kDescriptorSet = 34;

void HandWrittenSpirv() {
    // 1 float, 2 input pointer, 3 input, 4 output pointer, 5 output, 6 built-in output, 7 sampler type, 8 its pointer,
    // 9 used sampler, 10 a load's result, 11 unused sampler.
    Module module(12);
    module.Name(3, "in.var.TEXCOORD2");
    module.Op(kOpDecorate, { 3, kLocation, 1 });
    module.Op(kOpDecorate, { 5, kLocation, 2 });
    module.Op(kOpDecorate, { 6, kBuiltIn, 0 });
    module.Op(kOpDecorate, { 9, kDescriptorSet, 1 });
    module.Op(kOpDecorate, { 9, kBinding, 4 });
    module.Op(kOpDecorate, { 11, kBinding, 5 });
    module.Op(kOpTypeFloat, { 1, 32 });
    module.Op(kOpTypePointer, { 2, kInput, 1 });
    module.Op(kOpVariable, { 2, 3, kInput });
    module.Op(kOpTypePointer, { 4, kOutput, 1 });
    module.Op(kOpVariable, { 4, 5, kOutput });
    module.Op(kOpVariable, { 4, 6, kOutput });
    module.Op(kOpTypeSampler, { 7 });
    module.Op(kOpTypePointer, { 8, kUniformConstant, 7 });
    module.Op(kOpVariable, { 8, 9, kUniformConstant });
    module.Op(kOpVariable, { 8, 11, kUniformConstant });
    module.Op(kOpFunction, { 1, 0, 0, 0 });
    module.Op(kOpLoad, { 7, 10, 9 });
    module.Op(kOpFunctionEnd, {});

    ShaderReflection reflection;
    CHECK(!org::services::IsDxil(Bytes(module.Words())));
    CHECK(reflection.Parse(Bytes(module.Words())));
    CHECK(reflection.inputs.size() == 1);
    const auto* input = FindInput(reflection, "TEXCOORD", 2);
    CHECK(input && input->location == 1);
    CHECK(reflection.outputLocations == (1u << 2));
    CHECK(reflection.bindings.size() == 1);
    CHECK(HasBinding(reflection, 1, 4, Kind::Sampler));
    CHECK(!HasBindingNumber(reflection, 5));  // declared, never used
}

void RejectsOtherBytes() {
    ShaderReflection reflection;
    reflection.outputLocations = 7;
    const std::vector<uint32_t> notSpirv{ 1, 2, 3, 4, 5, 6 };
    CHECK(!reflection.Parse(Bytes(notSpirv)));
    CHECK(reflection.outputLocations == 0);
    // An instruction that runs past the end.
    std::vector<uint32_t> truncated{ 0x07230203u, 0x00010000u, 0u, 4u, 0u, (9u << 16) | 71u, 1u };
    CHECK(!reflection.Parse(Bytes(truncated)));
    std::vector<std::byte> dxil(32, std::byte{ 0 });
    std::memcpy(dxil.data(), "DXBC", 4);
    CHECK(org::services::IsDxil(dxil));
    CHECK(!org::services::IsDxil(std::span(dxil).first(31)));
}

#if defined(ORG_MODULE_SERVICES_HAS_DXC)
// Shifts as a host's SPIR-V builds might use them; DXIL is read with the same layout.
constexpr uint32_t kShiftT = 200, kShiftS = 400, kShiftU = 500;

constexpr char kPixelShader[] = R"(
cbuffer Block : register(b0) { float4 first; float4 second; };
cbuffer Pushed : register(b3) { float4 pushed; };
Texture2D colour : register(t0);
Texture2D unused : register(t1);
SamplerState linearSampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD3) : SV_Target1 {
    return colour.Sample(linearSampler, uv) * second + pushed;
}
)";

constexpr char kDepthShader[] = R"(
float4 main(float4 position : SV_Position, out float depth : SV_Depth) : SV_Target {
    if (position.x > 4) discard;
    depth = 0.5;
    return position;
}
)";

std::filesystem::path CompilerDirectory() {
    if (const char* directory = std::getenv("ORG_SERVICES_TEST_DXC_DIR")) return directory;
    return {};
}

org::services::ShaderCompileRequest Request(const char* source, org::services::ShaderBinaryFormat format) {
    org::services::ShaderCompileRequest request{};
    request.sourceName = "reflection-test.hlsl";
    request.source = { reinterpret_cast<const std::byte*>(source), std::strlen(source) };
    request.entryPoint = L"main";
    request.target = L"ps_6_0";
    request.format = format;
    request.keepInMemory = false;
    if (format == org::services::ShaderBinaryFormat::Spirv) {
        const auto shift = [&](const wchar_t* flag, uint32_t value) { request.arguments.insert(request.arguments.end(), { flag, std::to_wstring(value), L"0" }); };
        shift(L"-fvk-b-shift", 0);
        shift(L"-fvk-t-shift", kShiftT);
        shift(L"-fvk-s-shift", kShiftS);
        shift(L"-fvk-u-shift", kShiftU);
    }
    return request;
}

// The compiler stays alive while DXIL is reflected: reflection uses the dxcompiler.dll it loaded.
org::services::ShaderCompiler& Compiler() {
    static org::services::ShaderCompiler compiler({}, CompilerDirectory());
    return compiler;
}

std::optional<std::vector<std::byte>> Compile(const char* source, org::services::ShaderBinaryFormat format) {
    auto artifact = Compiler().CompileHere(Request(source, format));
    if (!artifact) {
        std::printf("  skipped: DXC could not compile here: %s\n", artifact.diagnostics.c_str());
        return std::nullopt;
    }
    return std::move(artifact.binary);
}

// What both formats must agree on for kPixelShader.
void CheckPixelShader(const ShaderReflection& reflection) {
    CHECK(FindInput(reflection, "TEXCOORD", 3) != nullptr);
    CHECK(reflection.outputLocations == (1u << 1));
    CHECK(HasBinding(reflection, 0, 0, Kind::ConstantBuffer));
    CHECK(HasBinding(reflection, 0, kShiftT, Kind::Texture));
    CHECK(HasBinding(reflection, 0, kShiftS, Kind::Sampler));
    CHECK(!HasBindingNumber(reflection, kShiftT + 1));
    CHECK(HasMember(reflection, "first", 0));
    CHECK(HasMember(reflection, "second", 16));
}

void CompiledSpirv() {
    const auto spirv = Compile(kPixelShader, org::services::ShaderBinaryFormat::Spirv);
    if (!spirv) return;
    ShaderReflection reflection;
    CHECK(!org::services::IsDxil(*spirv));
    CHECK(reflection.Parse(*spirv));
    CheckPixelShader(reflection);
    CHECK(HasBinding(reflection, 0, 3, Kind::ConstantBuffer));
    // SV_Position is a built-in, not an input by location.
    CHECK(FindInput(reflection, "SV_Position", 0) == nullptr);
}

void CompiledDxil() {
    const auto dxil = Compile(kPixelShader, org::services::ShaderBinaryFormat::Dxil);
    if (!dxil) return;
    CHECK(org::services::IsDxil(*dxil));
    org::services::DxilBindingLayout layout;
    layout.textureShift = kShiftT;
    layout.samplerShift = kShiftS;
    layout.unorderedShift = kShiftU;
    ShaderReflection reflection;
    CHECK(reflection.Parse(*dxil, layout));
    CheckPixelShader(reflection);
    CHECK(HasBinding(reflection, 0, 3, Kind::ConstantBuffer));
    // The push register is left out.
    layout.pushConstantRegister = 3;
    CHECK(org::services::ReflectDxil(*dxil, reflection, layout));
    CheckPixelShader(reflection);
    CHECK(!HasBindingNumber(reflection, 3));

    const auto plain = org::services::DxilFragment(*dxil);
    CHECK(!plain.defersDepth && !plain.exportsDepth && !plain.exportsSampleMask);
    const auto depth = Compile(kDepthShader, org::services::ShaderBinaryFormat::Dxil);
    if (!depth) return;
    const auto facts = org::services::DxilFragment(*depth);
    CHECK(facts.defersDepth && facts.exportsDepth && !facts.exportsSampleMask);
}
#endif

struct Test { const char* name; void (*run)(); };

}

int main() {
    const Test tests[] = {
        { "HandWrittenSpirv", HandWrittenSpirv },
        { "RejectsOtherBytes", RejectsOtherBytes },
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
        { "CompiledSpirv", CompiledSpirv },
        { "CompiledDxil", CompiledDxil },
#endif
    };
#if !defined(ORG_MODULE_SERVICES_HAS_DXC)
    std::printf("Compiled-stage tests skipped: built without DXC\n");
#endif
    for (const auto& test : tests) {
        const int before = failures;
        std::printf("[ RUN  ] %s\n", test.name);
        std::fflush(stdout);
        test.run();
        std::printf("[ %s ] %s\n", failures == before ? " OK " : "FAIL", test.name);
    }
    std::printf("%s: %d failed check(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
