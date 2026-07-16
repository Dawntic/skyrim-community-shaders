#include "CloudShadows/CloudShadows.hlsli"
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
		float visibility = srcOcclusionDepth.SampleCmpLevelZero(comparisonSampler, occlusionUV, cellCentreOS.z);

		//float Zenith = 90;
		//float rcpPdf = Math::PI * sin(radians(Zenith)) / max(settings.OcclusionDir.z, 0.05);
		//sh2 occlusionSH = SH::Scale(SH::Evaluate(settings.OcclusionDir.xyz), visibility * rcpPdf);
		sh2 occlusionSH = SH::Scale(SH::Evaluate(settings.OcclusionDir.xyz), visibility * 2 * Math::PI);
		//occlusionSH = SH::Add(occlusionSH, SH::Scale(SH::Evaluate(-settings.OcclusionDir.xyz), 0 * 2 * Math::PI));

		const float Y00 = 0.28209479f;
		const float Y1 = 0.48860251f;
		float c0 = occlusionSH.x;
		float3 c1 = occlusionSH.yzw;
		float l1 = length(c1);
		float dcVal = Y00 * c0;
		float amp = Y1 * l1;                         // reconstruction's linear amplitude
		float maxAllowed = min(1.0 - dcVal, dcVal);  // symmetric headroom around dcVal
		if (amp > maxAllowed && l1 > 1e-6) {
			float scale = max(maxAllowed, 0) / amp;
			//occlusionSH.yzw = c1 * scale; // doesnt rly work
		}

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
Texture2D GroundRadianceTex : register(t19);
Texture2D ReflectionDistribTex : register(t20);
Texture2D Card1 : register(t21);
Texture2D Card2 : register(t22);
Texture2D Diag1 : register(t23);
Texture2D Diag2 : register(t24);

RWTexture2DArray<float4> ProbeArray : register(u0);

static const float GOLDEN_ANGLE = 2.39996322972865332;  // PI * (3 - sqrt(5))

#	define SAMPLES 256     //256
#	define RAY_SAMPLES 16  //128

float2 LinearStep(float2 edge0, float2 edge1, float2 x)
{
	return saturate((x - edge0) / (edge1 - edge0));
}

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
	float cosT = lerp(1.0, 1 - ap, (i + 0.5) / n);  // ap == 2 gives full sphere
	float3 Out = float3(0, 0, cosT);
	sincos(i * GOLDEN_ANGLE, Out.y, Out.x);
	Out.xy *= sqrt(saturate(1.0 - cosT * cosT));
	return Out;
}

// Uniform (area-weighted) hemisphere/sphere sampler, stratified via Hammersley.
// ap == 1 -> hemisphere, solid angle 2PI.  ap == 2 -> full sphere.
// domain: xy[-1,1], z[1-ap, 1].
float3 UniformHemisphere(float i, float n, float ap)
{
	// radical inverse base 2 (van der Corput) for the second dimension
	uint bits = uint(i);
	bits = (bits << 16) | (bits >> 16);
	bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
	bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
	bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
	bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
	float u2 = float(bits) * 2.3283064365386963e-10;  // / 2^32

	float u1 = (i + 0.5) / n;  // stratified first dim

	float cosT = lerp(1.0, 1.0 - ap, u2);  // uniform in z -> area-uniform
	float phi = u1 * Math::PI * 2;

	float3 Out;
	sincos(phi, Out.y, Out.x);
	Out.xy *= sqrt(saturate(1.0 - cosT * cosT));
	Out.z = cosT;
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

float GetCloudProfile(float3 SamplePos, float Height)
{
	float Scroll = 0;
	float4 NoiseSample = CloudBaseTex.SampleLevel(LinearRepeatSampler, float3(SamplePos.xy, Scroll) * HeightScale, 0);  // Perlin-Worley + 3 octaves of worley
	float PerlinWorley = NoiseSample.x;
	float3 Worley = NoiseSample.yzw;

	float WorleyFBM = dot(Worley, float3(0.625, 0.25, 0.125));
	float cloudCover = Coverage2;

	// Method used in frost nova
	float layerDensity = GetDensityHeightGradientForPoint(SamplePos, CloudType, Height);
	float Density = layerDensity * LerpLinearStepClamped(PerlinWorley, 0.3, 1.0, 0.0, 1.0);
	float Coverage = pow(CloudCoverage, LerpLinearStep(Height, 0.7, 0.8, 1.0, 0.8));

	float Erosion = LerpLinearStepClamped(WorleyFBM, Coverage, 1.0, 0.0, 1.0);
	Erosion = LerpLinearStepClamped(Erosion, cloudCover, 1.0, 0.0, 1.0);

	Density = LerpLinearStepClamped(Density, Erosion, 1.0, 0.0, 1.0);

	return Density;
}

static const float3 CLOUD_AMBIENT = float3(0.4, 0.45, 0.5);  // flat skylight fill into the cloud
static const float CLOUD_MS_GAIN = 1.8;                      // flat multiple-scatter boost
void ComputeLightingV1(float density, float stepLength, float sunVisibility, CloudParticpatingMedium medium, inout float3 Inscattering, inout float Transmittance)
{
	float albedo = medium.scattering / medium.extinction;
	float Extinction = medium.extinction * density;
	float Tr = exp(-Extinction * stepLength);

	float3 Radiance = SharedData::DirLightColor.xyz * medium.phase * sunVisibility;
	Radiance = (Radiance + CLOUD_AMBIENT) * CLOUD_MS_GAIN;

	float3 inscatter = Radiance * albedo * (1.0 - Tr);

	Inscattering += inscatter * Transmittance;
	Transmittance *= Tr;
}

void RaymarchCloud(float3 worldDir, float3 cameraPosA, inout float3 Inscattering, inout float Transmittance)
{
	float GAME_UNIT_TO_KM = 1.428e-5;
	float3 cameraPos = cameraPosA.xyz * GAME_UNIT_TO_KM;
	cameraPos.z += groundRadius;

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

	//float3 Inscattering = float3(0,0,0);
	//float Transmittance = 1.0;

	CloudParticpatingMedium medium;
	medium.scattering = 10;
	medium.extinction = 25;
	medium.phase = CloudPhase(cosTheta);

	float TrDepthSum = 0.0;  // numerator   of Eq. 21
	float TrSum = 0.0;       // denominator of Eq. 21

	for (int i = 0; i < RAY_SAMPLES; i++) {
		float3 SamplePos = ray.direction * (RayT.x + i * StepLength) + cameraPos;
		float EnvelopeZ = GetEnvelopeRelativeZ(SamplePos, float2(bottomRadius, topRadius));

		float CloudDensity = GetCloudProfile(SamplePos, EnvelopeZ);  // + 1; // Tmp to make Optical depth low
		if (CloudDensity <= 0.0)
			continue;

		TrDepthSum += Transmittance * RayT.x;
		TrSum += Transmittance;

		CloudRaymarchStepState state;
		state.height = EnvelopeZ;
		state.rayStep = float4(0, 0, 0, StepLength);
		state.upVector = normalize(SamplePos);

		float sunVis = EnvelopeZ;
		ComputeLightingV1(CloudDensity, StepLength, sunVis, medium, Inscattering, Transmittance);
	}

	//float cloudDistance = (weightSum > 0.0) ? depthWeightedSum / weightSum : RayT.y;
	//float3 cloudCamPos  = ray.direction * cloudDistance;   // camera-relative; drop distanceSum entirely

	// ONLY works inside view frustum
	//float4 AP = PhysSky::SampleAp(normalize(cloudCamPos), length(cloudCamPos), 0.0, LinearSampler);
	//accum.totalInscattering = lerp(accum.totalInscattering, AP.xyz, AP.w);

	//InscattAccum = accum.totalInscattering;
	//TransAccum = accum.totalTransmittance;
}

struct HorizonData
{
	float4 SinC, SinD;    // CardinalOcclusionTex / DiagTex at probe UV
	float4 WallC, WallD;  // MeanHitDist bake, same layout
};

// azDir = normalize(SampleDir.xy + 1e-5)
void InterpAzimuth(float2 azDir, HorizonData H, out float sinH, out float OcclDist)
{
	// cosine-power hat weights against the 8 baked azimuths
	// CARD: E(+x) S(+y) W(-x) N(-y)   DIAG: (+ +)(- +)(- -)(+ -)
	float4 cC = azDir.xyxy * float2(1.0, -1.0).xxyy;  //float4(azDir.x, azDir.y, -azDir.x, -azDir.y);
	float4 cD = float4(azDir.x + azDir.y, -azDir.x + azDir.y, -azDir.x - azDir.y, azDir.x - azDir.y) * 0.70710678;

	float4 wC = pow(saturate(cC), 8);  // = saturate(cC); wC *= wC; wC *= wC; wC *= wC;   // cos^8
	float4 wD = pow(saturate(cD), 8);  // = saturate(wD); wD *= wD; wD *= wD; wD *= wD;

	float invSum = rcp(dot(wC, (float4)1.0) + dot(wD, (float4)1.0) + 1e-6);
	sinH = (dot(H.SinC, wC) + dot(H.SinD, wD)) * invSum;
	OcclDist = (dot(H.WallC, wC) + dot(H.WallD, wD)) * invSum;
}

#	define MIN_SAMPLE_RADIUS 10  // in texels

// Fix albedo and normal texture shape/size
// Some sections maybe picking up way more snow than others? - comes from GroundLight - probably too few sampling in SH

// Add volumetric effects to probe grid
// Add AO control

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;

	float GroundRadianceTexSize = 1024.0;  // remove hardcoded size later

	if (ThreadID.x >= settings.GridTexSize.x || ThreadID.y >= settings.GridTexSize.y)
		return;

	float2 CoordsUV = (ThreadID.xy + 0.5) * settings.InvGridTexSize.xy;

	float WorldHeight = HeightTex.SampleLevel(LinearSampler, float2(CoordsUV.x, 1.0 - CoordsUV.y), 0);  // * 65535;
																										//WorldHeight = (WorldHeight - 32767) * 8.0;

	float3 WorldPos = float3(lerp(settings.GridBounds.xy, settings.GridBounds.zw, float2(CoordsUV.x, 1 - CoordsUV.y)), WorldHeight);

	//debug
	//ProbeArray[ThreadID.xyz] = NormalTex.SampleLevel(LinearSampler, CoordsUV, 0);//float4(GroundRadianceTex.SampleLevel(LinearSampler, CoordsUV, 0).xyz, 1) * 6;//
	//float2 PlayerUV = LinearStep(settings.GridBounds.xy, settings.GridBounds.zw, FrameBuffer::CameraPosAdjust.xy);
	//	   PlayerUV.y = 1.0 - PlayerUV.y;
	//CoordsUV = PlayerUV;
	//WorldHeight = HeightTex.SampleLevel(LinearSampler, CoordsUV, 0);

	float4 BNSample = BentNormalTex.SampleLevel(LinearSampler, CoordsUV, 0);
	float3 BentNormalDir = BNSample.xyz * 2.0 - 1.0;  // - add bn back later
	float SkyAperture = BNSample.w;                   // AO

	// Cone Tracing
	float SkySolidAngle = 2.0 * Math::PI * SkyAperture;
	const float SkyWeight = SkySolidAngle / SAMPLES;

	float GroundAperture = 1.0 + (1.0 - SkyAperture);
	float GroundSolidAngle = 2.0 * Math::PI * GroundAperture;  // add the part of upper hemisphere thats occluded
	const float GroundWeight = GroundSolidAngle / SAMPLES;

	float3x3 BentTBN = BuildTBN(BentNormalDir);

	HorizonData HData;
	HData.SinC = Card1.SampleLevel(LinearSampler, CoordsUV, 0);
	HData.SinD = Diag1.SampleLevel(LinearSampler, CoordsUV, 0);
	HData.WallC = Card2.SampleLevel(LinearSampler, CoordsUV, 0);
	HData.WallD = Diag2.SampleLevel(LinearSampler, CoordsUV, 0);

	static const float ProbeHeightOffset = 100;  // world units
	float ProbeHeight = WorldHeight + ProbeHeightOffset;
	float MaxSampleDist = 25000;
	static const float MinSampleDistSq = 5000;

	float2 Extent = abs(settings.GridBounds.xy) + settings.GridBounds.zw;  // pull out later
	float2 WorldUnitsPerTexel = Extent / 1024;                             // remove 1024 later

	sh2vec3 Output = SH::ZeroSH2Vec3();
	for (int i = 0; i < SAMPLES; ++i) {
		float3 SkySampleDir = UniformHemisphere(i, SAMPLES, SkyAperture);
		SkySampleDir = mul(SkySampleDir, BentTBN);

		float3 SkyRadiance = SampleSkyRadiance(SkySampleDir) * 4.0;  // CHANGED

		float cloudTr = 1;
		float3 cloudInscattering = 0;
		RaymarchCloud(SkySampleDir, WorldPos, cloudInscattering, cloudTr);
		//cloudInscattering *= 0.1;

		SkyRadiance = SkyRadiance;  // * cloudTr + cloudInscattering;

		sh2vec3 SkySH = SH::Scale(SH::Evaluate(SkySampleDir), SkyRadiance * SkyWeight);
		Output = SH::Add(Output, SkySH);

		float3 GroundSampleDir = UniformHemisphere(i, SAMPLES, GroundAperture);
		GroundSampleDir.z = -GroundSampleDir.z;

		float sinH, OcclDist;
		InterpAzimuth(GroundSampleDir.xy, HData, sinH, OcclDist);  //only needed for OcclDist eh

		float GrSin = GroundSampleDir.z;
		float GrCos = sqrt(1 - GrSin * GrSin);

		float RayDist = min(ProbeHeight * GrCos / max(-GrSin, 1e-6), MaxSampleDist);
		float DeltaR = (GrSin > 0.0) ? OcclDist : RayDist;
		DeltaR = sqrt(DeltaR * DeltaR + MinSampleDistSq) / WorldUnitsPerTexel;

		float2 EnvOffset = float2(GroundSampleDir.x, -GroundSampleDir.y) * DeltaR * rcp(GroundRadianceTexSize);
		float3 BounceRadiance = GroundRadianceTex.SampleLevel(LinearSampler, CoordsUV + EnvOffset, 0).xyz;  // * 4;

		// The issue is if amb normal is pointing to ground then you get stronger ground light so overhangs are brighter...
		sh2vec3 GroundSH = SH::Scale(SH::Evaluate(GroundSampleDir), BounceRadiance * GroundWeight);
		Output = SH::Add(Output, GroundSH);

		// debug
		//ProbeArray[int3((CoordsUV + EnvOffset) * settings.GridTexSize.xy, 0)] = float4(1.0.xxx * 1, 1);
	}

	//Output = SH::Add(Output, DirOcclusionRGB);

	// debug
	//float3 BounceRadianceA = GroundRadianceTex.SampleLevel(LinearSampler, CoordsUV, 0);
	//ProbeArray[ThreadID.xyz] = float4(BounceRadianceA.xyz, 1);
	//ProbeArray[int3(PlayerUV * settings.GridTexSize.xy, 0)] = 1.0.xxxx;
	//float2 PlayerUV = LinearStep(settings.GridBounds.xy, settings.GridBounds.zw, FrameBuffer::CameraPosAdjust.xy);
	////PlayerUV.y = 1.0 - PlayerUV.y;
	//float sdf = length(CoordsUV - PlayerUV) - 0.008;
	//float sdf = length(WorldPos.xy - FrameBuffer::CameraPosAdjust.xy) - 2000;
	//if(sdf <= 0)
	//ProbeArray[ThreadID.xyz] = 1.0.xxxx;

	SH::PackSH2Vec3(Output, ThreadID.xy, ProbeArray);
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
Texture2D TexTrLut : register(t7);

RWTexture2D<float4> TerrainRelight : register(u0);

float3 SampleSkyRadiance(float3 rayDir)
{
	float azimuth = atan2(rayDir.y, rayDir.x);
	float u = azimuth * .5 * (1 / Math::PI);  // sampler wraps around so ok
	float zenith = asin(rayDir.z);
	float v = 0.5 - 0.5 * sign(zenith) * sqrt(abs(zenith) * 2 * (1 / Math::PI));
	v = max(v, 0.01);

	return SkyViewLUTTex.SampleLevel(LinearWrapSampler, frac(float2(u, v)), 0).rgb;
}

float3 SampleTr(float3 sunDir)
{
	SharedData::PhysSkyData data = SharedData::physSkyData;

	if (data.trMix < 1e-8)
		return 1;

	const float2 lutUv = PhysSky::TrLutUv(data.zCameraPlanet, sunDir.z);
	float3 tr = TexTrLut.SampleLevel(LinearWrapSampler, lutUv, 0).rgb;
	if (sunDir.z <= -0.414)
		tr = 0;
	tr = lerp(1, tr, data.trMix);

	return tr;
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

#	define DIR_LIGHT_MULT 1  // 0.2
#	define SKY_MULT 1
#	define ALBEDO_MULT 1.9  //account for dark tex - can maybe remove later

#	define AO_SCALE 1
#	define MIN_AMBIENT_LUM 0.3  // without this the contrast at dawn and dusk is just too high

// todo: fix Albedo map - wrong scale
[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;

	float2 TexSize = 1024;  // Fix me later
	float2 InvTexSize = 1 / TexSize;

	if (ThreadID.x >= TexSize.x || ThreadID.y >= TexSize.y)
		return;

	float2 CoordsUV = (ThreadID.xy + 0.5) * InvTexSize;

	float WorldHeight = HeightTex.SampleLevel(LinearSampler, float2(CoordsUV.x, 1.0 - CoordsUV.y), 0);
	float3 WorldPos = float3(lerp(settings.GridBounds.xy, settings.GridBounds.zw, float2(CoordsUV.x, 1.0 - CoordsUV.y)), WorldHeight);

	float4 BentNormal = BentNormalTex.SampleLevel(LinearSampler, CoordsUV, 0);
	//float3 BentNormalDir = BentNormal.xyz * 2.0 - 1.0;
	float BentNormalAO = BentNormal.w;
	float SkyAO = pow(BentNormalAO, AO_SCALE);
	float3 SkySampleDir = float3(0, 0, 1);  // looks better than using bent normal

	float3 NormalWS = NormalTex.SampleLevel(LinearSampler, CoordsUV, 0) * 2 - 1;
	float3 Albedo = AlbedoTex.SampleLevel(LinearSampler, CoordsUV, 0) * ALBEDO_MULT;
	//Albedo = Color::SkyrimGammaToLinear(Albedo) * Color::VanillaDiffuseColorMult(); // looks weird - surely lod isnt linear tho
	float3 EnvAlbedo = AlbedoTex.SampleLevel(LinearSampler, CoordsUV, 4) * ALBEDO_MULT;
	//EnvAlbedo = Color::SkyrimGammaToLinear(EnvAlbedo) * Color::VanillaDiffuseColorMult();

	// Ambient lighting //
	float NormalWeight = (1.0 + saturate(dot(NormalWS, SkySampleDir))) * 0.5;

	// Sky
	float SkyWeight = NormalWeight * SkyAO;

	float3 SkyRadiance = SampleSkyRadiance(SkySampleDir);
	float3 CloudRadiance = float3(0.5, 0.5, 0.5);  // no fast src for this yet
	float CloudShadow = 1;                         //CloudShadows::GetCloudShadowMult(WorldPos, LinearSampler);
	float3 SkyAmbient = lerp(CloudRadiance, SkyRadiance, CloudShadow);

	float3 SkyLight = SkyAmbient * SkyWeight;

	// Ground multi bounce approx
	float BounceWeight = (1.0 + 1.0 - BentNormalAO);
	BounceWeight += 1.0 - NormalWeight;
	BounceWeight *= saturate(Color::RGBToLuminance(SkyRadiance * SkyAO * Math::PI));  // approx sky irradiance otherwise ground is always lit

	float3 GroundLight = EnvAlbedo * BounceWeight;

	float3 AmbientLighting = SKY_MULT * SkyLight + GroundLight;
	float lum = Color::RGBToLuminance(AmbientLighting);
	float mult = MIN_AMBIENT_LUM / lum;
	AmbientLighting *= max(1, mult);

	// Direct lighting //
	float SunShadow = GetDirOcclusion(ThreadID.xy);
	float Shadow = SunShadow * max(CloudShadow, 0.5);  // 0.5 lim so cloud doesn't stomp dir light
	float NdotL = saturate(dot(NormalWS, SharedData::DirLightDirection));

	float llDirLightMult = SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear && !SharedData::InInterior ? SharedData::linearLightingSettings.dirLightMult : 1.0;
	float3 DirLightColor = Color::DirectionalLight(SharedData::DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * llDirLightMult;
	//DirLightColor *= SampleTr(normalize(SharedData::DirLightDirection.xyz));
	//DirLightColor = clamp(DirLightColor, 0, 1);

	DirLightColor = float3(1, 1, 1) * 3;  // Not const is inconsisent - fix later
	DirLightColor *= SampleTr(normalize(SharedData::DirLightDirection.xyz));
	DirLightColor *= DIR_LIGHT_MULT;

	float3 DirLighting = DirLightColor * Shadow * NdotL * BRDF::Diffuse_Lambert();

	float3 Lighting = (AmbientLighting + DirLighting);
	Lighting *= Albedo;

	//Lighting = Color::LinearToSkyrimGamma(Lighting);

	TerrainRelight[ThreadID.xy] = float4(Lighting, 1);
}
#endif
