#pragma once

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/GrowthPolicy.h>

#include <cstdint>
#include <type_traits>
#include <memory>
#include <optional>
#include <utility>

namespace org {
struct BufferVersion;
}

namespace org::services {

/**
 * @brief A growable buffer's size in units (rows, slots, events): fixed bytes (a header) plus bytes per unit. A structured buffer
 * has its stride (its versions are made in whole elements); a raw one has none (made in bytes).
 */
struct BufferExtent {
    std::uint64_t fixedBytes = 0;
    std::uint64_t unitBytes = 0;          // > 0
    std::uint32_t structuredStride = 0;  // 0: raw

    [[nodiscard]] constexpr std::uint64_t BytesFor(std::uint64_t units) const noexcept { return fixedBytes + units * unitBytes; }
    /** @brief The units a version of `bytes` holds. */
    [[nodiscard]] constexpr std::uint64_t UnitsIn(std::uint64_t bytes) const noexcept {
        return unitBytes && bytes > fixedBytes ? (bytes - fixedBytes) / unitBytes : 0;
    }
    /** @brief A structured version's elements for `units` (rounded up: it holds at least them). */
    [[nodiscard]] constexpr std::uint64_t ElementsFor(std::uint64_t units) const noexcept {
        return structuredStride ? (BytesFor(units) + structuredStride - 1) / structuredStride : 0;
    }
    /** @brief The pool class a version for `units` is made at (BackingRequest::capacityClass): its elements, or its bytes when raw. */
    [[nodiscard]] constexpr std::uint64_t CapacityClassFor(std::uint64_t units) const noexcept {
        return structuredStride ? ElementsFor(units) : BytesFor(units);
    }
};

/**
 * @brief `value` (a version) owning `lease`: the lease lives exactly as long as the last reference to the returned pointer (an
 * aliasing shared_ptr over one allocation holding both), so the backing returns to its pool when the version is released, and
 * not before. Any thread.
 */
template <class T>
[[nodiscard]] std::shared_ptr<const T> LeasedVersion(T value, BackingLease lease) {
    struct Held {
        T value;
        BackingLease lease;
    };
    auto held = std::make_shared<const Held>(Held{std::move(value), std::move(lease)});
    return std::shared_ptr<const T>(held, &held->value);
}

/**
 * @brief What a new version holds when it turns ready.
 * - Fresh: undefined (a pooled backing may hold an older version's contents): its writers write it whole (the contents rule,
 *   FilledVersion, keyed on the version's serial, never on the backing).
 * - Zero: zeros, filled through the uploader.
 * - Capture: what the registration's capture lays out from immutable state, with that state's contents stamp.
 */
enum class GrowableContents : std::uint8_t { Fresh, Zero, Capture };

/** @brief What a build asks of one buffer: at least `units`, and a version newer than serial `newerThan` (a relayout). */
struct GrowableRequirement {
    std::uint64_t units = 0;
    std::uint64_t newerThan = 0;

    /** @brief Merged with another ask of the same build: the most of each. */
    constexpr void Merge(const GrowableRequirement& other) noexcept {
        units = units > other.units ? units : other.units;
        newerThan = newerThan > other.newerThan ? newerThan : other.newerThan;
    }
    /** @brief Whether a version of (units, serial) holds it. */
    [[nodiscard]] constexpr bool MetBy(std::uint64_t heldUnits, std::uint64_t serial) const noexcept { return heldUnits >= units && serial > newerThan; }
    /** @brief The units a growth makes from a version of `heldUnits`: grown by the policy, or the same size for a relayout alone. */
    [[nodiscard]] constexpr std::uint64_t GrowTo(const GrowthPolicy& policy, std::uint64_t heldUnits) const {
        return heldUnits >= units ? heldUnits : policy.Next(heldUnits, units);
    }
};

/**
 * @brief One buffer as a sealed set names it. Capacity is read from the version itself, so a layout made from this entry and the
 * buffer a recording resolves never disagree. `serial` is process-unique per version the set names: it moves exactly when the
 * version does.
 */
struct NamedEntry {
    std::shared_ptr<const BufferVersion> version;
    std::uint64_t serial = 0;
    std::uint64_t units = 0;
    std::uint64_t bytes = 0;
    std::uint64_t deviceAddress = 0;  // registrations with `address`, else 0
    std::uint32_t srvIndex = 0;       // 0: the buffer has no SRV
    std::optional<std::uint64_t> contentsStamp;  // a captured version's: the state it was laid out from
    // The pooled backing the version was made into (empty: the buffer's own first version, not pooled). The version owns its own
    // lease; this copy lets the frame bind it (NamedVersions::Bind) so it is not reused while frames that read it are in flight.
    BackingLease lease;
};

/**
 * @brief A writer's record of the version it wrote into (the contents rule): before it writes, a named version it did not fill is
 * written whole, then recorded; one it filled takes only what changed. No adoption callback resets anything. Keyed on the
 * version's serial, not its backing: a version made into a reused backing (which still holds an older version's bytes) is another
 * serial, so it is written whole.
 */
struct FilledVersion {
    std::uint64_t serial = 0;  // 0: none filled

    [[nodiscard]] constexpr bool Holds(std::uint64_t named) const noexcept { return serial && serial == named; }
    [[nodiscard]] constexpr bool Holds(const NamedEntry& named) const noexcept { return Holds(named.serial); }
    constexpr void Filled(std::uint64_t named) noexcept { serial = named; }
    constexpr void Filled(const NamedEntry& named) noexcept { Filled(named.serial); }
};

/**
 * @brief FilledVersion with what the version holds as of (a writer's stamp: a kept-state version, a journal position): Since
 * gives where to send changes from, none when the version must be written whole.
 */
template <class Stamp = std::uint64_t>
struct FilledContents {
    std::uint64_t serial = 0;
    Stamp stamp{};

    /** @brief The stamp the named version holds: the one written if it is the version filled, else its capture's, else none. */
    [[nodiscard]] std::optional<Stamp> Since(std::uint64_t named, std::optional<Stamp> captured = std::nullopt) const {
        if (serial && serial == named) return stamp;
        return captured;
    }
    [[nodiscard]] std::optional<Stamp> Since(const NamedEntry& named) const
        requires std::is_convertible_v<std::uint64_t, Stamp>
    {
        return Since(named.serial, named.contentsStamp ? std::optional<Stamp>(static_cast<Stamp>(*named.contentsStamp)) : std::nullopt);
    }
    void Filled(std::uint64_t named, Stamp written) {
        serial = named;
        stamp = std::move(written);
    }
    void Filled(const NamedEntry& named, Stamp written) { Filled(named.serial, std::move(written)); }
};

} // namespace org::services
