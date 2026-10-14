#pragma once

#include <ORGModuleServices/Async/StateGraph.h>
#include <ORGModuleServices/BufferJournal.h>
#include <ORGModuleServices/VersionedBufferBuild.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace org::services {

/**
 * @brief The single manager-facing authority for one persistent buffer address (BasicRenderer's VersionedBufferFamily, generic):
 * it builds immutable VersionedBufferBuildInput requests for the versioned-buffer producer, against its pool, and remembers the
 * version last acknowledged as the successor's predecessor. Callers keep only the returned version handle.
 *
 * Binding: the graph's (StateGraph<Binding>) identity types. Requests (each method's template parameter): the host's request
 * service, providing
 *   - `typename Binding::ArtifactRequestResult SubmitLatest(typename Binding::ArtifactIntent)`;
 *   - `void PostLatest(typename Binding::ArtifactIntent)` (lock-free posting for the owner thread; Post* only).
 *
 * Thread-safe; its mutex is BasicRenderer's (moved with it, not new).
 */
template <class Binding>
class VersionedBufferFamily {
public:
    using ArtifactAddress = typename Binding::ArtifactAddress;
    using ArtifactIntent = typename Binding::ArtifactIntent;
    using ArtifactRequestResult = typename Binding::ArtifactRequestResult;
    using ArtifactVersionID = typename Binding::ArtifactVersionID;
    using ArtifactVersionHandle = typename Binding::ArtifactVersionHandle;

    struct Config {
        ArtifactAddress address{};
        std::string debugName;
        std::uint32_t elementStride = 0;
        BackingFlags flags;
        std::shared_ptr<const void> tag;  // copied into every request (BasicRenderer: catalog owner, usage, variant)
        bool gpuWritten = false;
        // The host's frames in flight (replaces BasicRenderer's hardcoded 3): Acknowledge binds the superseded version's backing
        // to Completed() + 2·framesInFlight + 1 of the pool's clock (BasicRenderer's safety window, D3).
        std::uint32_t framesInFlight = 3;
        std::shared_ptr<BackingPool> pool;  // required
    };

    /** @brief Throws std::invalid_argument without a stride or a pool. */
    explicit VersionedBufferFamily(Config config);

    /** @brief A complete image at `revision` (nonzero), submitted as the address's latest intent. */
    template <class Requests>
    [[nodiscard]] ArtifactRequestResult RequestSnapshot(Requests& requests, std::shared_ptr<runtime::IUploadService> uploads, std::uint64_t revision,
        std::span<const std::byte> bytes, std::uint64_t elementCount, std::uint64_t capacity = 0);
    /** @brief A GPU-written version (Config::gpuWritten): no contents uploaded. */
    template <class Requests>
    [[nodiscard]] ArtifactRequestResult RequestGpuWritten(
        Requests& requests, std::shared_ptr<runtime::IUploadService> uploads, std::uint64_t revision, std::uint64_t elementCount, std::uint64_t capacity = 0);
    /** @brief RequestSnapshot at a revision derived from the contents (unchanged contents: the same revision). */
    template <class Requests>
    [[nodiscard]] ArtifactRequestResult RequestContentSnapshot(Requests& requests, std::shared_ptr<runtime::IUploadService> uploads,
        std::span<const std::byte> bytes, std::uint64_t elementCount, std::uint64_t capacity = 0);
    /** @brief A journal capture at `revision` (its writeSequence); the same revision again returns the handle already requested. */
    template <class Requests>
    [[nodiscard]] ArtifactRequestResult RequestCapture(
        Requests& requests, std::shared_ptr<runtime::IUploadService> uploads, std::uint64_t revision, BufferJournal::Capture capture);
    /**
     * @brief Posted variants for the owner thread: queued without entering the graph mutex. The returned version has generation 0,
     * for Exact() requirements inside the same posted batch (bound by address + revision). Revision 0: nothing was posted.
     */
    template <class Requests>
    [[nodiscard]] ArtifactVersionID PostContentSnapshot(Requests& requests, std::shared_ptr<runtime::IUploadService> uploads,
        std::span<const std::byte> bytes, std::uint64_t elementCount, std::uint64_t capacity = 0);
    template <class Requests>
    [[nodiscard]] ArtifactVersionID PostCapture(
        Requests& requests, std::shared_ptr<runtime::IUploadService> uploads, std::uint64_t revision, BufferJournal::Capture capture);
    /**
     * @brief A version of this family became the published one: it is the successor's predecessor, and the version it replaces is
     * bound to the pool clock's Completed() + 2·framesInFlight + 1 (its backing is not reused before).
     */
    void Acknowledge(std::shared_ptr<const PublishedBufferVersion> version);
    [[nodiscard]] const Config& Configuration() const noexcept { return m_config; }

private:
    [[nodiscard]] std::shared_ptr<VersionedBufferBuildInput> MakeInput(std::shared_ptr<runtime::IUploadService> uploads) const;
    [[nodiscard]] ArtifactIntent MakeSnapshotIntent(std::shared_ptr<runtime::IUploadService> uploads, std::uint64_t revision,
        std::span<const std::byte> bytes, std::uint64_t elementCount, std::uint64_t capacity);
    [[nodiscard]] std::uint64_t ContentRevision(std::span<const std::byte> bytes, std::uint64_t elementCount, std::uint64_t capacity);
    [[nodiscard]] ArtifactIntent MakeCaptureIntent(std::shared_ptr<runtime::IUploadService> uploads, std::uint64_t revision, BufferJournal::Capture capture);

    Config m_config;
    std::mutex m_mutex;
    std::shared_ptr<const PublishedBufferVersion> m_previous;
    std::uint64_t m_lastJournalRevision = 0;
    ArtifactVersionHandle m_lastJournalHandle;
    // Only the current semantic image is remembered: keeping every historical fingerprint grew CPU memory with world streaming.
    std::uint64_t m_lastContentFingerprint = 0;
    std::uint64_t m_lastContentRevision = 0;
    std::uint64_t m_nextContentRevision = 0;
};


template <class Binding>
VersionedBufferFamily<Binding>::VersionedBufferFamily(Config config) : m_config(std::move(config)) {
    if (m_config.elementStride == 0 || !m_config.pool) throw std::invalid_argument("invalid versioned buffer family configuration");
}

template <class Binding>
std::shared_ptr<VersionedBufferBuildInput> VersionedBufferFamily<Binding>::MakeInput(std::shared_ptr<runtime::IUploadService> uploads) const {
    auto input = std::make_shared<VersionedBufferBuildInput>();
    input->uploadOwner = std::move(uploads);
    input->uploadService = input->uploadOwner.get();
    input->debugName = m_config.debugName;
    input->elementStride = m_config.elementStride;
    input->flags = m_config.flags;
    input->gpuWritten = m_config.gpuWritten;
    input->tag = m_config.tag;
    input->backingPool = m_config.pool;
    return input;
}

template <class Binding>
typename VersionedBufferFamily<Binding>::ArtifactIntent VersionedBufferFamily<Binding>::MakeSnapshotIntent(std::shared_ptr<runtime::IUploadService> uploads,
    std::uint64_t revision, std::span<const std::byte> bytes, std::uint64_t elementCount, std::uint64_t capacity) {
    auto input = MakeInput(std::move(uploads));
    input->writeSequence = revision;
    input->elementCount = elementCount;
    input->capacity = (std::max<std::uint64_t>)((std::max)(capacity, elementCount), 1u);
    {
        std::lock_guard lock(m_mutex);
        input->previous = m_previous;
    }
    input->bytes.assign(bytes.begin(), bytes.end());
    auto fingerprint = revision ^ (static_cast<std::uint64_t>(m_config.address.variantID) << 17u) ^ (elementCount << 1u) ^
        (static_cast<std::uint64_t>(m_config.elementStride) << 33u) ^ 0x5642554646414dull;
    for (const auto value : bytes) {
        fingerprint ^= static_cast<std::uint8_t>(value);
        fingerprint *= 1099511628211ull;
    }
    if (fingerprint == 0) fingerprint = 1;
    std::shared_ptr<const VersionedBufferBuildInput> frozen = std::move(input);
    return {m_config.address, revision, {}, async::ArtifactPayload::Make(std::move(frozen)), fingerprint};
}

template <class Binding>
template <class Requests>
typename VersionedBufferFamily<Binding>::ArtifactRequestResult VersionedBufferFamily<Binding>::RequestSnapshot(Requests& requests,
    std::shared_ptr<runtime::IUploadService> uploads, std::uint64_t revision, std::span<const std::byte> bytes, std::uint64_t elementCount,
    std::uint64_t capacity) {
    if (!uploads || revision == 0 || bytes.size() != elementCount * m_config.elementStride) return {async::ArtifactRequestStatus::ConflictingRevision, 0, {}};
    return requests.SubmitLatest(MakeSnapshotIntent(std::move(uploads), revision, bytes, elementCount, capacity));
}

template <class Binding>
template <class Requests>
typename VersionedBufferFamily<Binding>::ArtifactRequestResult VersionedBufferFamily<Binding>::RequestGpuWritten(
    Requests& requests, std::shared_ptr<runtime::IUploadService> uploads, std::uint64_t revision, std::uint64_t elementCount, std::uint64_t capacity) {
    if (!uploads || revision == 0 || !m_config.gpuWritten) return {async::ArtifactRequestStatus::ConflictingRevision, 0, {}};
    auto input = MakeInput(std::move(uploads));
    input->writeSequence = revision;
    input->elementCount = elementCount;
    input->capacity = (std::max<std::uint64_t>)((std::max)(capacity, elementCount), 1u);
    input->gpuWritten = true;
    const auto fingerprint = revision ^ (elementCount << 1u) ^ (input->capacity << 7u) ^
        (static_cast<std::uint64_t>(m_config.address.variantID) << 17u) ^ 0x4750555752495445ull;
    std::shared_ptr<const VersionedBufferBuildInput> frozen = std::move(input);
    return requests.SubmitLatest(
        ArtifactIntent{m_config.address, revision, {}, async::ArtifactPayload::Make(std::move(frozen)), fingerprint != 0u ? fingerprint : 1u});
}

template <class Binding>
std::uint64_t VersionedBufferFamily<Binding>::ContentRevision(std::span<const std::byte> bytes, std::uint64_t elementCount, std::uint64_t capacity) {
    std::uint64_t contentFingerprint = 1469598103934665603ull;
    for (const auto value : bytes) {
        contentFingerprint ^= static_cast<std::uint8_t>(value);
        contentFingerprint *= 1099511628211ull;
    }
    contentFingerprint ^= elementCount + (capacity << 1u);
    std::lock_guard lock(m_mutex);
    if (m_lastContentRevision == 0 || m_lastContentFingerprint != contentFingerprint) {
        m_lastContentFingerprint = contentFingerprint;
        m_lastContentRevision = ++m_nextContentRevision;
    }
    return m_lastContentRevision;
}

template <class Binding>
template <class Requests>
typename VersionedBufferFamily<Binding>::ArtifactRequestResult VersionedBufferFamily<Binding>::RequestContentSnapshot(
    Requests& requests, std::shared_ptr<runtime::IUploadService> uploads, std::span<const std::byte> bytes, std::uint64_t elementCount, std::uint64_t capacity) {
    return RequestSnapshot(requests, std::move(uploads), ContentRevision(bytes, elementCount, capacity), bytes, elementCount, capacity);
}

template <class Binding>
template <class Requests>
typename VersionedBufferFamily<Binding>::ArtifactVersionID VersionedBufferFamily<Binding>::PostContentSnapshot(
    Requests& requests, std::shared_ptr<runtime::IUploadService> uploads, std::span<const std::byte> bytes, std::uint64_t elementCount, std::uint64_t capacity) {
    if (!uploads || bytes.size() != elementCount * m_config.elementStride) return {};
    const auto revision = ContentRevision(bytes, elementCount, capacity);
    if (revision == 0) return {};
    requests.PostLatest(MakeSnapshotIntent(std::move(uploads), revision, bytes, elementCount, capacity));
    return {m_config.address, revision, 0};
}

template <class Binding>
typename VersionedBufferFamily<Binding>::ArtifactIntent VersionedBufferFamily<Binding>::MakeCaptureIntent(
    std::shared_ptr<runtime::IUploadService> uploads, std::uint64_t revision, BufferJournal::Capture capture) {
    auto input = MakeInput(std::move(uploads));
    input->gpuWritten = false;
    input->writeSequence = capture.writeSequence;
    input->elementCount = capture.elementCount;
    input->capacity = capture.capacity;
    input->previous = std::move(capture.previous);
    input->writes = std::move(capture.writes);
    input->image = std::move(capture.image);
    input->journalBaseSequence = capture.journalBaseSequence;
    std::uint64_t fingerprint = revision ^ (capture.elementCount << 1u) ^ (capture.capacity << 7u) ^
        (static_cast<std::uint64_t>(m_config.address.variantID) << 17u) ^ 0x5642464a4f5552ull;
    for (const auto& write : input->writes) fingerprint ^= write.sequence ^ (write.byteOffset << 3u) ^ (write.byteSize << 11u);
    if (fingerprint == 0) fingerprint = 1;
    std::shared_ptr<const VersionedBufferBuildInput> frozen = std::move(input);
    return {m_config.address, revision, {}, async::ArtifactPayload::Make(std::move(frozen)), fingerprint};
}

template <class Binding>
template <class Requests>
typename VersionedBufferFamily<Binding>::ArtifactRequestResult VersionedBufferFamily<Binding>::RequestCapture(
    Requests& requests, std::shared_ptr<runtime::IUploadService> uploads, std::uint64_t revision, BufferJournal::Capture capture) {
    if (!uploads || revision == 0 || capture.writeSequence != revision) return {async::ArtifactRequestStatus::ConflictingRevision, 0, {}};
    // A journal sequence names its desired image. Acknowledge may later compact the same image from a full replay into
    // previous + delta form, but that change of representation must not mint a new fingerprint for the version already requested.
    std::lock_guard familyLock(m_mutex);
    if (revision == m_lastJournalRevision && m_lastJournalHandle)
        return {async::ArtifactRequestStatus::AlreadyDesired, m_lastJournalHandle.version.generation, m_lastJournalHandle.version, m_lastJournalHandle.lease};
    auto result = requests.SubmitLatest(MakeCaptureIntent(std::move(uploads), revision, std::move(capture)));
    if (result) {
        m_lastJournalRevision = revision;
        m_lastJournalHandle = result.Handle();
    }
    return result;
}

template <class Binding>
template <class Requests>
typename VersionedBufferFamily<Binding>::ArtifactVersionID VersionedBufferFamily<Binding>::PostCapture(
    Requests& requests, std::shared_ptr<runtime::IUploadService> uploads, std::uint64_t revision, BufferJournal::Capture capture) {
    if (!uploads || revision == 0 || capture.writeSequence != revision) return {};
    std::lock_guard familyLock(m_mutex);
    if (revision == m_lastJournalRevision) return {m_config.address, revision, 0};
    requests.PostLatest(MakeCaptureIntent(std::move(uploads), revision, std::move(capture)));
    m_lastJournalRevision = revision;
    m_lastJournalHandle = {};
    return {m_config.address, revision, 0};
}

template <class Binding>
void VersionedBufferFamily<Binding>::Acknowledge(std::shared_ptr<const PublishedBufferVersion> version) {
    if (!version) return;
    std::lock_guard lock(m_mutex);
    if (m_previous && version->revision <= m_previous->revision) return;
    // The GPU's safety window starts when the old backing stops being the published one, not when it first became so: a table
    // may stay current for thousands of frames, and frames still in flight read the one it replaces.
    if (m_previous && m_previous->lease && m_previous->backingEpoch != version->backingEpoch) {
        const auto& clock = m_config.pool->Clock();
        const auto window = 2ull * (std::max)(m_config.framesInFlight, 1u) + 1ull;
        m_previous->lease.Bind(clock->Completed() + window);
    }
    m_previous = std::move(version);
}

} // namespace org::services
