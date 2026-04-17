#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

cbuffer buffer : register(b0)
{
	int2 GridTexSize;
	float2 InvGridTexSize;
	float2 GridMinCornerWS;
	float2 InvGridSpan;
	uint toggleLighting;
	uint toggleTrees;
	uint toggleGrass;
	uint toggleDeferred;
	uint toggleEffect;
	float pad[1];
}

#ifdef UPDATE_GRID

SamplerState LinearSampler : register(s0);
Texture2D BentNormalTex : register(t0);
RWTexture2DArray<float4> ProbeArray : register(u0);
RWTexture2D<float4> PlacementMap : register(u1);

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	float2 CoordsUV = (ThreadID.xy + 0.5) * InvGridTexSize.xy;

	float4 BNSample = BentNormalTex.SampleLevel(LinearSampler, CoordsUV, 0);
	float3 BentNormalDir = BNSample.xyz * 2.0 - 1.0;
	float BentNormalAO = BNSample.w;

	sh3 OcclusionSH = SphericalHarmonics::ScaleSH3(SphericalHarmonics::EvaluateSH3(BentNormalDir), BentNormalAO * 4.0 * Math::PI);

	SphericalHarmonics::PackSH3(OcclusionSH, ThreadID.xy, ProbeArray);

	//debug
	/*
	float2 placementMapSize = float2(1104, 768);
	float2 center = (float2(ThreadID.xy) + 0.5) / float2(GridTexSize.x, GridTexSize.y) * placementMapSize;
	int radius = 2;
	for (int y = -radius; y <= radius; y++) {
		for (int x = -radius; x <= radius; x++) {
			if (x * x + y * y <= radius * radius) {
				int2 px = int2(center) + int2(x, y);
				if (px.x >= 0 && px.x < placementMapSize.x && px.y >= 0 && px.y < placementMapSize.y)
					PlacementMap[px] = float4(1, 1, 1, 1);
			}
		}
	}
	*/
}
#endif
