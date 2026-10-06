#include <ORGModuleServices/VersionedBufferGrowth.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace org::services {

std::shared_ptr<const async::GpuSubmissionSet> TokenForTickets(std::vector<std::shared_ptr<TrackedUploadTicket>> tickets) {
    std::erase(tickets, nullptr);
    if (tickets.empty()) return {};
    auto token = std::make_shared<async::GpuSubmissionSet>();
    auto shared = std::make_shared<const std::vector<std::shared_ptr<TrackedUploadTicket>>>(std::move(tickets));
    token->submissions.reserve(shared->size());
    for (const auto& ticket : *shared) {
        async::GpuQueueSubmission submission;
        {
            std::lock_guard lock(ticket->timelineMutex);
            submission.timelineOwner = ticket->timelineOwner;
            submission.value = ticket->timelineValue;
        }
        submission.currentTimelineOwner = [ticket] {
            std::lock_guard lock(ticket->timelineMutex);
            return ticket->timelineOwner;
        };
        submission.currentValue = [ticket] {
            std::lock_guard lock(ticket->timelineMutex);
            return ticket->timelineValue;
        };
        token->submissions.push_back(std::move(submission));
    }
    token->isComplete = [shared] {
        return std::ranges::all_of(*shared, [](const auto& ticket) { return ticket->Complete(); });
    };
    token->isSubmitted = [shared] {
        return std::ranges::all_of(*shared, [](const auto& ticket) {
            const auto state = ticket->state.load(std::memory_order_acquire);
            return state == TrackedUploadTicketState::Submitted || state == TrackedUploadTicketState::Completed;
        });
    };
    token->isFailed = [shared] {
        return std::ranges::any_of(*shared, [](const auto& ticket) {
            return ticket->state.load(std::memory_order_acquire) == TrackedUploadTicketState::Cancelled;
        });
    };
    token->failure = [] { return std::string("a version's upload was cancelled"); };
    token->subscribe = [shared](std::function<void()> callback) {
        for (const auto& ticket : *shared) ticket->SetChangeCallback(callback);
        if (callback) callback();
    };
    token->cancel = [shared] {
        bool cancelled = false;
        for (const auto& ticket : *shared) cancelled = ticket->Cancel() || cancelled;
        return cancelled;
    };
    token->describe = [shared] { return "version-upload-tickets=" + std::to_string(shared->size()); };
    // Every transition notifies (the uploader's submitter completes its batches by their timeline value: Claimed, Submitted,
    // Completed; Cancel likewise), so a graph waiting on this needs no recovery polling.
    token->completionNotificationsAreAuthoritative = true;
    return token;
}

namespace {

// Makes the request's version and queues its fills, adding their tickets.
std::shared_ptr<const BufferVersion> MakeAndFill(const VersionGrowthRequest& request, std::vector<std::shared_ptr<TrackedUploadTicket>>& tickets) {
    if (!request.buffer) throw std::invalid_argument("GrowVersion: no buffer");
    auto version = request.elements ? request.buffer->MakeStructured(request.elements) : request.buffer->MakeBytes(request.bytes);
    std::vector<VersionFill> made;
    std::shared_ptr<const void> madeContents;
    const auto* fills = &request.fills;
    if (request.contentsOf) {
        request.contentsOf(*version, made, madeContents);
        fills = &made;
    }
    if (fills->empty()) return version;
    if (!request.uploads) throw std::invalid_argument("GrowVersion: contents without an upload service");
    const auto size = version->buffer->GetBufferSize();
    for (const auto& fill : *fills) {
        std::size_t bytes = 0;
        for (const auto& segment : fill.segments) bytes += segment.size;
        if (!bytes) continue;
        if (fill.offset + bytes > size) throw std::out_of_range("GrowVersion: a fill past the new version");
        // The version is pending: no queue uses it before its owner adopts it, after these copies complete.
        auto ticket = request.uploads->QueueTrackedStreamingUploadSegments(fill.segments, bytes,
            WorkerOwnedDestination{ version->buffer, WorkerOwnedDestination::Ownership::PendingVersion }, fill.offset);
        if (!ticket) throw std::runtime_error("GrowVersion: the uploader refused a fill");
        // The uploader copies the segments into its staging pages as it queues them: `madeContents` may go after this.
        tickets.push_back(std::move(ticket));
    }
    return version;
}

} // namespace

GrownVersion GrowVersion(const VersionGrowthRequest& request) {
    std::vector<std::shared_ptr<TrackedUploadTicket>> tickets;
    GrownVersion out;
    out.version = MakeAndFill(request, tickets);
    out.ready = TokenForTickets(std::move(tickets));
    return out;
}

GrownVersions GrowVersions(std::span<const VersionGrowthRequest> requests) {
    std::vector<std::shared_ptr<TrackedUploadTicket>> tickets;
    GrownVersions out;
    out.versions.reserve(requests.size());
    for (const auto& request : requests) out.versions.push_back(MakeAndFill(request, tickets));
    out.ready = TokenForTickets(std::move(tickets));
    return out;
}

} // namespace org::services
