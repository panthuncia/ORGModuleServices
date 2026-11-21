#pragma once

// A descriptor index in GPU-visible bytes must live as long as those bytes do. Rows declare such fields DescriptorIndex,
// which only OwnedRow::Bind sets, together with the binding's owner. The upload classes (CommitUploads.h) then keep those
// owners alive with the uploaded content.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace org::services {

template <class Row, class Binding>
class OwnedRow;

/** @brief A descriptor heap index in a GPU row. Set only by OwnedRow::Bind or as a sentinel meaning none. */
class DescriptorIndex {
public:
    constexpr DescriptorIndex() = default;
    /** @brief A value that names no descriptor, such as ~0u. */
    static constexpr DescriptorIndex Sentinel(std::uint32_t value) {
        DescriptorIndex index;
        index.m_value = value;
        return index;
    }
    constexpr std::uint32_t Value() const { return m_value; }

private:
    template <class Row, class Binding>
    friend class OwnedRow;
    std::uint32_t m_value = 0;
};
static_assert(sizeof(DescriptorIndex) == 4 && std::is_standard_layout_v<DescriptorIndex> &&
              std::is_trivially_copyable_v<DescriptorIndex>);

/**
 * @brief What OwnedRow::Bind takes: a descriptor's heap index and the owner that keeps the descriptor alive.
 * It converts to false when it names no descriptor. The owner may be null.
 */
template <class Binding>
concept DescriptorBinding = requires(Binding binding) {
    { static_cast<bool>(binding) };
    { binding.index } -> std::convertible_to<std::uint32_t>;
    { std::move(binding.owner) } -> std::convertible_to<std::shared_ptr<const void>>;
};

/** @brief A plain descriptor binding. UINT32_MAX names no descriptor. */
struct OwnedBinding {
    std::uint32_t index = UINT32_MAX;
    std::shared_ptr<const void> owner;
    explicit operator bool() const { return index != UINT32_MAX; }
};

/**
 * @brief A row plus the owners of the descriptors it names. Upload it whole so the owners travel with it.
 * A copy of the plain row carries no owners, so never upload that alone once a field is bound.
 * Binding is the host's descriptor binding type (see DescriptorBinding).
 */
template <class Row, class Binding = OwnedBinding>
class OwnedRow {
    static_assert(std::is_trivially_copyable_v<Row>, "a GPU row is bytes");
    static_assert(DescriptorBinding<Binding>, "a binding has an index, an owner and a validity test");

public:
    OwnedRow() = default;
    explicit OwnedRow(const Row& plain) : m_row(plain) {}

    Row& Plain() { return m_row; }
    const Row& Plain() const { return m_row; }

    /** @brief Writes binding's index into field and keeps its owner. False and nothing written without one. */
    bool Bind(DescriptorIndex Row::*field, Binding binding) {
        if (!binding)
            return false;
        (m_row.*field).m_value = binding.index;
        if (binding.owner)
            m_owners.push_back(std::move(binding.owner));
        return true;
    }
    /** @brief Sets field to a sentinel. An owner bound before stays held until the row goes. */
    void Unbind(DescriptorIndex Row::*field, DescriptorIndex sentinel = {}) { m_row.*field = sentinel; }

    std::span<const std::shared_ptr<const void>> Owners() const { return m_owners; }
    const void* Bytes() const { return &m_row; }
    static constexpr std::size_t kBytes = sizeof(Row);

private:
    Row m_row{};
    std::vector<std::shared_ptr<const void>> m_owners;
};

} // namespace org::services
