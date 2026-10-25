#include <ORGModuleServices/VersionedBufferBuild.h>

#include <algorithm>
#include <bit>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>

#include "Render/Runtime/IUploadService.h"
#include "Render/Runtime/StreamingUploadTypes.h"
#include "Resources/Buffers/Buffer.h"

namespace org::services {

namespace {

// A Replace's class: the request's capacity rounded to a power of two, and at least 1.5x the predecessor's (growth amortised).
std::uint64_t ReplacementCapacity(const VersionedBufferBuildInput& input) {
    const auto required = (std::max<std::uint64_t>)(input.capacity, 1u);
    const auto rounded = std::bit_ceil(required);
    if (!input.previous) return rounded;
    const auto grown = input.previous->capacity + (std::max<std::uint64_t>)(input.previous->capacity / 2u, 1u);
    return (std::max)(rounded, grown);
}

VersionedBufferBuildOutcome Fail(std::string error) {
    VersionedBufferBuildOutcome outcome;
    outcome.kind = VersionedBufferBuildOutcome::Kind::Failed;
    outcome.error = std::move(error);
    return outcome;
}

VersionedBufferBuildOutcome Cancel() {
    VersionedBufferBuildOutcome outcome;
    outcome.kind = VersionedBufferBuildOutcome::Kind::Cancelled;
    return outcome;
}

// The leased backing holds nothing a successor may catch up from (a build that failed after writing part of it).
void Forget(Backing& backing) noexcept {
    backing.image.reset();
    backing.contentEpoch.store(0, std::memory_order_release);
}

VersionedBufferBuildOutcome Build(const VersionedBufferBuildInput& input, const VersionedBufferBuildContext& context) {
    const auto stopped = [&] { return context.stopRequested && context.stopRequested(); };
    if (!input.uploadService || input.elementStride == 0u) return Fail("versioned buffer input/upload service/stride missing");
    if (!input.backingPool) return Fail("versioned buffer input has no backing pool");
    if (stopped()) return Cancel();
    const bool fitsBacking = input.previous && input.previous->resource && input.capacity <= input.previous->capacity;
    const auto mode = fitsBacking ? BufferRevisionMode::Patch : BufferRevisionMode::Replace;
    const auto capacityClass = mode == BufferRevisionMode::Replace ? ReplacementCapacity(input) : input.previous->capacity;
    auto& pool = *input.backingPool;

    BackingRequest request;
    request.capacityClass = capacityClass;
    request.stride = input.elementStride;
    request.flags = input.flags;
    request.name = input.debugName;
    BackingPool::AcquireInfo acquired;
    auto lease = pool.Acquire(request, input.writeSequence, &acquired);
    if (!lease) {
        // Exhausted (Config::maxPerClass): suspend until the pool's availability wakes the graph.
        if (!context.allocateSuspension) return Fail("versioned buffer backing pool exhausted and the caller cannot suspend");
        VersionedBufferBuildOutcome outcome;
        outcome.kind = VersionedBufferBuildOutcome::Kind::Suspended;
        outcome.suspension = context.allocateSuspension();
        outcome.error = "versioned buffer backing pool exhausted";
        if (context.notifySuspension) pool.SubscribeAvailability(outcome.suspension, context.notifySuspension);
        return outcome;
    }
    auto& backing = *lease;

    // The journal is replayed only once a backing is in hand: exhaustion is an expected transient state, and replaying on every
    // retry multiplied host work while waiting for retirement.
    std::shared_ptr<const BufferImage> image = input.image;
    if (!input.gpuWritten && !image) {
        std::string replayError;
        const auto bytes = ReplayVersionedBuffer(input, replayError);
        if (!bytes) return Fail(std::move(replayError));
        image = BufferImage::FromBytes(*bytes);
    }
    if (stopped()) return Cancel();

    const auto& resource = backing.buffer;
    if (!resource) return Fail("versioned buffer backing has no buffer");
    std::vector<std::shared_ptr<TrackedUploadTicket>> tickets;
    // Indirect argument buffers are transient UAV outputs whose producer writes every command before they are consumed: uploading
    // the zero-filled CPU image is unnecessary, and thousands of them would serialize behind the copy timeline at load.
    const bool needsInitialContent =
        !input.gpuWritten && !(input.flags.unorderedAccess && input.flags.indirectArguments && input.bytes.empty() && input.writes.empty());
    if (needsInitialContent && image && image->ByteSize() != 0u) {
        struct DirtyRange {
            std::size_t offset = 0, size = 0;
        };
        std::vector<DirtyRange> dirtyRanges;
        const auto held = backing.contentEpoch.load(std::memory_order_acquire);
        // Catch up from what the backing holds: every journal write since its contentEpoch, when the journal reaches back that far.
        const bool journalCoversBacking = !acquired.allocated && input.image && held != 0 && held >= input.journalBaseSequence &&
            held <= input.writeSequence && backing.image && backing.image->ByteSize() == image->ByteSize();
        if (journalCoversBacking) {
            for (const auto& write : input.writes) {
                if (write.sequence <= held || write.byteSize == 0u) continue;
                dirtyRanges.push_back({static_cast<std::size_t>(write.byteOffset), static_cast<std::size_t>(write.byteSize)});
            }
            std::ranges::sort(dirtyRanges, {}, &DirtyRange::offset);
            std::vector<DirtyRange> merged;
            for (const auto& range : dirtyRanges) {
                if (!merged.empty() && range.offset <= merged.back().offset + merged.back().size + 256u) {
                    const auto end = (std::max)(merged.back().offset + merged.back().size, range.offset + range.size);
                    merged.back().size = end - merged.back().offset;
                } else
                    merged.push_back(range);
            }
            dirtyRanges = std::move(merged);
        } else {
            dirtyRanges.push_back({0, image->ByteSize()});
        }
        const auto dirtyBytes = std::accumulate(
            dirtyRanges.begin(), dirtyRanges.end(), std::size_t{0}, [](std::size_t total, const DirtyRange& range) { return total + range.size; });
        if (dirtyRanges.size() > 128u || dirtyBytes * 2u > image->ByteSize()) dirtyRanges.assign(1u, DirtyRange{0, image->ByteSize()});
        // From here the backing's contents change: until the new contentEpoch is set it holds nothing a successor may rely on.
        Forget(backing);
        for (const auto& range : dirtyRanges) {
            std::size_t remaining = range.size;
            std::size_t offset = range.offset;
            std::vector<StreamingUploadSegment> segments;
            segments.reserve((range.size + BufferImage::PageBytes - 1u) / BufferImage::PageBytes + 1u);
            while (remaining != 0u) {
                const auto pageIndex = offset / BufferImage::PageBytes;
                const auto pageOffset = offset % BufferImage::PageBytes;
                if (pageIndex >= image->Pages().size() || !image->Pages()[pageIndex]) break;
                const auto size = (std::min)(remaining, image->Pages()[pageIndex]->size() - pageOffset);
                if (size == 0u) break;
                segments.push_back({image->Pages()[pageIndex]->data() + pageOffset, size});
                offset += size;
                remaining -= size;
            }
            if (remaining != 0u) return Fail("versioned buffer paged image does not cover upload range");
            // The backing is a pool lease (no lease and retired on the clock before it was handed out), so no queue touches it
            // during the copy: that is what makes the worker path legal.
            if (backing.retired.load(std::memory_order_acquire)) return Fail("versioned buffer backing was retired before its upload was queued");
            auto ticket = input.uploadService->QueueTrackedStreamingUploadSegments(segments, range.size,
                WorkerOwnedDestination{resource, WorkerOwnedDestination::Ownership::PooledBackingLease}, range.offset);
            if (!ticket) return Fail("versioned buffer streaming upload enqueue failed");
            tickets.push_back(std::move(ticket));
        }
    }
    backing.image = input.gpuWritten ? nullptr : image;
    backing.contentEpoch.store(input.writeSequence, std::memory_order_release);

    auto version = std::make_shared<PublishedBufferVersion>();
    version->revision = context.revision;
    version->writeSequence = input.writeSequence;
    version->elementCount = input.elementCount;
    version->capacity = backing.capacityClass;
    version->elementStride = input.elementStride;
    version->revisionMode = mode;
    version->contentVersion = context.revision;
    version->contentEpoch = input.writeSequence;
    version->backingEpoch = backing.generation;
    version->resource = resource;
    version->image = input.gpuWritten ? nullptr : std::move(image);
    version->tag = input.tag;
    version->lease = std::move(lease);

    VersionedBufferBuildOutcome outcome;
    outcome.kind = VersionedBufferBuildOutcome::Kind::Ready;
    outcome.version = std::move(version);
    outcome.ready = TokenForTickets(std::move(tickets));
    return outcome;
}

// Disjoint byte intervals [begin, end), keyed by begin; touching ones merge.
class ByteIntervals {
public:
    void Add(std::uint64_t begin, std::uint64_t end) {
        if (begin >= end) return;
        auto next = m_intervals.upper_bound(begin);
        if (next != m_intervals.begin()) {
            const auto before = std::prev(next);
            if (before->second >= begin) {
                begin = before->first;
                end = (std::max)(end, before->second);
                next = m_intervals.erase(before);
            }
        }
        while (next != m_intervals.end() && next->first <= end) {
            end = (std::max)(end, next->second);
            next = m_intervals.erase(next);
        }
        m_intervals.emplace(begin, end);
    }
    // emit(begin, end) for each part of [begin, end) outside the set, in order.
    template <class Emit>
    void ForEachGap(std::uint64_t begin, std::uint64_t end, Emit&& emit) const {
        auto next = m_intervals.upper_bound(begin);
        if (next != m_intervals.begin()) begin = (std::max)(begin, std::prev(next)->second);
        while (begin < end) {
            if (next == m_intervals.end() || next->first >= end) {
                emit(begin, end);
                return;
            }
            if (next->first > begin) emit(begin, next->first);
            begin = (std::max)(begin, next->second);
            ++next;
        }
    }
    [[nodiscard]] const std::map<std::uint64_t, std::uint64_t>& Intervals() const noexcept { return m_intervals; }

private:
    std::map<std::uint64_t, std::uint64_t> m_intervals;
};

// TokenForTickets' token, also holding `hold` (the predecessor the copy reads, the host's delta source) while it lives.
std::shared_ptr<const async::GpuSubmissionSet> TokenHolding(
    std::vector<std::shared_ptr<TrackedUploadTicket>> tickets, std::shared_ptr<const void> hold) {
    auto token = TokenForTickets(std::move(tickets));
    if (!token) return {};
    auto holding = std::make_shared<async::GpuSubmissionSet>(*token);
    holding->isComplete = [complete = token->isComplete, hold = std::move(hold)] { return complete(); };
    return holding;
}

// Input::predecessorCopy: previous's contents GPU-copied into the leased backing where it lacks them, plus the delta uploaded.
VersionedBufferBuildOutcome BuildFromPredecessor(const VersionedBufferBuildInput& input, const VersionedBufferBuildContext& context) {
    const auto stopped = [&] { return context.stopRequested && context.stopRequested(); };
    if (!input.uploadService || input.elementStride == 0u) return Fail("versioned buffer input/upload service/stride missing");
    if (!input.backingPool) return Fail("versioned buffer input has no backing pool");
    if (!input.emitDelta) return Fail("versioned buffer predecessor copy has no emitDelta");
    if (input.gpuWritten) return Fail("versioned buffer predecessor copy cannot be GPU-written");
    if (input.elementCount > input.capacity || input.elementCount > (std::numeric_limits<std::uint64_t>::max)() / input.elementStride)
        return Fail("versioned buffer count/capacity/stride invalid");
    const auto& previous = input.previous;
    if (previous && (!previous->resource || previous->elementStride != input.elementStride))
        return Fail("versioned buffer predecessor copy needs a predecessor with a buffer and the same stride");
    if (stopped()) return Cancel();
    const auto usedBytes = input.elementCount * input.elementStride;
    const bool fitsBacking = previous && input.capacity <= previous->capacity;
    const auto mode = fitsBacking ? BufferRevisionMode::Patch : BufferRevisionMode::Replace;
    const auto capacityClass = mode == BufferRevisionMode::Patch ? previous->capacity
        : input.growth ? input.growth->Next(previous ? previous->capacity : 0, (std::max<std::uint64_t>)(input.capacity, 1u))
                       : ReplacementCapacity(input);
    auto& pool = *input.backingPool;

    BackingRequest request;
    request.capacityClass = capacityClass;
    request.stride = input.elementStride;
    request.flags = input.flags;
    request.name = input.debugName;
    BackingPool::AcquireInfo acquired;
    auto lease = pool.Acquire(request, previous ? previous->writeSequence : input.writeSequence, &acquired);
    if (!lease) {
        if (!context.allocateSuspension) return Fail("versioned buffer backing pool exhausted and the caller cannot suspend");
        VersionedBufferBuildOutcome outcome;
        outcome.kind = VersionedBufferBuildOutcome::Kind::Suspended;
        outcome.suspension = context.allocateSuspension();
        outcome.error = "versioned buffer backing pool exhausted";
        if (context.notifySuspension) pool.SubscribeAvailability(outcome.suspension, context.notifySuspension);
        return outcome;
    }
    // A backing still bound to a frame in flight (Config::reuseBound) would need the copy and the delta to wait for it on the GPU.
    if (acquired.writeAfter != 0u) return Fail("versioned buffer predecessor copy needs a pool without reuseBound");
    auto& backing = *lease;
    const auto& resource = backing.buffer;
    if (!resource) return Fail("versioned buffer backing has no buffer");
    if (previous && resource == previous->resource) return Fail("versioned buffer predecessor copy leased its predecessor's backing");

    // The delta, overlaps resolved (the later write wins), so no two of the batch's writes to the backing touch the same byte.
    struct DeltaWrite {
        const std::byte* data = nullptr;
        std::uint64_t size = 0, offset = 0;
    };
    std::vector<DeltaWrite> writes;
    bool deltaInRange = true;
    input.emitDelta([&](const void* data, std::size_t bytes, std::uint64_t offset) {
        if (bytes == 0u) return;
        if (!data || offset > usedBytes || bytes > usedBytes - offset) {
            deltaInRange = false;
            return;
        }
        writes.push_back({static_cast<const std::byte*>(data), bytes, offset});
    });
    if (!deltaInRange) return Fail("versioned buffer delta write outside the version's bytes");
    if (stopped()) return Cancel();
    ByteIntervals delta;
    std::vector<StreamingUploadRange> uploads;
    uploads.reserve(writes.size());
    for (auto write = writes.rbegin(); write != writes.rend(); ++write) {
        delta.ForEachGap(write->offset, write->offset + write->size, [&](std::uint64_t begin, std::uint64_t end) {
            uploads.push_back({write->data + (begin - write->offset), static_cast<std::size_t>(end - begin), static_cast<std::size_t>(begin)});
        });
        delta.Add(write->offset, write->offset + write->size);
    }
    std::ranges::sort(uploads, {}, &StreamingUploadRange::dstOffset);

    // What the backing lacks of previous: changedSince its contentEpoch, or previous's used bytes whole; less the delta.
    std::vector<StreamingCopyRange> copies;
    bool copiedWhole = false;
    if (previous) {
        const auto copyLimit = (std::min)(previous->elementCount * previous->elementStride, usedBytes);
        const auto held = backing.contentEpoch.load(std::memory_order_acquire);
        ByteIntervals missing;
        bool whole = acquired.allocated || held == 0u || held > previous->writeSequence || backing.capacityClass != previous->capacity;
        if (!whole && held != previous->writeSequence) {
            std::optional<std::vector<BufferByteRange>> changed;
            if (input.changedSince) changed = input.changedSince(held);
            if (!changed)
                whole = true;
            else
                for (const auto& range : *changed) {
                    const auto begin = (std::min)(range.offset, copyLimit);
                    missing.Add(begin, begin + (std::min)(range.size, copyLimit - begin));
                }
        }
        if (whole) missing.Add(0, copyLimit);
        copiedWhole = whole && copyLimit != 0u;
        for (const auto& [begin, end] : missing.Intervals())
            delta.ForEachGap(begin, end, [&](std::uint64_t from, std::uint64_t to) {
                copies.push_back({static_cast<std::size_t>(from), static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)});
            });
    } else {
        // No predecessor: the delta is the whole version.
        const auto& covered = delta.Intervals();
        if (usedBytes != 0u && (covered.size() != 1u || covered.begin()->first != 0u || covered.begin()->second != usedBytes))
            return Fail("versioned buffer delta without a predecessor does not cover the version");
    }
    if (stopped()) return Cancel();

    // From here the backing's contents change: until the new contentEpoch is set it holds nothing a successor may rely on.
    Forget(backing);
    if (backing.retired.load(std::memory_order_acquire)) return Fail("versioned buffer backing was retired before its upload was queued");
    const WorkerOwnedDestination destination{resource, WorkerOwnedDestination::Ownership::PooledBackingLease};
    std::vector<std::shared_ptr<TrackedUploadTicket>> tickets;
    // The copy first: one batch has no barriers between its entries, and the delta's ranges are disjoint from the copy's.
    if (!copies.empty()) {
        auto ticket = input.uploadService->QueueTrackedStreamingCopyRanges(previous->resource, copies, destination);
        if (!ticket) return Fail("versioned buffer predecessor copy enqueue failed (the upload service cannot copy)");
        tickets.push_back(std::move(ticket));
    }
    if (!uploads.empty()) {
        auto ticket = input.uploadService->QueueTrackedStreamingUploadRanges(uploads, destination);
        if (!ticket) {
            for (const auto& queued : tickets) queued->Cancel();
            return Fail("versioned buffer streaming upload enqueue failed");
        }
        tickets.push_back(std::move(ticket));
    }
    backing.contentEpoch.store(input.writeSequence, std::memory_order_release);

    auto version = std::make_shared<PublishedBufferVersion>();
    version->revision = context.revision;
    version->writeSequence = input.writeSequence;
    version->elementCount = input.elementCount;
    version->capacity = backing.capacityClass;
    version->elementStride = input.elementStride;
    version->revisionMode = mode;
    version->contentVersion = context.revision;
    version->contentEpoch = input.writeSequence;
    version->backingEpoch = backing.generation;
    version->resource = resource;
    version->tag = input.tag;
    version->lease = std::move(lease);
    for (const auto& upload : uploads) version->uploadedBytes += upload.size;
    for (const auto& copy : copies) version->copiedBytes += copy.size;
    version->copiedWhole = copiedWhole;

    VersionedBufferBuildOutcome outcome;
    outcome.kind = VersionedBufferBuildOutcome::Kind::Ready;
    outcome.version = std::move(version);
    struct Hold {
        std::shared_ptr<const PublishedBufferVersion> previous;
        std::shared_ptr<const void> keep;
    };
    outcome.ready = TokenHolding(std::move(tickets), std::make_shared<const Hold>(Hold{previous, input.keep}));
    return outcome;
}

} // namespace

VersionedBufferBuildOutcome BuildVersionedBuffer(const VersionedBufferBuildInput& input, const VersionedBufferBuildContext& context) {
    try {
        return input.predecessorCopy ? BuildFromPredecessor(input, context) : Build(input, context);
    } catch (const std::exception& error) {
        return Fail(error.what());
    } catch (...) {
        return Fail("versioned buffer build failed");
    }
}

std::shared_ptr<const std::vector<std::byte>> ReplayVersionedBuffer(const VersionedBufferBuildInput& input, std::string& error) {
    error.clear();
    if (input.elementStride == 0u || input.elementCount > input.capacity || input.capacity > (std::numeric_limits<std::uint32_t>::max)()) {
        error = "versioned buffer count/capacity/stride invalid";
        return {};
    }
    if (input.elementCount > (std::numeric_limits<std::size_t>::max)() / input.elementStride) {
        error = "versioned buffer byte count overflows address space";
        return {};
    }
    const auto requiredBytes = static_cast<std::size_t>(input.elementCount * input.elementStride);
    // A journal keeps writes back to its pool's trim floor (journalBaseSequence), which may precede `previous`.
    std::uint64_t validatedSequence = input.previous ? (std::min)(input.previous->writeSequence, input.journalBaseSequence) : 0u;
    for (const auto& write : input.writes) {
        if (write.sequence <= validatedSequence || write.sequence > input.writeSequence || write.byteOffset > requiredBytes ||
            write.byteSize > requiredBytes - write.byteOffset || write.byteOffset % input.elementStride != 0u ||
            write.byteSize % input.elementStride != 0u) {
            error = "versioned buffer write journal sequence/range invalid";
            return {};
        }
        validatedSequence = write.sequence;
    }
    if (!input.writes.empty() && validatedSequence != input.writeSequence) {
        error = "versioned buffer is not closed through requested write sequence";
        return {};
    }
    if (input.image) {
        if (input.image->ByteSize() != requiredBytes) {
            error = "versioned buffer desired image byte count does not match rows";
            return {};
        }
        return input.image->Materialize();
    }
    auto shadow = std::make_shared<std::vector<std::byte>>(requiredBytes);
    if (!input.bytes.empty()) {
        if (requiredBytes != input.bytes.size()) {
            error = "versioned buffer byte count does not match rows";
            return {};
        }
        *shadow = input.bytes;
    } else if (input.previous && input.previous->image) {
        const auto previous = input.previous->image->Materialize();
        const auto copyBytes = (std::min)(shadow->size(), previous->size());
        std::copy_n(previous->begin(), copyBytes, shadow->begin());
    }
    std::uint64_t lastSequence = input.previous ? input.previous->writeSequence : 0u;
    for (const auto& write : input.writes) {
        if (write.sequence <= lastSequence) continue;  // kept for the trim floor: already in `previous`
        if (write.byteSize != 0u) {
            error = "versioned buffer deltas require an immutable paged image";
            return {};
        }
        lastSequence = write.sequence;
    }
    // A full snapshot is self-contained: its revision need not follow the previous journal sequence. Only delta captures must
    // prove that their journal closes through the requested sequence.
    if (input.bytes.empty() && ((!input.writes.empty() && lastSequence != input.writeSequence) ||
                                   (input.writes.empty() && input.previous && input.writeSequence != input.previous->writeSequence))) {
        error = "versioned buffer is not closed through requested write sequence";
        return {};
    }
    return shadow;
}

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
} // namespace org::services
