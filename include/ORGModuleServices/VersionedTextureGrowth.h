#pragma once

#include <ORGModuleServices/Async/ArtifactResources.h>
#include <ORGModuleServices/Async/RevisionAssembly.h>
#include <ORGModuleServices/Async/VersionGrowthArtifact.h>
#include <ORGModuleServices/GrowableBufferSet.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Render/Runtime/IUploadService.h"
#include "Resources/GloballyIndexedResource.h"
#include "Resources/VersionedTexture.h"

namespace org::services {

/** @brief What a new texture version holds before its first writer runs. */
enum class TextureContents : std::uint8_t {
    Undefined,  // nothing is written: the version's writers write it whole, or its maker initialized it
    Zero,       // zero-filled on the uploader's worker path (a History texture is born cleared)
    Fill,       // filled on that path with what the request's writer writes (a texture born with a value or contents)
};

/** @brief The size of a texture version made from the slot's current one. Depth is for a 3D texture. */
struct TextureExtent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
};

/**
 * @brief One texture's next version (the texture twin of VersionGrowthRequest).
 *
 * The version is one of these:
 * - `image`, a texture the caller made. It must have the slot's shape class (format, mip count, slice count). It may be any
 *   GloballyIndexedResource with a layout, such as a PixelBuffer or an ExternalTextureResource.
 * - When `image` is null, a PixelBuffer that MakeTextureVersions makes like the slot's current one, at `extent`. The current
 *   version must then be a PixelBuffer.
 *
 * A Zero or Fill version is filled on the uploader's copy queue, so it must be rhi::QueueSharing::Concurrent, single-sampled and
 * not a depth format. A PixelBuffer made from `extent` for such a request is made Concurrent.
 */
struct TextureVersionRequest {
    std::shared_ptr<VersionedTexture> slot;
    std::shared_ptr<GloballyIndexedResource> image;
    TextureExtent extent;
    TextureContents contents = TextureContents::Undefined;
    // Kept alive with the version, such as the caller's wrap of `image` or the lease of its memory.
    std::shared_ptr<const void> owner;
    // A PixelBuffer made from `extent` gets this name (empty: the current version's name).
    std::string name;
    // A Fill version's contents, written subresource by subresource on the thread that makes the versions.
    TextureFillWriter fill;
};

/** @brief Texture versions made together. They are adopted as one, so they are ready as one. */
struct TextureVersionsMade {
    // By the requests' order. Pending: none is current until the set is adopted (NamedVersions::AdoptAll).
    std::vector<std::shared_ptr<const TextureVersion>> versions;
    // Names every version. Recordings made against it bind them before they are current.
    std::shared_ptr<const NamedVersions> named;
    // Complete when every zero fill's copies completed on the uploader's queue (null: nothing to fill, ready now). Failed when a
    // copy was cancelled (the uploader shut down).
    std::shared_ptr<const async::GpuSubmissionSet> ready;
    // Resolves to `named` (Value<NamedVersions>) once `ready` is complete. Fails when `ready` fails. Nothing waits for it.
    std::shared_ptr<const async::RevisionFragment> fragment;
};

/**
 * @brief Any thread: makes each request's next version (VersionedTexture::MakeImage, pending, not current), queues the zero
 * fills on `uploads`' worker path (IUploadService::QueueTrackedStreamingTextureZeroFill) and names the versions in one texture
 * set (NamedVersions::Textures).
 *
 * Every version is made before any fill is queued, so a shape-class change queues nothing. Throws:
 * - std::invalid_argument for a request without a slot, a slot named twice, an `extent` request whose current version is not
 *   a PixelBuffer, an empty extent, and a version of another shape class than the slot's (ORG's MakeVersion).
 * - std::invalid_argument for a Zero or Fill request without `uploads`, a Fill request without a writer, or a texture the
 *   uploader cannot fill (or whose writer failed).
 */
TextureVersionsMade MakeTextureVersions(std::span<const TextureVersionRequest> requests,
    const std::shared_ptr<runtime::IUploadService>& uploads);

/**
 * @brief A revision fragment that resolves to `named` once `ready` is complete, or fails when it fails. With no `ready` it is
 * ready now. Settled from the uploader's notifications (GpuSubmissionSet::subscribe), with no wait and no polling. Throws
 * when `ready` cannot notify (no subscribe).
 */
std::shared_ptr<const async::RevisionFragment> FragmentWhenReady(std::shared_ptr<const NamedVersions> named,
    std::shared_ptr<const async::GpuSubmissionSet> ready);

/**
 * @brief MakeTextureVersions as graph work (PostVersionGrowth's VersionGrowthWork). The producer makes the versions and queues
 * the fills. `done` runs on the poster's done lane once the fills completed, with the made versions, or with null and why.
 */
async::VersionGrowthWork TextureVersionWork(std::vector<TextureVersionRequest> requests, std::shared_ptr<runtime::IUploadService> uploads,
    std::function<void(std::shared_ptr<const TextureVersionsMade> made, std::string error)> done);

/**
 * @brief Posts TextureVersionWork on `graph` (PostVersionGrowth): the producer of `posts`' kind runs it, and the graph waits for
 * the fills. False when the graph refused it (shutting down): `done` does not run. The posting thread only.
 */
template <class Binding>
bool PostTextureVersions(async::StateGraph<Binding>& graph, async::VersionGrowthPosts<Binding>& posts,
    std::vector<TextureVersionRequest> requests, std::shared_ptr<runtime::IUploadService> uploads,
    std::function<void(std::shared_ptr<const TextureVersionsMade> made, std::string error)> done) {
    return async::PostVersionGrowth(graph, posts, TextureVersionWork(std::move(requests), std::move(uploads), std::move(done)));
}

} // namespace org::services
