// BuildVersionedBuffer, ReplayVersionedBuffer, JournaledBufferSet and VersionedBufferFamily through a recording upload service.
// Compiled against OpenRenderGraph's headers (nothing of it is linked): backings carry stand-in org::Buffer pointers that only the
// recording service sees, as keys of their CPU-side contents.
#include <ORGModuleServices/Async/VersionedBufferFamily.h>
#include <ORGModuleServices/Async/VersionedBufferProducer.h>
#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/BufferJournal.h>
#include <ORGModuleServices/JournaledBufferSet.h>
#include <ORGModuleServices/VersionedBufferBuild.h>

#include "Render/Runtime/IUploadService.h"
#include "Resources/Buffers/Buffer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <source_location>
#include <span>
#include <stdexcept>
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

    void Initialize() override {}
    void SetUploadResolveContext(org::runtime::UploadResolveContext) override {}
    std::shared_ptr<org::RenderPass> GetUploadPass() const override { return {}; }
#if BUILD_TYPE == BUILD_TYPE_DEBUG
    void UploadData(const void*, size_t, org::runtime::UploadTarget, size_t, const char*, int) override {}
    void UploadTextureSubresources(org::runtime::UploadTarget, rhi::Format, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
        const rhi::helpers::SubresourceData*, uint32_t, const char*, int) override {}
#else
    void UploadData(const void*, size_t, org::runtime::UploadTarget, size_t) override {}
    void UploadTextureSubresources(
        org::runtime::UploadTarget, rhi::Format, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, const rhi::helpers::SubresourceData*, uint32_t) override {}
#endif
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

std::vector<std::byte> Bytes(std::span<const std::uint32_t> rows) {
    const auto view = std::as_bytes(rows);
    return {view.begin(), view.end()};
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

void TestReplay() {
    VersionedBufferBuildInput initial{};
    initial.elementStride = sizeof(std::uint32_t);
    initial.elementCount = 3;
    initial.capacity = 4;
    initial.writeSequence = 1;
    const std::uint32_t initialRows[]{10, 20, 30};
    initial.bytes = Bytes(initialRows);
    std::string error;
    const auto initialShadow = ReplayVersionedBuffer(initial, error);
    Check(initialShadow && error.empty());

    auto previous = std::make_shared<PublishedBufferVersion>();
    previous->writeSequence = 1;
    previous->image = BufferImage::FromBytes(*initialShadow);
    VersionedBufferBuildInput successor{};
    successor.elementStride = sizeof(std::uint32_t);
    successor.elementCount = 4;
    successor.capacity = 8;
    successor.writeSequence = 3;
    successor.previous = previous;
    const std::uint32_t successorRows[]{10, 200, 30, 40};
    successor.image = BufferImage::FromBytes(std::as_bytes(std::span(successorRows)));
    successor.writes = {{2, sizeof(std::uint32_t), sizeof(std::uint32_t)}, {3, 3 * sizeof(std::uint32_t), sizeof(std::uint32_t)}};
    const auto successorShadow = ReplayVersionedBuffer(successor, error);
    Check(successorShadow && error.empty());
    const auto* rows = reinterpret_cast<const std::uint32_t*>(successorShadow->data());
    Check(rows[0] == 10 && rows[1] == 200 && rows[2] == 30 && rows[3] == 40);

    successor.writeSequence = 4;  // the journal does not close through it
    Check(!ReplayVersionedBuffer(successor, error) && !error.empty());

    // A complete-image successor may advance independently of the prior journal sequence and replaces its contents.
    VersionedBufferBuildInput fullSuccessor{};
    fullSuccessor.elementStride = sizeof(std::uint32_t);
    fullSuccessor.elementCount = 3;
    fullSuccessor.capacity = 4;
    fullSuccessor.writeSequence = 9;
    fullSuccessor.previous = previous;
    const std::uint32_t fullRows[]{70, 80, 90};
    fullSuccessor.bytes = Bytes(fullRows);
    const auto fullShadow = ReplayVersionedBuffer(fullSuccessor, error);
    Check(fullShadow && error.empty() && std::memcmp(fullShadow->data(), fullRows, sizeof(fullRows)) == 0);

    // A journal capture replays through its image.
    BufferJournal journal(sizeof(std::uint32_t));
    journal.Initialize(std::as_bytes(std::span(initialRows)), 3, 4);
    auto published = std::make_shared<PublishedBufferVersion>();
    published->writeSequence = 1;
    published->image = journal.CaptureDesired().image;
    journal.Acknowledge(published);
    journal.RequestCapacity(8);
    const std::uint32_t replacement = 200;
    journal.AppendWrite(1, std::as_bytes(std::span(&replacement, 1)), 4);
    const auto capture = journal.CaptureDesired();
    VersionedBufferBuildInput input{};
    input.elementStride = sizeof(std::uint32_t);
    input.elementCount = capture.elementCount;
    input.capacity = capture.capacity;
    input.writeSequence = capture.writeSequence;
    input.previous = capture.previous;
    input.writes = capture.writes;
    input.image = capture.image;
    input.journalBaseSequence = capture.journalBaseSequence;
    const auto replay = ReplayVersionedBuffer(input, error);
    Check(replay && error.empty());
    rows = reinterpret_cast<const std::uint32_t*>(replay->data());
    Check(rows[0] == 10 && rows[1] == 200 && rows[2] == 30 && rows[3] == 0);
}

void TestBuildAndCatchUp() {
    auto clock = std::make_shared<RetirementClock>();
    auto pool = MakePool(clock);
    auto uploads = std::make_shared<RecordingUploads>();

    constexpr std::uint32_t kRows = 64;
    BufferJournal journal(sizeof(std::uint32_t));
    std::vector<std::uint32_t> rows(kRows);
    for (std::uint32_t i = 0; i < kRows; ++i) rows[i] = i;
    journal.Initialize(std::as_bytes(std::span(rows)), kRows, kRows);
    const auto inputOf = [&](const BufferJournal::Capture& capture) {
        VersionedBufferBuildInput input;
        input.uploadOwner = uploads;
        input.uploadService = uploads.get();
        input.debugName = "catchup";
        input.writeSequence = capture.writeSequence;
        input.elementStride = sizeof(std::uint32_t);
        input.elementCount = capture.elementCount;
        input.capacity = capture.capacity;
        input.previous = capture.previous;
        input.backingPool = pool;
        input.writes = capture.writes;
        input.image = capture.image;
        input.journalBaseSequence = capture.journalBaseSequence;
        input.tag = std::make_shared<const int>(7);
        return input;
    };

    // v1: a new backing, written whole.
    auto outcome = BuildVersionedBuffer(inputOf(journal.CaptureDesired()), Context(1));
    Check(outcome.kind == VersionedBufferBuildOutcome::Kind::Ready && outcome.version && outcome.ready && outcome.ready->Complete());
    auto v1 = std::shared_ptr<const PublishedBufferVersion>(std::move(outcome.version));
    Check(v1->revisionMode == BufferRevisionMode::Replace && v1->capacity == kRows && v1->lease && v1->lease->contentEpoch.load() == 1);
    Check(v1->resource == v1->lease->buffer && v1->tag && *std::static_pointer_cast<const int>(v1->tag) == 7);
    Check(uploads->UploadedBytes() == kRows * sizeof(std::uint32_t));
    CheckContents(*uploads, v1->lease, std::as_bytes(std::span(rows)));
    const auto* backingA = v1->lease.Get().get();

    // The journal acknowledges a record of v1 that holds no lease, so v1's backing is free once v1 goes.
    auto record = std::make_shared<PublishedBufferVersion>();
    record->writeSequence = v1->writeSequence;
    record->elementCount = v1->elementCount;
    record->capacity = v1->capacity;
    record->elementStride = v1->elementStride;
    record->resource = v1->resource;
    record->image = v1->image;
    journal.Acknowledge(record);
    v1.reset();

    // v2: a Patch into the same class; the free backing at epoch 1 catches up with the one write since.
    rows[5] = 500;
    journal.AppendWrite(5, std::as_bytes(std::span(&rows[5], 1)), kRows);
    uploads->uploads.clear();
    outcome = BuildVersionedBuffer(inputOf(journal.CaptureDesired()), Context(2));
    Check(outcome.kind == VersionedBufferBuildOutcome::Kind::Ready);
    auto v2 = std::shared_ptr<const PublishedBufferVersion>(std::move(outcome.version));
    Check(v2->revisionMode == BufferRevisionMode::Patch && v2->lease.Get().get() == backingA && v2->lease->contentEpoch.load() == 2);
    Check(uploads->uploads.size() == 1 && uploads->uploads[0].offset == 5 * sizeof(std::uint32_t) && uploads->UploadedBytes() == sizeof(std::uint32_t));
    CheckContents(*uploads, v2->lease, std::as_bytes(std::span(rows)));

    // v3 while v2 holds that backing: a new one, written whole (it holds nothing).
    rows[9] = 900;
    journal.AppendWrite(9, std::as_bytes(std::span(&rows[9], 1)), kRows);
    uploads->uploads.clear();
    outcome = BuildVersionedBuffer(inputOf(journal.CaptureDesired()), Context(3));
    auto v3 = std::shared_ptr<const PublishedBufferVersion>(std::move(outcome.version));
    Check(v3 && v3->lease.Get().get() != backingA && uploads->UploadedBytes() == kRows * sizeof(std::uint32_t));
    CheckContents(*uploads, v3->lease, std::as_bytes(std::span(rows)));

    // Growth: a Replace grows the class (at least 1.5x) and writes the whole image.
    journal.RequestCapacity(kRows * 2);
    std::vector<std::uint32_t> grownRows(rows);
    grownRows.push_back(1234);
    journal.AppendWrite(kRows, std::as_bytes(std::span(&grownRows[kRows], 1)), kRows + 1);
    auto grownCapture = journal.CaptureDesired();
    auto grownInput = inputOf(grownCapture);
    grownInput.previous = v3;
    uploads->uploads.clear();
    outcome = BuildVersionedBuffer(grownInput, Context(4));
    Check(outcome.kind == VersionedBufferBuildOutcome::Kind::Ready);
    Check(outcome.version->revisionMode == BufferRevisionMode::Replace && outcome.version->capacity == kRows * 2);
    Check(uploads->UploadedBytes() == (kRows + 1) * sizeof(std::uint32_t));
    CheckContents(*uploads, outcome.version->lease, std::as_bytes(std::span(grownRows)));

    // A GPU-written version uploads nothing and keeps no image.
    VersionedBufferBuildInput gpu;
    gpu.uploadOwner = uploads;
    gpu.uploadService = uploads.get();
    gpu.writeSequence = 1;
    gpu.elementStride = 16;
    gpu.elementCount = 8;
    gpu.capacity = 8;
    gpu.gpuWritten = true;
    gpu.backingPool = pool;
    uploads->uploads.clear();
    outcome = BuildVersionedBuffer(gpu, Context(1));
    Check(outcome.kind == VersionedBufferBuildOutcome::Kind::Ready && uploads->uploads.empty() && !outcome.version->image && !outcome.ready);

    // An uploader that refuses: a failure, and the backing holds nothing a successor could catch up from.
    uploads->refuse = true;
    VersionedBufferBuildInput refused;
    refused.uploadOwner = uploads;
    refused.uploadService = uploads.get();
    refused.writeSequence = 1;
    refused.elementStride = 4;
    refused.elementCount = 2;
    refused.capacity = 2;
    refused.bytes = Bytes(std::vector<std::uint32_t>{1, 2});
    refused.backingPool = pool;
    outcome = BuildVersionedBuffer(refused, Context(1));
    Check(outcome.kind == VersionedBufferBuildOutcome::Kind::Failed && !outcome.error.empty());
    std::uint64_t holding = 0;
    pool->Visit([&](const Backing& backing) { holding += backing.capacityClass == 2 && backing.contentEpoch.load() != 0; });
    Check(holding == 0);
    uploads->refuse = false;

    // A missing pool or service is a failure, never an exception.
    VersionedBufferBuildInput broken;
    outcome = BuildVersionedBuffer(broken, Context(1));
    Check(outcome.kind == VersionedBufferBuildOutcome::Kind::Failed);
}

// A backing reused at an older contentEpoch still catches up from the journal: Acknowledge keeps the writes back to the pool's
// trim floor (OldestContentEpoch), not only past the acknowledged version.
void TestCatchUpIntoReusedOlderBacking() {
    auto clock = std::make_shared<RetirementClock>();
    auto pool = MakePool(clock);
    auto uploads = std::make_shared<RecordingUploads>();

    constexpr std::uint32_t kRows = 64;
    BufferJournal journal(sizeof(std::uint32_t));
    journal.SetBackingPool(pool);
    std::vector<std::uint32_t> rows(kRows);
    for (std::uint32_t i = 0; i < kRows; ++i) rows[i] = i;
    journal.Initialize(std::as_bytes(std::span(rows)), kRows, kRows);
    const auto build = [&](std::uint64_t revision) {
        const auto capture = journal.CaptureDesired();
        VersionedBufferBuildInput input;
        input.uploadOwner = uploads;
        input.uploadService = uploads.get();
        input.debugName = "reuse";
        input.writeSequence = capture.writeSequence;
        input.elementStride = sizeof(std::uint32_t);
        input.elementCount = capture.elementCount;
        input.capacity = capture.capacity;
        input.previous = capture.previous;
        input.backingPool = pool;
        input.writes = capture.writes;
        input.image = capture.image;
        input.journalBaseSequence = capture.journalBaseSequence;
        uploads->uploads.clear();
        auto outcome = BuildVersionedBuffer(input, Context(revision));
        Check(outcome.kind == VersionedBufferBuildOutcome::Kind::Ready && outcome.version);
        return std::shared_ptr<const PublishedBufferVersion>(std::move(outcome.version));
    };

    // v1 into backing A (epoch 1), published.
    auto v1 = build(1);
    const auto* backingA = v1->lease.Get().get();
    journal.Acknowledge(v1);

    // v2 while v1 holds A: a new backing B, written whole. Acknowledging v2 keeps the write since A's epoch (the pool's floor).
    rows[5] = 500;
    journal.AppendWrite(5, std::as_bytes(std::span(&rows[5], 1)), kRows);
    auto v2 = build(2);
    Check(v2->lease.Get().get() != backingA && uploads->UploadedBytes() == kRows * sizeof(std::uint32_t));
    journal.Acknowledge(v2);
    auto capture = journal.CaptureDesired();
    Check(capture.previous == v2 && capture.journalBaseSequence == 1 && capture.writes.size() == 1);
    capture = {};
    v1.reset();  // A is free (never bound: reusable at once)

    // v3 while v2 holds B: A is reused at epoch 1 and catches up with the two writes since, not the whole image.
    rows[9] = 900;
    journal.AppendWrite(9, std::as_bytes(std::span(&rows[9], 1)), kRows);
    auto v3 = build(3);
    Check(v3->lease.Get().get() == backingA && v3->lease->contentEpoch.load() == 3);
    // Rows 5 and 9 (merged into one range: they are within 256 bytes), against 64 rows written whole.
    Check(uploads->uploads.size() == 1 && uploads->uploads[0].offset == 5 * sizeof(std::uint32_t) &&
        uploads->UploadedBytes() == 5 * sizeof(std::uint32_t));
    CheckContents(*uploads, v3->lease, std::as_bytes(std::span(rows)));

    // The floor follows the pool: acknowledged v3 keeps the writes since B's epoch 2; B, freed, catches up from there.
    journal.Acknowledge(v3);
    v2.reset();
    rows[11] = 1100;
    journal.AppendWrite(11, std::as_bytes(std::span(&rows[11], 1)), kRows);
    auto v4 = build(4);  // B (epoch 2) catches up with writes 3 and 4
    Check(v4->lease.Get().get() != backingA && uploads->uploads.size() == 1 && uploads->UploadedBytes() == 3 * sizeof(std::uint32_t));
    CheckContents(*uploads, v4->lease, std::as_bytes(std::span(rows)));
    journal.Acknowledge(v4);  // floor: A at 3, B at 4 (both leased)
    capture = journal.CaptureDesired();
    Check(capture.journalBaseSequence == 3 && capture.writes.size() == 1 && capture.writes[0].sequence == 4);

    // Without a pool the journal keeps only the writes past the acknowledged version (the previous behaviour).
    BufferJournal unpooled(sizeof(std::uint32_t));
    unpooled.Initialize(std::as_bytes(std::span(rows)), kRows, kRows);
    unpooled.AppendWrite(5, std::as_bytes(std::span(&rows[5], 1)), kRows);
    auto record = std::make_shared<PublishedBufferVersion>();
    record->writeSequence = 2;
    unpooled.Acknowledge(record);
    Check(unpooled.CaptureDesired().journalBaseSequence == 2 && unpooled.CaptureDesired().writes.empty());
}

void TestExhaustionSuspends() {
    auto clock = std::make_shared<RetirementClock>();
    auto pool = MakePool(clock, 1);
    auto uploads = std::make_shared<RecordingUploads>();
    VersionedBufferBuildInput input;
    input.uploadOwner = uploads;
    input.uploadService = uploads.get();
    input.writeSequence = 1;
    input.elementStride = 4;
    input.elementCount = 4;
    input.capacity = 4;
    input.bytes = Bytes(std::vector<std::uint32_t>{1, 2, 3, 4});
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

// A journaled part whose rows hold their own buffer's address, and a raw tail written whole.
struct SelfRow {
    std::uint64_t self = 0;
    std::uint32_t value = 0;
    std::uint32_t pad = 0;
};

struct RowModel {
    std::vector<std::uint32_t> values;
    std::vector<std::uint64_t> changedAt;
    std::uint64_t version = 0;
    bool throwInEmit = false;

    void Set(std::size_t row, std::uint32_t value) {
        if (row >= values.size()) {
            values.resize(row + 1);
            changedAt.resize(row + 1);
        }
        values[row] = value;
        changedAt[row] = ++version;
    }
    JournaledPartRequest Request() {
        JournaledPartRequest request;
        request.units = values.size();
        request.preferEpoch = version;
        request.emit = [this](const JournaledTarget& target, JournaledSink& sink) {
            if (throwInEmit) throw std::runtime_error("emit failed");
            for (std::size_t row = 0; row < values.size(); ++row) {
                if (target.contentEpoch != 0 && changedAt[row] <= target.contentEpoch) continue;
                const SelfRow data{target.address + row * sizeof(SelfRow), values[row], 0};  // patched with the target's address
                sink(&data, sizeof(data), row * sizeof(SelfRow));
            }
            return version;
        };
        return request;
    }
};

void CheckRows(const RecordingUploads& uploads, const JournaledPartVersion& part, const RowModel& model) {
    const auto& bytes = uploads.memory.at(KeyOf(part.lease->buffer));
    Check(bytes.size() >= model.values.size() * sizeof(SelfRow));
    for (std::size_t row = 0; row < model.values.size(); ++row) {
        SelfRow data;
        std::memcpy(&data, bytes.data() + row * sizeof(SelfRow), sizeof(data));
        Check(data.self == part.address + row * sizeof(SelfRow) && data.value == model.values[row]);
    }
}

void TestJournaledBufferSet() {
    auto clock = std::make_shared<RetirementClock>();
    auto uploads = std::make_shared<RecordingUploads>();
    BackingPoolHooks hooks;
    auto next = std::make_shared<std::uint64_t>(0);
    hooks.allocate = [next](const BackingRequest&) {
        const auto index = ++*next;
        auto storage = std::shared_ptr<void>(::operator new(sizeof(org::Buffer)), [](void* block) { ::operator delete(block); });
        BackingAllocation allocation;
        allocation.buffer = std::shared_ptr<org::Buffer>(storage, static_cast<org::Buffer*>(storage.get()));
        allocation.address = 0x100000ull * index;
        allocation.srvIndex = static_cast<std::uint32_t>(index);
        return allocation;
    };
    JournaledBufferSet set({{"rows", sizeof(SelfRow), {.deviceAddress = true}, GrowthPolicy::Doubling(16)}, {"tail", 0, {}, GrowthPolicy::Doubling(64)},
                               {"absent", 4, {}, GrowthPolicy::Doubling(4)}},
        hooks, clock, {.name = "", .maxPerClass = 0, .framesInFlight = 3, .spareFree = 4});
    Check(set.Parts() == 3 && set.Pool(0)->Configuration().name == "rows");

    RowModel model;
    for (std::uint32_t row = 0; row < 12; ++row) model.Set(row, 100 + row);
    std::uint32_t tailValue = 1;
    const auto tail = [&] {
        JournaledPartRequest request;
        request.units = sizeof(tailValue);
        request.emit = [&](const JournaledTarget&, JournaledSink& sink) {
            sink(&tailValue, sizeof(tailValue), 0);
            return std::uint64_t{0};  // not a journal position: written whole each time
        };
        return request;
    };
    const auto publish = [&] {
        std::vector<JournaledPartRequest> requests{model.Request(), tail(), JournaledPartRequest{}};
        return set.Publish(requests, *uploads);
    };

    // 1: everything into new backings.
    auto first = std::make_unique<JournaledPublication>(publish());
    const auto& rowsA = first->parts[0];
    Check(rowsA.lease && rowsA.allocated && rowsA.caughtUpFrom == 0 && rowsA.contentEpoch == model.version && rowsA.runs == 12);
    Check(rowsA.capacityUnits == 16 && rowsA.units == 12 && rowsA.address == rowsA.lease->address);
    Check(!first->parts[2].lease && first->ready && first->ready->Complete());
    CheckRows(*uploads, rowsA, model);
    const auto addressA = rowsA.address;
    first->Bind(1);

    // 2: row 2 changes; the first publication still holds its backing, so another (at another address) is written whole.
    model.Set(2, 222);
    auto second = std::make_unique<JournaledPublication>(publish());
    Check(second->parts[0].allocated && second->parts[0].address != addressA && second->parts[0].runs == 12);
    CheckRows(*uploads, second->parts[0], model);
    second->Bind(2);

    // 3: the first publication goes and its frames retire; row 5 changes. The backing at addressA comes back (nearest the journal
    // among the free ones) and catches up from its epoch: rows 2 and 5 only, patched with addressA, not the last publication's.
    first.reset();
    clock->Advance(1);
    model.Set(5, 555);
    auto third = std::make_unique<JournaledPublication>(publish());
    const auto& rowsAgain = third->parts[0];
    Check(!rowsAgain.allocated && rowsAgain.address == addressA && rowsAgain.caughtUpFrom != 0 && rowsAgain.runs == 2 &&
        rowsAgain.bytes == 2 * sizeof(SelfRow));
    CheckRows(*uploads, rowsAgain, model);
    Check(third->parts[1].runs == 1 && third->parts[1].caughtUpFrom == 0);  // the tail is written whole every time
    Check(set.OldestContentEpoch(0) == second->parts[0].contentEpoch);
    Check(!set.OldestContentEpoch(1));  // the tail's backings hold no journal position
    Check(!set.OldestContentEpoch(2));

    // Discard: the publication's uploads failed; its backings hold nothing, and the next lease writes them whole.
    set.Discard(*third);
    third.reset();
    second.reset();
    clock->Advance(2);
    model.Set(0, 7);
    auto fourth = publish();
    Check(fourth.parts[0].runs == 12 || fourth.parts[0].caughtUpFrom != 0);
    if (fourth.parts[0].address == addressA) Check(fourth.parts[0].caughtUpFrom == 0 && fourth.parts[0].runs == 12);
    CheckRows(*uploads, fourth.parts[0], model);

    // An emit that throws: Publish throws, the leases taken go, and their backings hold nothing.
    model.throwInEmit = true;
    bool threw = false;
    try {
        (void)publish();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Check(threw);
    model.throwInEmit = false;
    std::uint64_t leasedRows = 0;
    set.Pool(0)->Visit([&](const Backing& backing) { leasedRows += backing.leases.load() != 0; });
    Check(leasedRows == 1);  // only the fourth publication's

    // The wrong request count is refused.
    threw = false;
    try {
        std::vector<JournaledPartRequest> requests(1);
        (void)set.Publish(requests, *uploads);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    Check(threw);
}

// JournaledPartRequest::appendInto: writes past the head's sealed mark land in it (published versions keep their bytes), one below
// it is refused, and units past its capacity lease another as without it.
void TestJournaledAppend() {
    auto clock = std::make_shared<RetirementClock>();
    auto uploads = std::make_shared<RecordingUploads>();
    BackingPoolHooks hooks;
    auto next = std::make_shared<std::uint64_t>(0);
    hooks.allocate = [next](const BackingRequest&) {
        const auto index = ++*next;
        auto storage = std::shared_ptr<void>(::operator new(sizeof(org::Buffer)), [](void* block) { ::operator delete(block); });
        BackingAllocation allocation;
        allocation.buffer = std::shared_ptr<org::Buffer>(storage, static_cast<org::Buffer*>(storage.get()));
        allocation.address = 0x100000ull * index;
        allocation.srvIndex = static_cast<std::uint32_t>(index);
        return allocation;
    };
    JournaledBufferSet set({{"rows", sizeof(SelfRow), {.deviceAddress = true}, GrowthPolicy::Doubling(16)}}, hooks, clock,
        {.name = "", .maxPerClass = 0, .framesInFlight = 3, .spareFree = 4});

    RowModel model;
    for (std::uint32_t row = 0; row < 8; ++row) model.Set(row, 100 + row);
    const auto publish = [&](const BackingLease& into) {
        std::vector<JournaledPartRequest> requests{model.Request()};
        requests[0].appendInto = into;
        return set.Publish(requests, *uploads);
    };

    // 1: a lease, written whole; its units seal the backing.
    auto first = std::make_unique<JournaledPublication>(publish({}));
    const auto& head = first->parts[0];
    Check(head.lease && head.allocated && !head.appended && head.capacityUnits == 16 && head.firstByte == 0);
    Check(head.lease->sealedBytes.load() == 8 * sizeof(SelfRow));
    const auto* headBacking = head.lease.Get().get();

    // 2: rows 8..11 added: an append into the head, the rows past the mark only; the first publication names it still.
    for (std::uint32_t row = 8; row < 12; ++row) model.Set(row, 100 + row);
    const auto uploadsBefore = uploads->uploads.size();
    auto second = std::make_unique<JournaledPublication>(publish(head.lease));
    const auto& appended = second->parts[0];
    Check(appended.appended && !appended.allocated && appended.lease.Get().get() == headBacking && appended.address == head.address);
    Check(appended.sealedFrom == 8 * sizeof(SelfRow) && appended.firstByte == 8 * sizeof(SelfRow) && appended.runs == 4);
    Check(appended.caughtUpFrom == head.contentEpoch && appended.contentEpoch == model.version && appended.units == 12);
    Check(headBacking->sealedBytes.load() == 12 * sizeof(SelfRow) && headBacking->leases.load() == 2);
    for (std::size_t u = uploadsBefore; u < uploads->uploads.size(); ++u) Check(uploads->uploads[u].offset >= 8 * sizeof(SelfRow));
    CheckRows(*uploads, appended, model);

    // 3: row 3 changes: below the mark, so the append is refused (nothing sent) and the publication throws; the head keeps its bytes.
    model.Set(3, 333);
    const auto uploadsRefused = uploads->uploads.size();
    bool threw = false;
    try {
        (void)publish(appended.lease);
    } catch (const std::logic_error&) {
        threw = true;
    }
    Check(threw && uploads->uploads.size() == uploadsRefused && headBacking->leases.load() == 2);
    SelfRow row3;
    std::memcpy(&row3, uploads->memory.at(KeyOf(appended.lease->buffer)).data() + 3 * sizeof(SelfRow), sizeof(row3));
    Check(row3.value == 103);
    Check(headBacking->contentEpoch.load() == 0);  // the throw: it holds nothing a catch-up may build on

    // 4: rows up to 20: past the head's 16, so another backing is leased and written whole.
    for (std::uint32_t row = 12; row < 20; ++row) model.Set(row, 100 + row);
    auto grown = publish(appended.lease);
    const auto& fresh = grown.parts[0];
    Check(!fresh.appended && fresh.lease.Get().get() != headBacking && fresh.capacityUnits == 32 && fresh.caughtUpFrom == 0 && fresh.runs == 20);
    Check(fresh.sealedFrom == 0 && fresh.lease->sealedBytes.load() == 20 * sizeof(SelfRow));
    CheckRows(*uploads, fresh, model);
    Check(headBacking->sealedBytes.load() == 12 * sizeof(SelfRow));  // the head's mark only moves with what names it
}

// VersionedBufferFamily over the default graph binding, with a recording request service.
struct RecordingRequests {
    using Types = org::async::DefaultStateGraphTypes;
    std::vector<Types::ArtifactIntent> submitted, posted;
    Types::ArtifactRequestResult SubmitLatest(Types::ArtifactIntent intent) {
        Types::ArtifactRequestResult result{org::async::ArtifactRequestStatus::Accepted, submitted.size() + 1, {intent.key, intent.desiredRevision, submitted.size() + 1}, {}};
        submitted.push_back(std::move(intent));
        return result;
    }
    void PostLatest(Types::ArtifactIntent intent) { posted.push_back(std::move(intent)); }
};

void TestFamily() {
    using Family = VersionedBufferFamily<org::async::DefaultStateGraphTypes>;
    auto clock = std::make_shared<RetirementClock>();
    auto pool = MakePool(clock);
    auto uploads = std::make_shared<RecordingUploads>();
    Family::Config config;
    config.address = {3, 1, 0};
    config.debugName = "family";
    config.elementStride = 4;
    config.tag = std::make_shared<const int>(11);
    config.framesInFlight = 3;
    config.pool = pool;
    Family family(config);
    RecordingRequests requests;

    const std::vector<std::uint32_t> rows{1, 2, 3};
    const auto bytes = std::as_bytes(std::span(rows));
    auto result = family.RequestContentSnapshot(requests, uploads, bytes, 3);
    Check(result && requests.submitted.size() == 1);
    const auto input = requests.submitted[0].input.Get<VersionedBufferBuildInput>();
    Check(input && input->backingPool == pool && input->tag == config.tag && input->bytes.size() == bytes.size() && input->capacity == 3);
    const auto firstRevision = requests.submitted[0].desiredRevision;
    (void)family.RequestContentSnapshot(requests, uploads, bytes, 3);
    Check(requests.submitted[1].desiredRevision == firstRevision);  // unchanged contents: the same revision
    const std::vector<std::uint32_t> changed{1, 2, 4};
    const auto posted = family.PostContentSnapshot(requests, uploads, std::as_bytes(std::span(changed)), 3);
    Check(posted.revision == firstRevision + 1 && posted.generation == 0 && requests.posted.size() == 1);
    Check(!family.RequestSnapshot(requests, uploads, 0, bytes, 3));  // revision 0 is refused

    // Acknowledge: the superseded version's backing is bound to Completed() + 2·FIF + 1.
    auto v1 = BuildVersionedBuffer(*input, Context(firstRevision));
    auto v2 = BuildVersionedBuffer(*requests.posted[0].input.Get<VersionedBufferBuildInput>(), Context(firstRevision + 1));
    Check(v1.kind == VersionedBufferBuildOutcome::Kind::Ready && v2.kind == VersionedBufferBuildOutcome::Kind::Ready);
    std::shared_ptr<const PublishedBufferVersion> first = std::move(v1.version), second = std::move(v2.version);
    clock->Advance(10);
    family.Acknowledge(first);
    Check(first->lease->safeAt.load() == 0);
    family.Acknowledge(second);
    Check(first->lease->safeAt.load() == 10 + 2 * 3 + 1 && second->lease->safeAt.load() == 0);
    family.Acknowledge(first);  // older: ignored
    Check(second->lease->safeAt.load() == 0);

    // Journal captures: the input carries the capture; a successor's predecessor is the acknowledged version.
    BufferJournal journal(4);
    journal.Initialize(bytes, 3, 4);
    auto capture = journal.CaptureDesired();
    result = family.RequestCapture(requests, uploads, capture.writeSequence, capture);
    Check(result);
    const auto captured = requests.submitted.back().input.Get<VersionedBufferBuildInput>();
    Check(captured->image && captured->writeSequence == 1 && !captured->gpuWritten);
    Check(!family.RequestCapture(requests, uploads, 5, capture));  // revision != the capture's sequence
    auto snapshot = family.RequestSnapshot(requests, uploads, 9, bytes, 3);
    Check(snapshot && requests.submitted.back().input.Get<VersionedBufferBuildInput>()->previous == second);
}

} // namespace

int main() {
    TestReplay();
    TestBuildAndCatchUp();
    TestCatchUpIntoReusedOlderBacking();
    TestExhaustionSuspends();
    TestJournaledBufferSet();
    TestJournaledAppend();
    TestFamily();
    std::puts("ORGBufferBuildTests: all passed");
    return 0;
}
