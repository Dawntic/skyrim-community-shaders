#include "Common/Math.hlsli"

#ifdef CSHADER

cbuffer CacheGenBuffer : register(b0)
{
	float2 OutputTexSize;
	float2 HeightMapOffsetScale;
};

Texture2D<float> HeightTex : register(t0);
SamplerState LinearSampler : register(s0);

#	ifdef CARDINALS
RWTexture2D<float4> OutputHorizon0 : register(u0);
RWTexture2D<float4> OutputHorizon1 : register(u1);
#	else
RWTexture2D<float4> OutputBentNormal : register(u0);
#	endif

static const float2 CARD[4] = { float2(1, 0), float2(0, 1), float2(-1, 0), float2(0, -1) };
static const float2 DIAG[4] = { float2(0.70710678, 0.70710678), float2(-0.70710678, 0.70710678), float2(-0.70710678, -0.70710678), float2(0.70710678, -0.70710678) };

float TexelsToEdge(float2 uv, float2 Dir, float2 Dim)
{
	float2 p = uv * Dim;
	float2 target = float2(Dir.x > 0.0 ? Dim.x : 0.0, Dir.y > 0.0 ? Dim.y : 0.0);
	float2 d;
	d.x = abs(Dir.x) > 1e-6 ? (target.x - p.x) / Dir.x : 1e30;
	d.y = abs(Dir.y) > 1e-6 ? (target.y - p.y) / Dir.y : 1e30;
	return min(d.x, d.y);
}

// March one ray; return sin(elevation) of the highest occluder (>= 0 = flat).
float MarchHorizon(float2 CoordsUV, uint3 ThreadID, float SampleHeight, uint2 HeightMapPxSize, float2 Dir)
{
	float2 InvPxSize = 1.0 / HeightMapPxSize;
	float MaxHeight = 0.0;

	float StepCount = TexelsToEdge(CoordsUV, Dir, HeightMapPxSize);
	float TexelWorldSize = 128;

	float PosOffset = TexelWorldSize;
	[loop] for (int i = 1; i < StepCount; ++i)
	{
		float2 Offset = Dir * i * InvPxSize;
		float Height = HeightTex.SampleLevel(LinearSampler, CoordsUV + Offset, 0) * 65535;
		Height = (Height - HeightMapOffsetScale.x) * HeightMapOffsetScale.y;  // convert to game units

		float HDiff = Height - SampleHeight;
		float Hypot = sqrt(PosOffset * PosOffset + HDiff * HDiff);

		MaxHeight = max(MaxHeight, HDiff / Hypot);  //sin(elevation), downhill < 0
		PosOffset += TexelWorldSize;
	}
	return max(MaxHeight, 0.0);
}

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	if (any(ThreadID.xy >= OutputTexSize))
		return;

	float2 CoordsUV = (ThreadID.xy + 0.5) / OutputTexSize;
	float HeightSample = HeightTex.SampleLevel(LinearSampler, CoordsUV, 0) * 65535;
	HeightSample = (HeightSample - HeightMapOffsetScale.x) * HeightMapOffsetScale.y;  // convert to game units

	uint2 HeightMapPxSize;
	HeightTex.GetDimensions(HeightMapPxSize.x, HeightMapPxSize.y);

#	ifdef CARDINALS
	float4 card, diag;
	[unroll] for (int i = 0; i < 8; i++)
	{
		if (i < 4) {
			card[i] = MarchHorizon(CoordsUV, ThreadID, HeightSample, HeightMapPxSize, CARD[i]);
		} else {
			diag[i - 4] = MarchHorizon(CoordsUV, ThreadID, HeightSample, HeightMapPxSize, DIAG[i - 4]);
		}
	}
	OutputHorizon0[ThreadID.xy] = card;
	OutputHorizon1[ThreadID.xy] = diag;
#	else
	static const uint NUM_AZIMUTH = 1024;  // crank as high as you like

	const float AzStep = Math::TAU / NUM_AZIMUTH;
	const float RadiualWeight = 2.0 * sin(0.5 * AzStep);  // exact ∫ cos/sin over the wedge
	float3 DirAccum = 0.0;                                // unnormalized ∫_visible ω dω
	float VisAccum = 0.0;
	[loop] for (uint i = 0; i < NUM_AZIMUTH; ++i)
	{
		float phi = (i + 0.5) * AzStep;
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

	float AO = saturate(VisAccum * rcp(Math::TAU));
	//float AO = saturate(DirAccum.z * Math::INV_PI);  // cosine-weighted visibility
	float3 BentNormal = normalize(DirAccum);

	OutputBentNormal[ThreadID.xy] = float4(BentNormal * 0.5 + 0.5, AO);
#	endif
}
#endif
