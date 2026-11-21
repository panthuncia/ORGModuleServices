#include <ORGModuleServices/ImGui/ImGuiBackend.h>

#include <cstdint>
#include <utility>

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (false)

namespace {

namespace imgui = org::services::imgui;

// The draw pipeline: ImGui's shader, a 48-byte push block and imgui_impl_dx11's blending on one target.
int TestProgramDesc() {
    const auto desc = imgui::ProgramDesc(rhi::Format::R8G8B8A8_UNorm);
    CHECK(desc.vertexSource == imgui::kShader && desc.pixelSource == imgui::kShader);
    CHECK(desc.vertexEntry == L"VSMain" && desc.pixelEntry == L"PSMain");
    CHECK(desc.constantWords == 12);
    CHECK(desc.targets.size() == 1);
    const auto& target = desc.targets[0];
    CHECK(target.format == rhi::Format::R8G8B8A8_UNorm);
    CHECK(target.blend.enable);
    CHECK(target.blend.srcColor == rhi::BlendFactor::SrcAlpha && target.blend.dstColor == rhi::BlendFactor::InvSrcAlpha);
    CHECK(target.blend.srcAlpha == rhi::BlendFactor::One && target.blend.dstAlpha == rhi::BlendFactor::InvSrcAlpha);
    CHECK(desc.depthFormat == rhi::Format::Unknown);
    return 0;
}

// Own texture IDs carry the top bit; other IDs, such as view addresses, do not.
int TestOwnTextures() {
    CHECK(imgui::IsOwnTexture(1ull << 63));
    CHECK(imgui::IsOwnTexture((1ull << 63) | 0x1000));
    CHECK(!imgui::IsOwnTexture(0));
    CHECK(!imgui::IsOwnTexture(0x00007FFF12345678ull));
    return 0;
}

// Without a graph host nothing is made or committed, and nothing is reported.
int TestNoHost() {
    int errors = 0;
    imgui::HostDesc host;
    host.graphHost = [] { return static_cast<org::GraphHost*>(nullptr); };
    host.onError = [&](std::string_view) { ++errors; };
    imgui::SetHost(std::move(host));
    CHECK(!imgui::Installed());
    CHECK(!imgui::MakeUploadPass());
    CHECK(!imgui::MakePass({}));
    CHECK(!imgui::Commit(nullptr, {}, {}));
    CHECK(!imgui::BindingOf(nullptr, -1).texture);
    CHECK(errors == 0);
    return 0;
}

} // namespace

int main() {
    if (const int line = TestProgramDesc()) return line;
    if (const int line = TestOwnTextures()) return line;
    if (const int line = TestNoHost()) return line;
    return 0;
}
