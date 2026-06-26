#include "Common/BRDF.hlsli"
#include "Common/Math.hlsli"
#include "PhysicalSky/CloudCommon.hlsli"
#include "PhysicalSky/Common.hlsli"
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

//SamplerState LinearSampler : register(s0);
SamplerState LinearWrapSampler : register(s2);

// 0-11 is cloud stuff
Texture2D SkyViewLUTTex : register(t12);
Texture2D HeightTex : register(t13);
Texture2D BentNormalTex : register(t14);
Texture2D CardinalOcclusionTex : register(t15);
Texture2D CardinalOcclusionDiagTex : register(t16);
Texture2D AlbedoTex : register(t17);
Texture2D NormalTex : register(t18);
Texture2D TerrainIrradianceTex : register(t19);

RWTexture2DArray<float4> ProbeArray : register(u0);

static const float GOLDEN_ANGLE = 2.39996322972865332;  // PI * (3 - sqrt(5))

#	define SAMPLES 256
#	define RAY_SAMPLES 128

float3 SampleSkyRadiance(float3 rayDir)
{
	float azimuth = atan2(rayDir.y, rayDir.x);
	float u = azimuth * .5 * (1 / Math::PI);  // sampler wraps around so ok
	float zenith = asin(rayDir.z);
	float v = 0.5 - 0.5 * sign(zenith) * sqrt(abs(zenith) * 2 * (1 / Math::PI));
	v = max(v, 0.01);

	return SkyViewLUTTex.SampleLevel(LinearWrapSampler, frac(float2(u, v)), 0).rgb;
}

// Fibonacci sample direction over hemisphere with half angle Ap
// gives solid angle 4PI at Ap == -1
float3 FibonacciHemisphere(float i, float n, float ap)
{
	float cosT = lerp(1.0, ap, (i + 0.5) / n);  // uniform in solid angle within the cone
	float3 Out = float3(0, 0, cosT);
	sincos(i * GOLDEN_ANGLE, Out.y, Out.x);
	Out.xy *= sqrt(saturate(1.0 - cosT * cosT));
	return Out;
}

//  orthonormal basis (Frisvad, branchless)
float3x3 BuildTBN(float3 dir)
{
	float sign = dir.z >= 0.0 ? 1.0 : -1.0;
	float a = -1.0 / (sign + dir.z);
	float bb = dir.x * dir.y * a;
	float3 T = float3(1.0 + sign * dir.x * dir.x * a, sign * bb, -sign * dir.x);
	float3 B = float3(bb, sign + dir.y * dir.y * a, -dir.y);
	return float3x3(T, B, dir);
}

void ComputeLighting(float sampleDensity, float StepLightDensity, float CosTheta, CloudRaymarchStepState stepState, CloudParticpatingMedium medium, inout CloudRaymarchAccumState accumState)
{
	float lightOpticalDepth = medium.extinction * StepLightDensity;

	float cosLightZenith = dot(stepState.upVector, SharedData::DirLightDirection.xyz);

	float Scattering = medium.scattering * sampleDensity;
	float Extinction = medium.extinction * sampleDensity;

	float3 Irradiance = SharedData::DirLightColor.xyz * medium.phase;

	// Trasmittance
	float SkyTr = 1;  //TrSample; //atmospherics_air_lut_sampleTransmittance(atmosphere, cosLightZenith, stepState.height);
	float CloudTransmittance = exp(-(Extinction * stepState.rayStep.w));
	float LightTransmittance = exp(-lightOpticalDepth);
	float Transmittance = SkyTr * LightTransmittance;

	// Ambient lighting
	float3 ambientColor = float3(1, 1, 1);  // fake for now
											//float3 AmbientIrradiance = ambientColor * LightTransmittance;

	float ambLightDesnity = sampleDensity;  //lerp(sampleDensityLod, sampleDensity, 0.4);
	float3 ambLightOpticalDepth = medium.extinction * ambLightDesnity;
	float horizonFactor = saturate(pow3(CosTheta));
	ambLightOpticalDepth = lerp(ambLightOpticalDepth, lightOpticalDepth, horizonFactor);
	float3 ambientTransmittance = max(exp(-ambLightOpticalDepth), exp(-ambLightOpticalDepth * 0.25) * 0.7);
	float3 AmbientIrradiance = ambientColor * ambientTransmittance;

	// Multi Scattering Approximation
	float SETTING_CLOUDS_MS_RADIUS = -2.5;  /////////
	float D = exp2(SETTING_CLOUDS_MS_RADIUS);

	float UNIFORM_PHASE = 0.1;  ///////////
	float3 fMS = (Scattering / Extinction) * (1.0 - exp(-D * Extinction));
	fMS = lerp(fMS, fMS * 0.99, float3(LinearStep(0.9, 1.0, fMS.x), LinearStep(0.9, 1.0, fMS.y), LinearStep(0.9, 1.0, fMS.z)));
	float3 MultiScatIrradiance = SharedData::DirLightColor.xyz * Transmittance;
	MultiScatIrradiance *= UNIFORM_PHASE;
	MultiScatIrradiance += AmbientIrradiance;
	MultiScatIrradiance *= fMS / (1.0 - fMS);

	Irradiance = Irradiance * Transmittance + AmbientIrradiance + MultiScatIrradiance;

	// Inscattering interal
	float3 sampleInSctr = Irradiance * Scattering;
	float3 sampleInSctrInt = (sampleInSctr - sampleInSctr * CloudTransmittance) / max(Extinction, 1e-6);
	accumState.totalInscattering += sampleInSctrInt * accumState.totalTransmittance;
	accumState.totalTransmittance *= CloudTransmittance;
}

static const float3 CLOUD_AMBIENT = float3(0.4, 0.45, 0.5);  // flat skylight fill into the cloud
static const float CLOUD_MS_GAIN = 1.8;                      // flat multiple-scatter boost

// Stripped cone-trace lighting: no light march, no MS series, nothing unused.
// sunVisibility = 0..1 sun reaching this sample (height proxy, shadowmap tap, or just 1.0).
void ComputeLightingB(float density, float stepLength, float sunVisibility, CloudParticpatingMedium medium, inout CloudRaymarchAccumState accum)
{
	float albedo = medium.scattering / medium.extinction;  // loop-invariant; hoist if you want
	float Extinction = medium.extinction * density;
	float Tr = exp(-Extinction * stepLength);

	float3 Radiance = SharedData::DirLightColor.xyz * medium.phase * sunVisibility;
	Radiance = (Radiance + CLOUD_AMBIENT) * CLOUD_MS_GAIN;

	float3 inscatter = Radiance * albedo * (1.0 - Tr);

	accum.totalInscattering += inscatter * accum.totalTransmittance;
	accum.totalTransmittance *= Tr;
}

float GetCloudProfile(float3 SamplePos, float Height, bool UseCover)
{
	float3 cloudData = DataFieldTex.SampleLevel(LinearRepeatSampler, SamplePos.xy * cirrusCoverage, 0).xyz;
	float2 TopBottomType = cloudData.zx;
	float Cover = cloudData.y;

	float topProfile = VertProfileTex.SampleLevel(LinearSampler, float2(min(TopBottomType.x, temperatureDiff), Height), 0).x;
	float bottomProfile = VertProfileTex.SampleLevel(LinearSampler, float2(TopBottomType.y, Height), 0).y;

	float VertProfile = topProfile * bottomProfile;

	float Coverage = saturate(LerpLinearStep(Cover, cumulusCoverage, 1.0f, 0.0f, 1.0f));

	if (!UseCover)
		return VertProfile;

	return VertProfile * Coverage;
}

void RaymarchCloud(float3 worldDir, float3 cameraPosA, inout float3 InscattAccum, inout float TransAccum)
{
	float GAME_UNIT_TO_KM = 1.428e-5;
	float3 cameraPos = cameraPosA.xyz * GAME_UNIT_TO_KM;
	cameraPos.z += groundRadius;

	CloudRaymarchAccumState accum;
	accum.totalInscattering = float3(0, 0, 0);
	accum.totalTransmittance = 1.0;

	Ray ray;
	ray.origin = cameraPos;
	ray.direction = worldDir;

	float2 RayT = raycast2(float4((float3)0, topRadius), ray);
	if (!isIntersected(RayT))
		return;

	RayT.x = max(RayT.x, minDistance);

	float2 RayB = raycast2(float4(0.0.xxx, bottomRadius), ray);
	if (isIntersected(RayB))
		RayT = RayB.x < 0.0 ? float2(max(RayT.x, RayB.y), RayT.y) : float2(RayT.x, min(RayT.y, RayB.x));

	if (RayT.y <= RayT.x || RayT.x > maxDistance)
		return;

	float cosTheta = dot(ray.direction, SharedData::DirLightDirection.xyz);
	float StepLength = (RayT.y - RayT.x) / RAY_SAMPLES;

	CloudParticpatingMedium medium;
	medium.scattering = 10;
	medium.extinction = 25;
	medium.phase = CloudPhase(cosTheta, 0);

	float depthWeightedSum = 0.0;  // numerator   of Eq. 21
	float weightSum = 0.0;         // denominator of Eq. 21

	for (int i = 0; i < RAY_SAMPLES; i++) {
		float3 SamplePos = ray.direction * (RayT.x + i * StepLength) + cameraPos;
		float EnvelopeZ = GetEnvelopeRelativeZ(SamplePos, float2(bottomRadius, topRadius));

		float CloudDensity = GetCloudProfile(SamplePos, EnvelopeZ, true);  // + 1; // Tmp to make Optical depth low
		if (CloudDensity <= 0.0)
			continue;

		depthWeightedSum += accum.totalTransmittance * RayT.x;
		weightSum += accum.totalTransmittance;

		CloudRaymarchStepState state;
		state.height = EnvelopeZ;
		state.rayStep = float4(0, 0, 0, StepLength);
		state.upVector = normalize(SamplePos);

		//float SubStepDensity = 1; // value doesnt make a difference.
		//ComputeLighting(CloudDensity, SubStepDensity, cosTheta, state, medium, accum);

		float sunVis = EnvelopeZ;
		ComputeLightingB(CloudDensity, StepLength, sunVis, medium, accum);
	}

	float cloudDistance = (weightSum > 0.0) ? depthWeightedSum / weightSum : RayT.y;
	float3 cloudCamPos = ray.direction * cloudDistance;  // camera-relative; drop distanceSum entirely

	// ONLY works inside view frustum
	float4 AP = PhysSky::SampleAp(normalize(cloudCamPos), length(cloudCamPos), 0.0, LinearSampler);
	accum.totalInscattering = lerp(accum.totalInscattering, AP.xyz, AP.w);

	InscattAccum = accum.totalInscattering;
	TransAccum = accum.totalTransmittance;
}

// Height map and bent normal map must share the same map orientation
[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;

	if (ThreadID.x >= settings.GridTexSize.x || ThreadID.y >= settings.GridTexSize.y)
		return;

	float2 CoordsUV = (ThreadID.xy + 0.5) * settings.InvGridTexSize.xy;

	float WorldHeight = HeightTex.SampleLevel(LinearSampler, CoordsUV, 0) * 65535;
	WorldHeight = (WorldHeight - 32767) * 8.0;

	float3 WorldPos = float3(lerp(settings.GridBounds.xy, settings.GridBounds.zw, CoordsUV), WorldHeight);

	float4 BNSample = BentNormalTex[ThreadID.xy];
	float3 BentNormalDir = BNSample.xyz * 2.0 - 1.0;
	float BentNormalAO = BNSample.w;

	// Cone Tracing
	float Aperture = saturate(1.0 - BentNormalAO);
	float solidAngle = 2.0 * Math::PI * BentNormalAO;
	const float dOmega = solidAngle / SAMPLES;

	//float solidAngleAmb = 2.0 * Math::PI; // * (1.0 - BentNormalAO);
	//const float dOmegaAmb = solidAngleAmb / SAMPLES;

	float3x3 BentTBN = BuildTBN(BentNormalDir);

	sh2RGB output = SphericalHarmonics::Zero2RGB();
	for (int i = 0; i < SAMPLES; ++i) {
		float3 SampleDir = FibonacciHemisphere(i, SAMPLES, Aperture);
		SampleDir = mul(SampleDir, BentTBN);

		float3 skyRadiance = SampleSkyRadiance(SampleDir);

		float cloudTr = 1;
		float3 cloudInscattering = 0;
		//RaymarchCloud(SampleDir, WorldPos, cloudInscattering, cloudTr);

		float3 radiance = skyRadiance * cloudTr + cloudInscattering;

		sh2RGB value = SphericalHarmonics::Scale(SphericalHarmonics::Evaluate(SampleDir), radiance * dOmega);
		//output = SphericalHarmonics::Add(output, value);

		// ground bounce comes from all dirs that aren't sky
		// sky radiance effects bounce radiance
		// bent normal doesn't effect bounce light since downwards hemi can never be sky
		// but at most bounce light comes from the most occluded direction? - importance sampling?? -- NO
		// should sample uniform directions
		// what magnitude to use??  can we sample the nearest position thats
		// in a given direction sample the closest point which has a normal that is -sampleDir

		// does 1 - bentConeAO effect the amount of bounce light reaching the bottom half of a sphere?

		//Concretely: the top's AO measures occlusion of the sky, but the bottom is occluded from the sky anyway (it faces down)
		// POINT: if the bottom hemisphere was sky then the bounce radiance is 0 so if
		// at a given point, 20% of sky is occluded then 80% is ground BUT does that mean the irradiance(ground) increases?

		// Number of pixels we must walk to reach a pos with reflected normals(-sampleDir)

		// (1.0 - BentConeAO) is amount of upper hemi that is terrain
		// bottom hemi is always terrain, some of top hemi can also be terrain so its 2PI(bottom hemi) + (1.0 - BentConeAO)

		//float3 AmbSampleDir = -SampleDir; // using same Aperture might be wrong. flipping the dir means sampling -bentDir & down not up which is correct for sampling ground bounce i think
		//float3 ambientRadiance = AmibentMap.SampleLevel(LinearSampler, AmbSampleDir.xy, 0);
		//sh2RGB ambientSH = SphericalHarmonics::Scale(SphericalHarmonics::Evaluate(AmbSampleDir), ambientRadiance * dOmegaAmb);
		//output = SphericalHarmonics::Add(output, ambientSH);
	}

	// Albedo relighting
	// sh3RGB albedo = SphericalHarmonics::UnpackSH3(ThreadID.xy, BentNormalTex);
	// sh3RGB color = ProductSH3(EvaluateCosineLobeSH3(-SharedData::DirLightDirection.xyz), albedo);
	// color = SphericalHarmonics::ScaleSH3(color, SharedData::DirLightColor.xyz * (1.0 / Math::PI));    // env bounce + light color
	// SphericalHarmonics::PackSH3(color, ThreadID.xy, ProbeArray);

	float3 BounceIrradiance = TerrainIrradianceTex.SampleLevel(LinearSampler, CoordsUV, 0) * 2;
	sh2RGB ambientSH = SphericalHarmonics::Scale(SphericalHarmonics::Evaluate(float3(0, 0, -1)), BounceIrradiance * 2.0 * Math::PI);
	output = SphericalHarmonics::Add(output, ambientSH);
	SphericalHarmonics::PackSH2RGB(output, ThreadID.xy, ProbeArray);
}
#endif

#ifdef TERRAIN_RELIGHT

//SamplerState LinearSampler : register(s0);
SamplerState LinearWrapSampler : register(s2);

Texture2D SkyViewLUTTex : register(t0);
Texture2D HeightTex : register(t1);
Texture2D BentNormalTex : register(t2);
Texture2D CardinalOcclusionTex : register(t3);
Texture2D CardinalOcclusionDiagTex : register(t4);
Texture2D AlbedoTex : register(t5);
Texture2D NormalTex : register(t6);

RWTexture2D<float4> TerrainRelight : register(u0);

float GetDirOcclusion(float2 PxCoords)
{
	float Visibility;

	float4 Card = CardinalOcclusionTex[PxCoords];
	float4 Diag = CardinalOcclusionDiagTex[PxCoords];

	// Horizon height in sun direction
	float4 basis0 = SharedData::skylightingSettings.Basis0;
	float4 basis1 = SharedData::skylightingSettings.Basis1;
	float Horizon = max(dot(Card, basis0), dot(Diag, basis1));

	float Scale = 15.0;
	float Bias = 0.12;
	bool belowHorizon = (Horizon > SharedData::DirLightDirection.z);
	Visibility = saturate(abs(SharedData::DirLightDirection.z - Horizon) * Scale + Bias);
	Visibility = belowHorizon ? 0.5 * smoothstep(0.0, 1.0, 1.0 - Visibility) : 0.5 + smoothstep(0.0, 1.0, Visibility);

	return saturate(Visibility);
}

// Can we sample cardinal occlusion to figure out where most of the bounce light comes from? eg reflection from side of hill vs flat ground
// Wrong bc Albedo map is scaled incorrectly
[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;

	if (ThreadID.x >= settings.GridTexSize.x || ThreadID.y >= settings.GridTexSize.y)
		return;

	float3 AmbientLighting = 0.0;
	float3 Lighting = 0.0;

	float SunShadow = GetDirOcclusion(ThreadID.xy);
	float3 NormalWS = NormalTex[ThreadID.xy].xzy * 2.0 - 1.0;  // swizzle since N is +Y up

	float NdotL = saturate(dot(NormalWS, SharedData::DirLightDirection));
	float3 DirLighting = SharedData::DirLightColor.xyz * SunShadow * NdotL * BRDF::Diffuse_Lambert();

	Lighting = DirLighting + AmbientLighting;
	Lighting *= AlbedoTex[ThreadID.xy];

	TerrainRelight[ThreadID.xy] = float4(Lighting, 1);
}
#endif
