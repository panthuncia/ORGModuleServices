// The scatter step of a counting sort. A pass staged each item with a rank word, its key in the high bits and its rank among
// its key's items below, and a prefix sum (PrefixSum.hlsl) turned the per-key counts into first slots. Each item is copied to
// its key's first slot plus its rank. Every output slot is written exactly once.
//
// Defines (Passes/SortScatter.h writes them):
//   ORG_SORT_SCATTER_RANK_BITS: the rank's bits in a rank word. 20 when not defined.
//   ORG_SORT_SCATTER_RANGE_SLOT_WORD, ORG_SORT_SCATTER_RANGE_KEY_WORD: optional byte offsets into an item. When defined, each
//   item belongs to an output range whose keys are contiguous. The first word holds the range's first output slot and the
//   second its first key, and an item goes to that slot plus its key's place within the range plus its rank. The output
//   gets ORG_SORT_SCATTER_RANGE_SLOT_VALUE and ORG_SORT_SCATTER_RANGE_KEY_VALUE in those words.

// Must match SortScatterConstants in Passes/SortScatter.h.
cbuffer SortScatterConstants : register(b0)
{
	uint ItemsIndex;   // ByteAddressBuffer: the staged items, Stride bytes each
	uint RanksIndex;   // ByteAddressBuffer: uint per item, its key << ORG_SORT_SCATTER_RANK_BITS | its rank
	uint OffsetsIndex; // ByteAddressBuffer: uint per key, its first slot
	uint CountIndex;   // ByteAddressBuffer whose first word is the staged item count
	uint OutputIndex;  // RWByteAddressBuffer: the sorted items
	uint Stride;       // bytes, a multiple of 4
	uint Limit;        // at most this many items are sorted
	uint Padding;
}

#ifndef ORG_SORT_SCATTER_RANK_BITS
#	define ORG_SORT_SCATTER_RANK_BITS 20
#endif

static const uint kCountWord = 0;
static const uint kSortRankBits = ORG_SORT_SCATTER_RANK_BITS;
static const uint kSortRankMask = (1u << kSortRankBits) - 1;

#if defined(ORG_SORT_SCATTER_RANGE_SLOT_WORD) || defined(ORG_SORT_SCATTER_RANGE_KEY_WORD)
#	if !defined(ORG_SORT_SCATTER_RANGE_SLOT_WORD) || !defined(ORG_SORT_SCATTER_RANGE_KEY_WORD) || \
		!defined(ORG_SORT_SCATTER_RANGE_SLOT_VALUE) || !defined(ORG_SORT_SCATTER_RANGE_KEY_VALUE)
#		error "SortScatter.hlsl: a range needs both words and both values"
#	endif
static const uint kRangeSlotOffset = ORG_SORT_SCATTER_RANGE_SLOT_WORD;
static const uint kRangeKeyOffset = ORG_SORT_SCATTER_RANGE_KEY_WORD;
#endif

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	ByteAddressBuffer count = ResourceDescriptorHeap[CountIndex];
	const uint index = dispatchID.x;
	if (index >= min(count.Load(kCountWord), Limit))
		return;
	ByteAddressBuffer items = ResourceDescriptorHeap[ItemsIndex];
	ByteAddressBuffer ranks = ResourceDescriptorHeap[RanksIndex];
	ByteAddressBuffer offsets = ResourceDescriptorHeap[OffsetsIndex];
	RWByteAddressBuffer output = ResourceDescriptorHeap[OutputIndex];

	const uint source = index * Stride;
	const uint4 head = items.Load4(source);
	const uint keyRank = ranks.Load(index * 4);
#if defined(ORG_SORT_SCATTER_RANGE_SLOT_WORD)
	const uint rangeFirst = items.Load(source + kRangeSlotOffset);
	const uint rangeKeys = offsets.Load(items.Load(source + kRangeKeyOffset) * 4);
	const uint target = (rangeFirst + offsets.Load((keyRank >> kSortRankBits) * 4) - rangeKeys + (keyRank & kSortRankMask)) * Stride;
#else
	const uint target = (offsets.Load((keyRank >> kSortRankBits) * 4) + (keyRank & kSortRankMask)) * Stride;
#endif
	output.Store4(target, head);
	[loop] for (uint word = 16; word + 16 <= Stride; word += 16)
		output.Store4(target + word, items.Load4(source + word));
	[loop] for (uint tail = Stride & ~15u; tail < Stride; tail += 4)
		output.Store(target + tail, items.Load(source + tail));
#if defined(ORG_SORT_SCATTER_RANGE_SLOT_WORD)
	output.Store(target + kRangeSlotOffset, (uint)(ORG_SORT_SCATTER_RANGE_SLOT_VALUE));
	output.Store(target + kRangeKeyOffset, (uint)(ORG_SORT_SCATTER_RANGE_KEY_VALUE));
#endif
}
