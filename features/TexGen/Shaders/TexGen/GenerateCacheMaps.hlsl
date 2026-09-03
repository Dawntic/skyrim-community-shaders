#include "Common/Math.hlsli"

cbuffer CacheGenBuffer : register(b0)
{
	float2 OutputTexSize;
	float2 Padding;
	float4 GridBounds;   // world xy min/max of the cells the height map covers
	float4 SweepDir;     // xy: world direction, z: minor per major slope, w: major step
	float4 SweepParams;  // x: first line offset, y: line count, z: transpose, w: world units per step
	float4 SweepRect;    // xy: tile origin in atlas texels, z: tile size
};

//// Bent Normal and Cardinal AO Map ////////////////////////////////////////////////////
#ifdef CSHADER

Texture2D<float> HeightTex : register(t0);
SamplerState LinearSampler : register(s0);

#	ifdef CARDINALS
RWTexture2D<float4> OutputHorizon0 : register(u0);
RWTexture2D<float4> OutputHorizon1 : register(u1);
RWTexture2D<float4> OutputHorizon2 : register(u2);
RWTexture2D<float4> OutputHorizon3 : register(u3);
#	else
RWTexture2D<float4> OutputBentNormal : register(u0);
#	endif

static const float2 CARD[4] = {
	float2(1, 0), float2(0, 1),   // +X - East, +Y - South
	float2(-1, 0), float2(0, -1)  // -X - West, -Y - North
};
static const float2 DIAG[4] = { float2(0.70710678, 0.70710678), float2(-0.70710678, 0.70710678), float2(-0.70710678, -0.70710678), float2(0.70710678, -0.70710678) };

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

// March one ray; return sin(elevation) of the highest occluder (>= 0 = flat).
#	ifdef CARDINALS
float MarchHorizon(float2 CoordsUV, float SampleHeight, uint2 HeightMapPxSize, float2 Dir, out float MeanHitDist)
#	else
float MarchHorizon(float2 CoordsUV, float SampleHeight, uint2 HeightMapPxSize, float2 Dir)
#	endif
{
	float2 InvPxSize = 1.0 / HeightMapPxSize;
	float MaxHeight = 0.0;
	float WeightedDist = 0.0;

	float StepCount = TexelsToEdge(CoordsUV, Dir, HeightMapPxSize);

	float2 TexelWorldSize = (GridBounds.zw - GridBounds.xy) / (float2)HeightMapPxSize;

	float StepDist = length(Dir * TexelWorldSize);

	float PosOffset = StepDist;
	[loop] for (int step = 1; step < StepCount; ++step)
	{
		float2 Offset = Dir * step * InvPxSize;

		float2 SampCoords = CoordsUV + Offset;
		SampCoords.y = 1.0 - SampCoords.y;
		float Height = HeightTex.SampleLevel(LinearSampler, SampCoords, 0);

		float HDiff = Height - SampleHeight;
		float Hypot = sqrt(PosOffset * PosOffset + HDiff * HDiff);

		float SinElevation = HDiff / Hypot;
#	ifdef CARDINALS
		if (SinElevation > MaxHeight) {
			WeightedDist += (SinElevation - MaxHeight) * PosOffset;
			MaxHeight = SinElevation;
		}
#	else
		MaxHeight = max(SinElevation, MaxHeight);
#	endif

		PosOffset += StepDist;
	}

#	ifdef CARDINALS
	MeanHitDist = MaxHeight > 1e-4 ? WeightedDist / MaxHeight : 0.0;
#	endif

	return max(MaxHeight, 0.0);
}

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	if (any(ThreadID.xy >= (uint2)OutputTexSize))
		return;

	// Map the output texel onto the height map.
	float2 CoordsUV = (ThreadID.xy + 0.5) / OutputTexSize;

	float2 SampCoords = CoordsUV;
	SampCoords.y = 1.0 - SampCoords.y;
	float HeightSample = HeightTex.SampleLevel(LinearSampler, SampCoords, 0);

	uint2 HeightMapPxSize;
	HeightTex.GetDimensions(HeightMapPxSize.x, HeightMapPxSize.y);

#	ifdef CARDINALS
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
#	else
	static const uint NUM_AZIMUTH = 64;  // more than this doesn't improve quality

	const float AzStep = 2.0 * Math::PI / NUM_AZIMUTH;
	const float RadiualWeight = 2.0 * sin(0.5 * AzStep);  // exact ∫ cos/sin over the wedge
	float3 DirAccum = 0.0;                                // unnormalized ∫_visible ω dω
	float VisAccum = 0.0;
	[loop] for (uint step = 0; step < NUM_AZIMUTH; ++step)
	{
		float phi = (step + 0.5) * AzStep;
		float2 Dir;
		sincos(phi, Dir.x, Dir.y);

		float SinH = MarchHorizon(CoordsUV, HeightSample, HeightMapPxSize, Dir.yx);

		float SinH2 = SinH * SinH;
		float CosH = sqrt(max(0.0, 1.0 - SinH2));
		float AngleRad = asin(saturate(SinH));

		float Zenith = AzStep * 0.5 * (1.0 - SinH2);                                           // z of ∫ω dω
		float Radial = RadiualWeight * ((Math::PI / 4) - 0.5 * AngleRad - 0.5 * SinH * CosH);  // radial of (π/4 - θ/2 - sc/2)

		VisAccum += AzStep * (1.0 - SinH);  // ∫sinθ dθ over the wedge, no cosine
		DirAccum += float3(Radial * Dir.yx, Zenith);
	}

	float AO = saturate(VisAccum * rcp(2.0 * Math::PI));
	float3 BentNormal = normalize(DirAccum);

	OutputBentNormal[ThreadID.xy] = float4(BentNormal * 0.5 + 0.5, AO);
#	endif
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

	float2 CoordsUV = (ThreadID.xy + 0.5) / OutputTexSize;
	CoordsUV.y = 1.0 - CoordsUV.y;

	uint2 HeightMapPxSize;
	HeightTex.GetDimensions(HeightMapPxSize.x, HeightMapPxSize.y);

	int2 CoordsPx = CoordsUV * HeightMapPxSize;

	// Sobel gradient (8-tap) for a smoother result
	float hL = LoadHeight(CoordsPx + int2(-1, 0), HeightMapPxSize);
	float hR = LoadHeight(CoordsPx + int2(1, 0), HeightMapPxSize);
	float hD = LoadHeight(CoordsPx + int2(0, -1), HeightMapPxSize);
	float hU = LoadHeight(CoordsPx + int2(0, 1), HeightMapPxSize);
	float hDL = LoadHeight(CoordsPx + int2(-1, -1), HeightMapPxSize);
	float hDR = LoadHeight(CoordsPx + int2(1, -1), HeightMapPxSize);
	float hUL = LoadHeight(CoordsPx + int2(-1, 1), HeightMapPxSize);
	float hUR = LoadHeight(CoordsPx + int2(1, 1), HeightMapPxSize);

	float dHdx = (hR + hUR + hDR) - (hL + hUL + hDL);
	float dHdy = (hU + hUL + hUR) - (hD + hDL + hDR);

	float2 TexelWorldSize = (GridBounds.zw - GridBounds.xy) / (float2)HeightMapPxSize;

	dHdx /= (6.0 * TexelWorldSize.x);
	dHdy /= (6.0 * TexelWorldSize.y);

	float3 N = normalize(float3(-dHdx, -dHdy, 1.0));

	OutputNormal[ThreadID.xy] = float4(N * 0.5 + 0.5, 1.0);
}
#endif
