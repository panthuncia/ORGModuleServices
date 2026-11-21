// Builds a hierarchical depth buffer (HZB) from a depth texture with AMD's FidelityFX single-pass downsampler (ffx_spd.h).
//
// The HZB holds the depth as stored. Each level reduces the four texels below it with the build's reduction: the maximum, or
// the minimum with ORG_HZB_REDUCE_MIN defined to 1. Pick the one that keeps the farthest surface, so a footprint that covers
// some background never culls. SPD writes every mip in one dispatch, which needs a power-of-two domain, so the host makes
// mip 0 half the next power of two of the depth. Padding inside the domain loads as FarValue, which suppresses culling.
// Texels outside the domain or a non-square level load as the reduction's identity for depth in [0, 1]. Stores outside a
// level are dropped. A domain wider than 4096 overflows the last group's 64x64 at mip 5, so the host splits the chain into
// several dispatches (Passes/HzbPass.cpp).

// Must match HzbConstants in Passes/HzbPass.h.
cbuffer HzbConstants : register(b0)
{
	uint SourceIndex;   // FromDepth reads the depth Texture2D, otherwise an HZB level RWTexture2D
	uint FromDepth;
	uint2 ValidSize;    // the source texels holding depth
	uint2 DomainSize;   // the source's extent, of which [ValidSize, DomainSize) is padding
	uint2 TargetSize;   // the first written level's size, level i is max(1, TargetSize >> i)
	uint Mips;          // the levels this dispatch writes
	uint WorkGroups;    // the dispatch's group count, for the last-group test
	uint CounterIndex;  // RWStructuredBuffer<uint>, finished groups, zeroed again by the last
	float FarValue;     // what padding loads as
	uint4 TargetIndices[3];  // RWTexture2D<float> per written level, 12 at most
}

#if defined(ORG_HZB_REDUCE_MIN) && ORG_HZB_REDUCE_MIN
static const float kNoArea = 1.0;
#else
static const float kNoArea = 0.0;
#endif

#define A_GPU
#define A_HLSL
#include "ffx_a.h"

groupshared AU1 spdCounter;
groupshared AF1 spdIntermediate[16][16];

uint TargetIndex(uint a_level)
{
	return TargetIndices[a_level >> 2][a_level & 3];
}

bool InsideLevel(int2 a_position, uint a_level)
{
	const int2 size = int2(max(TargetSize >> a_level, uint2(1, 1)));
	return all(a_position >= 0) && all(a_position < size);
}

AF4 SpdLoadSourceImage(ASU2 p, AU1 slice)
{
	if (any(p < 0) || any(p >= int2(DomainSize)))
		return kNoArea.xxxx;
	if (any(p >= int2(ValidSize)))
		return FarValue.xxxx;
	if (FromDepth != 0) {
		Texture2D<float> depth = ResourceDescriptorHeap[SourceIndex];
		return depth.Load(int3(p, 0)).xxxx;
	}
	RWTexture2D<float> source = ResourceDescriptorHeap[SourceIndex];
	return source[p].xxxx;
}

// The last group's reads of level 5, written by every other group.
AF4 SpdLoad(ASU2 p, AU1 slice)
{
	if (!InsideLevel(p, 5))
		return kNoArea.xxxx;
	globallycoherent RWTexture2D<float> level = ResourceDescriptorHeap[TargetIndex(5)];
	return level[p].xxxx;
}

void SpdStore(ASU2 p, AF4 value, AU1 mip, AU1 slice)
{
	if (!InsideLevel(p, mip))
		return;
	if (mip == 5) {
		globallycoherent RWTexture2D<float> level = ResourceDescriptorHeap[TargetIndex(5)];
		level[p] = value.x;
		return;
	}
	RWTexture2D<float> level = ResourceDescriptorHeap[NonUniformResourceIndex(TargetIndex(mip))];
	level[p] = value.x;
}

AF4 SpdReduce4(AF4 v0, AF4 v1, AF4 v2, AF4 v3)
{
#if defined(ORG_HZB_REDUCE_MIN) && ORG_HZB_REDUCE_MIN
	return min(min(v0.x, v1.x), min(v2.x, v3.x)).xxxx;
#else
	return max(max(v0.x, v1.x), max(v2.x, v3.x)).xxxx;
#endif
}

void SpdIncreaseAtomicCounter(AU1 slice)
{
	globallycoherent RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[CounterIndex];
	// This thread's level 5 store must be visible before the group arrives, and every arrival before the last group reads.
	DeviceMemoryBarrier();
	InterlockedAdd(counter[0], 1, spdCounter);
	DeviceMemoryBarrier();
}

AU1 SpdGetAtomicCounter()
{
	return spdCounter;
}

void SpdResetAtomicCounter(AU1 slice)
{
	globallycoherent RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[CounterIndex];
	counter[0] = 0;
}

AF4 SpdLoadIntermediate(AU1 x, AU1 y)
{
	return spdIntermediate[x][y].xxxx;
}

void SpdStoreIntermediate(AU1 x, AU1 y, AF4 value)
{
	spdIntermediate[x][y] = value.x;
}

#include "ffx_spd.h"

[numthreads(256, 1, 1)] void main(uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	SpdDownsample(AU2(groupID.xy), AU1(localIndex), AU1(Mips), AU1(WorkGroups), AU1(0));
}
