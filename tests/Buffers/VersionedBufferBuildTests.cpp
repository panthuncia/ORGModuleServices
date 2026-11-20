// BuildVersionedBuffer through a recording upload service.
// Compiled against OpenRenderGraph's headers (nothing of it is linked): backings carry stand-in org::Buffer pointers that only the
// recording service sees, as keys of their CPU-side contents.
#include <chrono>
#include <ORGModuleServices/Async/VersionedBufferProducer.h>
#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/VersionedBufferBuild.h>

#include "Render/Runtime/IUploadService.h"
#include "Resources/Buffers/Buffer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <utility>
#include <vector>

using namespace org::services;

namespace {

void Check(bool condition, const std::source_location location = std::source_location::current()) {
    if (condition) return;
    std::fprintf(stderr, "check failed at %s:%u\n", location.file_name(), static_cast<unsigned>(location.line()));
    std::fflush(stderr);
    std::abort();
}

// Records the worker-path uploads into per-resource byte images; tickets complete at once.
class RecordingUploads final : public org::runtime::IUploadService {
public:
    struct Upload {
        const org::Resource* resource = nullptr;
        std::size_t offset = 0, size = 0;
    };
    std::map<const org::Resource*, std::vector<std::byte>> memory;
    std::vector<Upload> uploads;
    bool refuse = false;

    std::shared_ptr<org::TrackedUploadTicket> QueueTrackedStreamingUploadSegments(std::span<const org::StreamingUploadSegment> segments,
        size_t totalSize, org::WorkerOwnedDestination destination, size_t dstOffset) override {
        if (refuse) return {};
        Check(destination.ownership == org::WorkerOwnedDestination::Ownership::PooledBackingLease && destination.resource);
        auto& bytes = memory[destination.resource.get()];
        if (bytes.size() < dstOffset + totalSize) bytes.resize(dstOffset + totalSize);
        std::size_t cursor = dstOffset;
        for (const auto& segment : segments) {
            std::memcpy(bytes.data() + cursor, segment.data, segment.size);
            cursor += segment.size;
        }
        Check(cursor == dstOffset + totalSize);
        uploads.push_back({destination.resource.get(), dstOffset, totalSize});
        auto ticket = std::make_shared<org::TrackedUploadTicket>();
        ticket->state.store(org::TrackedUploadTicketState::Completed);
        return ticket;
    }
    std::size_t UploadedBytes() const {
        std::size_t total = 0;
        for (const auto& upload : uploads) total += upload.size;
        return total;
    }

    // GPU copies between the recorded images, applied in queue order (as the worker path orders them after earlier uploads).
    struct Copy {
        const org::Resource* source = nullptr;
        const org::Resource* destination = nullptr;
        std::size_t srcOffset = 0, dstOffset = 0, size = 0;
    };
    std::vector<Copy> copies;
    std::size_t copyCalls = 0;
    bool cannotCopy = false;

    std::shared_ptr<org::TrackedUploadTicket> QueueTrackedStreamingCopyRanges(std::shared_ptr<org::Resource> source,
        std::span<const org::StreamingCopyRange> ranges, org::WorkerOwnedDestination destination) override {
        if (cannotCopy) return {};
        Check(destination.ownership == org::WorkerOwnedDestination::Ownership::PooledBackingLease && destination.resource && source);
        Check(source.get() != destination.resource.get());
        ++copyCalls;
        const auto& from = memory.at(source.get());
        auto& to = memory[destination.resource.get()];
        for (const auto& range : ranges) {
            Check(range.size != 0u && from.size() >= range.srcOffset + range.size);
            if (to.size() < range.dstOffset + range.size) to.resize(range.dstOffset + range.size);
            std::memcpy(to.data() + range.dstOffset, from.data() + range.srcOffset, range.size);
            copies.push_back({source.get(), destination.resource.get(), range.srcOffset, range.dstOffset, range.size});
        }
        auto ticket = std::make_shared<org::TrackedUploadTicket>();
        ticket->state.store(org::TrackedUploadTicketState::Completed);
        return ticket;
    }
    std::size_t CopiedBytes() const {
        std::size_t total = 0;
        for (const auto& copy : copies) total += copy.size;
        return total;
    }
    void Clear() {
        uploads.clear();
        copies.clear();
        copyCalls = 0;
    }

    void Initialize() override {}
    void SetUploadResolveContext(org::runtime::UploadResolveContext) override {}
    std::shared_ptr<org::RenderPass> GetUploadPass() const override { return {}; }
    void UploadData(const void*, size_t, org::runtime::UploadTarget, size_t, const char*, int) override {}
    void UploadTextureSubresources(org::runtime::UploadTarget, rhi::Format, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
        const rhi::helpers::SubresourceData*, uint32_t, const char*, int) override {}
    void QueueResourceCopy(const std::shared_ptr<org::Resource>&, const std::shared_ptr<org::Resource>&, size_t) override {}
    void ProcessDeferredReleases(uint8_t) override {}
    void Cleanup() override {}
};

const org::Resource* KeyOf(const std::shared_ptr<org::Buffer>& buffer) { return std::shared_ptr<org::Resource>(buffer).get(); }

// A pool whose backings carry stand-in buffer pointers (never dereferenced; distinct per backing) and distinct device addresses.
std::shared_ptr<BackingPool> MakePool(std::shared_ptr<RetirementClock> clock, std::uint32_t maxPerClass = 0, std::uint32_t spareFree = 4) {
    BackingPoolHooks hooks;
    auto next = std::make_shared<std::uint64_t>(0);
    hooks.allocate = [next](const BackingRequest& request) {
        const auto index = ++*next;
        auto storage = std::shared_ptr<void>(::operator new(sizeof(org::Buffer)), [](void* block) { ::operator delete(block); });
        BackingAllocation allocation;
        allocation.buffer = std::shared_ptr<org::Buffer>(storage, static_cast<org::Buffer*>(storage.get()));
        allocation.address = 0x100000ull * index;
        allocation.srvIndex = static_cast<std::uint32_t>(index);
        (void)request;
        return allocation;
    };
    return BackingPool::Create(std::move(hooks), std::move(clock), {.name = "build", .maxPerClass = maxPerClass, .framesInFlight = 3, .spareFree = spareFree});
}

VersionedBufferBuildContext Context(std::uint64_t revision) {
    VersionedBufferBuildContext context;
    context.revision = revision;
    return context;
}

// The backing's recorded contents equal `expected` (over its length).
void CheckContents(const RecordingUploads& uploads, const BackingLease& lease, std::span<const std::byte> expected) {
    const auto found = uploads.memory.find(KeyOf(lease->buffer));
    Check(found != uploads.memory.end() && found->second.size() >= expected.size());
    Check(std::memcmp(found->second.data(), expected.data(), expected.size()) == 0);
}

// The leased backing GPU-copies what it lacks of the predecessor (changedSince its contentEpoch, or the
// predecessor whole), less the delta, which is uploaded once.
void TestPredecessorCopy() {
    auto clock = std::make_shared<RetirementClock>();
    auto pool = MakePool(clock);
    auto uploads = std::make_shared<RecordingUploads>();
    constexpr std::uint32_t kRows = 64;
    constexpr std::uint32_t kRow = sizeof(std::uint32_t);

    // The host: its rows, a log of (sequence, row) writes, and the rows written since the last version (the next delta).
    std::vector<std::uint32_t> rows;
    std::vector<std::pair<std::uint64_t, std::size_t>> log;
    std::vector<std::size_t> pending;
    std::uint64_t next = 1;
    std::uint64_t capacity = kRows;
    bool journalReaches = true;
    const auto set = [&](std::size_t row, std::uint32_t value) {
        if (row >= rows.size()) rows.resize(row + 1);
        rows[row] = value;
        log.emplace_back(next, row);
        pending.push_back(row);
    };
    const auto build = [&](std::shared_ptr<const PublishedBufferVersion> previous, std::shared_ptr<const void> keep = {}) {
        VersionedBufferBuildInput input;
        input.uploadOwner = uploads;
        input.uploadService = uploads.get();
        input.debugName = "copy";
        input.writeSequence = next;
        input.elementStride = kRow;
        input.elementCount = rows.size();
        input.capacity = capacity;
        input.backingPool = pool;
        const std::uint64_t through = previous ? previous->writeSequence : 0u;
        input.previous = std::move(previous);
        input.changedSince = [&, through](std::uint64_t from) -> std::optional<std::vector<BufferByteRange>> {
            if (!journalReaches) return std::nullopt;
            std::vector<BufferByteRange> ranges;
            for (const auto& [sequence, row] : log)
                if (sequence > from && sequence <= through) ranges.push_back({row * kRow, kRow});
            return ranges;
        };
        input.emitDelta = [&](const auto& write) {
            for (const auto row : pending) write(&rows[row], kRow, row * kRow);
        };
        input.keep = std::move(keep);
        uploads->Clear();
        auto outcome = BuildVersionedBuffer(input, Context(next));
        if (outcome.kind == VersionedBufferBuildOutcome::Kind::Ready) {
            ++next;
            pending.clear();
        }
        return outcome;
    };
    const auto take = [](VersionedBufferBuildOutcome& outcome) {
        Check(outcome.kind == VersionedBufferBuildOutcome::Kind::Ready && outcome.version);
        return std::shared_ptr<const PublishedBufferVersion>(std::move(outcome.version));
    };
    const auto contents = [&] { return std::as_bytes(std::span(rows)); };

    // v1, no predecessor: the delta is the whole version, uploaded alone.
    for (std::uint32_t row = 0; row < kRows; ++row) set(row, row);
    auto outcome = build(nullptr);
    auto v1 = take(outcome);
    Check(uploads->copyCalls == 0 && uploads->UploadedBytes() == kRows * kRow);
    Check(v1->writeSequence == 1 && v1->lease->contentEpoch.load() == 1);
    CheckContents(*uploads, v1->lease, contents());
    const auto* backingA = v1->lease.Get().get();

    // v2 while v1 holds A: a fresh backing B, so v1 is copied whole less the delta (row 5), which is uploaded once.
    set(5, 500);
    outcome = build(v1);
    auto v2 = take(outcome);
    const auto* backingB = v2->lease.Get().get();
    Check(backingB != backingA && v2->revisionMode == BufferRevisionMode::Patch && v2->lease->contentEpoch.load() == 2);
    Check(uploads->copyCalls == 1 && uploads->copies.size() == 2 && uploads->CopiedBytes() == (kRows - 1) * kRow);
    Check(uploads->copies[0].source == KeyOf(v1->resource) && uploads->copies[0].dstOffset == 0 && uploads->copies[0].size == 5 * kRow);
    Check(uploads->uploads.size() == 1 && uploads->uploads[0].offset == 5 * kRow && uploads->UploadedBytes() == kRow);
    CheckContents(*uploads, v2->lease, contents());

    // v3 while v1 and v2 hold theirs: another fresh backing C, v2 whole less rows 9..11.
    set(9, 900);
    set(10, 1000);
    set(11, 1100);
    outcome = build(v2);
    auto v3 = take(outcome);
    Check(v3->lease.Get().get() != backingA && v3->lease.Get().get() != backingB);
    Check(uploads->CopiedBytes() == (kRows - 3) * kRow && uploads->UploadedBytes() == 3 * kRow);
    CheckContents(*uploads, v3->lease, contents());

    // v4: v1 goes, so A (epoch 1, two versions behind v3) is reused. It copies only changedSince(1) - row 5 and rows 9..11 - less
    // the delta (row 10, and row 20 written twice), and the delta is uploaded once.
    v1.reset();
    set(10, 1001);
    set(20, 2000);
    set(20, 2001);
    outcome = build(v3);
    auto v4 = take(outcome);
    Check(v4->lease.Get().get() == backingA && v4->lease->contentEpoch.load() == 4);
    Check(uploads->copyCalls == 1 && uploads->copies.size() == 3 && uploads->CopiedBytes() == 3 * kRow);
    Check(uploads->copies[0].dstOffset == 5 * kRow && uploads->copies[1].dstOffset == 9 * kRow && uploads->copies[2].dstOffset == 11 * kRow);
    Check(uploads->copies[0].source == KeyOf(v3->resource) && uploads->copies[0].srcOffset == 5 * kRow);
    Check(uploads->uploads.size() == 2 && uploads->UploadedBytes() == 2 * kRow);
    CheckContents(*uploads, v4->lease, contents());

    // v5: v2 goes and B (epoch 2) is reused, but the journal no longer reaches back (changedSince is nullopt): v4 whole less row 30.
    v2.reset();
    journalReaches = false;
    set(30, 3000);
    outcome = build(v4);
    auto v5 = take(outcome);
    Check(v5->lease.Get().get() == backingB && v5->lease->contentEpoch.load() == 5);
    Check(uploads->CopiedBytes() == (kRows - 1) * kRow && uploads->UploadedBytes() == kRow);
    CheckContents(*uploads, v5->lease, contents());
    journalReaches = true;

    // v6, growth to 80 rows: a Replace into a bigger class copies v5's used bytes whole, less the delta (row 0 and the 16 new rows).
    capacity = 128;
    set(0, 7);
    for (std::uint32_t row = kRows; row < kRows + 16; ++row) set(row, 10000 + row);
    outcome = build(v5);
    auto v6 = take(outcome);
    Check(v6->revisionMode == BufferRevisionMode::Replace && v6->capacity == 128 && v6->elementCount == kRows + 16);
    Check(uploads->copies.size() == 1 && uploads->copies[0].dstOffset == kRow && uploads->CopiedBytes() == (kRows - 1) * kRow);
    Check(uploads->UploadedBytes() == 17 * kRow);
    CheckContents(*uploads, v6->lease, contents());

    // The ready token holds the predecessor (the copy's source) and the input's keep until it goes.
    auto keep = std::make_shared<int>(1);
    const std::weak_ptr<int> kept = keep;
    const std::weak_ptr<const PublishedBufferVersion> predecessor = v6;
    set(1, 11);
    outcome = build(v6, std::move(keep));
    auto v7 = take(outcome);
    v6.reset();
    Check(outcome.ready && outcome.ready->Complete() && !kept.expired() && !predecessor.expired());
    CheckContents(*uploads, v7->lease, contents());
    outcome.ready.reset();
    Check(kept.expired() && predecessor.expired());

    // An upload service that cannot copy: v6's freed backing (epoch 6) would catch up with a copy, so the build fails, nothing is
    // uploaded, and the backing holds nothing a successor may catch up from.
    uploads->cannotCopy = true;
    set(2, 22);
    outcome = build(v7);
    Check(outcome.kind == VersionedBufferBuildOutcome::Kind::Failed && !outcome.error.empty() && uploads->uploads.empty());
    std::uint64_t holdingSix = 0;
    pool->Visit([&](const Backing& backing) { holdingSix += backing.contentEpoch.load() == 6; });
    Check(holdingSix == 0);
    uploads->cannotCopy = false;

    // Without a predecessor the delta must cover the version.
    rows.assign(4, 1);
    pending = {0};
    capacity = 4;
    outcome = build(nullptr);
    Check(outcome.kind == VersionedBufferBuildOutcome::Kind::Failed && uploads->uploads.empty());
}

void TestExhaustionSuspends() {
    auto clock = std::make_shared<RetirementClock>();
    auto pool = MakePool(clock, 1);
    auto uploads = std::make_shared<RecordingUploads>();
    const std::vector<std::uint32_t> rows{1, 2, 3, 4};
    VersionedBufferBuildInput input;
    input.uploadOwner = uploads;
    input.uploadService = uploads.get();
    input.writeSequence = 1;
    input.elementStride = 4;
    input.elementCount = 4;
    input.capacity = 4;
    input.emitDelta = [&](const auto& write) { write(rows.data(), rows.size() * sizeof(std::uint32_t), 0); };
    input.backingPool = pool;
    auto first = BuildVersionedBuffer(input, Context(1));
    Check(first.kind == VersionedBufferBuildOutcome::Kind::Ready);

    std::uint64_t notified = 0;
    auto context = Context(2);
    context.allocateSuspension = [] { return std::uint64_t{42}; };
    context.notifySuspension = [&](std::uint64_t identity) { notified = identity; };
    input.writeSequence = 2;
    auto second = BuildVersionedBuffer(input, context);
    Check(second.kind == VersionedBufferBuildOutcome::Kind::Suspended && second.suspension == 42 && notified == 0);
    first.version.reset();  // its lease returns: the pool's availability wakes the graph
    Check(notified == 42);
    auto retried = BuildVersionedBuffer(input, context);
    Check(retried.kind == VersionedBufferBuildOutcome::Kind::Ready);
}

} // namespace

// Input::shrink: a chain whose capacity stayed at most the policy's share of its class for the window is made a Replace at the fresh
// class for the window's peak; the predecessor's copy is clipped to the smaller version's used bytes.
void TestPredecessorShrink() {
    auto clock = std::make_shared<RetirementClock>();
    auto pool = MakePool(clock);
    auto uploads = std::make_shared<RecordingUploads>();
    constexpr std::uint32_t kRow = sizeof(std::uint32_t);
    std::vector<std::uint32_t> rows;
    std::vector<std::size_t> pending;
    std::uint64_t next = 1;
    const auto build = [&](std::shared_ptr<const PublishedBufferVersion> previous) {
        VersionedBufferBuildInput input;
        input.uploadOwner = uploads;
        input.uploadService = uploads.get();
        input.debugName = "shrink";
        input.writeSequence = next;
        input.elementStride = kRow;
        input.elementCount = rows.size();
        input.capacity = (std::max<std::uint64_t>)(rows.size(), 1);
        input.backingPool = pool;
        input.shrink = org::services::ShrinkPolicy::WhenBelow(25, std::chrono::milliseconds(0), 2);
        input.previous = std::move(previous);
        input.changedSince = [](std::uint64_t) -> std::optional<std::vector<BufferByteRange>> { return std::nullopt; };
        input.emitDelta = [&](const auto& write) {
            for (const auto row : pending) write(&rows[row], kRow, row * kRow);
        };
        uploads->Clear();
        auto outcome = BuildVersionedBuffer(input, Context(next));
        Check(outcome.kind == VersionedBufferBuildOutcome::Kind::Ready && outcome.version);
        ++next;
        pending.clear();
        return std::shared_ptr<const PublishedBufferVersion>(std::move(outcome.version));
    };
    const auto contents = [&] { return std::as_bytes(std::span(rows)); };

    // v1: 256 rows, class 256 (a power of two).
    for (std::uint32_t row = 0; row < 256; ++row) {
        rows.push_back(row);
        pending.push_back(row);
    }
    auto v1 = build(nullptr);
    Check(v1->capacity == 256 && v1->shrinkWindow.seen == 0);

    // v2: the scene falls to 20 rows (row 3 changed): the first low ask opens the window, still a Patch at 256.
    rows.resize(20);
    rows[3] = 333;
    pending = {3};
    auto v2 = build(v1);
    Check(v2->revisionMode == BufferRevisionMode::Patch && v2->capacity == 256 && v2->shrinkWindow.seen == 1 && !v2->shrunk);
    Check(uploads->CopiedBytes() + uploads->UploadedBytes() == 20 * kRow);
    CheckContents(*uploads, v2->lease, contents());

    // v3: the window's second low ask: a Replace at the fresh class for the peak (32), v2 copied whole up to the 20 rows it holds.
    rows[7] = 777;
    pending = {7};
    auto v3 = build(v2);
    Check(v3->revisionMode == BufferRevisionMode::Replace && v3->capacity == 32 && v3->shrunk && v3->shrinkWindow.seen == 0);
    Check(uploads->CopiedBytes() == 19 * kRow && uploads->UploadedBytes() == kRow && v3->copiedWhole);
    for (const auto& copy : uploads->copies) Check(copy.dstOffset + copy.size <= 20 * kRow);
    CheckContents(*uploads, v3->lease, contents());

    // v4, v5: 20 of 32 is not low: Patches at 32, the window stays closed.
    auto v4 = build(v3);
    auto v5 = build(v4);
    Check(v5->revisionMode == BufferRevisionMode::Patch && v5->capacity == 32 && v5->shrinkWindow.seen == 0);

    // v6: growth past the class again: a Replace grown from 32, the window reset.
    for (std::uint32_t row = 20; row < 40; ++row) {
        rows.push_back(row);
        pending.push_back(row);
    }
    auto v6 = build(v5);
    Check(v6->revisionMode == BufferRevisionMode::Replace && v6->capacity >= 40 && !v6->shrunk);
    CheckContents(*uploads, v6->lease, contents());
}

int main() {
    TestPredecessorCopy();
    TestPredecessorShrink();
    TestExhaustionSuspends();
    std::puts("ORGBufferBuildTests: all passed");
    return 0;
}
