#include <ORGModuleServices/BufferChains.h>

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace org::services {

namespace {

constexpr std::uint64_t kNoFloor = ~0ull;  // Shared::floorEpoch: no backing holds anything

} // namespace

/** @brief A version's journal position and where its tails start. Together they say what a backing at this version lacks. */
struct BufferChains::Held {
    std::uint64_t version = 0, journalEpoch = 0, tailStart = BufferChainSource::kNoTail;
};

/** @brief Any thread. A chain's journal floor and report counters. */
struct BufferChains::Shared {
    std::atomic<std::uint64_t> floorEpoch{kNoFloor};  // the oldest journal position a backing of the pool holds
    std::atomic<std::uint64_t> floorVersion{0};       // the same as a version (the producer forgets the older ones)
    std::atomic<std::uint64_t> built{0}, uploadedBytes{0}, copiedBytes{0}, wholeCopies{0}, suspensions{0}, failures{0};
};

/** @brief One posted version's outcome. The producer reads it once `done` is set. */
struct BufferChains::PendingBuild {
    std::atomic<bool> done{false};
    std::shared_ptr<const PublishedBufferVersion> version;  // null: failed (error)
    std::string error;
    Held held;
    std::uint64_t units = 0, serial = 0;  // the source's
};

/** @brief The producer's. One chain. */
struct BufferChains::ChainState {
    std::shared_ptr<BackingPool> pool;                  // made at the chain's first post
    std::shared_ptr<const PublishedBufferVersion> last;  // the newest version built
    Held lastHeld;
    std::uint64_t lastUnits = 0, lastSerial = 0;  // the source last was built for
    std::uint64_t versions = 0;                   // the versions numbered (writeSequence)
    std::vector<Held> held;                       // by version, back to the pool's floor (last's included)
    std::shared_ptr<PendingBuild> pending;        // the version in flight
    bool failed = false;                          // the current source's build of it failed
};

namespace {

/**
 * @brief Publishes the oldest journal position any backing holds. Runs after a build, when the build is the pool's only acquirer.
 * A backing at an unknown version, such as one a failed build wrote, keeps every change.
 */
template <class HeldT, class SharedT>
void PublishFloor(BackingPool& pool, const std::vector<HeldT>& held, SharedT& shared) {
    const auto oldest = pool.OldestContentEpoch();
    if (!oldest) {
        shared.floorEpoch.store(kNoFloor, std::memory_order_release);
        return;
    }
    const auto it = std::find_if(held.begin(), held.end(), [&](const HeldT& entry) { return entry.version == *oldest; });
    shared.floorVersion.store(*oldest, std::memory_order_release);
    shared.floorEpoch.store(it != held.end() ? it->journalEpoch : 0, std::memory_order_release);
}

} // namespace

BufferChains::BufferChains(BufferChainsDesc desc) : m_desc(std::move(desc)) {
    if (!m_desc.makePool) throw std::invalid_argument("BufferChains: no makePool");
    if (!m_desc.post) throw std::invalid_argument("BufferChains: no post");
    m_shared = std::make_unique<Shared[]>(m_desc.chains.size());
    m_chains = std::make_unique<ChainState[]>(m_desc.chains.size());
}

BufferChains::~BufferChains() = default;

bool BufferChains::Settle() {
    bool idle = true;
    for (std::size_t k = 0; k < Count(); ++k) {
        auto& chain = m_chains[k];
        auto& shared = m_shared[k];
        if (chain.pending) {
            if (!chain.pending->done.load(std::memory_order_acquire)) {
                idle = false;
                continue;
            }
            const auto pending = std::move(chain.pending);
            if (const auto& version = pending->version) {
                chain.last = version;
                chain.lastHeld = pending->held;
                chain.lastUnits = pending->units;
                chain.lastSerial = pending->serial;
                chain.held.push_back(pending->held);
                shared.built.fetch_add(1, std::memory_order_relaxed);
                shared.uploadedBytes.fetch_add(version->uploadedBytes, std::memory_order_relaxed);
                shared.copiedBytes.fetch_add(version->copiedBytes, std::memory_order_relaxed);
                shared.wholeCopies.fetch_add(version->copiedWhole ? 1u : 0u, std::memory_order_relaxed);
            } else {
                // No fallback. The chain keeps its last version and the next source builds it again.
                chain.failed = true;
                shared.failures.fetch_add(1, std::memory_order_relaxed);
                if (m_desc.failed) m_desc.failed(k, pending->held.version, pending->error);
            }
        }
        // Forget versions below the pool's floor. The newest is always kept.
        const auto floor = shared.floorVersion.load(std::memory_order_acquire);
        std::erase_if(chain.held, [&](const Held& held) { return held.version < floor && held.version != chain.lastHeld.version; });
    }
    return idle;
}

bool BufferChains::Post(const std::shared_ptr<const BufferChainSource>& source, const std::shared_ptr<runtime::IUploadService>& uploads) {
    const BufferChainSource* view = source.get();  // held by each input (keep)
    bool posted = true;
    for (std::size_t k = 0; k < Count(); ++k) {
        const auto& desc = m_desc.chains[k];
        auto& chain = m_chains[k];
        chain.failed = false;
        if (!view->Present(k)) continue;  // the chain keeps its last version for a later source that has it
        const std::uint64_t units = view->Units(k);
        const std::uint64_t serial = view->Serial();
        const auto epoch = view->ContentEpoch(k);
        // A tailed or unjournalled chain also changes whenever the source does.
        const bool changed = !chain.last || units != chain.lastUnits || epoch != chain.lastHeld.journalEpoch ||
                             ((desc.tailed || !epoch) && serial != chain.lastSerial);
        // Rebuild a quiet chain when its shrink is due. A chain is otherwise asked only on change and would keep its large class.
        if (!changed && !chain.last->shrinkWindow.Due(m_desc.shrink, ShrinkWindow::Clock::now())) continue;
        if (!chain.pool) {
            chain.pool = m_desc.makePool(k, [shared = &m_shared[k]](BackingPoolEvent event, std::string_view, std::uint64_t) {
                if (event == BackingPoolEvent::Suspended) shared->suspensions.fetch_add(1, std::memory_order_relaxed);
            });
            if (!chain.pool) throw std::logic_error("BufferChains: makePool returned no pool for " + desc.name);
        }
        const Held held{++chain.versions, epoch, view->TailStart(k)};
        const Held previous = chain.last ? chain.lastHeld : Held{};
        auto known = std::make_shared<std::vector<Held>>(chain.held);
        known->push_back(held);
        const std::shared_ptr<const std::vector<Held>> versions = std::move(known);

        VersionedBufferBuildInput input;
        input.uploadOwner = uploads;
        input.uploadService = uploads.get();
        input.debugName = desc.name;
        input.writeSequence = held.version;
        input.elementStride = desc.stride;
        input.elementCount = view->Elements(k);
        input.capacity = std::max<std::uint64_t>(input.elementCount, 1);
        input.growth = m_desc.growth;
        // The predecessor's copy is clipped to this version's rows when it shrinks.
        input.shrink = m_desc.shrink;
        input.flags.deviceAddress = desc.deviceAddress;
        input.previous = chain.last;
        input.backingPool = chain.pool;
        input.keep = source;
        // What a backing at `from` lacks: the journal's runs since then, plus everything from the earlier tail start on.
        input.changedSince = [view, k, tailed = desc.tailed, versions, previousTail = previous.tailStart](
                                 std::uint64_t from) -> std::optional<std::vector<BufferByteRange>> {
            const auto it = std::find_if(versions->begin(), versions->end(), [&](const Held& entry) { return entry.version == from; });
            if (it == versions->end()) return std::nullopt;
            auto ranges = view->ChangedSince(k, it->journalEpoch);
            if (ranges && tailed)
                if (const auto tail = std::min(it->tailStart, previousTail); tail != BufferChainSource::kNoTail)
                    ranges->push_back({tail, ~std::uint64_t{0} - tail});
            return ranges;
        };
        // Everything when there is no predecessor position.
        const std::optional<std::uint64_t> from = chain.last && previous.journalEpoch ? std::optional<std::uint64_t>(previous.journalEpoch) : std::nullopt;
        input.emitDelta = [view, k, from, version = held.version, previousVersion = previous.version](const ChainDeltaWrite& write) {
            view->EmitDelta(k, from, version, previousVersion, write);
        };
        BufferChainBuilt built = [pool = chain.pool.get(), versions, shared = &m_shared[k]](const PublishedBufferVersion&) {
            PublishFloor(*pool, *versions, *shared);
        };

        auto pending = std::make_shared<PendingBuild>();
        pending->held = held;
        pending->units = units;
        pending->serial = serial;
        // The producer picks up the result in its next Settle.
        const bool accepted = m_desc.post(std::move(input), std::move(built),
            [pending, wake = m_desc.wake](std::shared_ptr<const PublishedBufferVersion> version, std::string error) {
                pending->version = std::move(version);
                pending->error = std::move(error);
                pending->done.store(true, std::memory_order_release);
                if (wake) wake();
            });
        if (!accepted) {
            chain.failed = true;
            posted = false;
            continue;
        }
        chain.pending = std::move(pending);
    }
    return posted;
}

bool BufferChains::Finish(const BufferChainSource& source, std::span<std::shared_ptr<const PublishedBufferVersion>> out) const {
    if (out.size() < Count()) throw std::invalid_argument("BufferChains: Finish's output has fewer entries than chains");
    for (std::size_t k = 0; k < Count(); ++k) {
        out[k] = nullptr;
        if (!source.Present(k)) continue;
        const auto& chain = m_chains[k];
        if (chain.failed || !chain.last || chain.lastUnits != source.Units(k)) return false;
        out[k] = chain.last;
    }
    return true;
}

std::optional<std::uint64_t> BufferChains::OldestContentEpoch(std::size_t chain) const noexcept {
    if (chain >= Count()) return std::nullopt;
    const auto floor = m_shared[chain].floorEpoch.load(std::memory_order_acquire);
    return floor == kNoFloor ? std::nullopt : std::optional<std::uint64_t>(floor);
}

std::vector<BufferChains::InFlight> BufferChains::Pending() const {
    std::vector<InFlight> out;
    for (std::size_t k = 0; k < Count(); ++k)
        if (const auto pending = m_chains[k].pending)
            out.push_back({k, pending->held.version, pending->done.load(std::memory_order_acquire)});
    return out;
}

std::uint64_t BufferChains::Suspensions() const noexcept {
    std::uint64_t suspensions = 0;
    for (std::size_t k = 0; k < Count(); ++k) suspensions += m_shared[k].suspensions.load(std::memory_order_relaxed);
    return suspensions;
}

std::vector<BufferChains::Report> BufferChains::TakeReport() {
    std::vector<Report> out(Count());
    for (std::size_t k = 0; k < Count(); ++k) {
        auto& shared = m_shared[k];
        auto& r = out[k];
        r.built = shared.built.exchange(0, std::memory_order_relaxed);
        r.uploadedBytes = shared.uploadedBytes.exchange(0, std::memory_order_relaxed);
        r.copiedBytes = shared.copiedBytes.exchange(0, std::memory_order_relaxed);
        r.wholeCopies = shared.wholeCopies.exchange(0, std::memory_order_relaxed);
        r.suspensions = shared.suspensions.exchange(0, std::memory_order_relaxed);
        r.failures = shared.failures.exchange(0, std::memory_order_relaxed);
        if (const auto& pool = m_chains[k].pool) {
            const auto census = pool->Count();
            r.liveBackings = census.live;
            r.liveBytes = census.bytes;
        }
        r.floor = OldestContentEpoch(k);
    }
    return out;
}

} // namespace org::services
