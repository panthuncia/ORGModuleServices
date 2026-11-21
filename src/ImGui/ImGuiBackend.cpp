#include <ORGModuleServices/ImGui/ImGuiBackend.h>

#include <imgui.h>

#include <OpenRenderGraph/GraphHost.h>
#include <OpenRenderGraph/Runtime.h>
#include <Render/FrameArena.h>
#include <Render/PassBuilders.h>
#include <Render/PreparedPass.h>
#include <Render/RunSlots.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/PixelBuffer.h>
#include <Resources/Resource.h>
#include <Resources/VersionedTexture.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <format>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace org::services::imgui {

namespace {

constexpr const char* kBackendName = "imgui_impl_basicrhi";
// An own texture's ImTextureID is this bit plus its address. Other textures' IDs never have it, such as D3D11 view
// addresses, which are below 2^47.
constexpr std::uint64_t kOwnTag = 1ull << 63;
// D3D12's copy rules, which Vulkan's satisfy: rows of a texture copy 256-byte aligned, a copy's start 512.
constexpr std::uint32_t kRowAlignment = 256;
constexpr std::uint64_t kPlacementAlignment = 512;
// The font atlas pages the passes declare. A resize makes a new page while the old one is drawn for a frame.
// A texture past the last page is not drawn.
constexpr std::size_t kAtlasPages = 4;

// Must match DrawConstants in ImGui.hlsl.
struct DrawConstants {
    std::uint32_t geometryIndex;
    std::uint32_t vertexOffset;
    std::uint32_t indexOffset;
    std::uint32_t indexSize;
    std::uint32_t textureIndex;
    std::uint32_t samplerIndex;
    float scale[2];
    float translate[2];
    std::uint32_t padding[2];
};
static_assert(sizeof(DrawConstants) == 48);
static_assert(std::is_trivially_copyable_v<DrawConstants>);
static_assert(sizeof(ImDrawVert) == 20, "ImGui.hlsl reads ImGui's default vertex layout");
static_assert(sizeof(ImDrawIdx) == 2 || sizeof(ImDrawIdx) == 4);
constexpr std::uint32_t kConstantWords = sizeof(DrawConstants) / sizeof(std::uint32_t);

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

}

class Texture {
public:
    std::shared_ptr<const void> storage;      // the host's, kept with the texture
    std::shared_ptr<Resource> resource;       // the image, kept by the views and the frames
    std::shared_ptr<PixelBuffer> image;       // the same resource, as a texture
    std::uint32_t width = 0, height = 0, mips = 1;
    // MakeTexture's exempt immutable class, never declared. Set before the texture is shared.
    bool exempt = false;
    // The atlas page it is a version of, kAtlasPages for none. Set before the texture is shared.
    std::size_t page = kAtlasPages;
    // Exempt reads. views[0] covers every level, views[1 + m] level m alone.
    std::vector<OwnedDescriptorBinding> views;
    std::vector<std::vector<std::byte>> pixels;  // render thread: MakeTexture's levels, until uploaded
    std::atomic<bool> uploaded{ false };         // a recorded upload pass copied its whole contents
};

namespace {

// One copy into a texture: a rectangle of a level, from the frame's staging buffer.
struct Region {
    std::uint32_t mip = 0, x = 0, y = 0, width = 0, height = 0;
    std::uint64_t offset = 0;
    std::uint32_t rowPitch = 0;
};

struct Upload {
    std::shared_ptr<Texture> texture;
    bool whole = false;  // every level's whole contents: the texture counts as uploaded once a recorded pass copied them
    std::vector<Region> regions;
};

struct Draw {
    rhi::ScissorRect scissor;
    std::uint32_t texture = kNoIndex;  // a descriptor index, when own is null
    const Texture* own = nullptr;      // one of the backend's textures (kept by the frame's owners): exempt, or an atlas page
    int mip = -1;
    std::uint32_t indexCount = 0;
    std::uint32_t indexOffset = 0;   // bytes
    std::uint32_t vertexOffset = 0;  // bytes
};

// A staging region of the runtime's frame arena, with a chunk of its own.
using Staging = std::shared_ptr<const FrameArena::Region>;

// What one execution of the passes records: immutable once Commit stored it.
struct Frame {
    std::shared_ptr<const RasterProgram> program;
    Staging staging;
    std::vector<Upload> uploads;
    std::vector<Draw> draws;
    std::uint32_t sampler = kNoIndex;
    float scale[2]{};
    float translate[2]{};
    std::uint32_t width = 0, height = 0;
    std::vector<std::shared_ptr<const void>> owners;  // the textures and descriptors the draws read
};

// The frames Commit stored, one per run; each pass takes its run's from its own slots.
using Slots = RunSlots<Frame>;

// The font atlas pages, a fixed set of versioned RGBA8 textures. Each ImGui texture is a version of one page.
// An unused page holds a 1x1 placeholder. A new version is a pending version of the next frame revision, adopted with
// the stages that declare the page.
struct Atlas {
    std::array<std::shared_ptr<VersionedTexture>, kAtlasPages> pages;
    std::array<std::shared_ptr<Texture>, kAtlasPages> placeholders;  // each page's own: a version's backing is its page's alone
};

// What the passes share with Commit: the run slots, and the atlas (once made).
struct Shared {
    std::shared_ptr<Slots> uploadSlots = std::make_shared<Slots>();
    std::shared_ptr<Slots> drawSlots = std::make_shared<Slots>();
    std::shared_ptr<const Atlas> atlas;  // render thread: set once, before any pass is made
};

// A pending texture upload's pixels: where they are now, before Commit copies them.
struct PendingRegion {
    Region region;
    const std::byte* source = nullptr;
    std::uint32_t sourcePitch = 0;
};

struct PendingUpload {
    std::shared_ptr<Texture> texture;
    bool whole = false;
    std::vector<PendingRegion> regions;
};

struct State {
    HostDesc host;
    bool installed = false;
    bool callbackLogged = false;
    std::unordered_map<std::uint64_t, std::shared_ptr<Texture>> textures;  // ImGui's, by ImTextureID
    std::vector<std::weak_ptr<Texture>> made;                              // MakeTexture's, until uploaded
    std::array<std::shared_ptr<Texture>, kAtlasPages> pages;               // the ImGui texture each page holds, adopted or pending
    bool pagesLogged = false;
    OwnedDescriptorBinding sampler;
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
};

State& Backend()
{
    static State s_state;
    return s_state;
}

void Log(LogLevel level, std::string_view message)
{
    if (const auto& log = Backend().host.log)
        log(level, message);
}

// A failure the call cannot return: the host's error handler, or the log without one.
void Error(std::string_view message)
{
    if (const auto& onError = Backend().host.onError)
        onError(message);
    else
        Log(LogLevel::Error, message);
}

GraphHost* Host()
{
    const auto& graphHost = Backend().host.graphHost;
    return graphHost ? graphHost() : nullptr;
}

// A staging region of at least bytes, which starts at its chunk's first byte. Null, reported, when it cannot be made.
Staging AcquireStaging(std::uint64_t bytes)
{
    auto* runtime = org::Runtime::Current();
    if (!runtime || !runtime->Arena()) {
        Error("a staging region was asked for with no render graph runtime");
        return nullptr;
    }
    try {
        return runtime->Arena()->AcquireOversize((std::max)(bytes, std::uint64_t{ 4 }));
    } catch (const std::exception& e) {
        Error(std::format("a staging region of {} bytes could not be made: {}", bytes, e.what()));
        return nullptr;
    }
}

std::uint64_t IdOf(const Texture* texture) { return kOwnTag | reinterpret_cast<std::uintptr_t>(texture); }

// A persistent graph-only RGBA8 texture. withViews adds the views the exempt class reads through.
std::shared_ptr<Texture> CreateTexture(State& state, std::uint32_t width, std::uint32_t height, std::uint32_t mips, const std::string& name,
    bool withViews)
{
    if (!width || !height)
        return nullptr;
    auto texture = std::make_shared<Texture>();
    texture->width = width;
    texture->height = height;
    texture->mips = mips;
    TextureStorage storage;
    if (state.host.createTexture)
        storage = state.host.createTexture(width, height, mips, name);
    texture->storage = std::move(storage.owner);
    if (!storage.image) {
        Log(LogLevel::Error, std::format("ImGui: the texture {} ({}x{}) could not be created", name, width, height));
        return nullptr;
    }
    texture->resource = storage.image;
    texture->image = std::move(storage.image);
    const std::uint32_t viewCount = withViews ? (mips > 1 ? mips + 1 : 1) : 0;
    for (std::uint32_t v = 0; v < viewCount; ++v) {
        rhi::SrvDesc srv{};
        srv.dimension = rhi::SrvDim::Texture2D;
        srv.formatOverride = rhi::Format::R8G8B8A8_UNorm;
        srv.tex2D.mostDetailedMip = v == 0 ? 0 : v - 1;
        srv.tex2D.mipLevels = v == 0 ? mips : 1;
        OwnedDescriptorBinding view;
        if (state.host.createShaderResourceView)
            view = state.host.createShaderResourceView(texture->image->GetAPIResource(), texture->resource, srv);
        if (!view)
            return nullptr;
        texture->views.push_back(std::move(view));
    }
    return texture;
}

// Makes the atlas once, with each page's placeholder as its first version. Render thread.
bool EnsureAtlas(State& state)
{
    if (state.shared->atlas)
        return true;
    auto atlas = std::make_shared<Atlas>();
    try {
        for (std::size_t page = 0; page < kAtlasPages; ++page) {
            atlas->placeholders[page] = CreateTexture(state, 1, 1, 1, std::format("ImGui::AtlasPage {} placeholder", page), false);
            if (!atlas->placeholders[page])
                return false;
            atlas->pages[page] = VersionedTexture::Create(atlas->placeholders[page]->image, atlas->placeholders[page]);
        }
    } catch (const std::exception& e) {
        Log(LogLevel::Error, std::format("ImGui: the font atlas pages could not be made: {}", e.what()));
        return false;
    }
    state.shared->atlas = std::move(atlas);
    return true;
}

// Makes texture its page's next version, or the page's placeholder when null. The version is pending until a frame
// revision adopts it. Until then, work on a texture the page does not bind is skipped. Render thread.
void SetPage(State& state, std::size_t page, const std::shared_ptr<Texture>& texture)
{
    state.pages[page] = texture;
    if (!state.shared->atlas)
        return;
    const auto& atlas = *state.shared->atlas;
    const auto& backing = texture ? texture : atlas.placeholders[page];
    if (state.host.publishPageVersion)
        state.host.publishPageVersion(atlas.pages[page], backing->image, backing);
}

// One rectangle of ImGui's pixels to stage.
PendingRegion RegionOf(ImTextureData& texture, int x, int y, int width, int height)
{
    PendingRegion region;
    region.region = { 0, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) };
    region.source = static_cast<const std::byte*>(texture.GetPixelsAt(x, y));
    region.sourcePitch = static_cast<std::uint32_t>(texture.GetPitch());
    return region;
}

void DestroyTexture(State& state, ImTextureData& texture)
{
    // Frames in flight own what they draw with, so the texture goes when the last retires. Its page takes the placeholder.
    if (const auto found = state.textures.find(texture.GetTexID()); found != state.textures.end()) {
        if (const std::size_t page = found->second->page; page < kAtlasPages && state.pages[page] == found->second)
            SetPage(state, page, nullptr);
        state.textures.erase(found);
    }
    texture.SetTexID(ImTextureID_Invalid);
    texture.BackendUserData = nullptr;
    texture.SetStatus(ImTextureStatus_Destroyed);
}

// Answers an ImGui texture request. A texture not yet fully uploaded takes no rectangles, since StageWholeTextures restages
// it whole.
void UpdateTexture(State& state, ImTextureData& texture, std::vector<PendingUpload>& uploads)
{
    if (texture.Status == ImTextureStatus_WantCreate) {
        if (texture.Format != ImTextureFormat_RGBA32) {
            Log(LogLevel::Error, std::format("ImGui: texture {} is not RGBA32; not created", texture.UniqueID));
            return;
        }
        // A texture created again keeps its page (a new version of it); a new texture takes a free page.
        const auto previous = state.textures.find(texture.GetTexID());
        std::size_t page = previous != state.textures.end() ? previous->second->page : 0;
        if (previous == state.textures.end())
            while (page < kAtlasPages && state.pages[page])
                ++page;
        if (page >= kAtlasPages) {
            if (!std::exchange(state.pagesLogged, true))
                Log(LogLevel::Warning, std::format("ImGui: texture {} needs more than {} atlas pages; it is not drawn while they are taken",
                    texture.UniqueID, kAtlasPages));
            return;  // asked for again next frame
        }
        auto created = CreateTexture(state, static_cast<std::uint32_t>(texture.Width), static_cast<std::uint32_t>(texture.Height), 1,
            std::format("ImGui::Texture {}", texture.UniqueID), false);
        if (!created)
            return;  // asked for again next frame
        created->page = page;
        SetPage(state, page, created);
        if (previous != state.textures.end())
            state.textures.erase(previous);
        const auto id = IdOf(created.get());
        texture.SetTexID(id);
        texture.BackendUserData = created.get();
        state.textures[id] = std::move(created);
        texture.SetStatus(ImTextureStatus_OK);
    } else if (texture.Status == ImTextureStatus_WantUpdates) {
        const auto found = state.textures.find(texture.GetTexID());
        if (found == state.textures.end())
            return;
        if (found->second->uploaded.load(std::memory_order_acquire)) {
            PendingUpload upload{ found->second };
            for (const ImTextureRect& r : texture.Updates)
                upload.regions.push_back(RegionOf(texture, r.x, r.y, r.w, r.h));
            uploads.push_back(std::move(upload));
        }
        texture.SetStatus(ImTextureStatus_OK);
    }
    if (texture.Status == ImTextureStatus_WantDestroy && texture.UnusedFrames > 0)
        DestroyTexture(state, texture);
}

// Restages each texture's whole contents until a recorded upload pass copied them, since a page copy needs the run to bind
// that version.
void StageWholeTextures(State& state, ImDrawData& drawData, std::vector<PendingUpload>& uploads)
{
    if (drawData.Textures)
        for (ImTextureData* data : *drawData.Textures) {
            if (data->Status != ImTextureStatus_OK || !data->BackendUserData || !data->GetPixels())
                continue;
            const auto found = state.textures.find(data->GetTexID());
            if (found == state.textures.end() || found->second->uploaded.load(std::memory_order_acquire))
                continue;
            PendingUpload upload{ found->second, true };
            upload.regions.push_back(RegionOf(*data, 0, 0, data->Width, data->Height));
            uploads.push_back(std::move(upload));
        }
    std::erase_if(state.made, [&](const std::weak_ptr<Texture>& weak) {
        const auto texture = weak.lock();
        if (!texture)
            return true;
        if (texture->uploaded.load(std::memory_order_acquire)) {
            // In the shader-read state from now on, and never written again.
            texture->pixels.clear();
            texture->pixels.shrink_to_fit();
            return true;
        }
        PendingUpload upload{ texture, true };
        for (std::uint32_t m = 0; m < texture->pixels.size(); ++m) {
            const std::uint32_t width = std::max(1u, texture->width >> m), height = std::max(1u, texture->height >> m);
            upload.regions.push_back({ { m, 0, 0, width, height }, texture->pixels[m].data(), width * 4 });
        }
        uploads.push_back(std::move(upload));
        return false;
    });
}

// Whether a run bound texture to its page: the handle its page's declaration resolves to is the texture's.
bool Bound(const rhi::ResourceHandle& bound, const Texture& texture)
{
    const auto own = texture.image->GetAPIResource().GetHandle();
    return bound.index == own.index && bound.generation == own.generation;
}

struct PassFrame {
    std::shared_ptr<const Slots> slots;
    std::shared_ptr<const std::string> name;  // the draw pass's debug label
};

// An exempt texture's barrier into a copy destination, or from there into the shader-read state it rests in.
rhi::TextureBarrier ExemptBarrier(const Texture& texture, bool toCopy)
{
    rhi::TextureBarrier barrier{};
    barrier.texture = texture.image->GetAPIResource().GetHandle();
    barrier.range = { 0, texture.mips, 0, 1, 0, 1 };
    if (toCopy) {
        barrier.afterSync = rhi::ResourceSyncState::Copy;
        barrier.afterAccess = rhi::ResourceAccessType::CopyDest;
        barrier.afterLayout = rhi::ResourceLayout::CopyDest;
        barrier.discard = true;
    } else {
        barrier.beforeSync = rhi::ResourceSyncState::Copy;
        barrier.beforeAccess = rhi::ResourceAccessType::CopyDest;
        barrier.beforeLayout = rhi::ResourceLayout::CopyDest;
        barrier.afterSync = rhi::ResourceSyncState::All;
        barrier.afterAccess = rhi::ResourceAccessType::ShaderResource;
        barrier.afterLayout = rhi::ResourceLayout::ShaderResource;
    }
    return barrier;
}

struct UploadBindings {
    std::vector<ResourceBindingToken> pages;  // by page
};

// The frame's texture copies. It declares every atlas page as a copy destination and skips copies into a page bound to
// another version. Exempt textures are copied between their own barriers. Both passes declare every page, so draws follow
// the copies.
class UploadPass final : public TypedRenderGraphPass<UploadPass, PassFrame, UploadBindings> {
public:
    UploadPass(std::shared_ptr<const Shared> shared, std::shared_ptr<const Atlas> atlas) :
        m_shared(std::move(shared)), m_atlas(std::move(atlas)) {}

    UploadBindings Declare(PassBuilder& builder)
    {
        builder.PreferQueue(QueueKind::Graphics);
        UploadBindings bindings;
        for (const auto& page : m_atlas->pages)
            bindings.pages.push_back(builder.CopyDestination(*page).Resource());
        return bindings;
    }

    // Record reads the run's slot, which the commit stores. It is recorded at execution, so this value never changes.
    void Revision(std::vector<std::uint64_t>& out) const { out.push_back(0); }

    PassFrame Prepare(const UploadBindings&, const PassPrepareContext&) const { return { m_shared->uploadSlots, nullptr }; }

    static void Record(const UploadBindings& bindings, const PassFrame& pass, PassRecordContext& recording)
    {
        if (!pass.slots)
            return;
        const auto frame = pass.slots->Take(recording.Run());
        if (!frame || !frame->staging || frame->uploads.empty())
            return;
        // The staging buffer and the textures stay until this recording's GPU work retires.
        recording.Retain(frame);
        auto& commands = recording.Commands();
        const auto staging = frame->staging->buffer->GetAPIResource().GetHandle();
        for (const auto& upload : frame->uploads) {
            const bool exempt = upload.texture->exempt;
            rhi::ResourceHandle texture{};
            if (exempt) {
                const auto barrier = ExemptBarrier(*upload.texture, true);
                commands.Barriers(rhi::BarrierBatch{ { &barrier, 1 }, {}, {} });
                texture = barrier.texture;
            } else {
                const std::size_t page = upload.texture->page;
                if (page >= bindings.pages.size())
                    continue;
                texture = recording.Resolve(bindings.pages[page]).GetHandle();
                if (!Bound(texture, *upload.texture))
                    continue;
            }
            for (const auto& region : upload.regions) {
                rhi::BufferTextureCopyFootprint copy{};
                copy.texture = texture;
                copy.buffer = staging;
                copy.mip = region.mip;
                copy.arraySlice = 0;
                copy.x = region.x;
                copy.y = region.y;
                copy.footprint = { frame->staging->offset + region.offset, region.rowPitch, region.width, region.height, 1 };
                commands.CopyBufferToTexture(copy);
            }
            if (exempt) {
                const auto barrier = ExemptBarrier(*upload.texture, false);
                commands.Barriers(rhi::BarrierBatch{ { &barrier, 1 }, {}, {} });
            }
            if (upload.whole)
                upload.texture->uploaded.store(true, std::memory_order_release);
        }
    }

private:
    std::shared_ptr<const Shared> m_shared;
    std::shared_ptr<const Atlas> m_atlas;
};

struct DrawBindings {
    DeclaredViewToken target;
    std::vector<DeclaredViewToken> pages;  // by page
};

// The frame's draws. It reads atlas pages through its declarations and skips a texture its page does not bind this run.
// Exempt textures are read through their own views once their upload was recorded.
class OverlayPass final : public TypedRenderGraphPass<OverlayPass, PassFrame, DrawBindings> {
public:
    OverlayPass(DrawTarget target, std::shared_ptr<const Shared> shared, std::shared_ptr<const Atlas> atlas, std::string name) :
        m_target(std::move(target)), m_shared(std::move(shared)), m_atlas(std::move(atlas)), m_name(std::make_shared<const std::string>(std::move(name))) {}

    DrawBindings Declare(PassBuilder& builder)
    {
        DrawBindings bindings{ builder.RenderTarget(m_target).View(), {} };
        for (const auto& page : m_atlas->pages)
            bindings.pages.push_back(builder.ShaderResource(*page).View());
        return bindings;
    }

    // Record reads the run's slot, which the commit stores. It is recorded at execution, so this value never changes.
    void Revision(std::vector<std::uint64_t>& out) const { out.push_back(0); }

    PassFrame Prepare(const DrawBindings&, const PassPrepareContext&) const { return { m_shared->drawSlots, m_name }; }

    static void Record(const DrawBindings& bindings, const PassFrame& pass, PassRecordContext& recording)
    {
        if (!pass.slots)
            return;
        const auto frame = pass.slots->Take(recording.Run());
        if (!frame || !frame->program || !frame->staging)
            return;
        // The staging buffer, textures and descriptors stay until this recording's GPU work retires.
        recording.Retain(frame);
        if (frame->draws.empty())
            return;

        auto& commands = recording.Commands();
        rhi::ColorAttachment colour{};
        colour.rtv = recording.View(bindings.target);
        colour.resource = recording.Resolve(bindings.target).GetHandle();
        colour.loadOp = rhi::LoadOp::Load;
        colour.storeOp = rhi::StoreOp::Store;
        rhi::PassBeginInfo begin{};
        begin.colors = { &colour, 1 };
        begin.width = frame->width;
        begin.height = frame->height;
        begin.debugName = pass.name ? pass.name->c_str() : "ImGui";
        commands.BeginPass(begin);
        commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
        commands.BindLayout(frame->program->layout->GetHandle());
        commands.BindPipeline(frame->program->Pipeline(recording));
        DrawConstants constants{};
        constants.geometryIndex = frame->staging->srvIndex;
        constants.indexSize = sizeof(ImDrawIdx);
        constants.samplerIndex = frame->sampler;
        std::memcpy(constants.scale, frame->scale, sizeof(constants.scale));
        std::memcpy(constants.translate, frame->translate, sizeof(constants.translate));
        // What each page binds this run.
        std::array<rhi::ResourceHandle, kAtlasPages> bound{};
        for (std::size_t page = 0; page < bindings.pages.size() && page < kAtlasPages; ++page)
            bound[page] = recording.Resolve(bindings.pages[page]).GetHandle();
        for (const auto& draw : frame->draws) {
            std::uint32_t textureIndex = draw.texture;
            if (draw.own && draw.own->exempt) {
                const auto& views = draw.own->views;
                const std::size_t view = draw.mip < 0 || views.size() == 1 ? 0 : std::min<std::size_t>(std::size_t(draw.mip) + 1, views.size() - 1);
                textureIndex = views[view].Index();
            } else if (draw.own) {
                const std::size_t page = draw.own->page;
                if (page >= bindings.pages.size() || !Bound(bound[page], *draw.own))
                    continue;
                textureIndex = recording.Index(bindings.pages[page]);
            }
            commands.SetScissors({ &draw.scissor, 1 });
            constants.vertexOffset = draw.vertexOffset;
            constants.indexOffset = draw.indexOffset;
            constants.textureIndex = textureIndex;
            commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, kConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
            commands.Draw(draw.indexCount, 1, 0, 0);
        }
        commands.EndPass();
    }

private:
    DrawTarget m_target;
    std::shared_ptr<const Shared> m_shared;
    std::shared_ptr<const Atlas> m_atlas;
    std::shared_ptr<const std::string> m_name;
};

}

void SetHost(HostDesc host) { Backend().host = std::move(host); }

RasterProgram::Desc ProgramDesc(rhi::Format format)
{
    RasterProgram::Desc desc{};
    desc.vertexSource = kShader;
    desc.vertexEntry = L"VSMain";
    desc.pixelSource = kShader;
    desc.pixelEntry = L"PSMain";
    desc.constantWords = kConstantWords;
    // imgui_impl_dx11's blending: colour over, alpha accumulated towards opaque.
    RasterProgram::Target target{ .format = format };
    target.blend.enable = true;
    target.blend.srcColor = rhi::BlendFactor::SrcAlpha;
    target.blend.dstColor = rhi::BlendFactor::InvSrcAlpha;
    target.blend.srcAlpha = rhi::BlendFactor::One;
    target.blend.dstAlpha = rhi::BlendFactor::InvSrcAlpha;
    desc.targets = { target };
    return desc;
}

void Init()
{
    auto& state = Backend();
    auto& io = ImGui::GetIO();
    io.BackendRendererName = kBackendName;
    io.BackendRendererUserData = &state;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    state.installed = true;
    Log(LogLevel::Info, std::format("ImGui: drawn by the render graph ({})", kBackendName));
}

void InvalidateTextures()
{
    auto& state = Backend();
    if (!state.installed)
        return;
    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
        if (texture->RefCount == 1 && texture->BackendUserData)
            DestroyTexture(state, *texture);
    state.textures.clear();
    if (state.shared->atlas)
        for (std::size_t page = 0; page < kAtlasPages; ++page)
            if (state.pages[page])
                SetPage(state, page, nullptr);
}

void Shutdown()
{
    auto& state = Backend();
    if (!state.installed)
        return;
    InvalidateTextures();
    state.made.clear();
    state.sampler = {};
    state.shared = std::make_shared<Shared>();
    state.pages = {};
    state.pagesLogged = false;
    auto& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    state.installed = false;
}

bool Installed() { return Backend().installed; }

bool IsOwnTexture(std::uint64_t texture) { return (texture & kOwnTag) != 0; }

std::shared_ptr<Texture> MakeTexture(std::uint32_t width, std::uint32_t height, std::span<const std::span<const std::byte>> levels, const char* name)
{
    auto& state = Backend();
    if (levels.empty())
        return nullptr;
    for (std::uint32_t m = 0; m < levels.size(); ++m) {
        const std::uint32_t levelWidth = std::max(1u, width >> m), levelHeight = std::max(1u, height >> m);
        if (levels[m].size() < std::size_t(levelWidth) * levelHeight * 4) {
            Log(LogLevel::Error, std::format("ImGui: {} level {} has {} bytes, not {}", name, m, levels[m].size(), std::size_t(levelWidth) * levelHeight * 4));
            return nullptr;
        }
    }
    auto texture = CreateTexture(state, width, height, static_cast<std::uint32_t>(levels.size()), name, true);
    if (!texture)
        return nullptr;
    // Uploaded once by the upload pass between its own barriers, then only read: the exempt class, never declared.
    texture->exempt = true;
    texture->resource->SetGraphOwnership(Resource::GraphOwnership::ExternalImmutableShaderResource);
    if (state.host.noteImmutable)
        state.host.noteImmutable(texture->resource);
    for (const auto& level : levels)
        texture->pixels.emplace_back(level.begin(), level.end());
    state.made.push_back(texture);
    return texture;
}

Binding BindingOf(const std::shared_ptr<Texture>& texture, int mip)
{
    // An exempt texture is read through its own views; ImGui's are read through the passes' declarations.
    if (!texture || (texture->exempt && texture->views.empty()))
        return {};
    return { kNoIndex, texture, texture, mip };
}

bool Commit(ImDrawData* drawData, const Target& target, const Resolver& resolve)
{
    auto& state = Backend();
    auto* host = Host();
    if (!state.installed || !host || !drawData)
        return false;
    if (!target.program || !EnsureAtlas(state))
        return false;
    if (!state.sampler) {
        // imgui_impl_dx11's: bilinear, clamped, the top level only (an icon's view starts at the level it wants).
        rhi::SamplerDesc sampler{};
        sampler.maxLod = 0.0f;
        if (state.host.createSampler)
            state.sampler = state.host.createSampler(sampler);
        if (!state.sampler)
            return false;
    }

    // ImGui's texture requests.
    std::vector<PendingUpload> pending;
    if (drawData->Textures)
        for (ImTextureData* texture : *drawData->Textures)
            if (texture->Status != ImTextureStatus_OK)
                UpdateTexture(state, *texture, pending);

    auto frame = std::make_shared<Frame>();
    // Resolve every texture the draws name first, since the resolver may make textures whose first upload is this frame's.
    struct Resolved {
        std::uint32_t index = kNoIndex;
        const Texture* own = nullptr;
        int mip = -1;
    };
    std::unordered_map<std::uint64_t, Resolved> resolved;
    for (const ImDrawList* list : drawData->CmdLists)
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.UserCallback || !command.ElemCount)
                continue;
            const std::uint64_t id = command.GetTexID();
            if (resolved.contains(id))
                continue;
            Binding binding;
            if (IsOwnTexture(id)) {
                if (const auto texture = state.textures.find(id); texture != state.textures.end())
                    binding = BindingOf(texture->second, -1);
            } else if (resolve) {
                binding = resolve(id);
            }
            resolved.emplace(id, Resolved{ binding.index, binding.texture.get(), binding.mip });
            if (binding.owner)
                frame->owners.push_back(std::move(binding.owner));
        }
    // The whole contents of the textures not copied yet (MakeTexture's included).
    StageWholeTextures(state, *drawData, pending);
    // Draws may read only textures already uploaded, or uploaded by this run's pass.
    std::unordered_set<const Texture*> staged;
    for (const auto& upload : pending)
        if (upload.whole)
            staged.insert(upload.texture.get());
    for (auto& entry : resolved)
        if (const Texture* own = entry.second.own; own && !own->uploaded.load(std::memory_order_acquire) && !staged.contains(own))
            entry.second = {};

    // The staging buffer: vertices, indices, then the uploads' rows.
    const std::uint64_t vertexBytes = std::uint64_t(drawData->TotalVtxCount) * sizeof(ImDrawVert);
    const std::uint64_t indexBase = AlignUp(vertexBytes, 4);
    std::uint64_t bytes = AlignUp(indexBase + std::uint64_t(drawData->TotalIdxCount) * sizeof(ImDrawIdx), kPlacementAlignment);
    for (auto& upload : pending)
        for (auto& region : upload.regions) {
            region.region.rowPitch = static_cast<std::uint32_t>(AlignUp(std::uint64_t(region.region.width) * 4, kRowAlignment));
            region.region.offset = bytes;
            bytes = AlignUp(bytes + std::uint64_t(region.region.rowPitch) * region.region.height, kPlacementAlignment);
        }

    frame->staging = AcquireStaging(bytes);
    if (!frame->staging)
        return false;
    std::byte* const base = frame->staging->mapped;
    for (auto& upload : pending) {
        Upload copy{ upload.texture, upload.whole, {} };
        for (const auto& region : upload.regions) {
            const std::size_t rowBytes = std::size_t(region.region.width) * 4;
            for (std::uint32_t row = 0; row < region.region.height; ++row)
                std::memcpy(base + region.region.offset + std::size_t(row) * region.region.rowPitch, region.source + std::size_t(row) * region.sourcePitch, rowBytes);
            copy.regions.push_back(region.region);
        }
        frame->uploads.push_back(std::move(copy));
    }

    const ImVec2 position = drawData->DisplayPos;
    const ImVec2 size = drawData->DisplaySize;
    const ImVec2 scale = drawData->FramebufferScale;
    frame->program = target.program;
    frame->sampler = state.sampler.Index();
    frame->owners.push_back(state.sampler.Owner());
    frame->width = target.width;
    frame->height = target.height;
    // imgui_impl_dx11's projection: the display rectangle onto clip space, y down.
    frame->scale[0] = 2.0f / size.x;
    frame->scale[1] = -2.0f / size.y;
    frame->translate[0] = -1.0f - position.x * frame->scale[0];
    frame->translate[1] = 1.0f - position.y * frame->scale[1];

    std::uint64_t vertexCursor = 0, indexCursor = indexBase;
    std::uint32_t globalVertex = 0, globalIndex = 0;
    for (const ImDrawList* list : drawData->CmdLists) {
        std::memcpy(base + vertexCursor, list->VtxBuffer.Data, std::size_t(list->VtxBuffer.Size) * sizeof(ImDrawVert));
        std::memcpy(base + indexCursor, list->IdxBuffer.Data, std::size_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
        vertexCursor += std::uint64_t(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        indexCursor += std::uint64_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.UserCallback) {
                if (command.UserCallback != ImDrawCallback_ResetRenderState && !std::exchange(state.callbackLogged, true))
                    Log(LogLevel::Warning, "ImGui: a draw callback was skipped (callbacks are D3D11 code); the draws around it are drawn");
                continue;
            }
            const float minX = std::max((command.ClipRect.x - position.x) * scale.x, 0.0f);
            const float minY = std::max((command.ClipRect.y - position.y) * scale.y, 0.0f);
            const float maxX = std::min((command.ClipRect.z - position.x) * scale.x, static_cast<float>(target.width));
            const float maxY = std::min((command.ClipRect.w - position.y) * scale.y, static_cast<float>(target.height));
            if (maxX <= minX || maxY <= minY || !command.ElemCount)
                continue;
            const auto found = resolved.find(command.GetTexID());
            if (found == resolved.end() || (!found->second.own && found->second.index == kNoIndex))
                continue;
            Draw draw;
            // As D3D11 truncates its scissor rectangle's corners.
            draw.scissor.x = static_cast<std::int32_t>(minX);
            draw.scissor.y = static_cast<std::int32_t>(minY);
            draw.scissor.width = static_cast<std::uint32_t>(static_cast<std::int32_t>(maxX) - draw.scissor.x);
            draw.scissor.height = static_cast<std::uint32_t>(static_cast<std::int32_t>(maxY) - draw.scissor.y);
            if (!draw.scissor.width || !draw.scissor.height)
                continue;
            draw.texture = found->second.index;
            draw.own = found->second.own;
            draw.mip = found->second.mip;
            draw.indexCount = command.ElemCount;
            draw.indexOffset = static_cast<std::uint32_t>(indexBase + std::uint64_t(globalIndex + command.IdxOffset) * sizeof(ImDrawIdx));
            draw.vertexOffset = static_cast<std::uint32_t>(std::uint64_t(globalVertex + command.VtxOffset) * sizeof(ImDrawVert));
            frame->draws.push_back(draw);
        }
        globalVertex += static_cast<std::uint32_t>(list->VtxBuffer.Size);
        globalIndex += static_cast<std::uint32_t>(list->IdxBuffer.Size);
    }
    const auto run = host->CurrentRun();
    state.shared->uploadSlots->Store(run, frame);
    state.shared->drawSlots->Store(run, std::move(frame));
    return true;
}

std::shared_ptr<RenderPass> MakePass(DrawTarget target)
{
    auto& state = Backend();
    if (!target || !Host() || !EnsureAtlas(state))
        return nullptr;
    // The draw list is the run's, so it records after the commit that stores it.
    return std::make_shared<OverlayPass>(std::move(target), state.shared, state.shared->atlas, state.host.passName);
}

std::shared_ptr<RenderPass> MakeUploadPass()
{
    auto& state = Backend();
    if (!Host() || !EnsureAtlas(state))
        return nullptr;
    return std::make_shared<UploadPass>(state.shared, state.shared->atlas);
}

}
