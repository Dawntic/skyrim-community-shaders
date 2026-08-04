// Builds the conservative min/max mip chain over the terrain shadow height map.
//
// ShadowHeightTexture stores, per XY texel, the penumbra band of the terrain shadow for
// the current sun direction: a point at normalised height z is fully lit above the band
// and fully shadowed below it. This chain stores, per texel and level:
//
//     .x = minimum lower-band height over the footprint  -> a ray below it is fully shadowed
//     .y = maximum upper-band height over the footprint  -> a ray above it is fully lit
//
// both in the same normalised [0,1] Z encoding as the source texture. Two bounds give the
// traversal two early-outs instead of one, and the fully-shadowed case is the common one at
// sunset -- worth skipping fast rather than descending through.
//
// Conservativeness is the correctness requirement. If a parent's band fails to bound every
// descendant's, the traversal skips a footprint it should have entered and light leaks
// through ridges. Two things follow from that:
//
//   * Level 0 reduces the 3x3 neighbourhood rather than a single texel, so the bounds also
//     hold for the bilinearly reconstructed height field the rest of the feature samples.
//     A bilinear tap inside texel (i, j) only ever reads (i +/- 1, j +/- 1).
//   * Level N extends its 2x2 reduction to 3 rows/columns whenever the corresponding source
//     dimension is odd, so the parent still covers the orphaned edge. Extending
//     unconditionally would be safe but loosens every level and compounds up the chain.
//
// The source and the chain share DXGI_FORMAT_R16G16_UNORM, so the level-0 round trip is
// exact and no epsilon padding is needed to keep the bounds valid.

#if defined(BUILD_LEVEL0)

Texture2D<float2> ShadowHeightTexture : register(t0);
RWTexture2D<float2> MinMaxMipRW : register(u0);

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	uint2 dim;
	ShadowHeightTexture.GetDimensions(dim.x, dim.y);
	if (any(tid >= dim))
		return;

	int2 maxCoord = int2(dim) - 1;

	float2 mm = float2(1.0, 0.0);
	[unroll] for (int y = -1; y <= 1; ++y)
	{
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			// The band is stored as (upper, lower); take both ways round so the chain stays
			// correct regardless of which channel happens to be higher at a given texel.
			float2 band = ShadowHeightTexture[clamp(int2(tid) + int2(x, y), int2(0, 0), maxCoord)];
			mm.x = min(mm.x, min(band.x, band.y));
			mm.y = max(mm.y, max(band.x, band.y));
		}
	}

	MinMaxMipRW[tid] = mm;
}

#elif defined(BUILD_LEVEL_N)

Texture2D<float2> MinMaxMipIn : register(t0);  // level L-1
RWTexture2D<float2> MinMaxMipRW : register(u0);

cbuffer MinMaxMipCB : register(b0)
{
	uint2 SrcDim;
	uint2 DstDim;
};

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= DstDim))
		return;

	int2 src = int2(tid) * 2;
	int2 maxCoord = int2(SrcDim) - 1;

	// 2x2, widened to 3 along any axis whose source dimension is odd. `extent` is uniform
	// across the dispatch, so the dynamic bound costs no divergence.
	uint2 extent = uint2(2, 2) + (SrcDim & 1);

	float2 mm = float2(1.0, 0.0);
	[loop] for (uint y = 0; y < extent.y; ++y)
	{
		[loop] for (uint x = 0; x < extent.x; ++x)
		{
			float2 s = MinMaxMipIn[min(src + int2(x, y), maxCoord)];
			mm.x = min(mm.x, s.x);
			mm.y = max(mm.y, s.y);
		}
	}

	MinMaxMipRW[tid] = mm;
}

#else
#	error "MinMaxMip.cs.hlsl requires BUILD_LEVEL0 or BUILD_LEVEL_N"
#endif
