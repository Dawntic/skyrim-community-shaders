#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

// EnvTexture removed — we read the SkyView LUT (TexSvLut, declared in your includes).
// Make sure the SkyView LUT SRV is bound for this dispatch.
RWTexture2D<sh2> IBLTexture : register(u0);
SamplerState LinearSampler : register(s0);

// cylinder map (unchanged — yours)
float2 SkyViewLutUv(float3 rayDir)
{
	float azimuth = atan2(rayDir.y, rayDir.x);
	float u = azimuth * .5 * RCP_PI;  // sampler wraps around so ok
	float zenith = asin(rayDir.z);
	float v = 0.5 - 0.5 * sign(zenith) * sqrt(abs(zenith) * 2 * RCP_PI);
	v = max(v, 0.01);
	return frac(float2(u, v));
}

#define AXIS_SAMPLE_COUNT 16
#define TOTAL_SAMPLES (AXIS_SAMPLE_COUNT * AXIS_SAMPLE_COUNT)

groupshared sh2 sharedR[TOTAL_SAMPLES];
groupshared sh2 sharedG[TOTAL_SAMPLES];
groupshared sh2 sharedB[TOTAL_SAMPLES];

[numthreads(AXIS_SAMPLE_COUNT, AXIS_SAMPLE_COUNT, 1)] void main(uint3 dispatchID : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
	uint az = dispatchID.x;
	uint ze = dispatchID.y;

	const float rcpAxisSampleCount = rcp((float)AXIS_SAMPLE_COUNT);
	const float shFactor = 4.0 * Math::PI * rcpAxisSampleCount * rcpAxisSampleCount;

	float2 sampleCoord = (float2(az, ze) + 0.5) * rcpAxisSampleCount;
	float3 rayDir = SphericalHarmonics::GetUniformSphereSample(sampleCoord.x, sampleCoord.y);

	float3 color = TexSvLut.SampleLevel(LinearSampler, SkyViewLutUv(rayDir), 0).rgb;

	sh2 sh = SphericalHarmonics::Evaluate(rayDir);

	sh2 contributionR = SphericalHarmonics::Scale(sh, color.r * shFactor);
	sh2 contributionG = SphericalHarmonics::Scale(sh, color.g * shFactor);
	sh2 contributionB = SphericalHarmonics::Scale(sh, color.b * shFactor);

	sharedR[groupIndex] = contributionR;
	sharedG[groupIndex] = contributionG;
	sharedB[groupIndex] = contributionB;

	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint stride = TOTAL_SAMPLES / 2; stride > 0; stride >>= 1)
	{
		if (groupIndex < stride) {
			sharedR[groupIndex] = SphericalHarmonics::Add(sharedR[groupIndex], sharedR[groupIndex + stride]);
			sharedG[groupIndex] = SphericalHarmonics::Add(sharedG[groupIndex], sharedG[groupIndex + stride]);
			sharedB[groupIndex] = SphericalHarmonics::Add(sharedB[groupIndex], sharedB[groupIndex + stride]);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (groupIndex == 0) {
		IBLTexture[int2(0, 0)] = sharedR[0];
		IBLTexture[int2(1, 0)] = sharedG[0];
		IBLTexture[int2(2, 0)] = sharedB[0];
	}
}
