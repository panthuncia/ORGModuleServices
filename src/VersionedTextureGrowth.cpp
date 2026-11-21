#include <ORGModuleServices/VersionedTextureGrowth.h>

#include <ORGModuleServices/VersionedBufferBuild.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>

#include "Resources/ExternalTextureResource.h"
#include "Resources/PixelBuffer.h"

namespace org::services {

namespace {

// A description like `current`'s at `extent`. Per-subresource sizes are made again for the new extent.
TextureDescription DescriptionAt(const TextureDescription& current, const TextureExtent& extent, std::uint32_t mips, std::uint32_t slices) {
    TextureDescription next = current;
    next.depth = current.type == rhi::ResourceType::Texture3D ? extent.depth : current.depth;
    next.imageDimensions.clear();
    if (current.imageDimensions.size() <= 1) {
        next.imageDimensions.push_back({extent.width, extent.height, 0, 0});
        return next;
    }
    next.imageDimensions.reserve(std::size_t{mips} * slices);
    for (std::uint32_t slice = 0; slice < slices; ++slice)
        for (std::uint32_t mip = 0; mip < mips; ++mip)
            next.imageDimensions.push_back({(std::max)(1u, extent.width >> mip), (std::max)(1u, extent.height >> mip), 0, 0});
    return next;
}

// The request's next texture: the caller's, or a PixelBuffer like the slot's current one at the request's extent.
std::shared_ptr<GloballyIndexedResource> NextTexture(const TextureVersionRequest& request) {
    if (request.image) return request.image;
    const auto current = request.slot->Get();
    if (!current)
        throw std::invalid_argument("MakeTextureVersions: the current version of a texture made from an extent must be a PixelBuffer");
    if (!request.extent.width || !request.extent.height || !request.extent.depth)
        throw std::invalid_argument("MakeTextureVersions: '" + current->GetName() + "' asks for an empty extent");
    auto description = DescriptionAt(current->GetDescription(), request.extent, current->GetMipLevels(), current->GetArraySize());
    // The uploader fills on its copy queue, which only a concurrent texture allows without ownership transfers.
    if (request.contents != TextureContents::Undefined) description.queueSharing = rhi::QueueSharing::Concurrent;
    auto next = PixelBuffer::CreateShared(description);
    next->SetName(request.name.empty() ? current->GetName() : request.name);
    return next;
}

// The format a Value request's texel is encoded in: the request's, or the version's own.
rhi::Format ValueFormatOf(const TextureVersionRequest& request, const std::shared_ptr<Resource>& resource) {
    if (request.valueFormat != rhi::Format::Unknown) return request.valueFormat;
    if (const auto pixels = std::dynamic_pointer_cast<PixelBuffer>(resource)) return pixels->GetFormat();
    if (const auto external = std::dynamic_pointer_cast<ExternalTextureResource>(resource)) return external->GetFormat();
    return rhi::Format::Unknown;
}

// Settles a fragment from the uploader's notifications. The tickets hold the callback until they end, and the callback holds
// this. So the fragment's inputs are dropped once it settled, which ends that cycle.
struct FragmentSettler {
    std::mutex mutex;
    std::shared_ptr<async::RevisionFragment> fragment;
    std::shared_ptr<const NamedVersions> named;
    std::shared_ptr<const async::GpuSubmissionSet> ready;

    void Check() {
        std::shared_ptr<const async::GpuSubmissionSet> token;
        {
            std::lock_guard lock(mutex);
            if (!fragment) return;
            token = ready;
        }
        // Asking may complete a ticket, whose notification calls this again. So the lock is not held here.
        const bool failed = token->Failed();
        if (!failed && !token->Complete()) return;
        std::shared_ptr<async::RevisionFragment> settling;
        std::shared_ptr<const NamedVersions> value;
        {
            std::lock_guard lock(mutex);
            if (!fragment) return;
            settling = std::move(fragment);
            value = std::move(named);
            ready.reset();
        }
        if (failed) settling->Fail(std::make_exception_ptr(std::runtime_error("texture versions: " + token->Failure())));
        else settling->Resolve(std::move(value));
    }
};

} // namespace

std::shared_ptr<const async::RevisionFragment> FragmentWhenReady(std::shared_ptr<const NamedVersions> named,
    std::shared_ptr<const async::GpuSubmissionSet> ready) {
    if (!named) throw std::invalid_argument("FragmentWhenReady: no versions");
    if (!ready) return async::RevisionFragment::MakeReady(std::move(named));
    if (!ready->subscribe) throw std::invalid_argument("FragmentWhenReady: a readiness that does not notify");
    auto fragment = std::make_shared<async::RevisionFragment>();
    auto settler = std::make_shared<FragmentSettler>();
    settler->fragment = fragment;
    settler->named = std::move(named);
    settler->ready = ready;
    // Subscribing checks once too, so fills that already completed settle it here.
    ready->subscribe([settler] { settler->Check(); });
    return fragment;
}

TextureVersionsMade MakeTextureVersions(std::span<const TextureVersionRequest> requests,
    const std::shared_ptr<runtime::IUploadService>& uploads) {
    std::vector<const void*> keys;
    keys.reserve(requests.size());
    for (const auto& request : requests) {
        if (!request.slot) throw std::invalid_argument("MakeTextureVersions: a request without a texture");
        if (std::ranges::find(keys, request.slot->Key()) != keys.end())
            throw std::invalid_argument("MakeTextureVersions: a texture asked for twice");
        keys.push_back(request.slot->Key());
        if (request.contents != TextureContents::Undefined && !uploads)
            throw std::invalid_argument("MakeTextureVersions: a filled version without an upload service");
        if (request.contents == TextureContents::Fill && !request.fill)
            throw std::invalid_argument("MakeTextureVersions: a filled version without contents");
    }
    TextureVersionsMade made;
    made.versions.reserve(requests.size());
    std::vector<NamedVersions::TextureEntry> entries;
    entries.reserve(requests.size());
    // Every version first: a shape-class change throws here, before any fill is queued.
    for (const auto& request : requests) {
        auto version = request.slot->MakeImage(NextTexture(request), request.owner);
        entries.emplace_back(request.slot, version);
        made.versions.push_back(std::move(version));
    }
    made.named = NamedVersions::Textures(entries);
    // A Value request becomes a fill whose writer repeats its encoded texel. Each writer is made before any fill is queued.
    std::vector<TextureFillWriter> values(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].contents != TextureContents::Value) continue;
        const auto& resource = made.versions[i]->resource;
        values[i] = ValueFillWriter(ValueFormatOf(requests[i], resource), requests[i].value);
        if (!values[i])
            throw std::invalid_argument("MakeTextureVersions: '" + resource->GetName() + "' has no texel encoding for its value");
    }
    std::vector<std::shared_ptr<TrackedUploadTicket>> tickets;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].contents == TextureContents::Undefined) continue;
        const auto& resource = made.versions[i]->resource;
        // The version is pending and its texture new: no queue uses it before its owner adopts it, after these copies.
        const WorkerOwnedDestination destination{resource, WorkerOwnedDestination::Ownership::FreshTexture};
        const auto& writer = requests[i].contents == TextureContents::Value ? values[i] : requests[i].fill;
        auto ticket = requests[i].contents == TextureContents::Zero ? uploads->QueueTrackedStreamingTextureZeroFill(destination) :
                                                                      uploads->QueueTrackedStreamingTextureFill(destination, writer);
        if (!ticket)
            throw std::invalid_argument("MakeTextureVersions: the uploader cannot fill '" + resource->GetName() +
                "'. It must be a concurrent, single-sampled colour texture, and its contents must be written");
        tickets.push_back(std::move(ticket));
    }
    made.ready = TokenForTickets(std::move(tickets));
    made.fragment = FragmentWhenReady(made.named, made.ready);
    return made;
}

async::VersionGrowthWork TextureVersionWork(std::vector<TextureVersionRequest> requests, std::shared_ptr<runtime::IUploadService> uploads,
    std::function<void(std::shared_ptr<const TextureVersionsMade> made, std::string error)> done) {
    async::VersionGrowthWork work;
    work.produce = [requests = std::move(requests), uploads = std::move(uploads)] {
        auto made = std::make_shared<const TextureVersionsMade>(MakeTextureVersions(requests, uploads));
        async::VersionGrowthWork::Produced produced;
        produced.ready = made->ready;
        produced.value = std::move(made);
        return produced;
    };
    work.done = [done = std::move(done)](std::shared_ptr<const void> value, std::string error) {
        done(std::static_pointer_cast<const TextureVersionsMade>(std::move(value)), std::move(error));
    };
    return work;
}

} // namespace org::services
