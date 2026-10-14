#include <ORGModuleServices/VersionedBufferGrowth.h>

#include <ORGModuleServices/GrowableBufferTypes.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace org::services {

namespace {

// Leases the request's backing, makes it the buffer's next version (owning the lease) and queues its fills, adding their tickets.
GrownVersion MakeAndFill(const VersionGrowthRequest& request, std::vector<std::shared_ptr<TrackedUploadTicket>>& tickets) {
    if (!request.buffer) throw std::invalid_argument("GrowVersion: no buffer");
    if (!request.pool) throw std::invalid_argument("GrowVersion: " + request.name + " has no pool");
    BackingRequest backing;
    backing.capacityClass = request.capacityClass ? request.capacityClass : request.elements ? request.elements : request.bytes;
    backing.stride = request.stride;
    backing.flags = request.flags;
    backing.compatibility = request.compatibility;
    backing.name = request.name;
    backing.like = request.buffer->Get();
    if (!backing.capacityClass) throw std::invalid_argument("GrowVersion: " + request.name + " asks for an empty version");
    if (request.elements && !request.stride) throw std::invalid_argument("GrowVersion: " + request.name + " is structured without a stride");
    GrownVersion out;
    out.lease = request.pool->Acquire(backing);
    // Exhausted (Config::maxPerClass): the batch fails and the next seal that needs it posts it again; no smaller version.
    if (!out.lease || !out.lease->buffer) throw std::runtime_error("GrowVersion: no backing for " + request.name);
    // The version is the backing (the pool keeps the buffer; a reused one holds an older version's bytes) and owns a lease of it.
    auto made = request.buffer->Make(out.lease->buffer);
    out.version = LeasedVersion(BufferVersion(*made), out.lease);
    const auto& version = out.version;
    std::vector<VersionFill> fills;
    std::shared_ptr<const void> madeContents;
    const auto* queued = &request.fills;
    if (request.contentsOf) {
        request.contentsOf(*version, fills, madeContents);
        queued = &fills;
    }
    if (queued->empty()) return out;
    if (!request.uploads) throw std::invalid_argument("GrowVersion: contents without an upload service");
    const auto size = version->buffer->GetBufferSize();
    for (const auto& fill : *queued) {
        std::size_t bytes = 0;
        for (const auto& segment : fill.segments) bytes += segment.size;
        if (!bytes) continue;
        if (fill.offset + bytes > size) throw std::out_of_range("GrowVersion: a fill past the new version");
        // The version is pending: no queue uses it before its owner adopts it, after these copies complete (a reused backing: no
        // frame reads it any more, its clock reached the point they bound).
        auto ticket = request.uploads->QueueTrackedStreamingUploadSegments(fill.segments, bytes,
            WorkerOwnedDestination{ version->buffer, WorkerOwnedDestination::Ownership::PendingVersion }, fill.offset);
        if (!ticket) throw std::runtime_error("GrowVersion: the uploader refused a fill");
        // The uploader copies the segments into its staging pages as it queues them: `madeContents` may go after this.
        tickets.push_back(std::move(ticket));
    }
    return out;
}

} // namespace

GrownVersion GrowVersion(const VersionGrowthRequest& request) {
    std::vector<std::shared_ptr<TrackedUploadTicket>> tickets;
    auto out = MakeAndFill(request, tickets);
    out.ready = TokenForTickets(std::move(tickets));
    return out;
}

GrownVersions GrowVersions(std::span<const VersionGrowthRequest> requests) {
    std::vector<std::shared_ptr<TrackedUploadTicket>> tickets;
    GrownVersions out;
    out.versions.reserve(requests.size());
    out.leases.reserve(requests.size());
    for (const auto& request : requests) {
        auto grown = MakeAndFill(request, tickets);
        out.versions.push_back(std::move(grown.version));
        out.leases.push_back(std::move(grown.lease));
    }
    out.ready = TokenForTickets(std::move(tickets));
    return out;
}

} // namespace org::services
