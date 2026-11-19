// Named texture versions (NamedVersions::Textures) and composed sets (ComposedVersions), on WARP.
//
// A preparation binds the version its host data names. These tests resolve through VersionedResource::SelectVersion with such
// host data, as a recording does, so a pending version is bound before it is current.

#include <ORGModuleServices/ComposedVersions.h>
#include <ORGModuleServices/GrowableBufferSet.h>

#include "Interfaces/IResourceResolver.h"
#include "OpenRenderGraph/Runtime.h"
#include "Render/PassExecutionContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Buffers/VersionedBuffer.h"
#include "Resources/PixelBuffer.h"
#include "Resources/VersionedTexture.h"

#include <rhi.h>
#include <rhi_interop_dx12.h>
#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <vector>

#define CHECK(...)                                                                          \
    do {                                                                                    \
        if (!(__VA_ARGS__)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);     \
            return __LINE__;                                                                \
        }                                                                                   \
    } while (false)

namespace {

using org::services::ComposedVersions;
using org::services::NamedVersions;
using Parts = std::initializer_list<std::shared_ptr<const org::IResourceVersions>>;

std::shared_ptr<org::PixelBuffer> MakePixels(uint32_t width) {
    org::TextureDescription description{};
    description.imageDimensions = {{width, 1, 0, 0}};
    description.format = rhi::Format::R32_UInt;
    description.channels = 1;
    return org::PixelBuffer::CreateShared(description);
}

std::shared_ptr<org::Buffer> MakeBuffer() { return org::Buffer::CreateShared(rhi::HeapType::Upload, 256); }

// Host data as a recording request carries it: the revision's versions behind TryGet(typeid(IResourceVersions)).
struct HostData final : org::IHostExecutionData {
    std::shared_ptr<const org::IResourceVersions> versions;
    const void* TryGet(std::type_index type) const noexcept override {
        return type == std::type_index(typeid(org::IResourceVersions)) ? versions.get() : nullptr;
    }
};

std::shared_ptr<const org::ResourceVersion> Selected(const org::VersionedResource& resource, std::shared_ptr<const org::IResourceVersions> versions) {
    auto data = std::make_shared<HostData>();
    data->versions = std::move(versions);
    const org::ResolverCaptureContext context(std::shared_ptr<const org::IHostExecutionData>(std::move(data)));
    return resource.SelectVersion(&context);
}

// A buffer-only part, as DCLF's sealed set is to a composed set.
struct BufferPart final : org::IResourceVersions {
    const void* key = nullptr;
    std::shared_ptr<const org::BufferVersion> version;
    std::shared_ptr<const org::BufferVersion> Find(const org::VersionedBuffer& buffer) const noexcept override {
        return buffer.Key() == key ? version : nullptr;
    }
};

int TestFindAndMiss() {
    auto a = org::VersionedTexture::Create(MakePixels(16));
    auto b = org::VersionedTexture::Create(MakePixels(16));
    auto buffer = org::VersionedBuffer::Create(MakeBuffer());
    const auto pending = a->Make(MakePixels(32));
    const std::vector<NamedVersions::TextureEntry> entries{{a, pending}};
    const auto named = NamedVersions::Textures(entries);
    CHECK(named && named->TextureCount() == 1);
    CHECK(named->Names(*a) && !named->Names(*b));
    CHECK(named->FindTexture(*a) == pending);
    CHECK(named->Misses() == 0);
    CHECK(named->FindTexture(*b) == nullptr);
    CHECK(named->Misses() == 1);
    // A texture set is not asked about buffers: no miss.
    CHECK(named->Find(*buffer) == nullptr);
    CHECK(named->Misses() == 1);
    // A clone shares the versions (Key), so the set names it too.
    const auto clone = a->Clone();
    CHECK(named->FindTexture(static_cast<const org::VersionedTexture&>(*clone)) == pending);
    // A preparation with the set as its host data's versions binds the pending version, and the current one of anything else.
    CHECK(a->Current() != pending);
    CHECK(Selected(*a, named) == pending);
    CHECK(Selected(*b, named) == b->Current());
    CHECK(Selected(*a, nullptr) == a->Current());
    return 0;
}

int TestSerialsAndErrors() {
    auto a = org::VersionedTexture::Create(MakePixels(16));
    const std::vector<NamedVersions::TextureEntry> entries{{a, a->Current()}};
    const auto first = NamedVersions::Textures(entries);
    const auto second = NamedVersions::Textures(entries);
    CHECK(first->Serial() != 0 && second->Serial() > first->Serial());
    const auto empty = NamedVersions::Textures({});
    CHECK(empty->TextureCount() == 0 && empty->Serial() > second->Serial());
    bool threw = false;
    try {
        const std::vector<NamedVersions::TextureEntry> twice{{a, a->Current()}, {a, a->Make(MakePixels(8))}};
        (void)NamedVersions::Textures(twice);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        const std::vector<NamedVersions::TextureEntry> none{{a, nullptr}};
        (void)NamedVersions::Textures(none);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    return 0;
}

int TestAdoptAll() {
    auto a = org::VersionedTexture::Create(MakePixels(16));
    auto b = org::VersionedTexture::Create(MakePixels(16));
    auto gone = org::VersionedTexture::Create(MakePixels(16));
    const auto nextA = a->Make(MakePixels(64));
    const auto goneNext = gone->Make(MakePixels(64));
    // b is named at its current version, so there is nothing to adopt for it. An expired texture is skipped.
    const auto named = [&] {
        const std::vector<NamedVersions::TextureEntry> entries{{a, nextA}, {b, b->Current()}, {gone, goneNext}};
        return NamedVersions::Textures(entries);
    }();
    const std::weak_ptr<org::VersionedTexture> goneWeak = gone;
    gone.reset();
    CHECK(goneWeak.expired());
    CHECK(named->AdoptAll() == 1);
    CHECK(a->Current() == nextA);
    CHECK(a->Get()->GetWidth() == 64);
    CHECK(named->AdoptAll() == 0);
    // Once adopted, the named version is the current one.
    CHECK(Selected(*a, named) == a->Current());
    return 0;
}

int TestComposition() {
    auto a = org::VersionedTexture::Create(MakePixels(16));
    auto buffer = org::VersionedBuffer::Create(MakeBuffer());
    const auto one = a->Make(MakePixels(32));
    const auto two = a->Make(MakePixels(48));
    const std::vector<NamedVersions::TextureEntry> oneEntries{{a, one}}, twoEntries{{a, two}};
    const auto setOne = NamedVersions::Textures(oneEntries);
    const auto setTwo = NamedVersions::Textures(twoEntries);
    auto buffers = std::make_shared<BufferPart>();
    buffers->key = buffer->Key();
    buffers->version = buffer->Make(MakeBuffer());

    // The first part that names a resource answers.
    const auto first = std::make_shared<ComposedVersions>(Parts{setOne, setTwo, buffers});
    const auto second = std::make_shared<ComposedVersions>(Parts{buffers, setTwo, setOne});
    CHECK(first->PartCount() == 3 && second->PartCount() == 3);
    CHECK(first->FindTexture(*a) == one);
    CHECK(second->FindTexture(*a) == two);
    CHECK(first->Find(*buffer) == buffers->version && second->Find(*buffer) == buffers->version);
    CHECK(Selected(*a, first) == one && Selected(*a, second) == two);
    CHECK(Selected(*buffer, first) == buffers->version);
    // A resource no part names resolves to its current version.
    auto other = org::VersionedTexture::Create(MakePixels(16));
    CHECK(first->FindTexture(*other) == nullptr && Selected(*other, first) == other->Current());
    // Serials are new per set, from the sequence NamedVersions draws from.
    CHECK(first->Serial() > setTwo->Serial() && second->Serial() > first->Serial());
    // Null parts are skipped. More than four parts is an error.
    const ComposedVersions sparse(Parts{nullptr, setTwo, nullptr});
    CHECK(sparse.PartCount() == 1 && sparse.FindTexture(*a) == two);
    bool threw = false;
    try {
        const ComposedVersions five(Parts{setOne, setTwo, buffers, setOne, setTwo});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    return 0;
}

} // namespace

int main() {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> warp;
    CHECK(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    CHECK(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    rhi::DeviceCreateInfo create{};
    create.backend = rhi::Backend::D3D12;
    create.nativeAdapter = warp.Get();
    create.framesInFlight = 2;
    rhi::DevicePtr device;
    CHECK(!rhi::Failed(rhi::CreateD3D12Device(create, device)) && device);
    auto runtime = org::Runtime::Create({.device = device.Get(), .backend = rhi::Backend::D3D12});
    for (const auto test : {&TestFindAndMiss, &TestSerialsAndErrors, &TestAdoptAll, &TestComposition})
        if (const int line = test()) return line;
    std::puts("PASS: NamedVersionsTests");
    return 0;
}
