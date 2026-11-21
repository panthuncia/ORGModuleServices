#pragma once

#include <ORGModuleServices/Programs/RasterProgram.h>

#include <Render/OwnedDescriptorBinding.h>
#include <rhi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ImDrawData;

namespace org {
class GloballyIndexedResource;
class GraphHost;
class IResourceResolver;
class PixelBuffer;
class RenderPass;
class Resource;
class VersionedTexture;
}

// A Dear ImGui renderer backend on BasicRHI, run as render-graph passes. imgui_impl_dx11's work without D3D11.
//
// It implements ImGui 1.92's texture protocol, owning the font atlas pages and other ImTextureData. Other ImTextureIDs go
// to the caller's resolver. Each frame Commit stages vertices, indices and texture uploads in one buffer for the run. The
// upload pass copies, and the draw pass draws one ImDrawCmd at a time with ImGui's blending.
// Both passes are static. ImGui's textures are versions of a fixed set of atlas pages, so new textures need no graph build.
// A texture is restaged whole each frame until an upload of its bound version was recorded.
// Commit runs on the render thread inside the epoch. Draw callbacks are skipped.
//
// The module is optional (cmake/ImGuiBackend.cmake). ImGui itself comes from the host.
namespace org::services::imgui {

inline constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;

// The shader, relative to the host's shader folder. The host ships ORG_MODULE_SERVICES_SHADER_DIR's files under
// "ORGModuleServices/".
inline constexpr const char* kShader = "ORGModuleServices/ImGui.hlsl";

// A texture made by MakeTexture or for ImGui: a full mip chain, a view of all of it and one per level.
class Texture;

// What a draw samples: a descriptor-heap index or one of this backend's textures, plus what keeps it alive until the
// frame retires.
struct Binding {
    std::uint32_t index = kNoIndex;
    std::shared_ptr<const void> owner;
    std::shared_ptr<Texture> texture;  // set: one of this backend's textures, read through the passes' declarations
    int mip = -1;                      // with texture: the level, or -1 for the whole chain
};

// Turns an ImTextureID the backend does not own into a binding; kNoIndex skips the draws that use it.
using Resolver = std::function<Binding(std::uint64_t texture)>;

// What a frame draws onto: the size of the pass's target, its colour format (rhi::Format, as its value) and its pipeline.
struct Target {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;
    std::shared_ptr<const RasterProgram> program;  // the host's build of ProgramDesc for this format
};

// The texture the draw pass renders onto: a resolver, such as a versioned texture, or else a plain resource. A pass
// builder declares the resolver when there is one (SelfDeclaringResource).
struct DrawTarget {
    std::shared_ptr<const IResourceResolver> resolver;
    std::shared_ptr<Resource> resource;

    explicit operator bool() const { return resolver || resource; }
    const IResourceResolver* DeclaredResolver() const { return resolver.get(); }
    std::shared_ptr<Resource> DeclaredResource() const { return resource; }
};

// A persistent graph texture the host made: the image, and what keeps it alive with it.
struct TextureStorage {
    std::shared_ptr<PixelBuffer> image;
    std::shared_ptr<const void> owner;
};

enum class LogLevel : std::uint8_t { Info, Warning, Error };

// What the backend needs from its host. Set once with SetHost, before any other call but IsOwnTexture.
struct HostDesc {
    // The graph host the passes run in; null while there is none.
    std::function<GraphHost*()> graphHost;
    // A persistent RGBA8 UNorm texture that shaders read, with a_mips levels, named a_name. A null image fails the call
    // that needed it.
    std::function<TextureStorage(std::uint32_t width, std::uint32_t height, std::uint32_t mips, const std::string& name)> createTexture;
    // A shader resource view in the graph's heap; empty on failure.
    std::function<OwnedDescriptorBinding(rhi::Resource resource, std::shared_ptr<const void> owner, const rhi::SrvDesc& desc)>
        createShaderResourceView;
    // A sampler in the graph's sampler heap; empty on failure.
    std::function<OwnedDescriptorBinding(const rhi::SamplerDesc& desc)> createSampler;
    // Makes image the next version of an atlas page, adopted with a later frame revision. owner stays alive with it.
    std::function<void(const std::shared_ptr<VersionedTexture>& page, std::shared_ptr<GloballyIndexedResource> image,
        std::shared_ptr<const void> owner)>
        publishPageVersion;
    // Optional: told of each MakeTexture texture, which shaders read without a declaration.
    std::function<void(const std::shared_ptr<Resource>& resource)> noteImmutable;
    // A failure a call cannot return, such as a staging region that could not be made. The call then fails.
    std::function<void(std::string_view message)> onError;
    // Optional log.
    std::function<void(LogLevel level, std::string_view message)> log;
    // The draw pass's debug label.
    std::string passName = "ImGui";
};

// Sets the host. Call once, before Init and before any pass is made.
void SetHost(HostDesc host);

// The draw pipeline's description for a colour target format: the shader, the push block and imgui_impl_dx11's blending.
// The host builds it (with its own defines) and passes the program in Target.
RasterProgram::Desc ProgramDesc(rhi::Format format);

// Becomes the context's renderer backend. Call with no other renderer backend installed.
void Init();

// Stops being the backend. Every texture is destroyed, and ImGui recreates them for the next backend.
void Shutdown();

// Whether Init ran and Shutdown did not.
bool Installed();

// Destroys every ImGui texture, so ImGui recreates them next frame, such as after a font reload.
void InvalidateTextures();

// Whether an ImTextureID is one of this backend's textures (ImGui's, or MakeTexture's).
bool IsOwnTexture(std::uint64_t texture);

// An RGBA8 texture from CPU pixels, one span per level, uploaded with the next Commit. Render thread. Null, logged, on
// failure.
std::shared_ptr<Texture> MakeTexture(std::uint32_t width, std::uint32_t height, std::span<const std::span<const std::byte>> levels,
    const char* name);

// A texture's binding. mip -1 means the whole chain, sampled at its top level.
Binding BindingOf(const std::shared_ptr<Texture>& texture, int mip);

// Answers texture requests, stages geometry and uploads, and stores the draws for the current run. Inside the pass's
// epoch. False, logged, when the frame cannot be drawn, so the caller falls back.
bool Commit(ImDrawData* drawData, const Target& target, const Resolver& resolve);

// A new draw pass onto target, reading every atlas page. Records only what Commit stored for its run. Null without atlas
// pages.
std::shared_ptr<RenderPass> MakePass(DrawTarget target);

// A new upload pass, ordered before the draw pass. Records nothing on a run with nothing to upload. Null without atlas
// pages.
std::shared_ptr<RenderPass> MakeUploadPass();

}
