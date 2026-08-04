#ifndef __TERRAIN_SHADOWS_TRAVERSAL_DEPENDENCY_HLSL__
#define __TERRAIN_SHADOWS_TRAVERSAL_DEPENDENCY_HLSL__

#include "Common/SharedData.hlsli"
#include "TerrainShadows/TerrainShadows.hlsli"

// Hierarchical traversal of the terrain shadow height field.
//
// TerrainShadows::GetTerrainShadow reduces to a height comparison -- a point is shadowed
// when its world Z falls below the penumbra band stored at its world XY -- so a camera ray
// through the height field can be resolved by a HiZ-style descent over a conservative
// min/max mip chain instead of a fixed-step march. What comes back is the exact set of
// ray parameters at which the ray transitions between light and shadow, from which a mean
// visibility along the ray can be evaluated in closed form (see ComputeVBar) rather than
// estimated by sampling.
//
// The traversal walks the ray in level-0 texel space so the DDA is a plain grid walk,
// ascending a level after every conclusive test and descending on ambiguity. Never
// ascending would degrade it into a fixed-step march at level 0, which is the thing this
// is replacing.
//
// Cost is a variable iteration count per pixel, so wave divergence dominates: neighbouring
// pixels with different terrain relationships diverge and the wave pays the worst case.
// Grazing rays along the height field are that worst case. TraversalMaxIterations caps it;
// on overflow the result reports Incomplete and the tail of the ray keeps whatever
// lit/shadowed state the last conclusive test established, which degrades gracefully
// instead of returning garbage.

#define TERRAIN_SHADOW_MAX_CROSSINGS 4

namespace TerrainShadows
{
	// Conservative min/max mip chain over ShadowHeightTexture, built by MinMaxMip.cs.hlsl.
	//   .x = lowest  "fully shadowed below this" height over the footprint
	//   .y = highest "fully lit above this"      height over the footprint
	// Both in the same normalised [0,1] Z encoding as ShadowHeightTexture.
	Texture2D<float2> ShadowMinMaxMipTexture : register(t61);

	static const uint MaxRayCrossings = TERRAIN_SHADOW_MAX_CROSSINGS;

	// Bisections used to pin down a crossing inside a leaf texel. The height field is
	// bilinear there, not constant, so this refines against the filtered value rather than
	// solving the nearest-sampled linear intersection.
	static const uint LeafBisectionSteps = 6;

	struct RayResult
	{
		float Crossings[TERRAIN_SHADOW_MAX_CROSSINGS];  // ray parameters, in world units from the origin, of each light <-> shadow transition
		uint NumCrossings;
		uint Iterations;        // traversal steps taken; this is where the divergence cost shows up
		bool StartsLit;         // whether the ray leaves its origin in light
		bool CrossingOverflow;  // more transitions than the buffer holds; the tail is approximated
		bool Incomplete;        // hit the iteration cap before reaching tMax
	};

	// The ray reduced to the spaces the height field is defined in.
	struct RaySetup
	{
		float2 UVOrigin;  // shadow map UV at t = 0
		float2 UVDir;     // d(UV)/dt
		float ZOrigin;    // normalised height at t = 0
		float ZDir;       // d(normalised height)/dt
	};

	float GetTerrainZExtent()
	{
		return max(SharedData::terraOccSettings.ZRange.y - SharedData::terraOccSettings.ZRange.x, 1e-4);
	}

	// Inverse of GetTerrainZ: world Z -> the normalised encoding the shadow textures use.
	float GetNormalizedTerrainZ(float worldZ)
	{
		return (worldZ + 256.0 - SharedData::terraOccSettings.ZRange.x) / GetTerrainZExtent();
	}

	RaySetup SetupTerrainRay(float3 originWS, float3 dirWS)
	{
		RaySetup ray;
		ray.UVOrigin = GetTerrainShadowUV(originWS.xy);
		ray.UVDir = dirWS.xy * SharedData::terraOccSettings.Scale.xy;
		ray.ZOrigin = GetNormalizedTerrainZ(originWS.z);
		ray.ZDir = dirWS.z / GetTerrainZExtent();
		return ray;
	}

	// Signed distance of the ray to the shadow boundary at parameter t, positive when lit.
	// Thresholds the soft penumbra band at its midpoint: the segment model behind ComputeVBar
	// is binary, so the band has to collapse to a single crossing somewhere. Using the
	// midpoint keeps the transition centred on the same place the filtered lookup puts it.
	float EvalSignedHeight(RaySetup ray, float t, SamplerState samp)
	{
		float2 band = ShadowHeightTexture.SampleLevel(samp, ray.UVOrigin + ray.UVDir * t, 0);
		return (ray.ZOrigin + ray.ZDir * t) - 0.5 * (band.x + band.y);
	}

	// Clips [tEnter, tLeave] to the span the ray spends inside the mapped [0,1]^2 region.
	// Anything outside the height map has no terrain data and is treated as lit.
	// Returns false when the ray never enters it.
	bool ClipToShadowMap(RaySetup ray, inout float tEnter, inout float tLeave)
	{
		[unroll] for (uint axis = 0; axis < 2; ++axis)
		{
			float o = ray.UVOrigin[axis];
			float d = ray.UVDir[axis];
			if (abs(d) < 1e-12) {
				if (o < 0.0 || o > 1.0)
					return false;
			} else {
				float rcpD = rcp(d);
				float ta = (0.0 - o) * rcpD;
				float tb = (1.0 - o) * rcpD;
				tEnter = max(tEnter, min(ta, tb));
				tLeave = min(tLeave, max(ta, tb));
			}
		}
		return tLeave > tEnter;
	}

	float2 LoadMinMax(int2 cell, int level, uint2 dim)
	{
		int2 levelDim = max(int2(dim) >> level, int2(1, 1));
		return ShadowMinMaxMipTexture.Load(int3(clamp(cell, int2(0, 0), levelDim - 1), level));
	}

	// Bisect [tA, tB] for the point where the lit state flips. The caller guarantees the
	// interval brackets a sign change of EvalSignedHeight.
	float BisectCrossing(RaySetup ray, float tA, float tB, bool litAtA, SamplerState samp)
	{
		[unroll] for (uint i = 0; i < LeafBisectionSteps; ++i)
		{
			float tM = 0.5 * (tA + tB);
			if ((EvalSignedHeight(ray, tM, samp) > 0.0) == litAtA)
				tA = tM;
			else
				tB = tM;
		}
		return 0.5 * (tA + tB);
	}

	/**
	 * @brief Resolves where a world-space ray enters and leaves terrain shadow.
	 *
	 * @param originWS  Absolute world-space ray origin (camera-relative positions need
	 *                  CameraPosAdjust added first, as elsewhere in this feature).
	 * @param dirWS     Normalised world-space ray direction.
	 * @param tMax      Far limit of the ray in world units.
	 * @param samp      Linear sampler used for leaf refinement against the filtered height field.
	 * @return          Transition parameters plus the traversal statistics needed to validate them.
	 */
	RayResult TraceTerrainShadowRay(float3 originWS, float3 dirWS, float tMax, SamplerState samp)
	{
		RayResult result;
		[unroll] for (uint c = 0; c < MaxRayCrossings; ++c)
			result.Crossings[c] = 0.0;
		result.NumCrossings = 0;
		result.Iterations = 0;
		result.StartsLit = true;
		result.CrossingOverflow = false;
		result.Incomplete = false;

		if (!SharedData::terraOccSettings.EnableTerrainShadow || tMax <= 0.0)
			return result;

		// Zero levels means the chain was never built; fall back to reporting a lit ray
		// rather than sampling an unbound resource.
		uint mipLevels = SharedData::terraOccSettings.ShadowMipLevels;
		if (mipLevels == 0)
			return result;

		uint2 dim;
		uint textureLevels;
		ShadowMinMaxMipTexture.GetDimensions(0, dim.x, dim.y, textureLevels);
		int maxLevel = int(min(mipLevels, textureLevels)) - 1;
		if (maxLevel < 0 || any(dim == 0))
			return result;

		RaySetup ray = SetupTerrainRay(originWS, dirWS);

		float tEnter = 0.0;
		float tLeave = tMax;
		if (!ClipToShadowMap(ray, tEnter, tLeave))
			return result;  // never touches the height field, so fully lit
		tEnter = max(tEnter, 0.0);

		float2 pOrigin = ray.UVOrigin * float2(dim);
		float2 pDir = ray.UVDir * float2(dim);

		// Nudge past every footprint boundary, otherwise the DDA stalls on a texel edge -- the
		// classic HiZ infinite-loop bug. Sized as a fraction of a texel and scaled with the map
		// dimension so it stays comfortably above a float32 ULP of the texel coordinate itself,
		// which is where a nudge that is merely "small" silently stops making progress.
		// Near-vertical rays make this large, but they exit on the first iteration anyway.
		float tEpsilon = float(max(dim.x, dim.y)) * 1e-5 * rcp(max(length(pDir), 1e-9));

		bool lit = EvalSignedHeight(ray, tEnter, samp) > 0.0;

		// Everything before the mapped region is unoccluded, so a ray that enters already in
		// shadow starts lit and crosses at the boundary.
		result.StartsLit = (tEnter > 0.0) || lit;
		if (result.StartsLit != lit) {
			result.Crossings[0] = tEnter;
			result.NumCrossings = 1;
		}

		float t = tEnter;
		int level = clamp(int(SharedData::terraOccSettings.TraversalStartLevel), 0, maxLevel);
		uint maxIterations = max(SharedData::terraOccSettings.TraversalMaxIterations, 1u);
		uint iteration = 0;

		[loop] while (t < tLeave && iteration < maxIterations)
		{
			++iteration;

			float cellSize = float(1u << uint(level));
			float2 p = pOrigin + pDir * t;
			int2 cell = int2(floor(p / cellSize));

			// Parameter at which the ray leaves this footprint in XY. An axis the ray does not
			// move along never bounds the segment.
			float2 boundary = (float2(cell) + step(0.0, pDir)) * cellSize;
			float2 tAxis;
			tAxis.x = abs(pDir.x) > 1e-12 ? (boundary.x - pOrigin.x) / pDir.x : 1e30;
			tAxis.y = abs(pDir.y) > 1e-12 ? (boundary.y - pOrigin.y) / pDir.y : 1e30;
			float tExit = min(min(tAxis.x, tAxis.y), tLeave);

			// Z is linear in t, so the ray's height range over the segment is bounded by its
			// endpoints. That is what makes the min/max test valid.
			float zA = ray.ZOrigin + ray.ZDir * t;
			float zB = ray.ZOrigin + ray.ZDir * tExit;
			float zLo = min(zA, zB);
			float zHi = max(zA, zB);

			float2 mm = LoadMinMax(cell, level, dim);

			bool newLit;
			float tCross = t;

			if (zLo >= mm.y) {
				newLit = true;  // stays above every occluder in the footprint
			} else if (zHi <= mm.x) {
				newLit = false;  // stays below every occluder in the footprint
			} else if (level > 0) {
				--level;  // ambiguous, so descend without advancing
				continue;
			} else {
				// Leaf: the segment genuinely straddles the boundary. Take the state at the far
				// end and, if it differs from where we came in, refine the crossing.
				float fB = EvalSignedHeight(ray, tExit, samp);
				newLit = fB > 0.0;
				if (newLit != lit) {
					bool litAtStart = EvalSignedHeight(ray, t, samp) > 0.0;
					if (litAtStart != newLit)
						tCross = BisectCrossing(ray, t, tExit, litAtStart, samp);
					// Otherwise the flip happened exactly at the segment start, where tCross already is.
				}
			}

			// A conclusive test that disagrees with the tracked state means the transition sits
			// on the footprint boundary; recording it at t keeps the segment list consistent.
			if (newLit != lit) {
				if (result.NumCrossings < MaxRayCrossings) {
					result.Crossings[result.NumCrossings] = tCross;
					result.NumCrossings += 1;
				} else {
					// Keep traversing so tLeave behaviour stays right; the tail parity is
					// approximated. NumCrossings is exposed so the debug view can tell you
					// empirically whether the budget is ever the limiting factor.
					result.CrossingOverflow = true;
				}
				lit = newLit;
			}

			t = tExit + tEpsilon;
			level = min(level + 1, maxLevel);
		}

		result.Iterations = iteration;
		result.Incomplete = t < tLeave;

		// Past the mapped region there is no height field, so the ray is lit again.
		if (!lit && tLeave < tMax) {
			if (result.NumCrossings < MaxRayCrossings) {
				result.Crossings[result.NumCrossings] = tLeave;
				result.NumCrossings += 1;
			} else {
				result.CrossingOverflow = true;
			}
		}

		return result;
	}

	/**
	 * @brief Fraction of the in-scattering weight accumulated by parameter t, normalised to
	 *        1 at tMax.
	 *
	 * Truncated-exponential weighting, matching what a march through a homogeneous medium of
	 * mean extinction `sigma` would have integrated, so results are directly comparable to
	 * the fixed-step reference. Degenerates to uniform weighting in the optically thin limit,
	 * where the exponential form loses all its precision to cancellation.
	 */
	float ScatterCDF(float t, float sigma, float tMax)
	{
		float opticalDepth = sigma * tMax;
		if (opticalDepth < 1e-3)
			return saturate(t / max(tMax, 1e-6));
		return saturate((1.0 - exp(-sigma * t)) / (1.0 - exp(-opticalDepth)));
	}

	/**
	 * @brief Mean visibility along the ray, weighted by where the medium actually scatters.
	 *
	 * Closed form over the lit segments the traversal found -- no sampling, so a thin ridge
	 * shadow is as sharp as the height field allows. A single crossing at t* collapses to the
	 * expected (1 - exp(-sigma * t*)) / (1 - exp(-sigma * tMax)).
	 *
	 * `sigma` is the mean extinction of the medium; pass 0 for an unweighted mean. Swapping
	 * ScatterCDF for a normalised (1 - exp(-tau(t))) built on an analytic optical depth is a
	 * drop-in upgrade for height fog -- the segment logic below does not change.
	 */
	float ComputeVBar(RayResult result, float tMax, float sigma)
	{
		float vbar = 0.0;
		float segmentStart = 0.0;
		bool lit = result.StartsLit;

		[unroll] for (uint i = 0; i < MaxRayCrossings; ++i)
		{
			bool active = i < result.NumCrossings;
			float tCross = active ? min(result.Crossings[i], tMax) : segmentStart;
			if (active && lit)
				vbar += ScatterCDF(tCross, sigma, tMax) - ScatterCDF(segmentStart, sigma, tMax);
			segmentStart = tCross;
			lit = active ? !lit : lit;
		}

		if (lit)
			vbar += ScatterCDF(tMax, sigma, tMax) - ScatterCDF(segmentStart, sigma, tMax);

		return saturate(vbar);
	}

	/**
	 * @brief Convenience wrapper: trace and reduce to a mean visibility in one call.
	 */
	float GetTerrainShadowAlongRay(float3 originWS, float3 dirWS, float tMax, float sigma, SamplerState samp)
	{
		RayResult result = TraceTerrainShadowRay(originWS, dirWS, tMax, samp);
		return ComputeVBar(result, tMax, sigma);
	}
}

#endif  // __TERRAIN_SHADOWS_TRAVERSAL_DEPENDENCY_HLSL__
