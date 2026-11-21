#pragma once

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/ElasticCapacity.h>
#include <ORGModuleServices/GrowthPolicy.h>
#include <ORGModuleServices/VersionedBufferBuild.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace org::runtime {
class IUploadService;
}

namespace org::services {

/** @brief Writes `bytes` from `data` at byte `offset` of the version being built. */
using ChainDeltaWrite = std::function<void(const void* data, std::size_t bytes, std::uint64_t offset)>;

/** @brief One chain's fixed description. */
struct BufferChainDesc {
    std::string name;  // the builds' debug name
    std::uint32_t stride = 0;
    bool deviceAddress = false;
    // Rows after the journalled ones, from BufferChainSource::TailStart on, that are written whole into each version. A tailed chain
    // also changes whenever its source does.
    bool tailed = false;
};

/**
 * @brief Where every chain's rows come from for one publication. Immutable. Each version built from it holds it until its upload
 * completes (VersionedBufferBuildInput::keep), so the build reads it on the build's thread.
 */
class BufferChainSource {
public:
    static constexpr std::uint64_t kNoTail = ~0ull;

    virtual ~BufferChainSource() = default;
    /** @brief Process-unique per source. */
    [[nodiscard]] virtual std::uint64_t Serial() const = 0;
    /** @brief Whether the source has the chain. An absent chain keeps its last version for a later source that has it. */
    [[nodiscard]] virtual bool Present(std::size_t chain) const = 0;
    /** @brief The host's units of the chain. A version holds the units of the source it was built for. */
    [[nodiscard]] virtual std::uint64_t Units(std::size_t chain) const = 0;
    /** @brief The chain's elements of its stride. */
    [[nodiscard]] virtual std::uint64_t Elements(std::size_t chain) const = 0;
    /** @brief The chain's journal position. 0 means it is not journalled and is written whole, so it changes with the source. */
    [[nodiscard]] virtual std::uint64_t ContentEpoch(std::size_t chain) const = 0;
    /** @brief A tailed chain's first tail byte, or kNoTail. */
    [[nodiscard]] virtual std::uint64_t TailStart(std::size_t chain) const = 0;
    /**
     * @brief The byte ranges the chain's journal changed since journal position `epoch`. A superset is fine, never too little.
     * nullopt when the journal does not reach back that far.
     */
    [[nodiscard]] virtual std::optional<std::vector<BufferByteRange>> ChangedSince(std::size_t chain, std::uint64_t epoch) const = 0;
    /**
     * @brief Build thread: the chain's delta since journal position `fromEpoch` (nullopt: everything), then its tails whole.
     * `version` is the version being built and `previousVersion` its predecessor's (0: none).
     */
    virtual void EmitDelta(std::size_t chain, std::optional<std::uint64_t> fromEpoch, std::uint64_t version, std::uint64_t previousVersion,
        const ChainDeltaWrite& write) const = 0;
};

/** @brief Build thread, once the version is made and before it is frozen. Must not throw. */
using BufferChainBuilt = std::function<void(const PublishedBufferVersion& version)>;
/** @brief A build's outcome, run once: the GPU-ready version, or null and why. */
using BufferChainDone = std::function<void(std::shared_ptr<const PublishedBufferVersion> version, std::string error)>;

/** @brief The chains and their ties to the host. */
struct BufferChainsDesc {
    std::vector<BufferChainDesc> chains;
    // Producer thread, at a chain's first post: the chain's backing pool, whose single acquirer the builds are. The host sets
    // `telemetry` as the pool's BackingPoolHooks::telemetry: it counts the pool's suspensions.
    std::function<std::shared_ptr<BackingPool>(std::size_t chain, std::function<void(BackingPoolEvent, std::string_view, std::uint64_t)> telemetry)>
        makePool;
    // Producer thread: posts one build as graph work (BuildVersionedBuffer, as RegisterVersionedBufferProducer runs it). The host
    // runs `built` on the build's thread once the version is made, before it is frozen, and `done` once at the end. False when
    // refused, and then `done` never runs.
    std::function<bool(VersionedBufferBuildInput input, BufferChainBuilt built, BufferChainDone done)> post;
    // Any thread, optional: a posted build finished (after `done`'s outcome is stored). The producer may settle again.
    std::function<void()> wake;
    // Producer thread, optional: a build failed (Settle). The chain keeps its last version and the next source builds it again.
    std::function<void(std::size_t chain, std::uint64_t version, std::string_view error)> failed;
    // Each build's VersionedBufferBuildInput::growth and ::shrink.
    std::optional<GrowthPolicy> growth;
    ShrinkPolicy shrink = ShrinkPolicy::Never();
};

/**
 * @brief N chains of GPU-built buffer versions, each from its predecessor, all from one kept source per publication.
 *
 * - Post (producer): for every present chain that changed (its units, its journal position, or the source of a tailed or
 *   unjournalled chain) or whose shrink is due, posts one build. The build GPU-copies what its backing lacks of the predecessor and
 *   uploads the journal's runs since the predecessor, plus the tails whole.
 * - Settle (producer): takes finished builds into the chains. True when none is in flight; nothing may be posted while false.
 * - Finish (producer): every present chain's newest version, once each was built for the source's units.
 * - Each chain has its own pool. After each build the chain publishes the oldest journal position any backing of its pool holds
 *   (OldestContentEpoch, any thread), so the host's journals keep their changes back to there.
 *
 * The object must outlive every build it posted and every pool it made: they hold pointers into it.
 */
class BufferChains {
public:
    explicit BufferChains(BufferChainsDesc desc);
    ~BufferChains();
    BufferChains(const BufferChains&) = delete;
    BufferChains& operator=(const BufferChains&) = delete;

    [[nodiscard]] std::size_t Count() const noexcept { return m_desc.chains.size(); }
    [[nodiscard]] const BufferChainDesc& Chain(std::size_t chain) const { return m_desc.chains.at(chain); }

    /** @brief Producer. Takes finished builds into the chains. True when none is in flight. */
    bool Settle();
    /**
     * @brief Producer, with Settle true. Posts a new version of each changed chain through `uploads`, which must copy
     * (IUploadService::QueueTrackedStreamingCopyRanges). False when a post was refused; the chains posted stay in flight.
     */
    bool Post(const std::shared_ptr<const BufferChainSource>& source, const std::shared_ptr<runtime::IUploadService>& uploads);
    /**
     * @brief Producer, once Settle is true after Post. Fills `out` (one entry per chain; null for an absent chain) with every
     * present chain's newest version. False when a chain failed or has no version for the source's units.
     */
    [[nodiscard]] bool Finish(const BufferChainSource& source, std::span<std::shared_ptr<const PublishedBufferVersion>> out) const;

    /** @brief Any thread. The oldest journal position any backing of the chain holds. nullopt means everything may be trimmed. */
    [[nodiscard]] std::optional<std::uint64_t> OldestContentEpoch(std::size_t chain) const noexcept;

    /** @brief A version in flight, as the producer last posted it. */
    struct InFlight {
        std::size_t chain = 0;
        std::uint64_t version = 0;
        bool done = false;  // finished, not yet settled
    };
    /** @brief Any thread, read racily for stall messages. */
    [[nodiscard]] std::vector<InFlight> Pending() const;
    /** @brief Any thread. Suspensions of every chain's pool since the last TakeReport. */
    [[nodiscard]] std::uint64_t Suspensions() const noexcept;

    /** @brief One chain's counters since the last take, then its pool's census. */
    struct Report {
        std::uint64_t built = 0, uploadedBytes = 0, copiedBytes = 0, wholeCopies = 0, suspensions = 0, failures = 0;
        std::uint64_t liveBackings = 0, liveBytes = 0;
        std::optional<std::uint64_t> floor;
    };
    /** @brief Producer. Every chain's report, which resets its counters. */
    std::vector<Report> TakeReport();

private:
    struct Held;
    struct Shared;
    struct PendingBuild;
    struct ChainState;

    BufferChainsDesc m_desc;
    std::unique_ptr<Shared[]> m_shared;      // any thread, by chain
    std::unique_ptr<ChainState[]> m_chains;  // the producer's, by chain
};

} // namespace org::services
