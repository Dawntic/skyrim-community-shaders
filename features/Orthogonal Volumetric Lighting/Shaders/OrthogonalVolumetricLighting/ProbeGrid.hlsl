#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

cbuffer buffer : register(b0)
{
	row_major float4x4 InverseViewProj;
	int2 GridTexSize;
	float2 InvGridTexSize;
	float2 GridMinCornerWS;
	float2 InvGridSpan;
}

sh3 SampleProbeGrid(Texture2DArray ProbeArray, float3 CoordsWS)
{
	if (!SharedData::InInterior) {
		float2 CoordsUV = (CoordsWS.xy - GridMinCornerWS) * InvGridSpan;
		if (all(CoordsUV >= 0) && all(CoordsUV <= 1)) {
			int2 Probe = clamp(int2(CoordsUV * GridTexSize), 0, GridTexSize - 1);
			return SphericalHarmonics::UnpackSH3(Probe, ProbeArray);
		}
	}
	return SphericalHarmonics::ScaleSH3(SphericalHarmonics::UnitSH3(), rcp(1e-8));
}

#ifdef UPDATE_GRID

SamplerState LinearSampler : register(s0);
Texture2D BentNormalTex : register(t0);
RWTexture2DArray<float4> ProbeArray : register(u0);
RWTexture2D<float4> PlacementMap : register(u1);

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	float2 CoordsUV = (ThreadID.xy + 0.5) * InvGridTexSize.xy;
	//float2 CoordsNDC = CoordsUV * 2.0 - 1.0;
	//float2 CoordsWS = mul(InverseViewProj, float4(CoordsNDC.x, -CoordsNDC.y, 0, 1)).xy;

	float4 BNSample = BentNormalTex.SampleLevel(LinearSampler, CoordsUV, 0);
	float3 BentNormalDir = BNSample.xyz;
	float BentNormalAO = BNSample.w;

	sh3 OcclusionSH = SphericalHarmonics::ScaleSH3(SphericalHarmonics::EvaluateSH3(BentNormalDir), BentNormalAO * 4.0 * Math::PI);

	SphericalHarmonics::PackSH3(OcclusionSH, ThreadID.xy, ProbeArray);

	float2 center = CoordsUV * float2(1104, 768);
	int radius = 3;

	for (int y = -radius; y <= radius; y++) {
		for (int x = -radius; x <= radius; x++) {
			if (x * x + y * y <= radius * radius) {
				int2 px = int2(center) + int2(x, y);
				if (px.x >= 0 && px.x < 1104 && px.y >= 0 && px.y < 768)
					PlacementMap[px] = float4(1, 1, 1, 1);
			}
		}
	}
}
#endif
