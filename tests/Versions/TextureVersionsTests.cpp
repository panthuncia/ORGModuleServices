// Texture versions (MakeTextureVersions) on WARP: made pending, zero-filled on the uploader's copy queue, named, and resolved
// as a revision fragment once the fills completed.
//
// The fills go through the runtime's real upload service, so these tests also cover the uploader's texture zero fill
// (IUploadService::QueueTrackedStreamingTextureZeroFill).

#include <ORGModuleServices/VersionedTextureGrowth.h>

#include "OpenRenderGraph/Runtime.h"
#include "Render/Runtime/IUploadService.h"
#include "Resources/PixelBuffer.h"
#include "Resources/ResourceStateTracker.h"
#include "Resources/VersionedTexture.h"

#include <rhi.h>
#include <rhi_helpers.h>
#include <rhi_interop_dx12.h>
#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#define CHECK(...)                                                                          \
    do {                                                                                    \
        if (!(__VA_ARGS__)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);     \
            return __LINE__;                                                                \
        }                                                                                   \
    } while (false)

namespace {

using org::services::MakeTextureVersions;
using org::services::NamedVersions;
using org::services::TextureContents;
using org::services::TextureVersionRequest;
using org::services::TextureVersionsMade;

struct Context {
    rhi::DevicePtr* device = nullptr;
    std::shared_ptr<org::runtime::IUploadService> uploads;
};

std::shared_ptr<org::PixelBuffer> MakePixels(uint32_t width, uint32_t height, rhi::Format format, uint32_t mips,
    rhi::QueueSharing sharing, const char* name) {
    org::TextureDescription description{};
    description.format = format;
    description.channels = 4;
    description.hasSRV = true;
    description.srvFormat = format;
    description.queueSharing = sharing;
    for (uint32_t mip = 0; mip < mips; ++mip) description.imageDimensions.push_back({(std::max)(1u, width >> mip), (std::max)(1u, height >> mip), 0, 0});
    auto pixels = org::PixelBuffer::CreateShared(description);
    pixels->SetName(name);
    return pixels;
}

// The fragment settles from the uploader's notifications. The test polls only to see it happen.
bool WaitSettled(const std::shared_ptr<const org::async::RevisionFragment>& fragment, int ms = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (fragment->GetState() == org::async::RevisionFragment::State::Pending) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

struct Footprints {
    std::vector<rhi::CopyableFootprint> subresources;
    uint64_t bytes = 0;
};

Footprints FootprintsOf(rhi::Device device, rhi::ResourceHandle texture, uint32_t mips) {
    Footprints out;
    out.subresources.resize(mips);
    rhi::FootprintRangeDesc range{};
    range.texture = texture;
    range.mipCount = mips;
    out.bytes = device.GetCopyableFootprints(range, out.subresources.data(), mips).totalBytes;
    return out;
}

// Graphics queue: fills every mip of `texture` with `value` bytes and leaves it in Common, then waits for the device.
int Prefill(Context& c, org::Resource& texture, uint32_t mips, uint8_t value) {
    auto& device = *c.device;
    const auto handle = texture.GetAPIResource().GetHandle();
    const auto footprints = FootprintsOf(device.Get(), handle, mips);
    CHECK(footprints.bytes);
    rhi::ResourcePtr upload;
    CHECK(!rhi::Failed(device->CreateCommittedResource(
        rhi::helpers::ResourceDesc::Buffer(footprints.bytes, rhi::HeapType::Upload, {}, "TextureVersionsTests prefill"), upload)));
    void* mapped = nullptr;
    upload->Map(&mapped, 0, footprints.bytes);
    CHECK(mapped);
    std::memset(mapped, value, static_cast<size_t>(footprints.bytes));
    upload->Unmap(0, footprints.bytes);
    rhi::CommandAllocatorPtr allocator;
    rhi::CommandListPtr list;
    CHECK(!rhi::Failed(device->CreateCommandAllocator(rhi::QueueKind::Graphics, allocator)));
    CHECK(!rhi::Failed(device->CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), list)));
    rhi::TextureBarrier barrier{};
    barrier.texture = handle;
    barrier.range = {0, mips, 0, 1};
    barrier.afterSync = rhi::ResourceSyncState::Copy;
    barrier.afterAccess = rhi::ResourceAccessType::CopyDest;
    barrier.beforeLayout = rhi::ResourceLayout::Undefined;
    barrier.afterLayout = rhi::ResourceLayout::CopyDest;
    barrier.discard = true;
    list->Barriers({.textures = {&barrier, 1}});
    for (uint32_t mip = 0; mip < mips; ++mip) {
        rhi::BufferTextureCopyFootprint copy{};
        copy.texture = handle;
        copy.buffer = upload->GetHandle();
        copy.mip = mip;
        copy.footprint = footprints.subresources[mip];
        list->CopyBufferToTexture(copy);
    }
    barrier.beforeSync = rhi::ResourceSyncState::Copy;
    barrier.afterSync = rhi::ResourceSyncState::All;
    barrier.beforeAccess = rhi::ResourceAccessType::CopyDest;
    barrier.afterAccess = rhi::ResourceAccessType::Common;
    barrier.beforeLayout = rhi::ResourceLayout::CopyDest;
    barrier.afterLayout = rhi::ResourceLayout::Common;
    barrier.discard = false;
    list->Barriers({.textures = {&barrier, 1}});
    list->End();
    const rhi::CommandList lists[] = {list.Get()};
    CHECK(!rhi::Failed(device->GetQueue(rhi::QueueKind::Graphics).Submit(lists)));
    CHECK(!rhi::Failed(device->WaitIdle()));
    return 0;
}

// Graphics queue, after the fills completed: reads every mip of `texture` (left in Common by the fill) and counts the bytes
// of its rows that are not zero.
int CountNonZero(Context& c, org::Resource& texture, uint32_t mips, uint32_t bytesPerTexel, uint64_t& nonZero) {
    auto& device = *c.device;
    const auto handle = texture.GetAPIResource().GetHandle();
    const auto footprints = FootprintsOf(device.Get(), handle, mips);
    CHECK(footprints.bytes);
    rhi::ResourcePtr readback;
    CHECK(!rhi::Failed(device->CreateCommittedResource(
        rhi::helpers::ResourceDesc::Buffer(footprints.bytes, rhi::HeapType::Readback, {}, "TextureVersionsTests readback"), readback)));
    rhi::CommandAllocatorPtr allocator;
    rhi::CommandListPtr list;
    CHECK(!rhi::Failed(device->CreateCommandAllocator(rhi::QueueKind::Graphics, allocator)));
    CHECK(!rhi::Failed(device->CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), list)));
    rhi::TextureBarrier barrier{};
    barrier.texture = handle;
    barrier.range = {0, mips, 0, 1};
    barrier.beforeSync = rhi::ResourceSyncState::All;
    barrier.afterSync = rhi::ResourceSyncState::Copy;
    barrier.beforeAccess = rhi::ResourceAccessType::Common;
    barrier.afterAccess = rhi::ResourceAccessType::CopySource;
    barrier.beforeLayout = rhi::ResourceLayout::Common;
    barrier.afterLayout = rhi::ResourceLayout::CopySource;
    list->Barriers({.textures = {&barrier, 1}});
    for (uint32_t mip = 0; mip < mips; ++mip) {
        rhi::BufferTextureCopyFootprint copy{};
        copy.texture = handle;
        copy.buffer = readback->GetHandle();
        copy.mip = mip;
        copy.footprint = footprints.subresources[mip];
        list->CopyTextureToBuffer(copy);
    }
    list->End();
    const rhi::CommandList lists[] = {list.Get()};
    CHECK(!rhi::Failed(device->GetQueue(rhi::QueueKind::Graphics).Submit(lists)));
    CHECK(!rhi::Failed(device->WaitIdle()));
    void* mapped = nullptr;
    readback->Map(&mapped, 0, footprints.bytes);
    CHECK(mapped);
    nonZero = 0;
    for (const auto& footprint : footprints.subresources) {
        for (uint32_t row = 0; row < footprint.height * footprint.depth; ++row) {
            const auto* bytes = static_cast<const uint8_t*>(mapped) + footprint.offset + uint64_t{row} * footprint.rowPitch;
            for (uint32_t i = 0; i < footprint.width * bytesPerTexel; ++i) nonZero += bytes[i] != 0;
        }
    }
    readback->Unmap(0, 0);
    return 0;
}

bool TrackerIsCommon(org::Resource& texture) {
    const auto* tracker = texture.GetStateTracker();
    if (!tracker) return false;
    for (const auto& segment : tracker->GetSegments())
        if (segment.state.layout != rhi::ResourceLayout::Common) return false;
    return true;
}

// A caller-made image, zero-filled in bands (its mip 0 is larger than the uploader's zero source), with a second mip.
int TestMakeFillResolve(Context& c) {
    constexpr uint32_t kWidth = 1024, kHeight = 1100, kMips = 2;
    auto slot = org::VersionedTexture::CreateImage(
        MakePixels(256, 256, rhi::Format::R32G32B32A32_Float, kMips, rhi::QueueSharing::Concurrent, "TextureVersionsTests first"));
    const auto first = slot->Current();
    auto image = MakePixels(kWidth, kHeight, rhi::Format::R32G32B32A32_Float, kMips, rhi::QueueSharing::Concurrent, "TextureVersionsTests next");
    if (const int line = Prefill(c, *image, kMips, 0xAB)) return line;
    uint64_t before = 0;
    if (const int line = CountNonZero(c, *image, kMips, 16, before)) return line;
    CHECK(before != 0);

    auto owner = std::make_shared<int>(7);
    TextureVersionRequest request;
    request.slot = slot;
    request.image = image;
    request.contents = TextureContents::Zero;
    request.owner = owner;
    const auto made = MakeTextureVersions({&request, 1}, c.uploads);
    CHECK(made.versions.size() == 1 && made.named && made.ready && made.fragment);
    const auto& version = made.versions[0];
    CHECK(version->resource == image && version->owner == owner);
    // Pending: named, not current.
    CHECK(slot->Current() == first);
    CHECK(made.named->FindTexture(*slot) == version);
    CHECK(made.named->TextureCount() == 1);
    // The fill told the graph where it leaves the texture.
    CHECK(TrackerIsCommon(*image));
    CHECK(WaitSettled(made.fragment));
    CHECK(made.fragment->GetState() == org::async::RevisionFragment::State::Ready);
    CHECK(made.fragment->Value<NamedVersions>() == made.named);
    CHECK(made.ready->Complete() && !made.ready->Failed());
    uint64_t after = 0;
    if (const int line = CountNonZero(c, *image, kMips, 16, after)) return line;
    CHECK(after == 0);
    // Still pending until the owner adopts the set.
    CHECK(slot->Current() == first);
    CHECK(made.named->AdoptAll() == 1);
    CHECK(slot->Current() == version);
    return 0;
}

// A PixelBuffer made like the slot's current one at a new extent, zero-filled. The slot's current one is Exclusive, the new
// one is made Concurrent for its fill.
int TestExtentForm(Context& c) {
    auto slot = org::VersionedTexture::Create(
        MakePixels(16, 16, rhi::Format::R8G8B8A8_UNorm, 1, rhi::QueueSharing::Exclusive, "TextureVersionsTests extent"));
    TextureVersionRequest zero;
    zero.slot = slot;
    zero.extent = {40, 24, 1};
    zero.contents = TextureContents::Zero;
    auto other = org::VersionedTexture::Create(
        MakePixels(8, 8, rhi::Format::R8G8B8A8_UNorm, 1, rhi::QueueSharing::Exclusive, "TextureVersionsTests undefined"));
    TextureVersionRequest undefined;
    undefined.slot = other;
    undefined.extent = {12, 10, 1};
    undefined.name = "TextureVersionsTests renamed";
    const TextureVersionRequest requests[] = {zero, undefined};
    const auto made = MakeTextureVersions(requests, c.uploads);
    CHECK(made.versions.size() == 2 && made.ready);
    const auto& texture = made.versions[0]->texture;
    CHECK(texture && texture->GetWidth() == 40 && texture->GetHeight() == 24);
    CHECK(texture->GetDescription().queueSharing == rhi::QueueSharing::Concurrent);
    CHECK(texture->GetName() == "TextureVersionsTests extent");
    const auto& plain = made.versions[1]->texture;
    CHECK(plain && plain->GetWidth() == 12 && plain->GetDescription().queueSharing == rhi::QueueSharing::Exclusive);
    CHECK(plain->GetName() == "TextureVersionsTests renamed");
    CHECK(made.named->FindTexture(*slot) == made.versions[0] && made.named->FindTexture(*other) == made.versions[1]);
    CHECK(WaitSettled(made.fragment));
    CHECK(made.fragment->Value<NamedVersions>() == made.named);
    uint64_t nonZero = 1;
    if (const int line = CountNonZero(c, *texture, 1, 4, nonZero)) return line;
    CHECK(nonZero == 0);
    CHECK(made.named->AdoptAll() == 2);
    CHECK(slot->Current() == made.versions[0] && other->Current() == made.versions[1]);
    return 0;
}

// Undefined contents queue nothing: the fragment is ready at once.
int TestUndefinedQueuesNothing(Context& c) {
    auto slot = org::VersionedTexture::CreateImage(
        MakePixels(32, 32, rhi::Format::R16G16B16A16_Float, 1, rhi::QueueSharing::Exclusive, "TextureVersionsTests plain"));
    TextureVersionRequest request;
    request.slot = slot;
    request.image = MakePixels(64, 64, rhi::Format::R16G16B16A16_Float, 1, rhi::QueueSharing::Exclusive, "TextureVersionsTests plain next");
    const auto made = MakeTextureVersions({&request, 1}, c.uploads);
    CHECK(!made.ready);
    CHECK(made.fragment && made.fragment->GetState() == org::async::RevisionFragment::State::Ready);
    CHECK(made.fragment->Value<NamedVersions>() == made.named);
    CHECK(slot->Current() != made.versions[0]);
    // No upload service is needed without a fill.
    const auto alone = MakeTextureVersions({&request, 1}, nullptr);
    CHECK(alone.versions.size() == 1 && !alone.ready);
    return 0;
}

// A version of another shape class throws, before anything is queued, and nothing becomes current.
int TestShapeClassChangeThrows(Context& c) {
    auto slot = org::VersionedTexture::CreateImage(
        MakePixels(32, 32, rhi::Format::R8G8B8A8_UNorm, 1, rhi::QueueSharing::Concurrent, "TextureVersionsTests shape"));
    const auto first = slot->Current();
    auto fine = org::VersionedTexture::CreateImage(
        MakePixels(32, 32, rhi::Format::R8G8B8A8_UNorm, 1, rhi::QueueSharing::Concurrent, "TextureVersionsTests shape fine"));
    TextureVersionRequest ok;
    ok.slot = fine;
    ok.image = MakePixels(48, 48, rhi::Format::R8G8B8A8_UNorm, 1, rhi::QueueSharing::Concurrent, "TextureVersionsTests shape fine next");
    ok.contents = TextureContents::Zero;
    TextureVersionRequest format;
    format.slot = slot;
    format.image = MakePixels(32, 32, rhi::Format::R32_Float, 1, rhi::QueueSharing::Concurrent, "TextureVersionsTests other format");
    format.contents = TextureContents::Zero;
    TextureVersionRequest mips = format;
    mips.image = MakePixels(32, 32, rhi::Format::R8G8B8A8_UNorm, 2, rhi::QueueSharing::Concurrent, "TextureVersionsTests other mips");
    for (const auto& bad : {format, mips}) {
        const TextureVersionRequest requests[] = {ok, bad};
        bool threw = false;
        try {
            (void)MakeTextureVersions(requests, c.uploads);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(slot->Current() == first);
    }
    return 0;
}

// What the uploader cannot fill, and malformed requests, throw.
int TestRefusals(Context& c) {
    auto slot = org::VersionedTexture::CreateImage(
        MakePixels(32, 32, rhi::Format::R8G8B8A8_UNorm, 1, rhi::QueueSharing::Exclusive, "TextureVersionsTests refusals"));
    const auto throws = [&](const std::vector<TextureVersionRequest>& requests, std::shared_ptr<org::runtime::IUploadService> uploads) {
        try {
            (void)MakeTextureVersions(requests, uploads);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    TextureVersionRequest exclusive;
    exclusive.slot = slot;
    exclusive.image = MakePixels(32, 32, rhi::Format::R8G8B8A8_UNorm, 1, rhi::QueueSharing::Exclusive, "TextureVersionsTests exclusive");
    exclusive.contents = TextureContents::Zero;
    CHECK(throws({exclusive}, c.uploads));  // the copy queue needs a concurrent texture
    CHECK(throws({exclusive}, nullptr));    // a fill without an uploader
    TextureVersionRequest none;
    CHECK(throws({none}, c.uploads));       // no slot
    TextureVersionRequest twice;
    twice.slot = slot;
    twice.extent = {8, 8, 1};
    CHECK(throws({twice, twice}, c.uploads));
    TextureVersionRequest empty;
    empty.slot = slot;
    CHECK(throws({empty}, c.uploads));      // an empty extent
    return 0;
}

// The graph-work form: what the producer makes, and `done` with it once the fills completed.
int TestWorkForm(Context& c) {
    auto slot = org::VersionedTexture::CreateImage(
        MakePixels(16, 16, rhi::Format::R8G8B8A8_UNorm, 1, rhi::QueueSharing::Concurrent, "TextureVersionsTests work"));
    TextureVersionRequest request;
    request.slot = slot;
    request.extent = {20, 20, 1};
    request.contents = TextureContents::Zero;
    std::shared_ptr<const TextureVersionsMade> delivered;
    auto work = org::services::TextureVersionWork({request}, c.uploads,
        [&](std::shared_ptr<const TextureVersionsMade> made, std::string error) {
            if (error.empty()) delivered = std::move(made);
        });
    auto produced = work.produce();
    CHECK(produced.value && produced.ready);
    const auto made = std::static_pointer_cast<const TextureVersionsMade>(produced.value);
    CHECK(WaitSettled(made->fragment));
    CHECK(produced.ready->Complete());
    work.done(produced.value, {});
    CHECK(delivered == made && delivered->versions.size() == 1);
    return 0;
}

// The fragment settles from notifications alone: ready once the set completes, failed when a copy was cancelled.
int TestFragmentSettling(Context&) {
    auto slot = org::VersionedTexture::CreateImage(
        MakePixels(8, 8, rhi::Format::R8G8B8A8_UNorm, 1, rhi::QueueSharing::Exclusive, "TextureVersionsTests settling"));
    const NamedVersions::TextureEntry entry{slot, slot->Current()};
    const auto named = NamedVersions::Textures({&entry, 1});
    struct Manual {
        bool complete = false, failed = false;
        std::function<void()> notify;
    };
    const auto makeSet = [](const std::shared_ptr<Manual>& manual) {
        auto set = std::make_shared<org::async::GpuSubmissionSet>();
        set->isComplete = [manual] { return manual->complete; };
        set->isFailed = [manual] { return manual->failed; };
        set->failure = [] { return std::string("a version's upload was cancelled"); };
        set->subscribe = [manual](std::function<void()> callback) {
            manual->notify = callback;
            callback();
        };
        return std::shared_ptr<const org::async::GpuSubmissionSet>(std::move(set));
    };
    auto completes = std::make_shared<Manual>();
    const auto ready = org::services::FragmentWhenReady(named, makeSet(completes));
    CHECK(ready->GetState() == org::async::RevisionFragment::State::Pending);
    completes->complete = true;
    completes->notify();
    CHECK(ready->GetState() == org::async::RevisionFragment::State::Ready && ready->Value<NamedVersions>() == named);
    completes->notify();  // a late notification changes nothing
    CHECK(ready->GetState() == org::async::RevisionFragment::State::Ready);
    auto cancels = std::make_shared<Manual>();
    const auto failed = org::services::FragmentWhenReady(named, makeSet(cancels));
    cancels->failed = true;
    cancels->notify();
    CHECK(failed->GetState() == org::async::RevisionFragment::State::Failed && failed->Error());
    auto silent = std::make_shared<org::async::GpuSubmissionSet>();
    bool threw = false;
    try {
        (void)org::services::FragmentWhenReady(named, silent);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    return 0;
}

} // namespace

// WARP (D3D12) by default. --vulkan runs on the default Vulkan device with validation.
int main(int argc, char** argv) {
    const bool vulkan = argc > 1 && std::strcmp(argv[1], "--vulkan") == 0;
    rhi::DeviceCreateInfo create{};
    create.framesInFlight = 2;
    rhi::DevicePtr device;
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> warp;
    if (vulkan) {
        create.backend = rhi::Backend::Vulkan;
        create.enableDebug = true;
        CHECK(!rhi::Failed(rhi::CreateVulkanDevice(create, device)) && device);
    } else {
        CHECK(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
        CHECK(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
        create.backend = rhi::Backend::D3D12;
        create.nativeAdapter = warp.Get();
        CHECK(!rhi::Failed(rhi::CreateD3D12Device(create, device)) && device);
    }
    auto runtime = org::Runtime::Create({.device = device.Get(), .backend = create.backend});
    CHECK(runtime && runtime->Uploads());
    Context context{&device, runtime->Uploads()};
    for (const auto test : {&TestMakeFillResolve, &TestExtentForm, &TestUndefinedQueuesNothing, &TestShapeClassChangeThrows,
             &TestRefusals, &TestWorkForm, &TestFragmentSettling})
        if (const int line = test(context)) return line;
    std::puts("PASS: TextureVersionsTests");
    return 0;
}
