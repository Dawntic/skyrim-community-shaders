#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

cbuffer CacheGenBuffer : register(b0)
{
	float2 OutputTexSize;
	float2 HeightMapOffsetScale;
	float4 RegionOffsetScale;  // xy: uv offset, zw: uv scale of the height map region to process
	float4 GridBounds;         // world xy min/max of the cells the height map covers
	float4 SweepDir;           // xy: world direction, z: minor per major slope, w: major step
	float4 SweepParams;        // x: first line offset, y: line count, z: transpose, w: world units per step
	float4 SweepRect;          // xy: tile origin in atlas texels, z: tile size
	float4 SmoothParams;       // xy: filter axis in texels, z: radius, w: spatial sigma in texels
	float4 SmoothRange;        // x: range sigma, y: preserve threshold, zw: height clamp, all game units
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
	float2 Cos;
	Cos.x = abs(Dir.x) > 1e-6 ? (target.x - p.x) / Dir.x : 1e30;
	Cos.y = abs(Dir.y) > 1e-6 ? (target.y - p.y) / Dir.y : 1e30;
	return min(Cos.x, Cos.y);
}

// March one ray; return sin(elevation) of the highest occluder (>= 0 = flat).
#	ifdef CARDINALS
float MarchHorizon(float2 CoordsUV, uint3 ThreadID, float SampleHeight, uint2 HeightMapPxSize, float2 Dir, out float MeanHitDist)
#	else
float MarchHorizon(float2 CoordsUV, uint3 ThreadID, float SampleHeight, uint2 HeightMapPxSize, float2 Dir)
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
		float Height = HeightTex.SampleLevel(LinearSampler, SampCoords, 0);  // * 65535;
																			 //Height = (Height - HeightMapOffsetScale.x) * HeightMapOffsetScale.y;

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
	if (any(ThreadID.xy >= OutputTexSize))
		return;

	// Map the output texel onto the height map. The full map is RegionOffsetScale = (0, 0, 1, 1);
	// a tiled run passes the sub rect it covers, so rays still march across the whole map and
	// occlusion from outside the tile is accounted for.
	float2 CoordsUV = (ThreadID.xy + 0.5) / OutputTexSize;
	CoordsUV = RegionOffsetScale.xy + CoordsUV * RegionOffsetScale.zw;

	float2 SampCoords = CoordsUV;
	SampCoords.y = 1.0 - SampCoords.y;
	float HeightSample = HeightTex.SampleLevel(LinearSampler, SampCoords, 0);  // * 65535;
																			   //HeightSample = (HeightSample - HeightMapOffsetScale.x) * HeightMapOffsetScale.y;

	uint2 HeightMapPxSize;
	HeightTex.GetDimensions(HeightMapPxSize.x, HeightMapPxSize.y);

#	ifdef CARDINALS
	float4 card, diag;
	float4 cardWeight, diagWeight;
	[unroll] for (int step = 0; step < 8; step++)
	{
		if (step < 4) {
			card[step] = MarchHorizon(CoordsUV, ThreadID, HeightSample, HeightMapPxSize, CARD[step], cardWeight[step]);
		} else {
			int idx = step - 4;
			diag[idx] = MarchHorizon(CoordsUV, ThreadID, HeightSample, HeightMapPxSize, DIAG[idx], diagWeight[idx]);
		}
	}
	OutputHorizon0[ThreadID.xy] = card;
	OutputHorizon1[ThreadID.xy] = diag;
	OutputHorizon2[ThreadID.xy] = cardWeight;
	OutputHorizon3[ThreadID.xy] = diagWeight;
#	else  // Bent Normals - outputted to 1024^2 map
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

		float SinH = MarchHorizon(CoordsUV, ThreadID, HeightSample, HeightMapPxSize, Dir.yx);

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
		const float H = (HeightTex[Px] - HeightMapOffsetScale.x) * HeightMapOffsetScale.y;

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
	float h = HeightTex[CoordsPx];                       // * 65535;
	return h;                                            //(h - HeightMapOffsetScale.x) * HeightMapOffsetScale.y;
}

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	if (any(ThreadID.xy >= OutputTexSize))
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
/////////////////////////////////////////////////////////////////////////////////////////

//// Height Smoothing ///////////////////////////////////////////////////////////////////
// Flattens the fine relief out of the height atlas without moving the terrain it belongs
// to: the 8 unit quantisation steps of the 16 bit export, LOD stair stepping and small
// bumps all go, while cliffs and mountain fronts stay where they are.
//
// The kernel is a bilateral one. Its spatial term is a plain Gaussian over the radius; its
// range term weights a neighbour by how far its height is from the centre, so a neighbour
// within SmoothRange.x game units averages in and one across a cliff does not. Small relief
// sits well inside that window and is averaged flat; large relief sits outside it and
// survives with its edge intact.
//
// Run separably, one dispatch per axis, ping ponging between two targets. A separable
// bilateral is an approximation - the true kernel is not the product of two 1D kernels -
// but two 2R tap passes cost a fraction of one R*R tap pass, and iterating cheap passes
// flattens more than one wide pass does. Short dispatches also keep a full atlas bake well
// clear of the driver timeout.
//
// Heights are decoded to game units on load, so the range sigma, the preserve threshold and
// the clamp are all in game units whether the atlas on disk is 16 bit or float. Intermediate
// targets are already decoded and are dispatched with an identity decode.

#ifdef HEIGHT_SMOOTH
Texture2D<float> HeightTex : register(t0);
RWTexture2D<float> OutputHeight : register(u0);

// Decoded height in game units, clamped to the range a worldspace can hold. The clamp is what keeps
// one bad texel from dragging its whole neighbourhood with it: a value far outside the range would
// otherwise carry a huge range distance into every tap that reads it. clamp is min/max, which per
// the D3D spec return the other operand for a NaN, so a NaN lands on the floor rather than spreading.
float LoadHeightUnits(int2 CoordsPx, int2 HeightMapPxSize)
{
	CoordsPx = clamp(CoordsPx, 0, HeightMapPxSize - 1);  // the atlas does not wrap, so edges repeat
	float Height = (HeightTex[CoordsPx] - HeightMapOffsetScale.x) * HeightMapOffsetScale.y;
	return clamp(Height, SmoothRange.z, SmoothRange.w);
}

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	if (any(ThreadID.xy >= OutputTexSize))
		return;

	uint2 HeightMapPxSize;
	HeightTex.GetDimensions(HeightMapPxSize.x, HeightMapPxSize.y);

	const int2 MapPxSize = (int2)HeightMapPxSize;
	const int2 CoordsPx = (int2)ThreadID.xy;
	const int2 Axis = (int2)SmoothParams.xy;  // (1, 0) or (0, 1)
	const int Radius = (int)SmoothParams.z;

	const float Centre = LoadHeightUnits(CoordsPx, MapPxSize);

	// Both terms are exp(-d^2 / 2 sigma^2), so the two exponents add into one exp per tap.
	const float SpatialFalloff = -0.5 / max(SmoothParams.w * SmoothParams.w, 1e-6);
	const float RangeFalloff = -0.5 / max(SmoothRange.x * SmoothRange.x, 1e-6);

	// The centre tap weighs 1: it is at no spatial and no range distance from itself.
	float Sum = Centre;
	float WeightSum = 1.0;

	[loop] for (int Tap = 1; Tap <= Radius; ++Tap)
	{
		const float Spatial = SpatialFalloff * (float)(Tap * Tap);

		[unroll] for (int Side = 0; Side < 2; ++Side)
		{
			const float Height = LoadHeightUnits(CoordsPx + Axis * (Side == 0 ? Tap : -Tap), MapPxSize);
			const float Delta = Height - Centre;
			const float Weight = exp(Spatial + RangeFalloff * Delta * Delta);

			Sum += Height * Weight;
			WeightSum += Weight;
		}
	}

	OutputHeight[ThreadID.xy] = Sum / WeightSum;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////

//// Height Smoothing Resolve ///////////////////////////////////////////////////////////
// Final pass of the smoothing chain: hands back the relief that was never meant to go, then
// clamps to the worldspace height range so what lands on disk is always a value the map can
// legitimately hold.

#ifdef HEIGHT_SMOOTH_RESOLVE
Texture2D<float> HeightTex : register(t0);    // source atlas, still in its on disk encoding
Texture2D<float> SmoothedTex : register(t1);  // filtered result, already in game units
RWTexture2D<float> OutputHeight : register(u0);

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	if (any(ThreadID.xy >= OutputTexSize))
		return;

	const int2 CoordsPx = (int2)ThreadID.xy;

	const float Source = clamp((HeightTex[CoordsPx] - HeightMapOffsetScale.x) * HeightMapOffsetScale.y, SmoothRange.z, SmoothRange.w);
	const float Base = SmoothedTex[CoordsPx];

	// Coring on the residual. Relief the filter took out returns only once it is tall enough to
	// be terrain rather than detail: below the threshold it is dropped outright, above twice it
	// the source height is restored exactly, so a ridge the range term still rounded off is not
	// left half flattened. A zero threshold leaves the smoothed result untouched.
	float Height = Base;
	const float Threshold = SmoothRange.y;
	if (Threshold > 0.0) {
		const float Residual = Source - Base;
		Height += Residual * smoothstep(Threshold, 2.0 * Threshold, abs(Residual));
	}

	OutputHeight[ThreadID.xy] = clamp(Height, SmoothRange.z, SmoothRange.w);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////

//// Normal Step ////////////////////////////////////////////////////////////////////////
#ifdef NORMAL_STEP

// texels from `origin` to the texture edge along cardinal `dir` (before going OOB)
int TexelsToEdge(int2 origin, float2 dir)
{
	if (dir.x > 0)
		return OutputTexSize.x - 1 - origin.x;  // toward +X
	if (dir.x < 0)
		return origin.x;  // toward -X
	if (dir.y > 0)
		return OutputTexSize.y - 1 - origin.y;  // toward +Y
	return origin.y;                            // toward -Y
}

static const float2 CARD[4] = {
	float2(1, 0), float2(0, 1),   // +X - East, +Y - South
	float2(-1, 0), float2(0, -1)  // -X - West, -Y - North
};

//from view to horizon

// what is the closest point that subtends an approx solid angle(of occlusion) > x
// not closest just biggest solid angle,

// What solid angle does this geographical feature subtend
// feature is defined by

Texture2D CardinalAO : register(t0);
Texture2D HeightTex : register(t1);
RWTexture2D<float4> OutputTex : register(u0);  // raw texel counts (see note at bottom)

float GetReflectingSlope(int2 ThreadID, int DirIDX)
{
	float2 Dir = CARD[DirIDX];
	uint StepsToEdge = TexelsToEdge(ThreadID, Dir);
	float OriginH = HeightTex[ThreadID].x * 65535;
	OriginH = (OriginH - 32767) * 8.0;

	float SlopeMax = 0.0;  // origin sightline, eye-level start
	float MaxArea = 0.0;
	float OutputA = 0;

	[loop] for (int step = 5; step <= StepsToEdge; ++step)
	{
		int halfWedge = max(3, (int)(step * tan(radians(45))));
		int2 perp = int2(Dir.y, -Dir.x);
		int2 center = ThreadID + Dir * step;

		// advance the origin's horizon using the CENTER px of this band //
		int2 cClamp = clamp(center, 0, OutputTexSize - 1);
		float centerH = HeightTex[cClamp].x * 65535;
		centerH = (centerH - 32767) * 8.0;
		float centerSlope = (centerH - OriginH) / (float)step;  // tan(elev) of center from origin
		SlopeMax = max(SlopeMax, centerSlope);                  // running horizon along the ray

		// sum visible vertical extent across the perpendicular row //
		float rowArea = 0.0;
		for (int i = -halfWedge; i <= halfWedge; ++i) {
			int2 SamplePosR = center + perp * i;
			if (any(SamplePosR < 0) || any(SamplePosR >= OutputTexSize))
				continue;

			float bestVisible = 0.0;
			for (int j = -1; j <= 1; j++) {  // sample a pixel either side incase incline is curved, take the strongest
				int2 SamplePosRS = clamp(SamplePosR + Dir * j, 0, OutputTexSize - 1);
				float ph = HeightTex[SamplePosRS].x * 65535;
				ph = (ph - 32767) * 8.0;
				float dist = max((float)(step + j), 1.0);     // radial distance of THIS sample
				float pSlope = (ph - OriginH) / dist;         // its elevation angle from origin
				float visible = max(0.0, pSlope - SlopeMax);  // height above the running sightline
				bestVisible = max(bestVisible, visible);      // strongest in the j window
			}
			rowArea += bestVisible;  // one contribution per lateral slot
		}

		if (rowArea > MaxArea) {
			MaxArea = rowArea;
			OutputA = step;
		}
	}

	return OutputA;
}

/*
    float3 Normal = LoadNormal(ThreadID.xy);
    float4 dist;
    dist.x = MarchToSlope(ThreadID.xy, float2( 1,  0), Normal);   // +X - East
    dist.y = MarchToSlope(ThreadID.xy, float2(-1,  0), Normal);   // -X - West
    dist.z = MarchToSlope(ThreadID.xy, float2( 0,  1), Normal);   // +Y - South
    dist.w = MarchToSlope(ThreadID.xy, float2( 0, -1), Normal);   // -Y - North
	OutputTex[ThreadID.xy] = dist;                        // raw texel counts

	float4 Output;
	Output[0] = GetReflectingSlope(ThreadID.xy, 0); // East
	Output[1] = GetReflectingSlope(ThreadID.xy, 1);
	Output[2] = GetReflectingSlope(ThreadID.xy, 2);
	Output[3] = GetReflectingSlope(ThreadID.xy, 3);

	OutputTex[ThreadID.xy] = Output;
*/

#	define FOV 90

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	float2 TexSize = OutputTexSize;
	if (ThreadID.x >= TexSize.x || ThreadID.y >= TexSize.y)
		return;

	float4 Output;

	for (int k = 0; k < 4; k++) {
		int2 Origin = int2(ThreadID.xy);
		float2 Dir = CARD[k];
		uint StepsToEdge = TexelsToEdge(Origin, Dir);

		float OriginH = HeightTex[Origin].x * 65535;
		OriginH = (OriginH - 32767) * 8.0;

		float SlopeMax = 0.0;
		float MaxArea = 0.0;
		float OutputA = 0;

		int2 perp = int2(Dir.y, -Dir.x);

		[loop] for (int step = 5; step <= StepsToEdge; ++step)
		{
			int halfWedge = max(3, (step * tan(radians(45))));
			int2 center = Origin + Dir * step;

			// advance origin horizon on the band center
			int2 cClamp = clamp(center, 0, OutputTexSize - 1);
			float centerH = HeightTex[cClamp].x * 65535;
			centerH = (centerH - 32767) * 8.0;
			SlopeMax = max(SlopeMax, (centerH - OriginH) / (float)step);

			// sum visible vertical extent across the perpendicular row
			float rowArea = 0.0;
			for (int i = -halfWedge; i <= halfWedge; ++i) {
				int2 SamplePosR = center + perp * i;
				if (any(SamplePosR < 0) || any(SamplePosR >= OutputTexSize))
					continue;

				float bestVisible = 0.0;
				for (int j = -1; j <= 1; ++j) {  // radial tolerance for curves
					int2 p = clamp(SamplePosR + Dir * j, 0, OutputTexSize - 1);
					float ph = HeightTex[p].x * 65535;
					ph = (ph - 32767) * 8.0;
					float dist = max((float)(step + j), 1.0);
					float visible = max(0.0, (ph - OriginH) / dist - SlopeMax);
					bestVisible = max(bestVisible, visible);
				}
				rowArea += bestVisible;
			}

			if (rowArea > MaxArea) {
				MaxArea = rowArea;
				OutputA = step;
			}
		}
		Output[k] = OutputA;
	}

	OutputTex[ThreadID.xy] = Output;
}
#endif

// for a given pixel calc wedge in cardinal dir
// walk each radial band
// for each pixel in band walk from band to origin and test vis
// band with the most wins

// Figure out best way to sample ground lighting
// Fix albedo and normal texture shape/size

// each pixel stores the
// PlayerVisibilityCS.hlsl
// Top-down visibility mask. White = player can see this texel, black = can't.
// Gates: (1) inside the FOV cone, (2) surface faces the player, (3) terrain
// doesn't occlude the eye->target line. World is Z-up: UV.xy -> world X/Y, height -> Z.

/*
#define SIN_LIM 0.1
// need to handle when we start on ridge cap
float GetReflectingSlope(float2 ThreadID, int DirIDX)
{
	float2 Dir = CARD[DirIDX];
	uint StepsToEdge = TexelsToEdge(ThreadID, Dir);
	float OriginH = HeightTex[ThreadID].x * 65535;
		  OriginH = (OriginH - 32767) * 8.0;

	//float SlopeMax = -1e9;
	float SlopeMax = 0.0;   // eye-level horizon; only terrain above you counts
	float MaxBandSinH = 0.0;                                     // first band measures from flat
	float MaxOmega = 0.0;                                        // accumulated occlusion

	float OutputA = 0;

    [loop] for (int step = 5; step <= StepsToEdge; ++step) {
        int2 SamplePos = clamp(ThreadID + Dir * step, 0, OutputTexSize-1);

		float SampleH = HeightTex[SamplePos].x * 65535;        // this texel's height
			  SampleH = (SampleH - 32767) * 8.0;
		float SlopeLOS = (SampleH - OriginH) / step;    // tan(elev) from origin
		bool Visible = SlopeLOS >= SlopeMax;      // not hidden by a nearer rise
		SlopeMax = max(SlopeMax, SlopeLOS);                    // advance origin horizon

		float SinH = CardinalAO[SamplePos][DirIDX];

		if(SinH >= SIN_LIM && Visible){
			int slope = 0;
			int valid = 0;
			float sinSum = 0;
			int occWidth = 0;                                  // contiguous slope width through center
			bool runOpen = true;                               // still connected to center
			int halfWedge = max(3, (int)(step * tan(radians(45))));
			for (int i = -halfWedge; i <= halfWedge; ++i){
				int2 SamplePosR = SamplePos + int2(Dir.y, -Dir.x) * i;
				if (any(SamplePosR < 0) || any(SamplePosR >= OutputTexSize))
					continue;

				++valid;

				bool hit = false;
				float bestS = 0;
				for(int j = -3; j<=3; j++){ // sample a pixel either side incase incline is curved, don't include extra in RNeeded
				 	int2 SamplePosRS = clamp(SamplePosR + Dir * j, 0, OutputTexSize-1);
				 	float SinHRS = CardinalAO[SamplePosRS][DirIDX];
				 	if(SinHRS >= SIN_LIM){
						hit = true;
						bestS = max(bestS, SinHRS);
					}
				}
				if (hit){
					++slope;
					sinSum += bestS;
				}
			}

			// contiguous occluder width through the band center (option A) //
			for (int k = 0; k <= halfWedge; ++k){              // walk +side from center
				int2 p = clamp(SamplePos + int2(Dir.y, -Dir.x) * k, 0, OutputTexSize-1);
				if (CardinalAO[p][DirIDX] < SIN_LIM) break;
				++occWidth;
			}
			for (int k = 1; k <= halfWedge; ++k){              // walk -side from center
				int2 p = clamp(SamplePos - int2(Dir.y, -Dir.x) * k, 0, OutputTexSize-1);
				if (CardinalAO[p][DirIDX] < SIN_LIM) break;
				++occWidth;
			}

			float dPhi = 2.0 * atan((occWidth * 0.5) / max(step, 1.0));  // true subtended azimuth
			float sinAvg = slope > 0 ? sinSum / slope : 0.0;      // mean sin(elev) of blockers
			float dTheta =  asin(sinAvg);                         // band thickness (absolute)
			float dOmega = sinAvg * dTheta * dPhi;                // solid angle this band subtends
			if(dOmega > MaxOmega){
				MaxOmega = dOmega;
				OutputA = step;
			}
		}
	}

	return OutputA;
}
*/

// South(Down) should = 0,-1,0 //correct
// East should = 1,0,0 //correct
// North should = 0,1,0 //correct
// West should = -1,0,0 //correct

/*
[numthreads(8, 8, 1)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;

	if (ThreadID.x >= settings.GridTexSize.x || ThreadID.y >= settings.GridTexSize.y)
		return;

	float2 CoordsUV = (ThreadID.xy + 0.5) * settings.InvGridTexSize.xy;

	float2 PlayerUV = LinearStep(settings.GridBounds.xy, settings.GridBounds.zw, FrameBuffer::CameraPosAdjust.xy);
	       PlayerUV.y = 1.0 - PlayerUV.y;

	float3 PlayerWorldDir = normalize(FrameBuffer::CameraViewInverse._m02_m12_m22); //normalize(mul(FrameBuffer::CameraViewInverse, float4(0, 0, 1, 0)).xyz);  // Correct!

	float2 PlayerUVDir = normalize(PlayerWorldDir.xy / (settings.GridBounds.zw - settings.GridBounds.xy));
	       PlayerUVDir.y = -PlayerUVDir.y;

	float WorldHeight = HeightTex.SampleLevel(LinearSampler, CoordsUV, 0) * 65535;
		  WorldHeight = (WorldHeight - 32767) * 8.0;

	float PlayerWorldHeight = FrameBuffer::CameraPosAdjust.z;//HeightTex.SampleLevel(LinearSampler, PlayerUV, 0) * 65535; // Close enough
		  //PlayerWorldHeight = (PlayerWorldHeight - 32767) * 8.0;

    float2 toT = CoordsUV - PlayerUV;
    float dist = length(toT);

    if (dist < 1e-6) {
        ProbeArray[ThreadID] = 1.0.xxxx;
    	return;
    }

    float2 dir = toT / dist;

	float2 nCoord = CoordsUV * float2(1024, 801);
	float4 NormalWS = float4(NormalTex[nCoord].xzy * 2.0 - 1.0, 1);

	// Outside view frustum
	float CosHalfFov = cos(radians(FOV * 0.5));
    if (dot(dir, PlayerUVDir) < CosHalfFov) {
		ProbeArray[ThreadID] = float4(0,0,0,1); //NormalWS;
        return;
    }

	// Inside view frustum
    uint2 HeightMapSize;
    HeightTex.GetDimensions(HeightMapSize.x, HeightMapSize.y);
    float texel = 1.0 / max(HeightMapSize.x, HeightMapSize.y);
    int steps = dist / texel;

	ProbeArray[ThreadID] = NormalWS;

    bool visible = true;
    [loop] for(int i = 1; i < steps; ++i) {
        float t = float(i) / steps;
        float2 sUV = PlayerUV + toT * t;
        float lineH = lerp(PlayerWorldHeight, WorldHeight, t);
		float terrH = HeightTex.SampleLevel(LinearSampler, sUV, 0) * 65535;
		  	  terrH = (terrH - 32767) * 8.0;
        if (terrH > lineH) {
            visible = false;
            break;
        }   // add +bias here if you get acne
    }

	if(visible) // Terrain is visible
		ProbeArray[ThreadID] = 1.0.xxxx;
}
#endif
*/
//if on slope
// walk normal find first normal thats

// if on slope walk across heightmap and find where we start going down again

// return first texel with N with theta3 > x
// if current(input) N is on a slope return number of texel to top of the slope(needs to handle plataus).
// if current(input) N is on downwards slope then find the next up slope
// x given in degrees

/*
			int halfWedge = step * tan(45);
			int slope = 0;
			for (int i = -halfWedge; i <= halfWedge; ++i){
				int2 SamplePos = clamp(ThreadID + int2(Dir.y, -Dir.x) * step, 0, OutputTexSize-1);
				if (CardinalAO[SamplePos][DirIDX] >= SIN_LIM)
					slope++;
			}

			// check point is unoccluded //
			float WorldHeight = HeightTex[SamplePos].x * 65535;
		 		  WorldHeight = (WorldHeight - 32767) * 8.0;
			float Slope = (WorldHeight - OriginH) / step;

			if(Slope < SlopeMax)
				continue;

			SlopeMax = Slope;

			// check angle distribution //
			int RadialTexc = 5;//step * tan(radians(45)); // should be tan 45 * 0.5 if we do 8 card dirs instead of 4
			int RNeeded = 7;//min(10 + (RadialTexc * 2) * 0.1, 100); // if at least x of radial texels are also incline
			int RPassed = 0;
			for(int i = -RadialTexc; i < RadialTexc; i++){
				int2 SamplePosR = clamp(SamplePos + Dir.yx * float2(i, -i), 0, OutputTexSize-1);
				for(int j = -3; j<=3; j++){ // sample a pixel either side incase incline is curved, don't include extra in RNeeded
				 	int2 SamplePosRS = clamp(SamplePosR + Dir * j, 0, OutputTexSize-1);
				 	if(CardinalAO[SamplePosRS][DirIDX] >= SIN_LIM){
						++RPassed;
						break;
					}
				}
			}

			//if(RPassed < RNeeded) // slope not wide enough
			//	continue;


			// find highest point //
			int c = step + 1;
			int2 SamplePosN = ThreadID + Dir * c;
			while(CardinalAO[SamplePosN][DirIDX] > SinH && c < StepsToEdge){ // continue until we hit the highest point
				SamplePosN = clamp(ThreadID + Dir * ++c, 0, OutputTexSize-1);
			}
			return c;
			*/

//Texture2D<float4>   NormalMap : register(t0);   // geometric normals, comparable space
//RWTexture2D<float4> OutDist   : register(u0);   // raw texel counts (see note at bottom)

/*

//#define MaxSteps 1024 * 0.3
//#define PLATEAU_LIMIT 100
#define WALL_SLOPE 24 // degrees inline
#define cosX cos(radians(WALL_SLOPE))

float3 LoadNormal(int2 SamplePos){
	float2 CoordsUV = (SamplePos.xy + 0.5) * rcp(OutputTexSize.xy);
	float2 nCoord = float2(1024, 801); // FIX ME
	float2 nCoordsPx = CoordsUV * nCoord;

	float3 Normal = NormalMap[nCoordsPx].xzy * 2.0 - 1.0;
	Normal.y = -Normal.y;
	return Normal;
}

#define LOOKAHEAD 6   // texels to confirm a transition
#define WIDTH_MIN 12   // wall must span at least this many texels across the march dir

// classify one texel: -1 downhill, 0 flat, +1 uphill
int SlopeState(int2 pos, float2 dir)
{
    float3 n = LoadNormal(pos);
    if (n.z >= cosX)
		return 0;                 // not steep enough -> flat
    return dot(n.xy, dir) < 0.0 ? 1 : -1;     // uphill : downhill
}

bool Confirm(int2 origin, int step, float2 dir, int want, int StepsToPass)
{
    int votes = 0, valid = 0;
    [unroll] for (int k = 0; k < StepsToPass; ++k)
    {
        int2 p = origin + dir * (step + k);
        if (any(p < 0) || any(p >= OutputTexSize))
			break;
        valid++;
        if (SlopeState(p, dir, cosX) == want)
			votes++;
    }
    return valid > 0 && votes * 2 > valid;     // majority
}

bool ConfirmWidth(int2 pos, float2 dir, int want)
{
    int2 perp = int2(dir.y, -dir.x);

    if (SlopeState(pos, dir, cosX) != want)
        return false;                       // center must match, else no run here

    int run = 1;                            // the center texel

    [unroll] for (int k = 1; k < WIDTH_MIN; ++k)   // walk one side until it breaks
    {
        int2 p = pos + perp * k;
        if (any(p < 0) || any(p >= OutputTexSize)) break;
        if (SlopeState(p, dir, cosX) != want) break;   // gap -> stop counting this side
        run++;
    }

    [unroll] for (int k = 1; k < WIDTH_MIN; ++k)   // walk the other side
    {
        int2 p = pos - perp * k;
        if (any(p < 0) || any(p >= OutputTexSize)) break;
        if (SlopeState(p, dir, cosX) != want) break;
        run++;
    }

    return run >= WIDTH_MIN;                 // connected span long enough
}

//count delined steps - if incline steps < decline then continue


// 1: walk to bottom continue on flat
// 2: walk to top, if incline < 10px, goto 1
// 3: walk till incline

// if decline, goto 1
// if incline, goto 2
// if flat, goto 3

// we are on incline only if > 50% of last 10 steps were this dir
//

// if we start flat walk till slope:
// if slope inline then climb slope, if peak > 10 steps return peak
// if slope decline then walk to bottom, continue on flat till slope   - can we track whether current point is visible from origin?

// if we start incline climb to peak, if climb > 10 texels return, else walk to bottom continue on flat

// if we start decline walk to bottom, continue on flat

#define Incline 1
#define Flat 0
#define Decline -1



// Walk cardinalAO map


uint MarchToSlope(int2 origin, float2 dir, float3 Normal)
{

    //bool Last = (Normal.z < cosX) && (dot(Normal.xy, dir) < 0.0); // history
	int2 SamplePos = origin + dir;
	int Last = SlopeState(SamplePos, dir);

	int InclineSteps = 0;
	int DeclineSteps = 0;
	int StepsSinceChange = 0;
    int lastTop = 0;
    //uint seekGap  = 0;
	uint MaxTexels = TexelsToEdge(origin, dir);

    [loop] for (int step = 1; step <= MaxTexels; ++step) {
        SamplePos = origin + dir * step;
        if (any(SamplePos < 0) || any(SamplePos >= OutputTexSize))
            return Last ? lastTop : MaxTexels;

        int CurrSlope = SlopeState(SamplePos, dir); // 1 = uphill, -1 = downhill, 0 = flat

		if(CurrSlope == Flat){
			++step;
			continue;
		}
		else if(CurrSlope == Incline){
			++InclineSteps;
			if(CurrSlope != Last && InclineSteps / 2 >= StepsSinceChange){
				InclineSteps = 0;
				if(Last == Flat){ // start of incline

				} else{ // start of decline
					if(InclineSteps > 5)  // was true hill
						return step - 1;
				}

				//Last = CurrSlope;
			}
		}
		else if(CurrSlope == Decline){
			++DeclineSteps;
			if(CurrSlope != Last){
				DeclineSteps = 0;
				if(
				//InclineSteps = 0;
			}
		}

		if (!Last) {
			if (CurrSlope == 1 && Confirm(origin, step, dir, 1)) //&& ConfirmWidth(SamplePos, dir, cosX, 1))
			{
				Last = true;
				lastTop = step;
				//seekGap = 0;
			}
			//else if (++seekGap > PLATEAU_LIMIT)
			//	return 0;
		}
        else { // if Climbing
            if (CurrSlope == 1) {
				//if(ConfirmWidth(SamplePos, dir, cosX, 1)) // keep the broad part of the slope
					lastTop = step;
			}
			// if not Climbing are we desending
            else if (Confirm(origin, step, dir, -1)) {
				if(lastTop == 0)
					Last = false;
				else
               		return lastTop;
			}
			// if not Climbing or desending are we on flat that wont climb in x texels
            else if (Confirm(origin, step, dir, 0) && !Confirm(origin, step, dir, 1)) {
                return lastTop;
			}
        }
    }
    return Last ? lastTop : MaxTexels;
}

//0.6 - 0.2, 0.83


int2 SamplePosN = ThreadID + Dir * (step + 1);
			float Next = CardinalAO[SamplePosN][DirIDX];
			int c = 0;
			while(Next > SinH){
				Next = CardinalAO[SamplePosN][DirIDX];
				++c;
			}
*/