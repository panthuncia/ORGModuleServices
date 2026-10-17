#include <ORGModuleServices/JournaledBufferSet.h>

#include <ORGModuleServices/VersionedBufferBuild.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include "Render/Runtime/IUploadService.h"
#include "Render/Runtime/StreamingUploadTypes.h"
#include "Resources/Buffers/Buffer.h"

namespace org::services {

void JournaledSink::CheckRange(std::uint64_t offset, std::uint64_t bytes) const {
    const auto& target = *m_target;
    if (offset > target.byteCapacity || bytes > target.byteCapacity - offset)
        throw std::out_of_range("JournaledSink: a write past its backing (" + std::to_string(offset) + "+" + std::to_string(bytes) + " of " +
            std::to_string(target.byteCapacity) + " bytes)");
    // An append: published versions read below the mark, so nothing there may change under them.
    if (offset < m_floor)
        throw std::logic_error("JournaledSink: an append below its backing's sealed mark (" + std::to_string(offset) + " < " + std::to_string(m_floor) + " bytes)");
}

void JournaledSink::Write(const void* data, std::size_t bytes, std::uint64_t offset) {
    if (bytes == 0) return;
    const StreamingUploadSegment segment{data, bytes};
    Write(std::span<const StreamingUploadSegment>(&segment, 1), bytes, offset);
}

void JournaledSink::Write(std::span<const StreamingUploadSegment> segments, std::size_t totalBytes, std::uint64_t offset) {
    if (totalBytes == 0) return;
    const auto& target = *m_target;
    CheckRange(offset, totalBytes);
    if (!target.buffer) throw std::logic_error("JournaledSink: the backing has no buffer");
    // A leased backing: no lease before this one and the clock past every frame that used it, so no queue reads it now; or an
    // append, past every byte a published version names.
    if (target.retired.load(std::memory_order_acquire)) throw std::logic_error("JournaledSink: the backing was retired before its write");
    auto ticket = m_uploads->QueueTrackedStreamingUploadSegments(segments, totalBytes,
        WorkerOwnedDestination{target.buffer, WorkerOwnedDestination::Ownership::PooledBackingLease}, static_cast<std::size_t>(offset));
    if (!ticket) throw std::runtime_error("JournaledSink: the uploader refused a write");
    m_tickets->push_back(std::move(ticket));
    m_bytes += totalBytes;
    ++m_runs;
    m_firstByte = (std::min)(m_firstByte, offset);
}

void JournaledSink::Write(std::span<const StreamingUploadRange> ranges) {
    const auto& target = *m_target;
    std::size_t totalBytes = 0, runs = 0;
    std::uint64_t firstByte = ~std::uint64_t{0};
    for (const auto& range : ranges) {
        if (range.size == 0) continue;
        CheckRange(range.dstOffset, range.size);
        totalBytes += range.size;
        ++runs;
        firstByte = (std::min)(firstByte, static_cast<std::uint64_t>(range.dstOffset));
    }
    if (totalBytes == 0) return;
    if (!target.buffer) throw std::logic_error("JournaledSink: the backing has no buffer");
    if (target.retired.load(std::memory_order_acquire)) throw std::logic_error("JournaledSink: the backing was retired before its write");
    auto ticket = m_uploads->QueueTrackedStreamingUploadRanges(ranges, WorkerOwnedDestination{target.buffer, WorkerOwnedDestination::Ownership::PooledBackingLease});
    if (!ticket || ticket->state.load(std::memory_order_acquire) == TrackedUploadTicketState::Cancelled)
        throw std::runtime_error("JournaledSink: the uploader refused a write");
    m_tickets->push_back(std::move(ticket));
    m_bytes += totalBytes;
    m_runs += runs;
    m_firstByte = (std::min)(m_firstByte, firstByte);
}

JournaledBufferSet::JournaledBufferSet(
    std::vector<JournaledPartConfig> parts, BackingPoolHooks hooks, std::shared_ptr<RetirementClock> clock, BackingPool::Config config) {
    m_parts.reserve(parts.size());
    for (auto& part : parts) {
        if (part.name.empty()) throw std::invalid_argument("JournaledBufferSet: a part without a name");
        auto partConfig = config;
        partConfig.name = part.name;
        PartState state;
        state.pool = BackingPool::Create(hooks, clock, std::move(partConfig));
        state.config = std::move(part);
        m_parts.push_back(std::move(state));
    }
}

JournaledBufferSet::~JournaledBufferSet() = default;

JournaledPublication JournaledBufferSet::Publish(std::span<const JournaledPartRequest> requests, runtime::IUploadService& uploads) {
    if (requests.size() != m_parts.size())
        throw std::invalid_argument("JournaledBufferSet::Publish: " + std::to_string(requests.size()) + " requests for " + std::to_string(m_parts.size()) + " parts");
    JournaledPublication publication;
    publication.parts.resize(m_parts.size());
    std::vector<std::shared_ptr<TrackedUploadTicket>> tickets;
    try {
        for (std::size_t index = 0; index < m_parts.size(); ++index) {
            const auto& request = requests[index];
            auto& part = m_parts[index];
            auto& version = publication.parts[index];
            if (!request.units && !request.emit) continue;  // absent from this publication
            if (!request.emit) throw std::invalid_argument("JournaledBufferSet::Publish: part '" + part.config.name + "' has units and no emit");
            // An append into the head (published versions name it): the units fit what it was made with, else a lease as without it.
            const auto& head = request.appendInto;
            if (head && (head->stride != part.config.stride || !(head->flags == part.config.flags)))
                throw std::invalid_argument("JournaledBufferSet::Publish: part '" + part.config.name + "' appends into another part's backing");
            const bool append = head && request.units <= head->capacityClass;
            const auto unitBytes = [&](std::uint64_t units) { return part.config.stride ? units * part.config.stride : units; };

            BackingLease lease;
            BackingPool::AcquireInfo acquired;
            std::uint64_t capacityClass = 0;
            if (append) {
                lease = head;
                capacityClass = head->capacityClass;
            } else {
                // A part's class only grows: content that shrinks keeps reusing its backings.
                capacityClass = part.config.policy.Next(part.highWater, request.units);
                part.highWater = (std::max)(part.highWater, capacityClass);

                BackingRequest backingRequest;
                backingRequest.capacityClass = capacityClass;
                backingRequest.stride = part.config.stride;
                backingRequest.flags = part.config.flags;
                backingRequest.name = part.config.name;
                lease = part.pool->Acquire(backingRequest, request.preferEpoch, &acquired);
                if (!lease) throw std::runtime_error("JournaledBufferSet::Publish: part '" + part.config.name + "' exhausted its pool (Config::maxPerClass)");
                // A fresh fill: no version names it yet (the pool reset the mark when it reused it; a new one has none).
                lease->sealedBytes.store(0, std::memory_order_relaxed);
            }
            auto& backing = *lease;
            const auto sealedFrom = append ? backing.sealedBytes.load(std::memory_order_relaxed) : 0;

            JournaledTarget target;
            target.contentEpoch = acquired.allocated ? 0 : backing.contentEpoch.load(std::memory_order_acquire);
            target.address = backing.address;
            target.srvIndex = backing.srvIndex;
            target.capacityUnits = capacityClass;
            target.allocated = acquired.allocated;
            version.lease = lease;
            // While the emit writes, the backing holds nothing a later publication may catch up from (an emit that throws midway).
            backing.contentEpoch.store(0, std::memory_order_release);
            JournaledSink sink(uploads, backing, tickets, sealedFrom);
            const auto contentEpoch = request.emit(target, sink);
            backing.contentEpoch.store(contentEpoch, std::memory_order_release);
            // This publication names its units of the backing: no later append writes below them.
            backing.sealedBytes.store((std::max)(sealedFrom, unitBytes(request.units)), std::memory_order_relaxed);

            version.address = backing.address;
            version.srvIndex = backing.srvIndex;
            version.units = request.units;
            version.capacityUnits = capacityClass;
            version.caughtUpFrom = target.contentEpoch;
            version.contentEpoch = contentEpoch;
            version.bytes = sink.Bytes();
            version.runs = sink.Runs();
            version.allocated = acquired.allocated;
            version.appended = append;
            version.sealedFrom = sealedFrom;
            version.firstByte = sink.FirstByte();
            publication.bytes += version.bytes;
            publication.runs += version.runs;
        }
    } catch (...) {
        // Writes already queued land in backings that now hold nothing (written whole when next leased); the leases go here.
        for (auto& version : publication.parts)
            if (version.lease) version.lease->contentEpoch.store(0, std::memory_order_release);
        throw;
    }
    publication.ready = TokenForTickets(std::move(tickets));
    return publication;
}

void JournaledBufferSet::Discard(const JournaledPublication& publication) {
    for (const auto& version : publication.parts)
        if (version.lease) version.lease->contentEpoch.store(0, std::memory_order_release);
}

std::optional<std::uint64_t> JournaledBufferSet::OldestContentEpoch(std::size_t part) const { return m_parts.at(part).pool->OldestContentEpoch(); }

} // namespace org::services
