#pragma once

#include <ORGModuleServices/Async/ArtifactResources.h>
#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/VersionedBufferBuild.h>
#include <ORGModuleServices/VersionFill.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Render/Runtime/IUploadService.h"
#include "Render/Runtime/StreamingUploadTypes.h"
#include "Resources/Buffers/VersionedBuffer.h"

namespace org::services {

/**
 * @brief Growth as graph work (the SARP/BasicRenderer model: BuildVersionedGpuBuffer). A producer, on any thread, makes the next
 * version of a VersionedBuffer at its new size and fills it through the dedicated uploader (IUploadService's worker path, the
 * copy queue's own submitter); the version is ready once those copies complete, and its owner adopts it when the revision naming
 * it is selected. Nothing waits: not the owner, not the producer.
 */
struct VersionGrowthRequest {
    std::shared_ptr<VersionedBuffer> buffer;
    // The new size: structured elements, else raw bytes.
    std::uint32_t elements = 0;
    std::uint64_t bytes = 0;
    // Its contents (none: written whole by its consumers, or by the GPU), and what keeps their memory alive until the copies are
    // staged (the uploader copies the segments into its pages when queued).
    std::vector<VersionFill> fills;
    std::shared_ptr<const void> contents;
    // Contents that depend on the version made (rows that embed their own device address): called on the producer's thread
    // once the version exists, before the fills are queued, to set `fills` and `contents` (it replaces those given above).
    std::function<void(const BufferVersion& version, std::vector<VersionFill>& fills, std::shared_ptr<const void>& contents)> contentsOf;
    std::shared_ptr<runtime::IUploadService> uploads;
    // The pool the version is made into (required: no fresh buffers): a backing of `capacityClass` leased for this request and
    // wrapped as the version (VersionedBuffer::Make). The version owns the lease (LeasedVersion): releasing its last reference
    // returns the backing, reusable once the pool's clock reached the point the frames that read it bound (NamedVersions::Bind).
    // A reused backing holds an older version's contents: the fills, or the version's writers (FilledVersion), write it whole.
    std::shared_ptr<BackingPool> pool;
    std::uint64_t capacityClass = 0;  // elements of `stride`, or bytes for a raw buffer (0: `elements`, else `bytes`)
    std::uint32_t stride = 0;         // a structured buffer's element stride (0: raw)
    BackingFlags flags;
    std::uint64_t compatibility = 0;  // BackingRequest::compatibility: a backing is reused only within one key
    std::string name;                 // a new backing's
};

struct GrownVersion {
    std::shared_ptr<const BufferVersion> version;
    BackingLease lease;  // the version's backing (a copy: the version owns its own)
    // Complete when every fill's copy has completed on the uploader's queue (null: nothing to copy, ready now). Failed when a copy
    // was cancelled (the uploader shut down).
    std::shared_ptr<const async::GpuSubmissionSet> ready;
};

/**
 * @brief Any thread, one growth at a time per pool (BackingPool's single acquirer: a set posts one batch at a time): leases the
 * version's backing and queues its fills. Throws without a pool, when no backing is leased, or when a fill is not queued.
 */
GrownVersion GrowVersion(const VersionGrowthRequest& request);

/** @brief Versions that grow together (one owner's sizing): adopted as one, so ready as one. */
struct GrownVersions {
    std::vector<std::shared_ptr<const BufferVersion>> versions;  // by the requests' order
    std::vector<BackingLease> leases;                            // by the requests' order: each version's backing (copies)
    std::shared_ptr<const async::GpuSubmissionSet> ready;        // every fill's copy (null: nothing to copy)
};
/** @brief Any thread: GrowVersion for each request, ready together. */
GrownVersions GrowVersions(std::span<const VersionGrowthRequest> requests);

} // namespace org::services
