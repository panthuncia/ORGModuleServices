// The program service without a device: slots, the setup join, Collect's gating and failures, fixed handles, and the
// environment's defines and fingerprints.
#include <ORGModuleServices/Programs/ProgramBuild.h>
#include <ORGModuleServices/Programs/ProgramService.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using org::services::ProgramService;

namespace {

std::shared_ptr<const void> Value(int value) { return std::make_shared<const int>(value); }

void TestEnvironment() {
    org::services::ProgramEnvironment environment;
    environment.shaderRoot = "Shaders";
    environment.backend = [] { return rhi::Backend::D3D12; };
    environment.commonDefines = [](const org::services::ProgramDefines& own, std::wstring_view target) {
        auto defines = own;
        defines.emplace_back(L"HOST", std::wstring(target));
        return defines;
    };
    environment.requestDefines = [] { return org::services::ProgramDefines{ { L"REQUEST", L"1" } }; };
    org::services::SetProgramEnvironment(std::move(environment));
    namespace build = org::services::program_build;
    assert(build::Format() == org::services::ShaderBinaryFormat::Dxil);
    assert(build::SourcePath("A/B.hlsl") == std::filesystem::path("Shaders") / "A/B.hlsl");
    const auto defines = build::WithCommonDefines({ { L"OWN", L"" } }, L"cs_6_6");
    assert(defines.size() == 2 && defines[0].first == L"OWN" && defines[1].second == L"cs_6_6");
    const std::byte bytes[] = { std::byte{ 1 }, std::byte{ 2 } };
    const auto request = build::MakeRequest("Shaders/A.hlsl", bytes, L"main", L"cs_6_6", defines);
    // The API define, the request's, then the program's own, in order.
    assert(request.defines.size() == 4 && request.defines[0].name == L"BASICRHI_SHADER_API_D3D12" && request.defines[1].name == L"REQUEST" &&
           request.defines[2].name == L"OWN" && request.arguments.empty());
    const std::byte other[] = { std::byte{ 2 }, std::byte{ 1 } };
    assert(build::HashBinary(bytes) == build::HashBinary(bytes) && build::HashBinary(bytes) != build::HashBinary(other));
    assert(build::HashBinaries(bytes, other) != build::HashBinaries(other, bytes));
    org::services::ComputeProgram::Desc compute{ .source = "A.hlsl", .defines = { { L"OWN", L"" } } };
    org::services::ResolveProgramDefines(compute);
    assert(!compute.commonDefines && compute.defines.size() == 2);
    org::services::SetProgramEnvironment({});
}

void TestSlots() {
    org::async::Executor executor(4);
    std::vector<std::string> errors;
    ProgramService service({ .executor = &executor, .laneName = "test programs", .onError = [&](std::string_view message) { errors.emplace_back(message); } });
    // No host: nothing can be built, so a desc queues nothing.
    assert(!service.Available());
    assert(!service.Queue("test", org::services::ComputeProgram::Desc{ .source = "A.hlsl" }));
    std::atomic_uint ran{ 0 };
    auto gating = service.Queue("gating", [&] { ++ran; return Value(1); }, true);
    auto quiet = service.Queue("quiet", [&] { ++ran; return Value(2); }, false);
    auto failing = service.Queue("failing", [&]() -> std::shared_ptr<const void> { ++ran; return nullptr; }, false);
    assert(service.Pending());
    service.JoinSetup("the test");
    assert(ran.load() == 3 && !service.Pending());
    assert(gating->collected && quiet->collected && !failing->collected);
    assert(errors.size() == 1 && errors[0].find("failing could not be built") == 0);
    const org::services::FixedProgram<int> fixed(gating);
    assert(fixed.Ready() && *fixed.Get() == 1);
    // A handle read before its build is done reports and gives null.
    auto early = std::make_shared<org::services::ProgramSlot>();
    early->label = "early";
    early->service = &service;
    assert(!org::services::FixedProgram<int>(early).Get() && errors.size() == 2);
    // A gating build holds Collect false until it is done.
    std::atomic_bool release{ false };
    auto held = service.Queue("held", [&] { while (!release.load()) std::this_thread::yield(); return Value(3); }, true);
    assert(!service.Collect());
    release = true;
    held->done.wait(false);
    assert(service.Collect() && held->collected);
    // After Stop the pool refuses, which is reported once.
    service.Stop();
    auto refused = service.Queue("refused", [] { return Value(4); }, true);
    assert(refused->done.load() && errors.size() == 3 && errors[2].find("refused the build of refused") != std::string::npos);
    assert(service.Collect() && errors.size() == 3);
}

} // namespace

int main() {
    TestEnvironment();
    TestSlots();
}
