// Validation view for the hierarchical terrain shadow traversal.
//
// Runs the traversal for the primary camera ray of every pixel and, alongside it, the
// fixed-step reference march the traversal replaces. Nothing else consumes the traversal
// yet, so this is how the min/max chain gets checked before anything depends on it:
//
//   * Reference difference: the two should agree to within the march's noise floor.
//     Disagreement in dense shadow interiors means the mip chain is not conservative;
//     disagreement only at edges means leaf refinement is too coarse.
//   * Crossing count: whether the four-crossing budget is ever the limiting factor on
//     real terrain, and whether a single-crossing shortcut would have sufficed.
//   * Iteration count: where the divergence cost lives. Grazing rays along the height
//     field are the worst case.
//
// Camera-in-shadow is worth checking directly too -- start inside a ridge shadow and
// confirm the visibility view does not read inverted.

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"
#include "TerrainShadows/TerrainShadowsTraversal.hlsli"

// Matches the C++ depth binding format. TERRAIN_BLENDING ON -> R32_FLOAT blendedDepth,
// OFF -> R24_UNORM_X8_TYPELESS game depth.
#if defined(TERRAIN_BLENDING)
Texture2D<float> SceneDepthTexture : register(t0);
#else
Texture2D<unorm float> SceneDepthTexture : register(t0);
#endif

RWTexture2D<float4> DebugRW : register(u0);

SamplerState LinearSampler : register(s0);

cbuffer TraversalDebugCB : register(b0)
{
	float2 BufferDim;
	float2 RcpBufferDim;

	uint DebugMode;
	uint ReferenceSteps;
	float Sigma;           // mean extinction used to weight the mean visibility
	float DifferenceGain;  // amplification for the difference view

	float MaxRayLength;
	uint MaxIterationsForDisplay;
	float2 pad0;
};

#define DEBUG_MODE_VISIBILITY 0
#define DEBUG_MODE_REFERENCE 1
#define DEBUG_MODE_DIFFERENCE 2
#define DEBUG_MODE_CROSSINGS 3
#define DEBUG_MODE_ITERATIONS 4

// Fixed-step march with the same truncated-exponential weighting the closed form uses, so
// the two are directly comparable rather than merely similar.
float ReferenceMarch(float3 originWS, float3 dirWS, float tMax, float sigma, uint steps)
{
	float weighted = 0.0;
	float weightSum = 0.0;
	float previousCDF = 0.0;

	[loop] for (uint i = 0; i < steps; ++i)
	{
		float tEnd = tMax * float(i + 1) / float(steps);
		float tMid = tMax * (float(i) + 0.5) / float(steps);

		float cdf = TerrainShadows::ScatterCDF(tEnd, sigma, tMax);
		float weight = cdf - previousCDF;
		previousCDF = cdf;

		weighted += weight * TerrainShadows::GetTerrainShadow(originWS + dirWS * tMid, LinearSampler);
		weightSum += weight;
	}

	return weightSum > 0.0 ? saturate(weighted / weightSum) : 1.0;
}

// Blue -> green -> red ramp for the count views. Cheap and monotonic in luminance, which is
// what matters when you are reading a heatmap for outliers.
float3 HeatColor(float x)
{
	x = saturate(x);
	return saturate(float3(x * 2.0 - 1.0, 1.0 - abs(x * 2.0 - 1.0), 1.0 - x * 2.0));
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	uint2 pixel = dispatchID.xy;
	if (any(float2(pixel) >= BufferDim))
		return;

	float2 uv = (float2(pixel) + 0.5) * RcpBufferDim;
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	float2 monoUV = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	float depth = SceneDepthTexture[pixel];

	// Reconstruct the camera-relative world position of this pixel, then the absolute
	// world-space ray the height field is indexed in.
	float4 positionWS = float4(2.0 * float2(monoUV.x, -monoUV.y + 1.0) - 1.0, depth, 1.0);
	positionWS = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], positionWS);
	positionWS.xyz /= positionWS.w;

	float3 originWS = FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	float rayLength = length(positionWS.xyz);
	float3 dirWS = rayLength > 1e-4 ? positionWS.xyz / rayLength : float3(0.0, 0.0, 1.0);

	// Sky pixels have no opaque hit, so fall back to the configured far limit.
	float tMax = min(depth >= 1.0 ? MaxRayLength : rayLength, MaxRayLength);

	TerrainShadows::RayResult result = TerrainShadows::TraceTerrainShadowRay(originWS, dirWS, tMax, LinearSampler);
	float vbar = TerrainShadows::ComputeVBar(result, tMax, Sigma);

	float3 color;
	switch (DebugMode) {
	default:
	case DEBUG_MODE_VISIBILITY:
		color = vbar.xxx;
		break;
	case DEBUG_MODE_REFERENCE:
		color = ReferenceMarch(originWS, dirWS, tMax, Sigma, ReferenceSteps).xxx;
		break;
	case DEBUG_MODE_DIFFERENCE:
		{
			float reference = ReferenceMarch(originWS, dirWS, tMax, Sigma, ReferenceSteps);
			float difference = (vbar - reference) * DifferenceGain;
			// Red where the traversal reports more light than the march (the light-leak signature
			// of a non-conservative chain), blue where it reports less.
			color = float3(saturate(difference), 0.0, saturate(-difference));
			break;
		}
	case DEBUG_MODE_CROSSINGS:
		color = result.CrossingOverflow ? float3(1.0, 1.0, 1.0) : HeatColor(float(result.NumCrossings) / float(TERRAIN_SHADOW_MAX_CROSSINGS));
		break;
	case DEBUG_MODE_ITERATIONS:
		color = result.Incomplete ? float3(1.0, 1.0, 1.0) : HeatColor(float(result.Iterations) / float(max(MaxIterationsForDisplay, 1u)));
		break;
	}

	DebugRW[pixel] = float4(color, 1.0);
}
