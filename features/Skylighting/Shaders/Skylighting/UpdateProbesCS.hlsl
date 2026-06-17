#include "Common/Math.hlsli"
#include "Skylighting/Skylighting.hlsli"

#ifdef DENSE_PROBE_GRID

Texture2D<unorm float> srcOcclusionDepth : register(t0);

RWTexture3D<sh2> outProbeArray : register(u0);
RWTexture3D<uint> outAccumFramesArray : register(u1);

SamplerComparisonState comparisonSampler : register(s0);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const float fadeInThreshold = 15;
	const static sh2 unitSH = float4(sqrt(4.0 * Math::PI), 0, 0, 0);
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;
	uint3 cellID = uint3(max(int3(dtid) - settings.ArrayOrigin.xyz, 0) % Skylighting::ARRAY_DIM);
	uint3 validMin = (uint3)max(0, settings.ValidMargin.xyz);
	uint3 validMax = Skylighting::ARRAY_DIM - 1 + (uint3)min(0, settings.ValidMargin.xyz);
	bool isValid = all(cellID >= validMin) && all(cellID <= validMax);  // check if the cell is newly added
	float3 cellCentreMS = cellID + 0.5 - Skylighting::ARRAY_DIM / 2;
	cellCentreMS = cellCentreMS / Skylighting::ARRAY_DIM * Skylighting::ARRAY_SIZE + settings.PosOffset.xyz;

	float3 cellCentreOS = mul(settings.OcclusionViewProj, float4(cellCentreMS, 1)).xyz;
	cellCentreOS.y = -cellCentreOS.y;
	float2 occlusionUV = cellCentreOS.xy * 0.5 + 0.5;

	if (all(occlusionUV > 0) && all(occlusionUV < 1)) {
		uint accumFrames = isValid ? (outAccumFramesArray[dtid] + 1) : 1;
		float occlusionDepth = srcOcclusionDepth.SampleCmpLevelZero(comparisonSampler, occlusionUV, 0);
		float visibility = srcOcclusionDepth.SampleCmpLevelZero(comparisonSampler, occlusionUV, cellCentreOS.z);

		sh2 occlusionSH = SphericalHarmonics::Scale(SphericalHarmonics::Evaluate(settings.OcclusionDir.xyz), visibility * 4.0 * Math::PI);  // 4 pi from monte carlo
		if (isValid) {
			float lerpFactor = rcp(accumFrames);
			sh2 prevProbeSH = unitSH;
			if (accumFrames > 1)
				prevProbeSH += (outProbeArray[dtid] - unitSH) * fadeInThreshold / min(fadeInThreshold, accumFrames - 1);  // inverse confidence
			occlusionSH = lerp(prevProbeSH, occlusionSH, lerpFactor);
		}
		occlusionSH = lerp(unitSH, occlusionSH, min(fadeInThreshold, accumFrames) / fadeInThreshold);  // confidence fade in

		outProbeArray[dtid] = occlusionSH;
		outAccumFramesArray[dtid] = accumFrames;
	} else if (!isValid) {
		outProbeArray[dtid] = unitSH;
		outAccumFramesArray[dtid] = 0;
	}
}
#endif

#ifdef SPARSE_PROBE_GRID

SamplerState LinearSampler : register(s0);
Texture2D BentNormalTex : register(t0);
Texture2D SkyViewLUTTex : register(t1);
RWTexture2DArray<float4> ProbeArray : register(u0);

static const float GOLDEN_ANGLE = 2.39996322972865332;  // PI * (3 - sqrt(5))
#	define SAMPLES 256

float3 SampleSkyRadiance(float3 rayDir)
{
	float azimuth = atan2(rayDir.y, rayDir.x);
	float u = azimuth * .5 * (1 / Math::PI);  // sampler wraps around so ok
	float zenith = asin(rayDir.z);
	float v = 0.5 - 0.5 * sign(zenith) * sqrt(abs(zenith) * 2 * (1 / Math::PI));
	v = max(v, 0.01);

	return SkyViewLUTTex.SampleLevel(LinearSampler, frac(float2(u, v)), 0).rgb;
}

// i-th Fibonacci direction over hemisphere
// Uniform in z + golden angle azimuth => every sample subtends solid angle (2*PI / N)
float3 FibonacciHemisphere(uint i, uint samples)
{
	float z = 1.0 - (float(i) + 0.5) / float(samples);
	float r = sqrt(saturate(1.0 - z * z));
	float theta = GOLDEN_ANGLE * float(i);
	return float3(r * cos(theta), r * sin(theta), z);
}

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;

	if (ThreadID.x >= settings.GridTexSize.x || ThreadID.y >= settings.GridTexSize.y)
		return;

	float2 CoordsUV = (ThreadID.xy + 0.5) * settings.InvGridTexSize.xy;

	float4 BNSample = BentNormalTex.SampleLevel(LinearSampler, CoordsUV, 0);
	float3 BentNormalDir = BNSample.xyz * 2.0 - 1.0;
	float BentNormalAO = BNSample.w;

	//sh3 OcclusionSH = SphericalHarmonics::ScaleSH3(SphericalHarmonics::EvaluateSH3(BentNormalDir), BentNormalAO * 4.0 * Math::PI);
	//SphericalHarmonics::PackSH3(OcclusionSH, ThreadID.xy, ProbeArray);

	sh2RGB output = SphericalHarmonics::Zero2RGB();

	const float dOmega = (2.0 * Math::PI) / float(SAMPLES);  // constant: equal-area samples

	for (uint i = 0; i < SAMPLES; ++i) {
		float3 dir = FibonacciHemisphere(i, SAMPLES);
		float3 skyRadiance = SampleSkyRadiance(dir);
		sh2 basis = SphericalHarmonics::Evaluate(dir);

		sh2RGB value = SphericalHarmonics::Scale(basis, skyRadiance * dOmega);
		output = SphericalHarmonics::Add(output, value);
	}

	SphericalHarmonics::PackSH2RGB(output, ThreadID.xy, ProbeArray);
}
#endif
