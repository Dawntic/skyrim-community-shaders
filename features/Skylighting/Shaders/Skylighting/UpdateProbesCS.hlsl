#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
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
Texture2D TerrainRadianceTex : register(t19);
Texture2D ReflectionDistribTex : register(t20);

RWTexture2DArray<float4> ProbeArray : register(u0);

static const float GOLDEN_ANGLE = 2.39996322972865332;  // PI * (3 - sqrt(5))

#	define SAMPLES 64      //256
#	define RAY_SAMPLES 64  //128

float3 SampleSkyRadiance(float3 rayDir)
{
	float azimuth = atan2(rayDir.y, rayDir.x);
	float u = azimuth * .5 * (1 / Math::PI);  // sampler wraps around so ok
	float zenith = asin(rayDir.z);
	float v = 0.5 - 0.5 * sign(zenith) * sqrt(abs(zenith) * 2 * (1 / Math::PI));
	v = max(v, 0.01);

	return SkyViewLUTTex.SampleLevel(LinearWrapSampler, frac(float2(u, v)), 0).rgb;
}

// Fibonacci sample direction over hemisphere
// gives solid angle 2PI at input ap == 1
// domain: xy[-1, 1] z[1-ap, 1]
float3 FibonacciHemisphere(float i, float n, float ap)
{
	float cosT = lerp(1.0, 1 - ap, (i + 0.5) / n);  // uniform in solid angle within the cone
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

uint Hash(uint x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

uint CardinalIndex(float2 dir)
{
	if (abs(dir.x) >= abs(dir.y))
		return dir.x >= 0.0 ? 0 : 2;  // +X / -X
	else
		return dir.y >= 0.0 ? 1 : 3;  // +Y / -Y
}
float2 LinearStep(float2 edge0, float2 edge1, float2 x)
{
	return saturate((x - edge0) / (edge1 - edge0));
}

static const float2 CARD[4] = { float2(1, 0), float2(0, 1), float2(-1, 0), float2(0, -1) };
static const float2 DIAG[4] = { float2(0.70710678f, 0.70710678f), float2(-0.70710678f, 0.70710678f),
	float2(-0.70710678f, -0.70710678f), float2(0.70710678f, -0.70710678f) };

float4 BuildHorizonBasis(float2 dirs[4], float ax, float ay, float sharpness)
{
	float w[4];
	float sum = 0.f;
	[unroll] for (int k = 0; k < 4; ++k)
	{
		float d = max(0.f, ax * dirs[k].x + ay * dirs[k].y);  // project azimuth onto bin
		d = pow(d, sharpness);                                // 1 = linear cosine blend
		w[k] = d;
		sum += d;
	}
	float inv = sum > 1e-5f ? 1.f / sum : 0.f;  // 0 weights when sun is overhead -> occVal 0 -> lit
	return float4(w[0] * inv, w[1] * inv, w[2] * inv, w[3] * inv);
}

void BuildOcclusionBasis(float3 lightDir, out float4 basis0, out float4 basis1, float sharpness = 1.f)
{
	float hl = sqrt(lightDir.x * lightDir.x + lightDir.y * lightDir.y);  // azimuth length
	float ax = hl > 1e-5f ? lightDir.x / hl : 0.f;
	float ay = hl > 1e-5f ? lightDir.y / hl : 0.f;
	basis0 = BuildHorizonBasis(CARD, ax, ay, sharpness);
	basis1 = BuildHorizonBasis(DIAG, ax, ay, sharpness);
}
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

float SinToTan(float s)
{
	return s / sqrt(max(1.0 - s * s, 1e-6));
}

// Reconstruct approx distance (game units) to the horizon-defining occluder.
// DirIdx: 0-3 -> HorizonTex0 (cardinals), 4-7 -> HorizonTex1 (diagonals)
float ReconstructHorizonDist(float2 CoordsUV, float2 Dir, uint DirIdx)
{
	float2 InvPxSize = 1.0 / float2(3808, 3008);
	float TexelWorldSize = 128;
	float2 StepUV = Dir * InvPxSize;  // one texel toward the occluder

	// stored horizon sin at current texel and one texel along Dir
	float4 h0 = (DirIdx < 4) ? CardinalOcclusionTex.SampleLevel(LinearSampler, CoordsUV, 0) : CardinalOcclusionDiagTex.SampleLevel(LinearSampler, CoordsUV, 0);
	float4 h1 = (DirIdx < 4) ? CardinalOcclusionTex.SampleLevel(LinearSampler, CoordsUV + StepUV, 0) : CardinalOcclusionDiagTex.SampleLevel(LinearSampler, CoordsUV + StepUV, 0);
	uint c = DirIdx & 3;

	float t = SinToTan(h0[c]);
	float tn = SinToTan(h1[c]);

	// local terrain slope in Dir (world units per world step)
	float H0 = HeightTex.SampleLevel(LinearSampler, CoordsUV, 0) * 65535;
	H0 = (H0 - 32767) * 8.0;
	float H1 = HeightTex.SampleLevel(LinearSampler, CoordsUV + StepUV, 0) * 65535;
	H1 = (H1 - 32767) * 8.0;

	float gp = (H1 - H0) / TexelWorldSize;
	float dtdx = (tn - t) / TexelWorldSize;

	// d = (t - g') / (dt/dx); reject flat/degenerate/behind cases
	if (abs(dtdx) < 1e-6)
		return -1.0;

	float d = (t - gp) / dtdx;

	return (d > 0.0) ? d : -1.0;
}

// Figure out best way to sample ground lighting
// Fix albedo and normal texture shape/size

// each pixel stores the

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;

	if (ThreadID.x >= settings.GridTexSize.x || ThreadID.y >= settings.GridTexSize.y)
		return;

	float2 CoordsUV = (ThreadID.xy + 0.5) * settings.InvGridTexSize.xy;
	CoordsUV.y = 1.0 - CoordsUV.y;

	float WorldHeight = HeightTex.SampleLevel(LinearSampler, CoordsUV, 0) * 65535;
	WorldHeight = (WorldHeight - 32767) * 8.0;

	float3 WorldPos = float3(lerp(settings.GridBounds.xy, settings.GridBounds.zw, CoordsUV), WorldHeight);

	float4 BNSample = BentNormalTex.SampleLevel(LinearSampler, CoordsUV, 0);  //BentNormalTex[ThreadID.xy];
	float3 BentNormalDir = BNSample.xyz * 2.0 - 1.0;
	float SkyAperture = BNSample.w;  // AO

	// debugging
	//float2 nCoord = float2(CoordsUV.x, 1.0 - CoordsUV.y) * float2(1024, 801);
	//float4 NormalWS = float4(NormalTex[nCoord].xzy * 2.0 - 1.0, 1);

	//ProbeArray[ThreadID.xyz] = NormalWS;
	//ProbeArray[int3(0,0,0)] = CardinalOcclusionTex[int2(0,0)];
	//float sdf = length(WorldPos - FrameBuffer::CameraPosAdjust.xy) - 2000;
	//if(sdf <= 0)
	//	ProbeArray[ThreadID.xyz] = 1.0.xxxx;
	//CoordsUV = LinearStep(settings.GridBounds.xy, settings.GridBounds.zw, FrameBuffer::CameraPosAdjust.xy);  // player pos
	//CoordsUV.y = 1 - CoordsUV.y;

	//float4 Steps = ReflectionDistribTex.SampleLevel(LinearSampler, CoordsUV, 0);//[CoordsUV * 1024]; // XYZW = east,west,south,north

	// Cone Tracing
	float SkySolidAngle = 2.0 * Math::PI * SkyAperture;
	const float SkyWeight = SkySolidAngle / SAMPLES;

	float GroundAperture = 1.0 + (1.0 - SkyAperture);
	float GroundSolidAngle = 2.0 * Math::PI * GroundAperture;  // add the part of upper hemisphere thats occluded
	const float GroundWeight = GroundSolidAngle / SAMPLES;

	float3x3 BentTBN = BuildTBN(BentNormalDir);

	sh2RGB Output = SphericalHarmonics::Zero2RGB();
	for (int i = 0; i < SAMPLES; ++i) {
		// add back once terrain sampling works
		// /*
		float3 SkySampleDir = FibonacciHemisphere(i, SAMPLES, SkyAperture);
		SkySampleDir = mul(SkySampleDir, BentTBN);

		float3 SkyRadiance = SampleSkyRadiance(SkySampleDir) * 0.1;

		float cloudTr = 1;
		float3 cloudInscattering = 0;
		// RaymarchCloud(SkySampleDir, WorldPos, cloudInscattering, cloudTr);

		cloudInscattering *= 0.2;
		SkyRadiance = SkyRadiance * cloudTr + cloudInscattering;

		sh2RGB SkySH = SphericalHarmonics::Scale(SphericalHarmonics::Evaluate(SkySampleDir), SkyRadiance * SkyWeight);
		Output = SphericalHarmonics::Add(Output, SkySH);
		// */

		// how do i sample this direction in terrainTex?

		float3 GroundSampleDir = FibonacciHemisphere(i, SAMPLES, GroundAperture);  // use 1 instead of 1-skyAp since lower hemi is always ground and Ap > 1.0 breaks sampling
		GroundSampleDir = -GroundSampleDir;                                        //mul(-GroundSampleDir, BentTBN); // -GroundSampleDir; // gives axis -BentNormal i think  // so we sample ground not sky

		//uint SampleCardIdx = CardinalIndex(GroundSampleDir.xy); // Simplify sample dir to cardinal
		float OcclusionBandDist = 1000;  //Steps[SampleCardIdx]; // Num of texel we can go in sample dir before we hit a wall/slope  -- will fix later

		float MIN_SAMPLE_RADIUS = 10;                                                  //texel radius
		float RadialBand = lerp(MIN_SAMPLE_RADIUS, 1024 - 1, abs(GroundSampleDir.z));  // 1024 = terrain tex size
		float RadialLimit = min(OcclusionBandDist, RadialBand);
		RadialLimit = 40;

		float2 AmbCoordsUV = CoordsUV + (GroundSampleDir.xy * RadialLimit * rcp(1024));  // Radial distribution is implicit in sample dir xy   // will remove hardcoded size later

		float3 BounceRadiance = TerrainRadianceTex.SampleLevel(LinearSampler, AmbCoordsUV, 0) * 2;
		sh2RGB GroundSH = SphericalHarmonics::Scale(SphericalHarmonics::Evaluate(GroundSampleDir), BounceRadiance * GroundWeight);
		Output = SphericalHarmonics::Add(Output, GroundSH);
	}

	// debugging
	//sh2RGB skyTest = SphericalHarmonics::Scale(SphericalHarmonics::Evaluate(float3(0,0,1)), 2 * Math::PI * float3(0,0,0.3));
	//sh2RGB GroundTest = SphericalHarmonics::Scale(SphericalHarmonics::Evaluate(float3(0,0,-1)), 2 * Math::PI * float3(0,0.1,0));
	//Output = SphericalHarmonics::Add(skyTest, Output);
	//Output = SphericalHarmonics::Add(skyTest, GroundTest);
	//Output = SphericalHarmonics::Scale(SphericalHarmonics::Evaluate(float3(0,0,-1)), 2 * Math::PI * float3(0.5, 0.5, 0.5));

	//Output = SphericalHarmonics::Zero2RGB();
	//sh2RGB skyTest = SphericalHarmonics::Scale(SphericalHarmonics::Evaluate(float3(0,0,1)), SkyAperture.xxx);
	//Output = SphericalHarmonics::Add(Output, skyTest);

	SphericalHarmonics::PackSH2RGB(Output, ThreadID.xy, ProbeArray);
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

	float4 BentNormal = BentNormalTex[ThreadID.xy];
	float SkyAO = BentNormal.w;

	float3 SkyAmbient = float3(0.5, 0.5, 1.0) * SkyAO;  // for this factor to work we need to sample terrain accuratly by accounting for geo norm
	float3 AmbientLighting = 0;                         //SkyAmbient; // probs dont have amibent here since sky ambient is already is cone trace? idk - sky is part of bounce so maybe..
	float3 Lighting = 0.0;

	float SunShadow = GetDirOcclusion(ThreadID.xy);

	float2 CoordsUV = (ThreadID.xy + 0.5) * settings.InvGridTexSize.xy;

	float2 nCoord = float2(1024, 801);
	float3 NormalWS = NormalTex[CoordsUV * nCoord].xzy * 2.0 - 1.0;
	//float3 NormalWS = NormalTex[ThreadID.xy].xzy * 2.0 - 1.0; // swizzle since N is +Y up

	float2 aCoord = float2(1024, 814);
	float3 Albedo = AlbedoTex[CoordsUV * aCoord].xyz;  //AlbedoTex[ThreadID.xy];

	float NdotL = saturate(dot(NormalWS, SharedData::DirLightDirection));
	float3 DirLighting = SharedData::DirLightColor.xyz * SunShadow * NdotL * BRDF::Diffuse_Lambert();

	Lighting = DirLighting;  // + AmbientLighting;
	Lighting *= Albedo;

	//Lighting = float3(1,0,0);

	TerrainRelight[ThreadID.xy] = float4(Lighting, 1);
}
#endif

// sin = 1.0 is vertical wall
// Store first texel in dir x with sin >= 0.7ish only if the next texel is decreasing otherwise store max since thats the max height of hill
