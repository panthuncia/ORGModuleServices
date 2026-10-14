#include <ORGModuleServices/GrowableBufferSet.h>

#include <algorithm>
#include <array>
#include <stdexcept>

#include "Render/LatchBlock.h"

namespace org::services {

namespace {

// Serials of named versions and of sealed sets: one process-wide sequence, so no two sets or versions share one.
std::atomic<std::uint64_t> g_serials{0};
std::uint64_t NextSerial() { return g_serials.fetch_add(1, std::memory_order_relaxed) + 1; }

} // namespace

// ---- NamedVersions ----

std::uint32_t NamedVersions::IndexOf(const VersionedBuffer& buffer) const noexcept {
    const void* key = buffer.Key();
    const auto it = std::lower_bound(m_byKey.begin(), m_byKey.end(), key, [](const auto& entry, const void* k) { return entry.first < k; });
    return it != m_byKey.end() && it->first == key ? it->second : UINT32_MAX;
}

std::shared_ptr<const BufferVersion> NamedVersions::Find(const VersionedBuffer& buffer) const noexcept {
    if (const std::uint32_t index = IndexOf(buffer); index != UINT32_MAX) return m_entries[index].version;
    m_misses.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

const NamedEntry& NamedVersions::Of(GrowableHandle handle) const {
    if (!Names(handle)) throw std::out_of_range("NamedVersions: the set names no version of this buffer");
    return m_entries[handle.index];
}

const NamedEntry& NamedVersions::Of(const VersionedBuffer& buffer) const {
    const std::uint32_t index = IndexOf(buffer);
    if (index == UINT32_MAX) throw std::out_of_range("NamedVersions: the set names no version of this buffer");
    return m_entries[index];
}

const NamedEntry& NamedVersions::Of(const std::shared_ptr<VersionedBuffer>& buffer) const {
    if (!buffer) throw std::invalid_argument("NamedVersions: no buffer");
    return Of(*buffer);
}

std::shared_ptr<LatchBlock> NamedVersions::Latch(LatchHandle handle) const noexcept {
    return handle.index < m_latches.size() ? m_latches[handle.index] : nullptr;
}

std::size_t NamedVersions::AdoptAll() const {
    std::size_t adopted = 0;
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        const auto& version = m_entries[i].version;
        if (!version) continue;
        const auto buffer = m_buffers[i].lock();
        if (!buffer || buffer->Current() == version) continue;
        buffer->Adopt(version);
        ++adopted;
    }
    return adopted;
}

void NamedVersions::Bind(std::uint64_t safeAt) const noexcept {
    for (const auto& entry : m_entries)
        if (entry.lease) entry.lease.Bind(safeAt);
}

// ---- Build ----

void GrowableBufferSet::Build::Require(GrowableHandle handle, std::uint64_t units) {
    if (!handle) throw std::invalid_argument("GrowableBufferSet: a requirement without a buffer");
    if (handle.index >= m_required.size()) m_required.resize(handle.index + 1);
    m_required[handle.index].Merge({units, 0});
}

void GrowableBufferSet::Build::RequireNewer(GrowableHandle handle, std::uint64_t serial) {
    if (!handle) throw std::invalid_argument("GrowableBufferSet: a requirement without a buffer");
    if (handle.index >= m_required.size()) m_required.resize(handle.index + 1);
    m_required[handle.index].Merge({0, serial});
}

void GrowableBufferSet::Build::Require(LatchHandle handle, std::uint32_t bytes, std::uint64_t layoutKey) {
    if (!handle) throw std::invalid_argument("GrowableBufferSet: a requirement without a latch");
    if (handle.index >= m_latches.size()) m_latches.resize(handle.index + 1);
    auto& ask = m_latches[handle.index];
    if (ask.asked && ask.layoutKey != layoutKey) throw std::logic_error("GrowableBufferSet: one latch asked with two layouts");
    ask.asked = true;
    ask.bytes = std::max(ask.bytes, bytes);
    ask.layoutKey = layoutKey;
}

std::uint64_t GrowableBufferSet::Build::Units(GrowableHandle handle) const {
    return handle.index < m_set->m_slots.size() ? m_set->m_slots[handle.index].current.units : 0;
}

std::uint64_t GrowableBufferSet::Build::Asked(GrowableHandle handle) const {
    const std::uint64_t required = handle.index < m_required.size() ? m_required[handle.index].units : 0;
    const std::uint64_t inFlight = handle.index < m_set->m_slots.size() ? m_set->m_slots[handle.index].asked : 0;
    return std::max(required, inFlight);
}

std::uint64_t GrowableBufferSet::Build::Serial(GrowableHandle handle) const {
    return handle.index < m_set->m_slots.size() ? m_set->m_slots[handle.index].current.serial : 0;
}

// ---- GrowableBufferSet ----

// A registration not yet taken by the producer.
struct GrowableBufferSet::Registered {
    std::uint32_t index = 0;
    std::shared_ptr<const GrowableRegistration> buffer;  // one of the two
    std::shared_ptr<const LatchRegistration> latch;
    std::weak_ptr<VersionedBuffer> versioned;
    std::uint64_t key = 0;  // its backings' compatibility (process-unique)
    Registered* next = nullptr;
};

// One posted growth: written by its `done` before `state` leaves pending, read by the producer after.
struct GrowableBufferSet::Batch {
    enum : std::uint8_t { kPending, kReady, kFailed };
    std::vector<std::uint32_t> slots;                                 // by request
    std::vector<std::shared_ptr<const BufferVersion>> versions;       // by request
    std::vector<BackingLease> leases;                                 // by request: each version's backing
    std::vector<std::optional<std::uint64_t>> stamps;                 // by request: Capture's (the producer of the growth writes them)
    std::string error;
    std::atomic<std::uint8_t> state = kPending;
};

GrowableBufferSet::GrowableBufferSet(GrowableBufferSetHooks hooks) : m_hooks(std::move(hooks)) {}

GrowableBufferSet::~GrowableBufferSet() {
    for (Registered* node = m_registered.exchange(nullptr, std::memory_order_acquire); node;) delete std::exchange(node, node->next);
}

namespace {
template <class Node>
void Push(std::atomic<Node*>& stack, Node* node) {
    node->next = stack.load(std::memory_order_relaxed);
    while (!stack.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {}
}
} // namespace

GrowableHandle GrowableBufferSet::Register(GrowableRegistration registration) {
    if (!registration.buffer) throw std::invalid_argument("GrowableBufferSet: " + registration.name + " has no buffer");
    if (!registration.extent.unitBytes) throw std::invalid_argument("GrowableBufferSet: " + registration.name + " has no unit size");
    if (registration.contents == GrowableContents::Capture && !registration.capture)
        throw std::invalid_argument("GrowableBufferSet: " + registration.name + " captures without a capture");
    // Every version the set makes is pooled: no fresh buffers.
    if (!registration.pool) registration.pool = m_hooks.pool;
    if (!registration.pool) throw std::invalid_argument("GrowableBufferSet: " + registration.name + " has no pool (neither its own nor the set's)");
    auto* node = new Registered();
    node->index = m_nextHandle.fetch_add(1, std::memory_order_relaxed);
    node->versioned = registration.buffer;
    node->key = NextSerial();
    registration.buffer.reset();  // the set holds it weakly
    node->buffer = std::make_shared<const GrowableRegistration>(std::move(registration));
    const GrowableHandle handle{node->index};
    Push(m_registered, node);
    return handle;
}

GrowableHandle GrowableBufferSet::HandleOf(const VersionedBuffer& buffer) {
    TakeRegistered();
    const void* key = buffer.Key();
    const auto it = std::lower_bound(m_handles.begin(), m_handles.end(), key, [](const auto& entry, const void* k) { return entry.first < k; });
    if (it == m_handles.end() || it->first != key) throw std::out_of_range("GrowableBufferSet: a buffer not in the set");
    return GrowableHandle{it->second};
}

LatchHandle GrowableBufferSet::Register(LatchRegistration registration) {
    if (!registration.slots) throw std::invalid_argument("GrowableBufferSet: latch " + registration.name + " has no slots");
    auto* node = new Registered();
    node->index = m_nextLatch.fetch_add(1, std::memory_order_relaxed);
    node->latch = std::make_shared<const LatchRegistration>(std::move(registration));
    const LatchHandle handle{node->index};
    Push(m_registered, node);
    return handle;
}

NamedEntry GrowableBufferSet::EntryOf(const Slot& slot, std::shared_ptr<const BufferVersion> version, std::optional<std::uint64_t> stamp,
    BackingLease lease) const {
    NamedEntry entry;
    if (!version || !version->buffer) return entry;
    const auto& registration = *slot.registration;
    const auto& buffer = *version->buffer;
    entry.serial = NextSerial();
    entry.bytes = buffer.GetSize();
    entry.units = registration.extent.UnitsIn(entry.bytes);
    entry.srvIndex = buffer.HasSRV() ? buffer.GetSRVInfo(0).slot.index : 0;
    entry.deviceAddress = registration.address && m_hooks.addressOf ? m_hooks.addressOf(buffer) : 0;
    entry.contentsStamp = stamp;
    entry.lease = std::move(lease);
    entry.version = std::move(version);
    return entry;
}

void GrowableBufferSet::TakeRegistered() {
    for (Registered* node = m_registered.exchange(nullptr, std::memory_order_acquire); node;) {
        Registered* taken = std::exchange(node, node->next);
        if (taken->buffer) {
            if (taken->index >= m_slots.size()) m_slots.resize(taken->index + 1);
            auto& slot = m_slots[taken->index];
            slot.registration = std::move(taken->buffer);
            slot.buffer = std::move(taken->versioned);
            slot.key = taken->key;
            slot.made = false;
            if (const auto buffer = slot.buffer.lock()) {
                slot.current = EntryOf(slot, buffer->Current(), std::nullopt, {});  // the owner's first version: not pooled
                const std::pair<const void*, std::uint32_t> handle{buffer->Key(), taken->index};
                m_handles.insert(std::lower_bound(m_handles.begin(), m_handles.end(), handle), handle);
            }
        } else {
            if (taken->index >= m_latchSlots.size()) m_latchSlots.resize(taken->index + 1);
            m_latchSlots[taken->index].registration = std::move(taken->latch);
        }
        delete taken;
        m_changed = true;
    }
}

void GrowableBufferSet::Settle() {
    if (!m_batch) return;
    const auto state = m_batch->state.load(std::memory_order_acquire);
    if (state == Batch::kPending) return;
    for (std::size_t i = 0; i < m_batch->slots.size(); ++i) {
        auto& slot = m_slots[m_batch->slots[i]];
        slot.asked = 0;
        if (state == Batch::kReady && slot.registration) {
            slot.current = EntryOf(slot, m_batch->versions[i], m_batch->stamps[i], m_batch->leases[i]);
            slot.made = true;
            ++m_stats.grown;
            m_changed = true;
        }
    }
    if (state == Batch::kFailed) {
        ++m_stats.failed;
        m_failure = m_batch->error;
    }
    m_batch.reset();
}

GrowableBufferSet::Build GrowableBufferSet::Begin() {
    TakeRegistered();
    Settle();
    // An expired buffer leaves the set.
    for (std::uint32_t i = 0; i < m_slots.size(); ++i)
        if (auto& slot = m_slots[i]; slot.registration && slot.buffer.expired()) {
            slot = {};
            std::erase_if(m_handles, [i](const auto& handle) { return handle.second == i; });
            m_changed = true;
        }
    return Build(*this);
}

bool GrowableBufferSet::Post(std::vector<std::uint32_t> unmet, const std::vector<GrowableRequirement>& required) {
    auto batch = std::make_shared<Batch>();
    const auto uploader = m_hooks.uploads ? m_hooks.uploads() : nullptr;
    std::vector<VersionGrowthRequest> requests;
    for (const std::uint32_t index : unmet) {
        auto& slot = m_slots[index];
        auto buffer = slot.buffer.lock();
        if (!buffer) continue;
        const auto& registration = *slot.registration;
        const std::uint64_t units = required[index].GrowTo(registration.policy, slot.current.units);
        if (m_hooks.grown) m_hooks.grown(registration, slot.current.units, units, registration.extent.BytesFor(units));
        auto& request = requests.emplace_back();
        request.buffer = std::move(buffer);
        if (registration.extent.structuredStride) {
            const std::uint64_t elements = registration.extent.ElementsFor(units);
            if (elements > UINT32_MAX) throw std::length_error("GrowableBufferSet: " + registration.name + " past a structured buffer's elements");
            request.elements = static_cast<std::uint32_t>(elements);
        } else {
            request.bytes = registration.extent.BytesFor(units);
        }
        // Pooled: a backing of the policy's class (the grown units' elements, or bytes), reused only by this registration's
        // versions (its key: a backing is made like its buffer).
        request.pool = registration.pool;
        request.capacityClass = registration.extent.CapacityClassFor(units);
        request.stride = registration.extent.structuredStride;
        request.flags.deviceAddress = registration.address;
        request.compatibility = slot.key;
        request.name = registration.name;
        const std::size_t at = batch->slots.size();
        switch (registration.contents) {
        case GrowableContents::Fresh:
            break;
        case GrowableContents::Zero:
            request.contentsOf = [](const BufferVersion& version, std::vector<VersionFill>& fills, std::shared_ptr<const void>&) {
                fills.push_back(ZeroFill(version.buffer->GetSize()));
            };
            request.uploads = uploader;
            break;
        case GrowableContents::Capture:
            request.contentsOf = [batch = std::weak_ptr<Batch>(batch), registration = slot.registration, addressOf = m_hooks.addressOf, at](
                                     const BufferVersion& version, std::vector<VersionFill>& fills, std::shared_ptr<const void>& contents) {
                const std::uint64_t address = registration->address && addressOf ? addressOf(*version.buffer) : 0;
                const std::uint64_t stamp = registration->capture(version, address, fills, contents);
                if (const auto held = batch.lock()) held->stamps[at] = stamp;
            };
            request.uploads = uploader;
            break;
        }
        if (request.contentsOf && !request.uploads) throw std::logic_error("GrowableBufferSet: " + registration.name + " fills without an uploader");
        batch->slots.push_back(index);
        slot.asked = units;
    }
    if (requests.empty()) return true;
    batch->stamps.resize(batch->slots.size());
    async::VersionGrowthWork work;
    work.produce = [requests = std::move(requests)] {
        auto grown = std::make_shared<GrownVersions>(GrowVersions(requests));
        auto ready = grown->ready;
        return async::VersionGrowthWork::Produced{std::shared_ptr<const void>(std::move(grown)), std::move(ready)};
    };
    work.done = [batch, wake = m_hooks.wake](std::shared_ptr<const void> value, std::string error) {
        const auto* grown = static_cast<const GrownVersions*>(value.get());
        if (grown && grown->versions.size() == batch->slots.size() && grown->leases.size() == batch->slots.size()) {
            batch->versions = grown->versions;
            batch->leases = grown->leases;
            batch->state.store(Batch::kReady, std::memory_order_release);
        } else {
            batch->error = error.empty() ? std::string("a growth made the wrong versions") : std::move(error);
            batch->state.store(Batch::kFailed, std::memory_order_release);
        }
        if (wake) wake();
    };
    m_batch = batch;
    ++m_stats.batches;
    if (m_hooks.post && m_hooks.post(std::move(work))) return true;
    m_batch.reset();
    for (const std::uint32_t index : batch->slots) m_slots[index].asked = 0;
    ++m_stats.failed;
    return false;
}

GrowableBufferSet::Sealed GrowableBufferSet::Seal(Build&& build) {
    // Registered between Begin and the requirements naming them.
    TakeRegistered();
    std::vector<std::uint32_t> unmet;
    for (std::uint32_t i = 0; i < build.m_required.size(); ++i) {
        const auto& required = build.m_required[i];
        if (!required.units && !required.newerThan) continue;
        if (i >= m_slots.size() || !m_slots[i].registration)
            throw std::logic_error("GrowableBufferSet: a requirement of a buffer not in the set");
        if (!required.MetBy(m_slots[i].current.units, m_slots[i].current.serial)) unmet.push_back(i);
    }
    // A Zero registration's first version: replaced by a zero-filled one at its size before any set names it.
    for (std::uint32_t i = 0; i < m_slots.size(); ++i) {
        const auto& slot = m_slots[i];
        if (slot.registration && !slot.made && slot.registration->contents == GrowableContents::Zero && !slot.buffer.expired() &&
            std::find(unmet.begin(), unmet.end(), i) == unmet.end())
            unmet.push_back(i);
    }
    if (build.m_required.size() < m_slots.size()) build.m_required.resize(m_slots.size());
    if (!unmet.empty()) {
        Sealed out;
        // One batch at a time: what it does not cover is posted once it settled.
        if (m_batch) {
            out.pending = true;
            return out;
        }
        out.error = std::exchange(m_failure, {});
        if (!Post(std::move(unmet), build.m_required)) {
            out.error = "the growth batch was refused";
            return out;
        }
        if (m_batch) {
            out.pending = true;
            return out;
        }
    }
    for (std::uint32_t i = 0; i < build.m_latches.size(); ++i) {
        const auto& ask = build.m_latches[i];
        if (!ask.asked) continue;
        if (i >= m_latchSlots.size() || !m_latchSlots[i].registration) throw std::logic_error("GrowableBufferSet: a requirement of a latch not in the set");
        auto& slot = m_latchSlots[i];
        const bool sameLayout = slot.block && slot.layoutKey == ask.layoutKey;
        if (sameLayout && slot.block->Stride() >= ask.bytes) continue;
        // A new layout from exactly what it asks; the same one outgrown by the registration's policy. Frames in flight keep the
        // block the sets they adopted name.
        const std::uint32_t bytes = slot.registration->policy.NextAs<std::uint32_t>(sameLayout ? slot.block->Stride() : 0, ask.bytes);
        slot.block = LatchBlock::Create(slot.registration->name, bytes, slot.registration->slots);
        slot.layoutKey = ask.layoutKey;
        m_changed = true;
        ++m_stats.latches;
    }
    if (!m_changed && m_last.named) {
        ++m_stats.kept;
        return m_last;
    }
    auto named = std::make_shared<NamedVersions>();
    named->m_serial = NextSerial();
    named->m_entries.resize(m_slots.size());
    named->m_buffers.resize(m_slots.size());
    for (std::uint32_t i = 0; i < m_slots.size(); ++i) {
        const auto& slot = m_slots[i];
        if (!slot.registration || !slot.current.version || (!slot.made && slot.registration->contents == GrowableContents::Zero)) continue;
        named->m_entries[i] = slot.current;
        named->m_buffers[i] = slot.buffer;
        if (const auto buffer = slot.buffer.lock()) named->m_byKey.emplace_back(buffer->Key(), i);
    }
    std::sort(named->m_byKey.begin(), named->m_byKey.end());
    named->m_latches.reserve(m_latchSlots.size());
    for (const auto& slot : m_latchSlots) named->m_latches.push_back(slot.block);
    std::shared_ptr<const NamedVersions> sealed = std::move(named);
    m_last.fragment = async::RevisionFragment::MakeReady(sealed);
    m_last.named = std::move(sealed);
    m_changed = false;
    ++m_stats.sealed;
    return m_last;
}

GrowableBufferSet::Stats GrowableBufferSet::TakeStats() { return std::exchange(m_stats, {}); }

} // namespace org::services
