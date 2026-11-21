// Owned rows and content owners: bound descriptors' owners travel with a row, and the owners of uploaded ranges are
// replaced, kept and sealed as ranges are rewritten. CPU only.

#include <ORGModuleServices/ContentOwners.h>
#include <ORGModuleServices/OwnedRows.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

#define CHECK(...)                                                                      \
    do {                                                                                \
        if (!(__VA_ARGS__)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
            return __LINE__;                                                            \
        }                                                                               \
    } while (false)

namespace {

using org::services::ContentOwners;
using org::services::DescriptorIndex;
using org::services::OwnedBinding;
using org::services::OwnedRow;

struct Row {
    std::uint32_t value = 0;
    DescriptorIndex texture, sampler;
};

// A host binding type with its own invalid index, as a descriptor service might have.
struct HostBinding {
    static constexpr std::uint32_t kInvalid = 0xFFFFu;
    std::uint32_t index = kInvalid;
    std::shared_ptr<const void> owner;
    bool pending = false;
    explicit operator bool() const { return index != kInvalid; }
};

std::shared_ptr<const void> Owner() { return std::make_shared<int>(0); }

int TestOwnedRow() {
    OwnedRow<Row> row(Row{.value = 7});
    CHECK(row.Plain().value == 7 && row.Owners().empty());
    // No descriptor: nothing written, no owner kept.
    CHECK(!row.Bind(&Row::texture, OwnedBinding{}));
    CHECK(row.Plain().texture.Value() == 0 && row.Owners().empty());
    const auto texture = Owner();
    CHECK(row.Bind(&Row::texture, OwnedBinding{.index = 12, .owner = texture}));
    CHECK(row.Plain().texture.Value() == 12 && row.Owners().size() == 1 && row.Owners()[0] == texture);
    // A binding with no owner writes the index and keeps nothing.
    CHECK(row.Bind(&Row::sampler, OwnedBinding{.index = 3}));
    CHECK(row.Plain().sampler.Value() == 3 && row.Owners().size() == 1);
    // Unbinding writes the sentinel and keeps the owner until the row goes.
    row.Unbind(&Row::texture, DescriptorIndex::Sentinel(~0u));
    CHECK(row.Plain().texture.Value() == ~0u && row.Owners().size() == 1);
    CHECK(row.Bytes() == &row.Plain() && OwnedRow<Row>::kBytes == sizeof(Row));
    return 0;
}

int TestHostBinding() {
    OwnedRow<Row, HostBinding> row;
    CHECK(!row.Bind(&Row::texture, HostBinding{}));
    CHECK(!row.Bind(&Row::texture, HostBinding{.index = HostBinding::kInvalid, .owner = Owner()}));
    CHECK(row.Owners().empty());
    // Index 0 is valid for this host, and UINT32_MAX would be too.
    CHECK(row.Bind(&Row::texture, HostBinding{.index = 0, .owner = Owner()}));
    CHECK(row.Bind(&Row::sampler, HostBinding{.index = UINT32_MAX, .owner = Owner()}));
    CHECK(row.Plain().sampler.Value() == UINT32_MAX && row.Owners().size() == 2);
    return 0;
}

// Seals by holding the owners in a vector, and counts the calls.
struct CountingSeal {
    int calls = 0;
    std::size_t lastOwners = 0;
    ContentOwners::Seal Fn() {
        return [this](std::vector<std::shared_ptr<const void>> owners) -> std::shared_ptr<const void> {
            ++calls;
            lastOwners = owners.size();
            return std::make_shared<std::vector<std::shared_ptr<const void>>>(std::move(owners));
        };
    }
};

int TestContentOwners() {
    ContentOwners content;
    CountingSeal seal;
    const int target = 0, other = 0;
    // Nothing set: no snapshot, no seal.
    CHECK(!content.Snapshot(seal.Fn()) && seal.calls == 0);
    // Plain bytes into an empty set change nothing.
    content.Set(&target, 0, 16, {});
    CHECK(!content.Snapshot(seal.Fn()) && seal.calls == 0);

    const std::vector a{Owner(), Owner()};
    content.Set(&target, 0, 16, a);
    const auto first = content.Snapshot(seal.Fn());
    CHECK(first && seal.calls == 1 && seal.lastOwners == 2);
    // No change: the same snapshot, no new seal.
    CHECK(content.Snapshot(seal.Fn()) == first && seal.calls == 1);
    // The same range rewritten with the same owners is no change.
    content.Set(&target, 0, 16, a);
    CHECK(content.Snapshot(seal.Fn()) == first && seal.calls == 1);

    // Another target's range does not overlap.
    const std::vector b{Owner()};
    content.Set(&other, 0, 16, b);
    CHECK(content.Snapshot(seal.Fn()) != first && seal.calls == 2 && seal.lastOwners == 3);

    // Plain bytes over part of a's range replace it. Its owners stay in the next snapshot only.
    content.Set(&target, 8, 4, {});
    CHECK(content.Snapshot(seal.Fn()) && seal.calls == 3 && seal.lastOwners == 3);
    content.Set(&target, 32, 4, b); // a change, so the next snapshot is rebuilt without a's owners
    CHECK(content.Snapshot(seal.Fn()) && seal.calls == 4 && seal.lastOwners == 2);

    // A range overlapping the one before it, from an earlier offset.
    const std::vector c{Owner()};
    content.Set(&target, 30, 4, c);
    CHECK(content.Snapshot(seal.Fn()) && seal.calls == 5 && seal.lastOwners == 3); // b, c, and the replaced b

    const auto counts = content.TakeCounts();
    CHECK(counts.ranges == 2 && counts.owners == 2 && counts.snapshotsMade == 5);
    CHECK(content.TakeCounts().snapshotsMade == 0);
    return 0;
}

int TestSealMayReturnNull() {
    ContentOwners content;
    content.Set(nullptr, 0, 4, std::vector{Owner()});
    CHECK(!content.Snapshot([](std::vector<std::shared_ptr<const void>>) { return std::shared_ptr<const void>(); }));
    // The change was taken: the null snapshot stays until the next change.
    int calls = 0;
    CHECK(!content.Snapshot([&](std::vector<std::shared_ptr<const void>>) {
        ++calls;
        return std::shared_ptr<const void>(std::make_shared<int>(0));
    }));
    CHECK(calls == 0);
    return 0;
}

} // namespace

int main() {
    if (const int line = TestOwnedRow())
        return line;
    if (const int line = TestHostBinding())
        return line;
    if (const int line = TestContentOwners())
        return line;
    if (const int line = TestSealMayReturnNull())
        return line;
    std::puts("ORGUploadsTests passed");
    return 0;
}
