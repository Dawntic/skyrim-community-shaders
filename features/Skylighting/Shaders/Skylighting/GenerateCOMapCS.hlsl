
#ifdef CSHADER

cbuffer CacheGenBuffer : register(b0)
{
	float2 OutputDim;
	float2 OffsetScale;
	int2 pad2;
};

Texture2D<float> HeightTex : register(t0);
SamplerState LinearSampler : register(s0);
RWTexture2D<float4> OutputHorizon0 : register(u0);
RWTexture2D<float4> OutputHorizon1 : register(u1);

static const float2 CARD[4] = { float2(1, 0), float2(0, 1), float2(-1, 0), float2(0, -1) };
static const float2 DIAG[4] = { float2(0.70710678, 0.70710678), float2(-0.70710678, 0.70710678), float2(-0.70710678, -0.70710678), float2(0.70710678, -0.70710678) };

float TexelsToEdge(float2 uv, float2 dir, float2 Dim)
{
	float2 p = uv * Dim;
	float2 target = float2(dir.x > 0.0 ? Dim.x : 0.0, dir.y > 0.0 ? Dim.y : 0.0);
	float2 d = (target - p) / dir;
	d = (dir == 0.0) ? 1e30 : d;
	return min(d.x, d.y);
}

// March one ray; return sin(elevation) of the highest occluder (>= 0 = flat).
float MarchHorizon(float2 CoordsUV, uint3 ThreadID, float SampleHeight, uint2 HeightMapPxSize, float2 dir)
{
	float2 InvPxSize = 1.0 / HeightMapPxSize;
	float MaxHeight = 0.0;

	float StepCount = TexelsToEdge(CoordsUV, dir, HeightMapPxSize);
	float TexelWorldSize = 128;

	float PosOffset = TexelWorldSize;
	[loop] for (int i = 1; i < StepCount; ++i)
	{
		float2 Offset = dir * i * InvPxSize;
		float Height = HeightTex.SampleLevel(LinearSampler, CoordsUV + Offset, 0) * 65535;
		Height = (Height - OffsetScale.x) * OffsetScale.y;  // convert to game units

		float HDiff = Height - SampleHeight;
		float Hypot = sqrt(PosOffset * PosOffset + HDiff * HDiff);

		MaxHeight = max(MaxHeight, HDiff / Hypot);  //sin(elevation), downhill < 0
		PosOffset += TexelWorldSize;
	}
	return max(MaxHeight, 0.0);
}

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	if (any(ThreadID.xy >= OutputDim))
		return;

	float2 CoordsUV = (ThreadID.xy + 0.5) / OutputDim;
	float HeightSample = HeightTex.SampleLevel(LinearSampler, CoordsUV, 0) * 65535;
	HeightSample = (HeightSample - OffsetScale.x) * OffsetScale.y;  // convert to game units

	uint2 HeightMapPxSize;
	HeightTex.GetDimensions(HeightMapPxSize.x, HeightMapPxSize.y);

	float4 card, diag;
	card.x = MarchHorizon(CoordsUV, ThreadID, HeightSample, HeightMapPxSize, CARD[0]);
	card.y = MarchHorizon(CoordsUV, ThreadID, HeightSample, HeightMapPxSize, CARD[1]);
	card.z = MarchHorizon(CoordsUV, ThreadID, HeightSample, HeightMapPxSize, CARD[2]);
	card.w = MarchHorizon(CoordsUV, ThreadID, HeightSample, HeightMapPxSize, CARD[3]);
	diag.x = MarchHorizon(CoordsUV, ThreadID, HeightSample, HeightMapPxSize, DIAG[0]);
	diag.y = MarchHorizon(CoordsUV, ThreadID, HeightSample, HeightMapPxSize, DIAG[1]);
	diag.z = MarchHorizon(CoordsUV, ThreadID, HeightSample, HeightMapPxSize, DIAG[2]);
	diag.w = MarchHorizon(CoordsUV, ThreadID, HeightSample, HeightMapPxSize, DIAG[3]);

	OutputHorizon0[ThreadID.xy] = card;
	OutputHorizon1[ThreadID.xy] = diag;
}
#endif