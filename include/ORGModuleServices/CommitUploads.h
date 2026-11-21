#pragma once

// A commit's uploads on the upload service's owner thread, either staged or latched into a latch block.
// Every write states which descriptors its bytes name (OwnedRow), so they stay alive while the bytes are current
// (ContentOwners). A versioned buffer is written in the version a named set gives it.

#include <ORGModuleServices/ContentOwners.h>
#include <ORGModuleServices/GrowableBufferSet.h>
#include <ORGModuleServices/OwnedRows.h>

#include <Render/LatchedCopies.h>
#include <Render/RunRef.h>
#include <Render/Runtime/IUploadService.h>
#include <Render/Runtime/StagedUploadBatch.h>
#include <Render/Runtime/UploadServiceAccess.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/Buffers/VersionedBuffer.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace org::services {

/** @brief Reuses a released batch from pool (one only the pool holds), or adds a new one. The batch comes back reset. */
inline std::shared_ptr<org::runtime::StagedUploadBatch> AcquireStagedBatch(
    std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& pool, std::size_t pageSize = std::size_t{4} << 20) {
    std::shared_ptr<org::runtime::StagedUploadBatch> batch;
    for (const auto& candidate : pool) {
        if (candidate.use_count() == 1) {
            batch = candidate;
            break;
        }
    }
    if (!batch) {
        batch = org::runtime::StagedUploadBatch::Create(pageSize);
        pool.push_back(batch);
    }
    batch->Reset();
    return batch;
}

/** @brief Where the upload classes report the bytes they write, for statistics. */
class IUploadBytesSink {
public:
    virtual ~IUploadBytesSink() = default;
    /** @brief Asked before each write is reported, so nothing is looked up for a write that is not counted. */
    [[nodiscard]] virtual bool Enabled() const = 0;
    /**
     * @brief bytes written to a target at offset. route is "staged" or "latched". target is the target's resource name,
     * empty for an unnamed or null target, or nullopt for a target type that has no name.
     */
    virtual void Note(std::string_view route, std::optional<std::string_view> target, std::uint64_t offset, std::size_t bytes) = 0;
};

/** @brief What the host gives the upload classes. Keep it alive as long as they run (a static is usual). */
struct CommitUploadHooks {
    // Gets an error the commit can't return, such as a row naming descriptors uploaded with no content owners.
    // After it returns, the write's owners are dropped. Required.
    std::function<void(std::string_view message)> onError;
    // Optional byte statistics.
    IUploadBytesSink* bytes = nullptr;
};

/**
 * @brief A commit's own uploads, staged directly and submitted to the active upload service when the commit ends. Each
 * is one memcpy and one copy. A versioned buffer is written in the version the adopted set names. Owner thread only.
 */
class CommitUploads {
public:
    /** @brief content may be null when the commit writes no row naming descriptors. */
    CommitUploads(std::shared_ptr<org::runtime::StagedUploadBatch> batch, const NamedVersions& adopted, const CommitUploadHooks& hooks,
        ContentOwners* content = nullptr)
        : m_batch(std::move(batch)), m_adopted(adopted), m_hooks(hooks), m_content(content) {}
    /** @brief Takes a batch from pool, with 1 MiB pages. */
    CommitUploads(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& pool, const NamedVersions& adopted,
        const CommitUploadHooks& hooks, ContentOwners* content = nullptr)
        : CommitUploads(AcquireStagedBatch(pool, std::size_t{1} << 20), adopted, hooks, content) {}
    ~CommitUploads() {
        if (!m_batch->Entries().empty())
            org::runtime::GetActiveUploadService()->SubmitStagedUploads(std::move(m_batch));
    }
    CommitUploads(const CommitUploads&) = delete;
    CommitUploads& operator=(const CommitUploads&) = delete;

    template <class Target>
    void operator()(const Target& target, const void* data, std::size_t bytes, std::uint64_t offset) {
        if (!data || !bytes)
            return;
        NoteContent(ContentKey(target), offset, bytes, {});
        Stage(target, data, bytes, offset);
    }
    // The row's descriptors become the range's content owners.
    template <class Target, class Row, class Binding>
    void operator()(const Target& target, const OwnedRow<Row, Binding>& row, std::uint64_t offset) {
        Stage(target, row.Bytes(), OwnedRow<Row, Binding>::kBytes, offset);
        NoteContent(ContentKey(target), offset, OwnedRow<Row, Binding>::kBytes, row.Owners());
    }
    const std::shared_ptr<org::Buffer>& TargetOf(const std::shared_ptr<org::VersionedBuffer>& target) const {
        return m_adopted.Of(target).version->buffer;
    }

private:
    friend class LatchedUploads;
    // Versioned buffers are keyed by Key() so owners survive growth to a new version.
    static const void* ContentKey(const std::shared_ptr<org::VersionedBuffer>& target) { return target->Key(); }
    template <class T>
    static const void* ContentKey(const std::shared_ptr<T>& target) {
        return target.get();
    }
    template <class Target>
    static std::optional<std::string_view> NameOf(const Target& target) {
        if constexpr (requires { target->GetName(); }) {
            if (!target)
                return std::string_view{};
            return std::string_view(target->GetName());
        } else {
            return std::nullopt;
        }
    }
    bool Counting() const { return m_hooks.bytes && m_hooks.bytes->Enabled(); }
    template <class Target>
    void Stage(const Target& target, const void* data, std::size_t bytes, std::uint64_t offset) {
        m_batch->Stage(org::runtime::UploadTarget::FromShared(target), static_cast<std::size_t>(offset), data, bytes);
        if (Counting())
            m_hooks.bytes->Note("staged", NameOf(target), offset, bytes);
    }
    // Every set sealed after a buffer's registration names it, so the adopted set always has a version here.
    void Stage(const std::shared_ptr<org::VersionedBuffer>& target, const void* data, std::size_t bytes, std::uint64_t offset) {
        Stage(TargetOf(target), data, bytes, offset);
    }
    void NoteContent(const void* key, std::uint64_t offset, std::uint64_t bytes, std::span<const std::shared_ptr<const void>> owners) {
        if (m_content)
            m_content->Set(key, offset, bytes, owners);
        else if (!owners.empty() && m_hooks.onError)
            m_hooks.onError("a row naming descriptors was uploaded by a commit with no content owners (its descriptors would outlive their owners)");
    }

    std::shared_ptr<org::runtime::StagedUploadBatch> m_batch;
    const NamedVersions& m_adopted;
    const CommitUploadHooks& m_hooks;
    ContentOwners* m_content = nullptr;
};

/**
 * @brief A commit's per-run values, written into its latched copies' region for the run and copied to their targets by
 * the work's latched copy pass. A value with no copy is staged by the fallback instead.
 */
class LatchedUploads {
public:
    LatchedUploads(const org::LatchedList& layout, CommitUploads& fallback, const org::RunRef& run)
        : m_fallback(fallback), m_run(run), m_layout(layout), m_written(layout.copies.size(), 0) {}
    /** @brief Values that had no copy and were staged instead. */
    std::uint32_t Misses() const { return m_misses; }

    void operator()(const std::shared_ptr<org::Buffer>& target, const void* data, std::size_t bytes, std::uint64_t offset) {
        if (!data || !bytes)
            return;
        m_fallback.NoteContent(target.get(), offset, bytes, {});
        Write(target, data, bytes, offset);
    }
    void operator()(const std::shared_ptr<org::VersionedBuffer>& target, const void* data, std::size_t bytes, std::uint64_t offset) {
        if (!data || !bytes)
            return;
        m_fallback.NoteContent(target->Key(), offset, bytes, {});
        Write(target, data, bytes, offset);
    }
    // The row's descriptors become the range's content owners.
    template <class Target, class Row, class Binding>
    void operator()(const Target& target, const OwnedRow<Row, Binding>& row, std::uint64_t offset) {
        Write(target, row.Bytes(), OwnedRow<Row, Binding>::kBytes, offset);
        m_fallback.NoteContent(CommitUploads::ContentKey(target), offset, OwnedRow<Row, Binding>::kBytes, row.Owners());
    }

    /** @brief The run's list, with every copy not written this commit zeroed. */
    org::LatchedList Finish() {
        // Zero unwritten copies so a published copy's region never replays an older run's bytes.
        for (std::size_t i = 0; i < m_written.size(); ++i)
            if (!m_written[i] && m_layout.latch) {
                const auto& copy = m_layout.copies[i];
                auto region = m_layout.latch->Bytes(m_run).subspan(copy.latchOffset, copy.bytes);
                std::memset(region.data(), 0, region.size());
            }
        return m_layout;
    }

private:
    void Write(const std::shared_ptr<org::Buffer>& target, const void* data, std::size_t bytes, std::uint64_t offset) {
        if (Latch(target.get(), data, bytes, offset, [&] { m_fallback.Stage(target, data, bytes, offset); }) && m_fallback.Counting())
            m_fallback.m_hooks.bytes->Note("latched", CommitUploads::NameOf(target), offset, bytes);
    }
    void Write(const std::shared_ptr<org::VersionedBuffer>& target, const void* data, std::size_t bytes, std::uint64_t offset) {
        if (Latch(target->Key(), data, bytes, offset, [&] { m_fallback.Stage(target, data, bytes, offset); }) && m_fallback.Counting())
            m_fallback.m_hooks.bytes->Note("latched", CommitUploads::NameOf(m_fallback.TargetOf(target)), offset, bytes);
    }
    // False when nothing was written or the fallback staged it.
    template <class Fallback>
    bool Latch(const void* key, const void* data, std::size_t bytes, std::uint64_t offset, Fallback&& fallback) {
        if (!data || !bytes)
            return false;
        const auto& listed = m_layout.copies;
        for (std::size_t i = 0; i < listed.size(); ++i) {
            const auto& copy = listed[i];
            if (copy.target != key || copy.dstOffset != offset || bytes > copy.bytes || !m_layout.latch)
                continue;
            auto region = m_layout.latch->Bytes(m_run).subspan(copy.latchOffset, copy.bytes);
            std::memcpy(region.data(), data, bytes);
            std::memset(region.data() + bytes, 0, copy.bytes - bytes);
            m_written[i] = 1;
            return true;
        }
        ++m_misses;
        fallback();
        return false;
    }

    CommitUploads& m_fallback;
    org::RunRef m_run{};
    const org::LatchedList& m_layout;
    std::vector<std::uint8_t> m_written;
    std::uint32_t m_misses = 0;
};

} // namespace org::services
