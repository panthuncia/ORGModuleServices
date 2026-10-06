#pragma once

#include <ORGModuleServices/Async/ArtifactResources.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
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
struct VersionFill {
    std::uint64_t offset = 0;                      // bytes into the new version
    std::vector<StreamingUploadSegment> segments;  // gathered in order; their memory is the request's `contents`
};

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
};

struct GrownVersion {
    std::shared_ptr<const BufferVersion> version;
    // Complete when every fill's copy has completed on the uploader's queue (null: nothing to copy, ready now). Failed when a copy
    // was cancelled (the uploader shut down).
    std::shared_ptr<const async::GpuSubmissionSet> ready;
};

/** @brief Any thread: makes the version and queues its fills. Throws when the version cannot be made or a fill not queued. */
GrownVersion GrowVersion(const VersionGrowthRequest& request);

/** @brief Versions that grow together (one owner's sizing): adopted as one, so ready as one. */
struct GrownVersions {
    std::vector<std::shared_ptr<const BufferVersion>> versions;  // by the requests' order
    std::shared_ptr<const async::GpuSubmissionSet> ready;        // every fill's copy (null: nothing to copy)
};
/** @brief Any thread: GrowVersion for each request, ready together. */
GrownVersions GrowVersions(std::span<const VersionGrowthRequest> requests);

/** @brief A readiness token over upload tickets (TokenForTickets in BasicRenderer): complete when all are, failed when one was cancelled. */
std::shared_ptr<const async::GpuSubmissionSet> TokenForTickets(std::vector<std::shared_ptr<TrackedUploadTicket>> tickets);

} // namespace org::services
