#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace org::services {

// The scatter step of a counting sort (SortScatter.hlsl), after a prefix sum (Passes/PrefixSumPass.h). The host owns the pass,
// since what it sorts and when is its own, and records it through its ComputeKernel: kSortScatterConstantWords constants,
// one group of kSortScatterGroupSize threads per kSortScatterGroupSize items.
// The shader, relative to the host's shader folder, as for kGenerateMipsShader.
inline constexpr const char* kSortScatterShader = "ORGModuleServices/SortScatter.hlsl";
inline constexpr uint32_t kSortScatterGroupSize = 64;

// The shader's push constants. The indices are the views' descriptor heap indices.
struct SortScatterConstants {
    uint32_t itemsIndex;    // ByteAddressBuffer: the staged items, stride bytes each
    uint32_t ranksIndex;    // ByteAddressBuffer: uint per item, its key << rankBits | its rank among its key's items
    uint32_t offsetsIndex;  // ByteAddressBuffer: uint per key, its first slot (the prefix sum's offsets)
    uint32_t countIndex;    // ByteAddressBuffer whose first word is the staged item count
    uint32_t outputIndex;   // RWByteAddressBuffer: the sorted items
    uint32_t stride;        // bytes per item, a multiple of 4
    uint32_t limit;         // at most this many items are sorted
    uint32_t padding;
};
inline constexpr uint32_t kSortScatterConstantWords = sizeof(SortScatterConstants) / 4;

// What the kernel is built for. Each build is its own permutation.
struct SortScatterLayout {
    // The rank's bits in a rank word; the key is above them.
    uint32_t rankBits = 20;
    // Output ranges. Off: an item goes to offsets[key] + rank. On: an item belongs to a range whose keys are contiguous. The
    // item's word at rangeSlotWord (a byte offset) holds the range's first output slot and its word at rangeKeyWord the
    // range's first key. The item goes to that slot plus offsets[key] - offsets[first key] plus its rank, and the output
    // gets rangeSlotValue and rangeKeyValue in those two words.
    bool ranges = false;
    uint32_t rangeSlotWord = 0, rangeKeyWord = 0;
    uint32_t rangeSlotValue = 0, rangeKeyValue = 0;
};

// The defines that build the kernel for a layout.
inline std::vector<std::pair<std::wstring, std::wstring>> SortScatterDefines(const SortScatterLayout& layout) {
    std::vector<std::pair<std::wstring, std::wstring>> defines{{L"ORG_SORT_SCATTER_RANK_BITS", std::to_wstring(layout.rankBits)}};
    if (layout.ranges) {
        defines.emplace_back(L"ORG_SORT_SCATTER_RANGE_SLOT_WORD", std::to_wstring(layout.rangeSlotWord));
        defines.emplace_back(L"ORG_SORT_SCATTER_RANGE_KEY_WORD", std::to_wstring(layout.rangeKeyWord));
        defines.emplace_back(L"ORG_SORT_SCATTER_RANGE_SLOT_VALUE", std::to_wstring(layout.rangeSlotValue));
        defines.emplace_back(L"ORG_SORT_SCATTER_RANGE_KEY_VALUE", std::to_wstring(layout.rangeKeyValue));
    }
    return defines;
}

}
