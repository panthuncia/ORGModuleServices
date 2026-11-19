#pragma once

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace org::services {

/**
 * @brief How a variable may be used.
 *
 * A variable with `sizes = false` holds constants only. It may not be a dependency of a texture description (decision 6a).
 * RenderSize and the dynamic resolution ratios are such variables.
 */
struct VariableTraits {
    bool sizes = true;
};

/** @brief A variable's position in its set, in declaration order. */
using VariableIndex = std::uint32_t;
/** @brief The index of a handle that names no variable. */
inline constexpr VariableIndex kNoVariable = std::numeric_limits<VariableIndex>::max();

/** @brief A typed handle to one variable of a VariableSet. Cheap to copy. */
template <class T>
struct Variable {
    VariableIndex index = kNoVariable;
    /** @brief True when the handle names a variable. */
    explicit operator bool() const noexcept { return index != kNoVariable; }
    friend bool operator==(const Variable&, const Variable&) = default;
};

/** @brief A type a variable can hold: copyable and equality-comparable. */
template <class T>
concept VariableValue = std::copy_constructible<T> && std::equality_comparable<T>;

class VariableSnapshot;

namespace detail {

// One written value and its serial. Immutable once published, so a value is never paired with another write's serial.
struct VariableEntryBase {
    explicit VariableEntryBase(std::uint64_t serial) noexcept
        : serial(serial) {}
    virtual ~VariableEntryBase() = default;
    std::uint64_t serial;
};

template <class T>
struct VariableEntry final : VariableEntryBase {
    VariableEntry(T value, std::uint64_t serial)
        : VariableEntryBase(serial), value(std::move(value)) {}
    T value;
};

template <class T>
std::string DescribeValue(const T& value) {
    if constexpr (std::is_default_constructible_v<std::formatter<T, char>>) {
        return std::format("{}", value);
    } else if constexpr (requires(std::ostream& os, const T& v) { os << v; }) {
        std::ostringstream out;
        out << value;
        return std::move(out).str();
    } else if constexpr (std::is_enum_v<T>) {
        return std::format("{}", static_cast<std::underlying_type_t<T>>(value));
    } else {
        return "<value>";
    }
}

// The storage of one variable. Its description never changes after it is published.
struct VariableSlotBase {
    VariableSlotBase(std::string name, VariableTraits traits, const std::type_info& type, std::vector<VariableIndex> deps)
        : name(std::move(name)), traits(traits), type(&type), deps(std::move(deps)) {}
    virtual ~VariableSlotBase() = default;

    [[nodiscard]] virtual std::shared_ptr<const VariableEntryBase> Load() const = 0;
    [[nodiscard]] virtual std::string Describe(const VariableEntryBase& entry) const = 0;
    // Derived variables only, on the consumer's thread. Null when the value is unchanged. A new entry is also stored for Get.
    [[nodiscard]] virtual std::shared_ptr<const VariableEntryBase> Evaluate(const VariableSnapshot& snapshot) = 0;
    [[nodiscard]] bool IsDerived() const noexcept { return derived; }

    std::string name;
    VariableTraits traits;
    const std::type_info* type;
    // Derived variables only. All of them are declared before this one.
    std::vector<VariableIndex> deps;
    bool derived = false;
    // The dependency serials of the last evaluation. Only the consumer touches them after publication.
    std::vector<std::uint64_t> evaluatedDepSerials;
};

template <class T>
struct VariableSlot : VariableSlotBase {
    VariableSlot(std::string name, VariableTraits traits, std::vector<VariableIndex> deps, T initial)
        : VariableSlotBase(std::move(name), traits, typeid(T), std::move(deps)),
          current(std::make_shared<const VariableEntry<T>>(std::move(initial), 1)) {}

    [[nodiscard]] std::shared_ptr<const VariableEntryBase> Load() const override {
        return current.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::string Describe(const VariableEntryBase& entry) const override {
        return DescribeValue(static_cast<const VariableEntry<T>&>(entry).value);
    }

    [[nodiscard]] std::shared_ptr<const VariableEntryBase> Evaluate(const VariableSnapshot& snapshot) override {
        T value = derive(snapshot);
        auto previous = current.load(std::memory_order_acquire);
        if (previous->value == value) return nullptr;
        auto next = std::make_shared<const VariableEntry<T>>(std::move(value), previous->serial + 1);
        current.store(next, std::memory_order_release);
        return next;
    }

    std::atomic<std::shared_ptr<const VariableEntry<T>>> current;
    std::function<T(const VariableSnapshot&)> derive;
};

// Slots in fixed chunks, so a slot never moves while others are declared. A slot is visible once `count` covers it.
struct VariableStorage {
    static constexpr std::size_t kChunkSize = 256;
    static constexpr std::size_t kMaxChunks = 256;

    [[nodiscard]] VariableSlotBase& At(std::size_t index) const noexcept {
        return *chunks[index / kChunkSize][index % kChunkSize];
    }

    std::array<std::unique_ptr<std::unique_ptr<VariableSlotBase>[]>, kMaxChunks> chunks{};
    std::atomic<std::size_t> count{ 0 };
};

} // namespace detail

/**
 * @brief The values and serials of every variable of a set at one Take. Immutable and shared.
 *
 * Snapshots of one set compare by serial. A variable's serial bumps on each change of its value. The snapshot's own Serial is
 * the set's change serial at Take, so two snapshots with equal serials hold equal values.
 */
class VariableSnapshot {
public:
    /** @brief The variable's value. Throws std::out_of_range for an index past Size and std::invalid_argument for another type. */
    template <class T>
    [[nodiscard]] const T& Get(Variable<T> variable) const {
        const Item& item = At(variable.index, "Get");
        if (*item.slot->type != typeid(T))
            throw std::invalid_argument("VariableSnapshot::Get: '" + item.slot->name + "' holds another type");
        return static_cast<const detail::VariableEntry<T>&>(*item.entry).value;
    }
    /** @brief The variable's serial. It bumps on each change of its value. Throws std::out_of_range for an index past Size. */
    [[nodiscard]] std::uint64_t Serial(VariableIndex index) const { return At(index, "Serial").entry->serial; }
    /** @brief The set's change serial at Take. Equal serials mean equal values. */
    [[nodiscard]] std::uint64_t Serial() const noexcept { return m_serial; }
    /** @brief How many variables were declared at Take. */
    [[nodiscard]] std::size_t Size() const noexcept { return m_items.size(); }
    /** @brief The name the variable was declared with. */
    [[nodiscard]] std::string_view Name(VariableIndex index) const { return At(index, "Name").slot->name; }
    /** @brief The traits the variable was declared with. */
    [[nodiscard]] const VariableTraits& Traits(VariableIndex index) const { return At(index, "Traits").slot->traits; }
    /** @brief True for a variable made with Derive. */
    [[nodiscard]] bool IsDerived(VariableIndex index) const { return At(index, "IsDerived").slot->IsDerived(); }

    /**
     * @brief The variables whose serial differs from `older`'s, in index order.
     *
     * A variable declared after `older` was taken counts as changed. Both snapshots must come from the same set.
     */
    [[nodiscard]] std::vector<VariableIndex> ChangedSince(const VariableSnapshot& older) const {
        std::vector<VariableIndex> changed;
        for (std::size_t i = 0; i < m_items.size(); ++i) {
            if (i >= older.m_items.size() || older.m_items[i].entry->serial != m_items[i].entry->serial)
                changed.push_back(static_cast<VariableIndex>(i));
        }
        return changed;
    }

    /**
     * @brief The value as text for logs.
     *
     * Types with a std::formatter or an operator<< print through it. Enums without either print their number. Others print
     * "<value>".
     */
    [[nodiscard]] std::string Describe(VariableIndex index) const {
        const Item& item = At(index, "Describe");
        return item.slot->Describe(*item.entry);
    }

private:
    friend class VariableSet;
    struct Item {
        std::shared_ptr<const detail::VariableEntryBase> entry;
        detail::VariableSlotBase* slot = nullptr;
    };

    [[nodiscard]] const Item& At(VariableIndex index, const char* what) const {
        if (index >= m_items.size())
            throw std::out_of_range(std::string("VariableSnapshot::") + what + ": no variable at this index");
        return m_items[index];
    }

    // The slots live as long as any snapshot that points at them.
    std::shared_ptr<const detail::VariableStorage> m_storage;
    std::vector<Item> m_items;
    std::uint64_t m_serial = 0;
};

/**
 * @brief Named, typed values that writers set from any thread and one consumer reads as snapshots.
 *
 * - Writers call Set. It is lock-free with respect to other variables and to Take. A write of an equal value is no change. A
 *   change bumps the variable's serial and calls the wake.
 * - The consumer (the revision lane) calls Take. It returns an immutable snapshot and evaluates derived variables.
 * - Declare and Derive are rare. They take a mutex but are safe while other threads Set and Take.
 *
 * Each variable holds its newest value and serial as one immutable entry behind an atomic shared pointer. A write swaps the
 * whole entry, so a reader never sees a value with another write's serial.
 */
class VariableSet {
public:
    /** @brief `wake` is called after each change. See SetWake. */
    explicit VariableSet(std::function<void()> wake = {})
        : m_storage(std::make_shared<detail::VariableStorage>()) {
        SetWake(std::move(wake));
    }
    VariableSet(const VariableSet&) = delete;
    VariableSet& operator=(const VariableSet&) = delete;

    /**
     * @brief Any thread. Replaces the wake.
     *
     * The wake is called after each change, outside every lock, on the thread that made the change. Keep it cheap, such as a
     * post to the consumer. A declaration counts as a change.
     */
    void SetWake(std::function<void()> wake) {
        std::shared_ptr<const std::function<void()>> next;
        if (wake) next = std::make_shared<const std::function<void()>>(std::move(wake));
        m_wake.store(std::move(next), std::memory_order_release);
    }

    /**
     * @brief Declares a variable that writers set. Its first serial is 1.
     *
     * Throws std::invalid_argument when the name is empty or already declared, and std::length_error when the set is full.
     */
    template <VariableValue T>
    Variable<T> Declare(std::string name, T initial, VariableTraits traits = {}) {
        auto slot = std::make_unique<detail::VariableSlot<T>>(std::move(name), traits, std::vector<VariableIndex>{},
            std::move(initial));
        return Variable<T>{ Publish(std::move(slot)) };
    }

    /**
     * @brief Declares a variable computed from others.
     *
     * `fn` runs in Take, on the consumer's thread, with the snapshot being built. It may read its dependencies and any variable
     * declared before this one. It runs again only when a dependency's serial changed. The variable's serial bumps only when
     * the result differs from the last one. It also runs once here, against the newest values, to give the first value.
     *
     * Set on a derived variable throws. Get returns the value of the last evaluation.
     *
     * Throws std::invalid_argument when the name is taken, `fn` is empty, a dependency is not declared yet, or `traits.sizes`
     * is true while a dependency has `sizes = false` (a constant may not feed a size through a derived variable).
     */
    template <VariableValue T>
    Variable<T> Derive(std::string name, std::vector<VariableIndex> deps, std::function<T(const VariableSnapshot&)> fn,
        VariableTraits traits = {}) {
        if (!fn) throw std::invalid_argument("VariableSet::Derive: '" + name + "' has no function");
        std::unique_lock lock(m_declareMutex);
        const std::size_t count = m_storage->count.load(std::memory_order_acquire);
        for (const VariableIndex dep : deps) {
            if (dep >= count)
                throw std::invalid_argument("VariableSet::Derive: '" + name + "' depends on an undeclared variable");
            if (traits.sizes && !m_storage->At(dep).traits.sizes)
                throw std::invalid_argument("VariableSet::Derive: '" + name + "' may size textures but depends on constant '" +
                                            m_storage->At(dep).name + "'");
        }
        // The first value, from the newest values of every declared variable.
        const auto current = BuildSnapshot(count, 0);
        auto slot = std::make_unique<detail::VariableSlot<T>>(std::move(name), traits, std::move(deps), fn(*current));
        slot->derived = true;
        slot->derive = std::move(fn);
        for (const VariableIndex dep : slot->deps) slot->evaluatedDepSerials.push_back(current->Serial(dep));
        return Variable<T>{ PublishLocked(std::move(slot), lock) };
    }

    /**
     * @brief Any thread, lock-free. Sets the variable's value.
     *
     * Returns false and does nothing when the value equals the current one. Otherwise bumps the variable's serial, calls the
     * wake and returns true. Floats compare exactly. Throws std::out_of_range for an undeclared variable and
     * std::invalid_argument for another type or a derived variable.
     */
    template <VariableValue T>
    bool Set(Variable<T> variable, T value) {
        auto& slot = TypedSlot<T>(variable, "Set");
        if (slot.derived) throw std::invalid_argument("VariableSet::Set: '" + slot.name + "' is derived");
        auto current = slot.current.load(std::memory_order_acquire);
        std::shared_ptr<const detail::VariableEntry<T>> next;
        do {
            if (current->value == value) return false;
            next = std::make_shared<const detail::VariableEntry<T>>(value, current->serial + 1);
        } while (!slot.current.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_acquire));
        // Counted after the entry is published. A Take that sees the count also sees the value.
        m_changes.fetch_add(1, std::memory_order_acq_rel);
        Wake();
        return true;
    }

    /** @brief Any thread. The newest value. For a derived variable, the value of the last evaluation. */
    template <VariableValue T>
    [[nodiscard]] T Get(Variable<T> variable) const {
        return TypedSlot<T>(variable, "Get").current.load(std::memory_order_acquire)->value;
    }

    /** @brief Any thread. The variable's newest serial. */
    [[nodiscard]] std::uint64_t Serial(VariableIndex index) const { return Slot(index, "Serial").Load()->serial; }

    /** @brief Any thread. The index of the variable declared with this name, or kNoVariable. */
    [[nodiscard]] VariableIndex Find(std::string_view name) const {
        std::lock_guard lock(m_declareMutex);
        const auto found = m_names.find(std::string(name));
        return found == m_names.end() ? kNoVariable : found->second;
    }

    /** @brief Any thread. How many variables are declared. */
    [[nodiscard]] std::size_t Size() const noexcept { return m_storage->count.load(std::memory_order_acquire); }

    /**
     * @brief The consumer's thread only (one consumer). A snapshot of every variable.
     *
     * Returns the previous snapshot when nothing changed since it. Otherwise it reads every variable's newest entry and
     * evaluates the derived variables whose dependencies changed, in declaration order. Never waits on writers. An exception
     * from a derived function propagates, and the previous snapshot stays the newest.
     */
    std::shared_ptr<const VariableSnapshot> Take() {
        // Read before the entries. A write counted after this read is seen by the next Take.
        const std::uint64_t changes = m_changes.load(std::memory_order_acquire);
        if (m_last && m_last->Serial() == changes) return m_last;
        const std::size_t count = m_storage->count.load(std::memory_order_acquire);
        auto snapshot = BuildSnapshot(count, changes);
        for (std::size_t i = 0; i < count; ++i) {
            auto& slot = m_storage->At(i);
            if (!slot.derived) continue;
            bool stale = false;
            for (std::size_t d = 0; d < slot.deps.size(); ++d)
                stale |= snapshot->m_items[slot.deps[d]].entry->serial != slot.evaluatedDepSerials[d];
            if (!stale) continue;
            if (auto next = slot.Evaluate(*snapshot)) snapshot->m_items[i].entry = std::move(next);
            for (std::size_t d = 0; d < slot.deps.size(); ++d)
                slot.evaluatedDepSerials[d] = snapshot->m_items[slot.deps[d]].entry->serial;
        }
        m_last = std::move(snapshot);
        return m_last;
    }

private:
    template <class T>
    detail::VariableSlot<T>& TypedSlot(Variable<T> variable, const char* what) const {
        auto& slot = Slot(variable.index, what);
        if (*slot.type != typeid(T))
            throw std::invalid_argument(std::string("VariableSet::") + what + ": '" + slot.name + "' holds another type");
        return static_cast<detail::VariableSlot<T>&>(slot);
    }

    detail::VariableSlotBase& Slot(VariableIndex index, const char* what) const {
        if (index >= m_storage->count.load(std::memory_order_acquire))
            throw std::out_of_range(std::string("VariableSet::") + what + ": no variable at this index");
        return m_storage->At(index);
    }

    std::shared_ptr<VariableSnapshot> BuildSnapshot(std::size_t count, std::uint64_t serial) const {
        auto snapshot = std::make_shared<VariableSnapshot>();
        snapshot->m_storage = m_storage;
        snapshot->m_serial = serial;
        snapshot->m_items.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto& slot = m_storage->At(i);
            snapshot->m_items.push_back({ slot.Load(), &slot });
        }
        return snapshot;
    }

    VariableIndex Publish(std::unique_ptr<detail::VariableSlotBase> slot) {
        std::unique_lock lock(m_declareMutex);
        return PublishLocked(std::move(slot), lock);
    }

    VariableIndex PublishLocked(std::unique_ptr<detail::VariableSlotBase> slot, std::unique_lock<std::mutex>& lock) {
        if (slot->name.empty()) throw std::invalid_argument("VariableSet: a variable needs a name");
        if (m_names.contains(slot->name))
            throw std::invalid_argument("VariableSet: '" + slot->name + "' is declared twice");
        auto& storage = *m_storage;
        const std::size_t index = storage.count.load(std::memory_order_relaxed);
        if (index >= detail::VariableStorage::kChunkSize * detail::VariableStorage::kMaxChunks)
            throw std::length_error("VariableSet: too many variables");
        auto& chunk = storage.chunks[index / detail::VariableStorage::kChunkSize];
        if (!chunk) chunk = std::make_unique<std::unique_ptr<detail::VariableSlotBase>[]>(detail::VariableStorage::kChunkSize);
        m_names.emplace(slot->name, static_cast<VariableIndex>(index));
        chunk[index % detail::VariableStorage::kChunkSize] = std::move(slot);
        storage.count.store(index + 1, std::memory_order_release);
        m_changes.fetch_add(1, std::memory_order_acq_rel);
        lock.unlock();
        Wake();
        return static_cast<VariableIndex>(index);
    }

    void Wake() const {
        if (const auto wake = m_wake.load(std::memory_order_acquire)) (*wake)();
    }

    std::shared_ptr<detail::VariableStorage> m_storage;
    // Bumped after each change and declaration. A snapshot's serial is its value at Take.
    std::atomic<std::uint64_t> m_changes{ 0 };
    std::atomic<std::shared_ptr<const std::function<void()>>> m_wake;
    mutable std::mutex m_declareMutex;
    std::unordered_map<std::string, VariableIndex> m_names;
    // The consumer's newest snapshot.
    std::shared_ptr<const VariableSnapshot> m_last;
};

/**
 * @brief Throws std::invalid_argument when a dependency of `what` (a texture description) has `sizes = false`, or is not
 * declared in `snapshot`. Used at DeclareTexture.
 */
inline void RequireSizes(const VariableSnapshot& snapshot, std::span<const VariableIndex> deps, std::string_view what) {
    for (const VariableIndex dep : deps) {
        if (dep >= snapshot.Size())
            throw std::invalid_argument(std::string(what) + " depends on an undeclared variable");
        if (!snapshot.Traits(dep).sizes)
            throw std::invalid_argument(std::string(what) + " depends on constant '" + std::string(snapshot.Name(dep)) +
                                        "', which may not size a texture");
    }
}

} // namespace org::services
