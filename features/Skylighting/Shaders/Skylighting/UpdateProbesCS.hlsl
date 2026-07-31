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

		sh2 occlusionSH = SH::Scale(SH::Evaluate(settings.OcclusionDir.xyz), visibility * 2 * Math::PI);

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

Texture2D HeightTex : register(t0);
Texture2D SkyViewLUTTex : register(t1);
// t8 is cloud base density

RWTexture2DArray<float4> ProbeArray : register(u0);

#	define SAMPLES 64            //256
#	define CLOUD_RAY_SAMPLES 64  //128

// These matches physical sky
float2 SkyViewLutUv(float3 rayDir)
{
	float azimuth = atan2(rayDir.y, rayDir.x);
	float u = azimuth * .5 * rcp(Math::PI);  // sampler wraps around so ok
	float zenith = asin(rayDir.z);
	float v = 0.5 - 0.5 * sign(zenith) * sqrt(abs(zenith) * 2 * rcp(Math::PI));
	v = max(v, 0.01);
	return frac(float2(u, v));
}

float3 SampleSky(float3 viewDir, SamplerState sampSv)
{
	SharedData::PhysSkyData data = SharedData::physSkyData;

	const float2 skyLutUv = SkyViewLutUv(viewDir);
	float3 skyColor = SkyViewLUTTex.SampleLevel(sampSv, skyLutUv, 0).rgb;

	if (data.tonemapper == 1)
		skyColor = Color::LLLinearToGamma(skyColor);
	else if (data.tonemapper == 2)
		skyColor = skyColor / (1 + skyColor);

	return skyColor;
}

// Alt method without tonemap
float3 SampleSkyRadiance(float3 rayDir)
{
	float azimuth = atan2(rayDir.y, rayDir.x);
	float u = azimuth * .5 * (1 / Math::PI);  // sampler wraps around so ok
	float zenith = asin(rayDir.z);
	float v = 0.5 - 0.5 * sign(zenith) * sqrt(abs(zenith) * 2 * (1 / Math::PI));
	v = max(v, 0.01);

	return SkyViewLUTTex.SampleLevel(LinearWrapSampler, frac(float2(u, v)), 0).rgb;
}

static const float3 CLOUD_AMBIENT = float3(0.4, 0.45, 0.5);  // flat skylight fill into the cloud
static const float CLOUD_MS_GAIN = 1.8;
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
	float StepLength = (RayT.y - RayT.x) / CLOUD_RAY_SAMPLES;

	CloudParticpatingMedium medium;
	medium.scattering = CloudScattering;
	medium.extinction = CloudExtinction;
	medium.phase = CloudPhase(cosTheta);

	for (int i = 0; i < CLOUD_RAY_SAMPLES; i++) {  // 128 samples
		float3 SamplePos = ray.direction * (RayT.x + i * StepLength) + cameraPos;
		float EnvelopeZ = GetEnvelopeRelativeZ(SamplePos, float2(bottomRadius, topRadius));

		float CloudDensity = GetCloudProfile(SamplePos, EnvelopeZ);
		if (CloudDensity <= 0.0)
			continue;

		CloudRaymarchStepState state;
		state.height = EnvelopeZ;
		state.rayStep = float4(0, 0, 0, StepLength);
		state.upVector = normalize(SamplePos);

		float sunVis = 0;
		ComputeLightingV1(CloudDensity, StepLength, sunVis, medium, Inscattering, Transmittance);
	}

	// ONLY works inside view frustum
	//float4 AP = PhysSky::SampleAp(normalize(cloudCamPos), length(cloudCamPos), 0.0, LinearSampler);
	//accum.totalInscattering = lerp(accum.totalInscattering, AP.xyz, AP.w);
}

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;

	if (ThreadID.x >= settings.GridTexSize.x || ThreadID.y >= settings.GridTexSize.y)
		return;

	float2 CoordsUV = (ThreadID.xy + 0.5) * rcp(settings.GridTexSize);

	// gives correct height values
	const float2 atlasMin = settings.AtlasBounds.xy;
	const float2 atlasMax = settings.AtlasBounds.zw;
	float3 WorldPos = float3(lerp(atlasMin, atlasMax, float2(CoordsUV.x, 1 - CoordsUV.y)), 0);

	float2 AtlasUV = LinearStep(atlasMin, atlasMax, WorldPos.xy);
	AtlasUV.y = 1.0 - AtlasUV.y;

	float WorldHeight = HeightTex.SampleLevel(LinearSampler, AtlasUV, 0);
	WorldHeight -= 14000;  // since map is offset
	WorldPos.z = WorldHeight;

	// Cone Tracing
	float SkySolidAngle = 2.0 * Math::PI;
	const float SkyWeight = SkySolidAngle / SAMPLES;

	sh2vec3 Output = SH::ZeroSH2Vec3();

	//float3 SkyRadianceTest = SampleSkyRadiance(float3(0,0,1));
	//Output = SH::Scale(SH::HemisphereSH2(), SkyRadianceTest);

	for (int i = 0; i < SAMPLES; ++i) {
		float3 SkySampleDir = Math::UniformHemisphere(i, SAMPLES);

		//float3 SkyRadiance = SampleSkyRadiance(SkySampleDir) * settings.SkyInfluence;  // Why does this mult need to be like 4.0? should work at 1.0
		float3 SkyRadiance = SampleSky(SkySampleDir, LinearWrapSampler) * settings.SkyInfluence;

		float cloudTr = 1;
		float3 cloudInscattering = 0;
		RaymarchCloud(SkySampleDir, WorldPos, cloudInscattering, cloudTr);

		SkyRadiance = SkyRadiance * cloudTr + cloudInscattering;

		sh2vec3 SkySH = SH::Scale(SH::Evaluate(SkySampleDir), SkyRadiance * SkyWeight);
		Output = SH::Add(Output, SkySH);
	}

	//ProbeArray[ThreadID.xyz] = SkyRadianceTest.xyzz;

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

float GetDirOcclusion(float2 CoordsUV)
{
	float Visibility;

	//float4 Card = CardinalOcclusionTex[PxCoords];
	//float4 Diag = CardinalOcclusionDiagTex[PxCoords];
	float4 Card = CardinalOcclusionTex.SampleLevel(LinearSampler, CoordsUV, 0);
	float4 Diag = CardinalOcclusionDiagTex.SampleLevel(LinearSampler, CoordsUV, 0);

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

	//float2 settings.EnvRadianceTexSize = 1024;  // Fix me later
	//float2 InvEnvRadianceTexSize = 1 / settings.EnvRadianceTexSize;

	if (ThreadID.x >= settings.EnvRadianceTexSize.x || ThreadID.y >= settings.EnvRadianceTexSize.y)
		return;

	float2 CoordsUV = (ThreadID.xy + 0.5) * settings.InvEnvRadianceTexSize;

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
	float SunShadow = GetDirOcclusion(CoordsUV.xy);
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
