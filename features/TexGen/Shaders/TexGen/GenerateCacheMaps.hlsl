#include "Common/Math.hlsli"

cbuffer CacheGenBuffer : register(b0)
{
	float2 OutputTexSize;
	float2 Padding;
	float4 GridBounds;   // world xy min/max of the cells the height map covers
	float4 SweepDir;     // xy: world direction, z: minor per major slope, w: major step
	float4 SweepParams;  // x: first line offset, y: line count, z: transpose, w: world units per step
	float4 SweepRect;    // xy: tile origin in atlas texels, z: tile size
	float4 SmoothParams;
	float4 SmoothRange;
};

//// Cardinal AO Map ///////////////////////////////////////////////////////////////////
#ifdef CARDINALS

Texture2D<float> HeightTex : register(t0);
SamplerState LinearSampler : register(s0);

RWTexture2D<float4> OutputHorizon0 : register(u0);
RWTexture2D<float4> OutputHorizon1 : register(u1);
RWTexture2D<float4> OutputHorizon2 : register(u2);
RWTexture2D<float4> OutputHorizon3 : register(u3);

static const float2 CARD[4] = {
	float2(1, 0), float2(0, -1),  // +X - East, +Y - North
	float2(-1, 0), float2(0, 1)   // -X - West, -Y - South
};
static const float2 DIAG[4] = { float2(0.70710678, -0.70710678), float2(-0.70710678, -0.70710678), float2(-0.70710678, 0.70710678), float2(0.70710678, 0.70710678) };

float TexelsToEdge(float2 uv, float2 Dir, float2 Dim)
{
	float2 p = uv * Dim;
	float2 target = float2(Dir.x > 0.0 ? Dim.x : 0.0, Dir.y > 0.0 ? Dim.y : 0.0);
	float2 Cos = 1e30;
	if (abs(Dir.x) > 1e-6)
		Cos.x = (target.x - p.x) / Dir.x;
	if (abs(Dir.y) > 1e-6)
		Cos.y = (target.y - p.y) / Dir.y;

	return min(Cos.x, Cos.y);
}

float MarchHorizon(float2 CoordsUV, float SampleHeight, uint2 HeightMapPxSize, float2 Dir, out float VisibleDist)
{
	float2 InvPxSize = 1.0 / HeightMapPxSize;
	float MaxHeight = 0.0;
	float Horizon = -1.0;

	float StepCount = TexelsToEdge(CoordsUV, Dir, HeightMapPxSize);
	float2 TexelWorldSize = (GridBounds.zw - GridBounds.xy) / (float2)HeightMapPxSize;
	float StepDist = length(Dir * TexelWorldSize);

	float PosOffset = StepDist;
	VisibleDist = 0.0;
	[loop] for (int step = 1; step < StepCount; ++step)
	{
		float2 Offset = Dir * step * InvPxSize;

		float2 SampCoords = CoordsUV + Offset;
		float Height = HeightTex.SampleLevel(LinearSampler, SampCoords, 0);

		float HDiff = Height - SampleHeight;
		float Hypot = sqrt(PosOffset * PosOffset + HDiff * HDiff);
		float SinElevation = HDiff / Hypot;

		MaxHeight = max(MaxHeight, SinElevation);

		if (SinElevation >= Horizon - 1e-4) {
			Horizon = max(Horizon, SinElevation);
			VisibleDist = PosOffset;
		}

		PosOffset += StepDist;
	}

	return max(MaxHeight, 0.0);
}

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	if (any(ThreadID.xy >= (uint2)OutputTexSize))
		return;

	float2 CoordsUV = (ThreadID.xy + 0.5) / OutputTexSize;

	float HeightSample = HeightTex.SampleLevel(LinearSampler, CoordsUV, 0);

	uint2 HeightMapPxSize;
	HeightTex.GetDimensions(HeightMapPxSize.x, HeightMapPxSize.y);

	float4 card, diag;
	float4 cardWeight, diagWeight;
	[unroll] for (int step = 0; step < 8; step++)
	{
		if (step < 4) {
			card[step] = MarchHorizon(CoordsUV, HeightSample, HeightMapPxSize, CARD[step], cardWeight[step]);
		} else {
			int idx = step - 4;
			diag[idx] = MarchHorizon(CoordsUV, HeightSample, HeightMapPxSize, DIAG[idx], diagWeight[idx]);
		}
	}
	OutputHorizon0[ThreadID.xy] = card;
	OutputHorizon1[ThreadID.xy] = diag;
	OutputHorizon2[ThreadID.xy] = cardWeight;
	OutputHorizon3[ThreadID.xy] = diagWeight;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////

//// Bent Normal Line Sweep /////////////////////////////////////////////////////////////
// Same horizon as the per texel march above, computed once per line instead of once per
// texel. Walking a line from its far end inward while keeping an upper convex hull of the
// points already passed gives every texel on that line its horizon in O(1) amortised: the
// steepest slope from a point to anything ahead of it is always a tangent to that hull, and
// a point that loses to its neighbour can never win for anything further back either.
//
// Maximising sin(elevation) and maximising slope pick the same point, since sin is strictly
// increasing in slope, so the hull result matches the march exactly for the same samples.
//
// The lines run along the dominant axis of the azimuth so each output texel is touched once
// and only once. One dispatch per azimuth accumulates into the target; SWEEP_FINALIZE then
// resolves the accumulated integral into the bent normal and AO.
#ifdef SWEEP

Texture2D<float> HeightTex : register(t0);
RWTexture2D<float4> OutputAccum : register(u0);
RWStructuredBuffer<float2> HullStack : register(u1);  // (t, height) pairs, HULL_CAPACITY per line

#	define HULL_CAPACITY 1024
#	define NUM_AZIMUTH 64

[numthreads(64, 1, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	const uint LineIndex = ThreadID.x;
	if (LineIndex >= (uint)SweepParams.y)
		return;

	const float2 WorldDir = SweepDir.xy;
	const float Slope = SweepDir.z;  // minor axis texels per major axis texel, |Slope| <= 1
	const int MajorStep = (int)SweepDir.w;
	const bool Transpose = SweepParams.z > 0.5;
	const float StepWorldDist = SweepParams.w;
	const int LineOffset = (int)SweepParams.x + (int)LineIndex;

	uint2 AtlasSize;
	HeightTex.GetDimensions(AtlasSize.x, AtlasSize.y);

	// Everything below works in (major, minor) axis order so one code path covers both halves.
	const int2 Dim = Transpose ? int2(AtlasSize.y, AtlasSize.x) : int2(AtlasSize.x, AtlasSize.y);
	const int TileSize = (int)SweepRect.z;
	const int2 TileMin = Transpose ? (int2)SweepRect.yx : (int2)SweepRect.xy;

	// Walk backwards along the ray: start at the atlas edge the rays point towards and end at
	// the near edge of the tile, the last texel that still needs a horizon.
	const int MajorStart = MajorStep > 0 ? Dim.x - 1 : 0;
	const int MajorEnd = MajorStep > 0 ? TileMin.x : TileMin.x + TileSize - 1;
	const int StepCount = abs(MajorStart - MajorEnd) + 1;

	const uint StackBase = LineIndex * HULL_CAPACITY;
	int StackSize = 0;

	const float AzStep = 2.0 * Math::PI / NUM_AZIMUTH;
	const float RadialWeight = 2.0 * sin(0.5 * AzStep);  // exact ∫ cos/sin over the wedge

	[loop] for (int i = 0; i < StepCount; ++i)
	{
		const int Major = MajorStart - i * MajorStep;
		const int Minor = LineOffset + (int)round(Slope * Major);
		if (Minor < 0 || Minor >= Dim.y)
			continue;  // this line is off the map here, no sample and no hull point

		const int2 Px = Transpose ? int2(Minor, Major) : int2(Major, Minor);
		const float t = (float)(Major * MajorStep);  // grows along the ray direction
		const float H = HeightTex[Px];

		// Peel back the hull until its top is the tangent point seen from here.
		while (StackSize >= 2) {
			const float2 HullNear = HullStack[StackBase + StackSize - 1];
			const float2 HullFar = HullStack[StackBase + StackSize - 2];
			// slope(H, HullNear) <= slope(H, HullFar), cross multiplied; both gaps are positive
			if ((HullNear.y - H) * (HullFar.x - t) <= (HullFar.y - H) * (HullNear.x - t))
				StackSize--;
			else
				break;
		}

		float SinH = 0.0;
		if (StackSize >= 1) {
			const float2 Tangent = HullStack[StackBase + StackSize - 1];
			const float s = (Tangent.y - H) / ((Tangent.x - t) * StepWorldDist);
			SinH = s > 0.0 ? s * rsqrt(1.0 + s * s) : 0.0;  // sin(atan(s)), flat when nothing rises
		}

		const int2 Rel = int2(Major, Minor) - TileMin;
		if (all(Rel >= 0) && all(Rel < TileSize)) {
			const float SinH2 = SinH * SinH;
			const float CosH = sqrt(max(0.0, 1.0 - SinH2));
			const float AngleRad = asin(saturate(SinH));

			const float Zenith = AzStep * 0.5 * (1.0 - SinH2);                                          // z of ∫ω dω
			const float Radial = RadialWeight * ((Math::PI / 4) - 0.5 * AngleRad - 0.5 * SinH * CosH);  // radial of (π/4 - θ/2 - sc/2)
			const float Vis = AzStep * (1.0 - SinH);                                                    // ∫sinθ dθ over the wedge, no cosine

			// Atlas rows run north to south, the tiles run south to north.
			const int2 RelPx = Transpose ? Rel.yx : Rel;
			const int2 OutPx = int2(RelPx.x, TileSize - 1 - RelPx.y);

			OutputAccum[OutPx] += float4(Radial * WorldDir, Zenith, Vis);
		}

		// Capacity is far beyond the hull size real terrain produces; dropping the newest
		// candidate is only a safety net against a pathologically convex line.
		if (StackSize < HULL_CAPACITY) {
			HullStack[StackBase + StackSize] = float2(t, H);
			StackSize++;
		}
	}
}
#endif

#ifdef SWEEP_FINALIZE
RWTexture2D<float4> OutputAccum : register(u0);

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	if (any(ThreadID.xy >= (uint2)OutputTexSize))
		return;

	const float4 Accum = OutputAccum[ThreadID.xy];  // xyz: ∫_visible ω dω, w: ∫sinθ dθ

	const float AO = saturate(Accum.w * rcp(2.0 * Math::PI));
	const float LenSq = dot(Accum.xyz, Accum.xyz);
	const float3 BentNormal = LenSq > 1e-12 ? Accum.xyz * rsqrt(LenSq) : float3(0, 0, 1);

	OutputAccum[ThreadID.xy] = float4(BentNormal * 0.5 + 0.5, AO);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////

#ifdef HEIGHT_SMOOTH
Texture2D<float> HeightTex : register(t0);
RWTexture2D<float> OutputHeight : register(u0);

float LoadHeightUnits(int2 CoordsPx, int2 HeightMapPxSize)
{
	CoordsPx = clamp(CoordsPx, 0, HeightMapPxSize - 1);
	return clamp(HeightTex[CoordsPx], SmoothRange.z, SmoothRange.w);
}

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	if (any(ThreadID.xy >= (uint2)OutputTexSize))
		return;

	uint2 HeightMapPxSize;
	HeightTex.GetDimensions(HeightMapPxSize.x, HeightMapPxSize.y);

	const int2 MapPxSize = (int2)HeightMapPxSize;
	const int2 CoordsPx = (int2)ThreadID.xy;
	const int2 Axis = (int2)SmoothParams.xy;
	const int Radius = (int)SmoothParams.z;

	const float Centre = LoadHeightUnits(CoordsPx, MapPxSize);

	const float Sigma = max(SmoothParams.z * 0.5, 0.5);
	const float SpatialFalloff = -0.5 / (Sigma * Sigma);

	const float FlattenHeight = SmoothRange.x;
	const float RolloffHeight = max(SmoothRange.x * SmoothRange.y, SmoothRange.x + 1e-3);

	float Sum = Centre;
	float WeightSum = 1.0;

	[loop] for (int Tap = 1; Tap <= Radius; ++Tap)
	{
		const float Spatial = exp(SpatialFalloff * (float)(Tap * Tap));

		[unroll] for (int Side = 0; Side < 2; ++Side)
		{
			const float Height = LoadHeightUnits(CoordsPx + Axis * (Side == 0 ? Tap : -Tap), MapPxSize);
			const float Delta = abs(Height - Centre);
			const float Weight = Spatial * (1.0 - smoothstep(FlattenHeight, RolloffHeight, Delta));

			Sum += Height * Weight;
			WeightSum += Weight;
		}
	}

	OutputHeight[ThreadID.xy] = Sum / WeightSum;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////

//// Normal Map /////////////////////////////////////////////////////////////////////////

#ifdef NORMALS
Texture2D<float> HeightTex : register(t0);
RWTexture2D<float4> OutputNormal : register(u0);

float LoadHeight(int2 CoordsPx, int2 HeightMapPxSize)
{
	CoordsPx = clamp(CoordsPx, 0, HeightMapPxSize - 1);  // clamp at edges
	return HeightTex[CoordsPx];
}

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	if (any(ThreadID.xy >= (uint2)OutputTexSize))
		return;

	uint2 HeightMapPxSize;
	HeightTex.GetDimensions(HeightMapPxSize.x, HeightMapPxSize.y);

	int2 CoordsPx = ThreadID.xy;

	// Sobel gradient (8-tap) for a smoother result
	float hL = LoadHeight(CoordsPx + int2(-1, 0), HeightMapPxSize);
	float hR = LoadHeight(CoordsPx + int2(1, 0), HeightMapPxSize);
	float hN = LoadHeight(CoordsPx + int2(0, -1), HeightMapPxSize);
	float hS = LoadHeight(CoordsPx + int2(0, 1), HeightMapPxSize);
	float hNW = LoadHeight(CoordsPx + int2(-1, -1), HeightMapPxSize);
	float hNE = LoadHeight(CoordsPx + int2(1, -1), HeightMapPxSize);
	float hSW = LoadHeight(CoordsPx + int2(-1, 1), HeightMapPxSize);
	float hSE = LoadHeight(CoordsPx + int2(1, 1), HeightMapPxSize);

	float dHdx = (hR + hNE + hSE) - (hL + hNW + hSW);
	float dHdy = (hN + hNW + hNE) - (hS + hSW + hSE);

	float2 TexelWorldSize = (GridBounds.zw - GridBounds.xy) / (float2)HeightMapPxSize;

	dHdx /= (6.0 * TexelWorldSize.x);
	dHdy /= (6.0 * TexelWorldSize.y);

	float3 N = normalize(float3(-dHdx, -dHdy, 1.0));

	OutputNormal[ThreadID.xy] = float4(N * 0.5 + 0.5, 1.0);
}
#endif

//// Static Geometry Raster /////////////////////////////////////////////////////////////
// Places the meshes a cell's references carry on top of the terrain tile. The terrain is
// already in the target when this runs, and geometry may only ever raise a texel, which is
// what the Havok raycast this replaces produced: the topmost terrain, ground or static hit.

#ifdef STATIC_RASTER

struct RasterTriangle
{
	float3 v0;
	float3 v1;
	float3 v2;
};

struct RasterInstance
{
	float4 rotationScale0;  // xyz: first row of rotation times scale, w: translation x
	float4 rotationScale1;
	float4 rotationScale2;
	uint firstTriangle;
	uint triangleCount;
	uint2 pad;
};

StructuredBuffer<RasterTriangle> Triangles : register(t0);
StructuredBuffer<RasterInstance> Instances : register(t1);

// Heights are held as a sortable unsigned so InterlockedMax can run on them; a raster with no
// fixed submission order has no other way to keep the topmost surface.
RWTexture2D<uint> OutputHeight : register(u0);

// SweepRect.xy: tile origin in world units, .z: world units per texel, .w: tile size in texels.

float3 TransformPoint(RasterInstance Instance, float3 P)
{
	return float3(
		dot(Instance.rotationScale0.xyz, P) + Instance.rotationScale0.w,
		dot(Instance.rotationScale1.xyz, P) + Instance.rotationScale1.w,
		dot(Instance.rotationScale2.xyz, P) + Instance.rotationScale2.w);
}

// IEEE floats do not order correctly when reinterpreted as unsigned, because the sign bit reads
// as the largest magnitude. Flipping every bit of a negative and only the sign bit of a positive
// makes the unsigned comparison agree with the float one, which is what lets InterlockedMax work
// on terrain heights that go below zero.
uint HeightToSortable(float H)
{
	uint U = asuint(H);
	return (U & 0x80000000u) ? ~U : (U | 0x80000000u);
}

void RasteriseTriangle(float3 P0, float3 P1, float3 P2)
{
	float2 Origin = SweepRect.xy;
	float UnitsPerTexel = SweepRect.z;
	float TileSize = SweepRect.w;

	// Into texel space. Row zero is the southern edge, matching how the terrain fill lays the
	// tile out and how the tiles later stitch.
	float2 T0 = (P0.xy - Origin) / UnitsPerTexel;
	float2 T1 = (P1.xy - Origin) / UnitsPerTexel;
	float2 T2 = (P2.xy - Origin) / UnitsPerTexel;

	float2 MinT = min(T0, min(T1, T2));
	float2 MaxT = max(T0, max(T1, T2));

	int2 MinPx = (int2)max(floor(MinT), 0.0);
	int2 MaxPx = (int2)min(ceil(MaxT), TileSize - 1.0);
	if (any(MinPx > MaxPx))
		return;  // entirely outside this tile

	// Edge functions in texel space. A degenerate triangle covers nothing.
	float Area = (T1.x - T0.x) * (T2.y - T0.y) - (T2.x - T0.x) * (T1.y - T0.y);
	if (abs(Area) < 1e-6)
		return;
	float InvArea = 1.0 / Area;

	for (int y = MinPx.y; y <= MaxPx.y; ++y) {
		for (int x = MinPx.x; x <= MaxPx.x; ++x) {
			float2 P = float2(x, y) + 0.5;

			float W0 = ((T1.x - P.x) * (T2.y - P.y) - (T2.x - P.x) * (T1.y - P.y)) * InvArea;
			float W1 = ((T2.x - P.x) * (T0.y - P.y) - (T0.x - P.x) * (T2.y - P.y)) * InvArea;
			float W2 = 1.0 - W0 - W1;

			if (W0 < 0.0 || W1 < 0.0 || W2 < 0.0)
				continue;

			float Height = W0 * P0.z + W1 * P1.z + W2 * P2.z;

			uint Previous;
			InterlockedMax(OutputHeight[int2(x, y)], HeightToSortable(Height), Previous);
		}
	}
}

// One group per instance, its threads sharing that instance's triangles. Dispatching this way
// needs no per triangle work list, which for a tile's worth of placements would be larger than
// the geometry it indexes.
[numthreads(64, 1, 1)] void main(uint3 GroupID : SV_GroupID, uint3 ThreadID : SV_GroupThreadID) {
	RasterInstance Instance = Instances[GroupID.x];

	for (uint t = ThreadID.x; t < Instance.triangleCount; t += 64) {
		RasterTriangle Tri = Triangles[Instance.firstTriangle + t];

		float3 P0 = TransformPoint(Instance, Tri.v0);
		float3 P1 = TransformPoint(Instance, Tri.v1);
		float3 P2 = TransformPoint(Instance, Tri.v2);

		RasteriseTriangle(P0, P1, P2);
	}
}
#endif
