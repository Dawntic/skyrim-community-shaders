#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

cbuffer CacheGenBuffer : register(b0)
{
	float2 OutputTexSize;
	float2 HeightMapOffsetScale;
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
RWTexture2D<float4> OutputHorizon4 : register(u4);
RWTexture2D<float4> OutputHorizon5 : register(u5);
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
float MarchHorizon(float2 CoordsUV, float SampleHeight, uint2 HeightMapPxSize,
	float2 Dir, out float2 DomDist)  // x = wall base dist, y = rim dist
{
	float2 InvPxSize = 1.0 / HeightMapPxSize;
	float StepCount = TexelsToEdge(CoordsUV, Dir, HeightMapPxSize);

	float4 GridBounds = float4(-233472.00, -176128.00, 253952.00, 208896.00);  // pull from settings later
	float2 TexelWorldSize = (GridBounds.zw - GridBounds.xy) / (float2)HeightMapPxSize;
	float StepDist = length(Dir * TexelWorldSize);

	float MaxSin = 0.0;
	float RunRise = 0.0, RunScore = 0.0;  // Δsin total, Σ Δsin·d
	float RunStartD = 0.0, RunEndD = 0.0, LastRiseD = -1e30;
	float BestScore = 0.0;
	float2 Best = 0.0;
	const float MergeGap = 4.0 * StepDist;  // dips shorter than this don't split an occluder

	float PosOffset = StepDist;
	[loop] for (int step = 1; step < StepCount; ++step)
	{
		float2 Offset = Dir * step * InvPxSize;
		float2 SampCoords = CoordsUV + Offset;
		SampCoords.y = 1.0 - SampCoords.y;
		float Height = HeightTex.SampleLevel(LinearSampler, SampCoords, 0);  // * 65535;
		//Height = (Height - HeightMapOffsetScale.x) * HeightMapOffsetScale.y;

		float HDiff = Height - SampleHeight;
		float SinE = HDiff * rsqrt(PosOffset * PosOffset + HDiff * HDiff);

		if (SinE > MaxSin) {
			if (RunRise > 0.0 && PosOffset - LastRiseD > MergeGap) {  // new occluder starts
				if (RunScore > BestScore) {
					BestScore = RunScore;
					Best = float2(RunStartD, RunEndD);
				}
				RunRise = 0.0;
				RunScore = 0.0;
			}
			if (RunRise == 0.0)
				RunStartD = PosOffset;
			float dS = SinE - MaxSin;
			RunRise += dS;
			RunScore += dS * PosOffset;
			RunEndD = PosOffset;
			LastRiseD = PosOffset;
			MaxSin = SinE;
		}
		PosOffset += StepDist;
	}
	if (RunScore > BestScore)
		Best = float2(RunStartD, RunEndD);

	DomDist = Best;
	return MaxSin;  // horizon itself unchanged — the bump still occludes correctly
}

#	ifdef CARDINALS
float MarchHorizonOld(float2 CoordsUV, uint3 ThreadID, float SampleHeight, uint2 HeightMapPxSize, float2 Dir, out float MeanHitDist)
#	else
float MarchHorizonOld(float2 CoordsUV, uint3 ThreadID, float SampleHeight, uint2 HeightMapPxSize, float2 Dir)
#	endif
{
	float2 InvPxSize = 1.0 / HeightMapPxSize;
	float MaxHeight = 0.0;
	float WeightedDist = 0.0;

	float StepCount = TexelsToEdge(CoordsUV, Dir, HeightMapPxSize);

	//const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;
	float4 GridBounds = float4(-233472.00, -176128.00, 253952.00, 208896.00);  // pull from settings later

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

	float2 CoordsUV = (ThreadID.xy + 0.5) / OutputTexSize;

	float2 SampCoords = CoordsUV;
	SampCoords.y = 1.0 - SampCoords.y;
	float HeightSample = HeightTex.SampleLevel(LinearSampler, SampCoords, 0);  // * 65535;
																			   //HeightSample = (HeightSample - HeightMapOffsetScale.x) * HeightMapOffsetScale.y;

	uint2 HeightMapPxSize;
	HeightTex.GetDimensions(HeightMapPxSize.x, HeightMapPxSize.y);

#	ifdef CARDINALS
	float4 card, diag;
	float2 cardWeight[4], diagWeight[4];
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

	OutputHorizon2[ThreadID.xy] = float4(cardWeight[0].x, cardWeight[1].x, cardWeight[2].x, cardWeight[3].x);
	OutputHorizon3[ThreadID.xy] = float4(cardWeight[0].y, cardWeight[1].y, cardWeight[2].y, cardWeight[3].y);
	OutputHorizon4[ThreadID.xy] = float4(diagWeight[0].x, diagWeight[1].x, diagWeight[2].x, diagWeight[3].x);
	OutputHorizon5[ThreadID.xy] = float4(diagWeight[0].y, diagWeight[1].y, diagWeight[2].y, diagWeight[3].y);

	//OutputHorizon2[ThreadID.xy] = cardWeight;
	//OutputHorizon3[ThreadID.xy] = diagWeight;
	//OutputHorizon4[ThreadID.xy] = cardWeight;
	//OutputHorizon5[ThreadID.xy] = diagWeight;
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

		// blur to smoothen result
		float SinH = 0.0;
		float wSum = 0.0;
		[unroll] for (int ky = -2; ky <= 2; ++ky)
		{
			[unroll] for (int kx = -2; kx <= 2; ++kx)
			{
				float2 nUV = CoordsUV + float2(kx, ky) / HeightMapPxSize;

				float nH = HeightTex.SampleLevel(LinearSampler, float2(nUV.x, 1.0 - nUV.y), 0);  // * 65535;
																								 //nH = (nH - HeightMapOffsetScale.x) * HeightMapOffsetScale.y;
				float w = exp(-(kx * kx + ky * ky) * 0.5);
				SinH += MarchHorizonOld(nUV, ThreadID, nH, HeightMapPxSize, Dir.yx) * w;
				wSum += w;
			}
		}
		SinH /= wSum;

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

	float4 GridBounds = float4(-233472.00, -176128.00, 253952.00, 208896.00);  // pull out later
	float2 TexelWorldSize = (GridBounds.zw - GridBounds.xy) / (float2)HeightMapPxSize;

	dHdx /= (6.0 * TexelWorldSize.x);
	dHdy /= (6.0 * TexelWorldSize.y);

	float3 N = normalize(float3(-dHdx, -dHdy, 1.0));

	OutputNormal[ThreadID.xy] = float4(N * 0.5 + 0.5, 1.0);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////
