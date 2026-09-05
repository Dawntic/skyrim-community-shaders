#include "CloudShadows/CloudShadows.hlsli"
#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "PhysicalSky/CloudCommon.hlsli"
#include "PhysicalSky/Common.hlsli"
#include "Skylighting/Skylighting.hlsli"
#include "TerrainShadows/TerrainShadows.hlsli"

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

Texture2D<float> HeightTex : register(t0);
Texture2D TerrainRelight : register(t1);
Texture2D AlbedoTex : register(t2);
Texture2D BentNormalTex : register(t3);
Texture2D CardinalHorizonTex : register(t4);
Texture2D DiagonalHorizonTex : register(t5);
Texture2D CardinalWallDistTex : register(t6);
Texture2D DiagonalWallDistTex : register(t7);
// placeholder for cloud noise
Texture2D NormalTex : register(t9);
Texture2D IBLSkySHTex : register(t77);
// t8 is cloud base density

RWTexture2DArray<float4> ProbeArray : register(u0);

#	define AZIMUTHS 8
#	define ELEVATIONS 8
#	define SAMPLES (AZIMUTHS * ELEVATIONS)
#	define CLOUD_RAY_SAMPLES 64  //128

static const float3 CLOUD_AMBIENT = float3(0.4, 0.45, 0.5);  // flat skylight fill into the cloud
static const float CLOUD_MS_GAIN = 1.8;
void ComputeLightingV1(float density, float stepLength, float sunVisibility, CloudParticpatingMedium medium, inout float3 Inscattering, inout float Transmittance)
{
	float3 albedo = medium.scattering / medium.extinction;
	float Extinction = medium.extinction.x * density;
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

struct HorizonData
{
	float4 SinC, SinD;    // CardinalOcclusionTex / DiagTex at probe UV
	float4 WallC, WallD;  // MeanHitDist bake, same layout
};

void AzimuthWeights(float2 azDir, out float4 wC, out float4 wD)
{
	float4 cC = azDir.xyxy * float2(1.0, -1.0).xxyy;  //float4(azDir.x, azDir.y, -azDir.x, -azDir.y);
	float4 cD = float4(azDir.x + azDir.y, -azDir.x + azDir.y, -azDir.x - azDir.y, azDir.x - azDir.y) * 0.70710678;

	wC = saturate(cC);
	wC *= wC;
	wC *= wC;
	wC *= wC;
	wD = saturate(cD);
	wD *= wD;
	wD *= wD;
	wD *= wD;

	float invSum = rcp(dot(wC, (float4)1.0) + dot(wD, (float4)1.0) + 1e-6);
	wC *= invSum;
	wD *= invSum;
}

void InterpAzimuth(float4 wC, float4 wD, HorizonData H, out float sinH, out float OcclDist)
{
	sinH = dot(H.SinC, wC) + dot(H.SinD, wD);

	float OcclWeight = dot(step(1e-4, H.WallC), wC) + dot(step(1e-4, H.WallD), wD);
	OcclDist = OcclWeight > 0.25 ? (dot(H.WallC, wC) + dot(H.WallD, wD)) / OcclWeight : 0.0;
}

float3 ConeSampleDir(float i)
{
	return Math::UniformSphereSample(i, SAMPLES);
}

float4 MakeConeSample(float i)
{
	float3 RayDir = ConeSampleDir(i);

	float GrCos = sqrt(1 - RayDir.z * RayDir.z);

	float2 Azimuth = normalize(float2(RayDir.x, -RayDir.y) + 1e-8);

	return float4(Azimuth, RayDir.z, GrCos);
}

float4 MakeConeWeightC(float i)
{
	float4 wC, wD;
	AzimuthWeights(normalize(ConeSampleDir(i).xy + 1e-5), wC, wD);
	return wC;
}

float4 MakeConeWeightD(float i)
{
	float4 wC, wD;
	AzimuthWeights(normalize(ConeSampleDir(i).xy + 1e-5), wC, wD);
	return wD;
}

// Atlas space, matching the CARD then DIAG bin order in TexGen/GenerateCacheMaps.hlsl
static const float2 ConeAzimuths[AZIMUTHS] = {
	float2(1, 0), float2(0, -1), float2(-1, 0), float2(0, 1),
	float2(0.70710678, -0.70710678), float2(-0.70710678, -0.70710678),
	float2(-0.70710678, 0.70710678), float2(0.70710678, 0.70710678)
};

float2 MakeConeElevation(float k)
{
	float GrSin = 1.0 - 2.0 * (k + 0.5) / ELEVATIONS;
	return float2(GrSin, sqrt(saturate(1.0 - GrSin * GrSin)));
}

#	define ELEV_1(k) MakeConeElevation(k)
#	define ELEV_4(k) ELEV_1(k), ELEV_1(k + 1), ELEV_1(k + 2), ELEV_1(k + 3)
#	define ELEV_8(k) ELEV_4(k), ELEV_4(k + 4)

static const float2 ConeElevations[ELEVATIONS] = { ELEV_8(0) };

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;

	if (ThreadID.x >= (uint)settings.GridTexSize.x || ThreadID.y >= (uint)settings.GridTexSize.y)
		return;

	float2 CoordsUV = (ThreadID.xy + 0.5) * rcp(settings.GridTexSize);

	// gives correct height values
	const float2 atlasMin = settings.AtlasBounds.xy;
	const float2 atlasMax = settings.AtlasBounds.zw;
	const float2 AtlasUVPerWorldUnit = rcp(atlasMax - atlasMin);
	float3 WorldPos = float3(lerp(atlasMin, atlasMax, float2(CoordsUV.x, 1 - CoordsUV.y)), 0);

	// debug
	//WorldPos.xy = FrameBuffer::CameraPosAdjust.xy;
	if (any(ThreadID.xyz != 0)) {
		//ProbeArray[uint3(ThreadID.xyz)] = 0.0.xxxx;
		//return;
	}

	float2 AtlasUV = LinearStep(atlasMin, atlasMax, WorldPos.xy);
	AtlasUV.y = 1 - AtlasUV.y;

	float WorldHeight = HeightTex.SampleLevel(LinearSampler, AtlasUV, 0);
	WorldPos.z = WorldHeight;

	static const float2 HorizonTaps[4] = {
		float2(-0.25, -0.25), float2(0.25, -0.25), float2(-0.25, 0.25), float2(0.25, 0.25)
	};
	const float2 ProbeFootprintUV = rcp((float2)settings.GridTexSize);

	HorizonData HData;
	HData.SinC = 0;
	HData.SinD = 0;
	HData.WallC = 0;
	HData.WallD = 0;
	float4 WallCountC = 0;
	float4 WallCountD = 0;
	[unroll] for (int tap = 0; tap < 4; ++tap)
	{
		float2 TapUV = AtlasUV + HorizonTaps[tap] * ProbeFootprintUV;
		float4 TapWallC = CardinalWallDistTex.SampleLevel(LinearSampler, TapUV, 0);
		float4 TapWallD = DiagonalWallDistTex.SampleLevel(LinearSampler, TapUV, 0);
		HData.SinC += CardinalHorizonTex.SampleLevel(LinearSampler, TapUV, 0);
		HData.SinD += DiagonalHorizonTex.SampleLevel(LinearSampler, TapUV, 0);
		HData.WallC += TapWallC;
		HData.WallD += TapWallD;
		WallCountC += step(1e-4, TapWallC);
		WallCountD += step(1e-4, TapWallD);
	}
	HData.SinC *= 0.25;
	HData.SinD *= 0.25;
	HData.WallC /= max(WallCountC, 1.0);
	HData.WallD /= max(WallCountD, 1.0);

	static const float MaxReach = 50000;
	static const float MinReach = 8000;

	static const float SampleSolidAngle = 4.0 * Math::PI / SAMPLES;

	float4 BNSample = BentNormalTex.SampleLevel(LinearSampler, AtlasUV, 0);
	float3 BentNormalDir = BNSample.xyz * 2.0 - 1.0;
	float SkyAperture = BNSample.w;  // AO

	sh2vec3 SkyAverageSH = SH::UnpackSH2Vec3(IBLSkySHTex);

	const float HorizonBand = max(settings.HorizonBand, 1e-5);
	const float HorizonBias = settings.HorizonBias;

	// Cone Tracing
	sh2vec3 ResultSH = SH::ZeroSH2Vec3();
	float HorizonSin[AZIMUTHS] = { HData.SinC[0], HData.SinC[1], HData.SinC[2], HData.SinC[3],
		HData.SinD[0], HData.SinD[1], HData.SinD[2], HData.SinD[3] };
	float HorizonDist[AZIMUTHS] = { HData.WallC[0], HData.WallC[1], HData.WallC[2], HData.WallC[3],
		HData.WallD[0], HData.WallD[1], HData.WallD[2], HData.WallD[3] };

	for (uint i = 0; i < SAMPLES; ++i) {
		uint a = i / ELEVATIONS;
		float2 Azimuth = ConeAzimuths[a];
		float sinH = HorizonSin[a];
		float Reach = clamp(HorizonDist[a], MinReach, MaxReach);

		float2 ConeElevation = ConeElevations[i % ELEVATIONS];
		float GrSin = ConeElevation.x;
		float GrCos = ConeElevation.y;
		float3 RayDir = float3(Azimuth.x * GrCos, -Azimuth.y * GrCos, GrSin);

		float SkyWeight = smoothstep(-HorizonBand, HorizonBand, GrSin - sinH + HorizonBias);

		float3 Radiance = 0;

		if (SkyWeight > 0.0) {
			float3 SkyRadiance = PhysSky::SampleSky(RayDir, 0.0, LinearWrapSampler);  // * settings.SkyInfluence;

			//float cloudTr = 1;
			//float3 cloudInscattering = 0;
			//RaymarchCloud(RayDir, WorldPos, cloudInscattering, cloudTr);

			//SkyRadiance = SkyRadiance * cloudTr + cloudInscattering;

			Radiance += SkyRadiance * SkyWeight;
		}

		if (SkyWeight >= 1.0) {
			ResultSH = SH::Add(ResultSH, SH::Scale(SH::Evaluate(RayDir), Radiance * SampleSolidAngle));
			continue;
		}

		float t = saturate((sinH - GrSin) / (sinH + 1.0));
		float DeltaR = Reach * sqrt(1.0 - t);

		// This gives us first surface pos in dir = RayDir
		float2 UVOffset = Azimuth * DeltaR * AtlasUVPerWorldUnit;
		float2 RaySampleUV = AtlasUV + UVOffset;

		/*
		float BounceHeight = HeightTex.SampleLevel(LinearSampler, RaySampleUV, 0).x;
		float3 SampleWorldPos = float3(lerp(atlasMin, atlasMax, float2(RaySampleUV.x, 1 - RaySampleUV.y)), BounceHeight);

		float3 BounceAlbedo = AlbedoTex.SampleLevel(LinearSampler, RaySampleUV, 0).xyz;
		float4 BounceBN = BentNormalTex.SampleLevel(LinearSampler, RaySampleUV, 0);
		float BouncedSkyVis = BounceBN.w;

		float3 BounceNormal = NormalTex.SampleLevel(LinearSampler, RaySampleUV, 0).xyz * 2.0 - 1.0;

		// Sky Bounce
		float3 BounceSkyIrradiance = BouncedSkyVis * SH::FuncProductIntegral(SkyAverageSH, SH::EvaluateCosineLobe(-BounceNormal));
		float3 SkyBounce = BounceAlbedo * BounceSkyIrradiance * (1.0 / Math::PI);

		// Sun Bounce
		float NdotL = saturate(dot(BounceNormal, SharedData::DirLightDirection.xyz));
		float SunShadow = TerrainShadows::GetTerrainShadow(SampleWorldPos, LinearSampler);
		float llDirLightMult = SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear && !SharedData::InInterior ? SharedData::linearLightingSettings.dirLightMult : 1.0;
		float3 DirLightColor = Color::DirectionalLight(SharedData::DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * llDirLightMult;
		float3 SunBounce = BounceAlbedo * NdotL * SunShadow * DirLightColor * BRDF::Diffuse_Lambert();

		Radiance += (SkyBounce + SunBounce) * (1.0 - SkyWeight);
		*/

		Radiance += TerrainRelight.SampleLevel(LinearSampler, RaySampleUV, 0).xyz * (1.0 - SkyWeight);  //

		ResultSH = SH::Add(ResultSH, SH::Scale(SH::Evaluate(RayDir), Radiance * SampleSolidAngle));

		// debug
		//ProbeArray[int3((RaySampleUV)*settings.GridTexSize.xy, 0)] = float4(1.0.xxx, 1);
	}

	//ProbeArray[uint3(ThreadID.xyz)] = SkyAperture.xxxx * 0.5;
	SH::PackSH2Vec3(ResultSH, ThreadID.xy, ProbeArray);
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
Texture2D IBLSkySHTex : register(t77);

RWTexture2D<float4> TerrainRelight : register(u0);

float GetDirOcclusion(float2 AtlasUV)
{
	float Visibility;

	//float4 Card = CardinalOcclusionTex[PxCoords];
	//float4 Diag = CardinalOcclusionDiagTex[PxCoords];
	float4 Card = CardinalOcclusionTex.SampleLevel(LinearSampler, AtlasUV, 0);
	float4 Diag = CardinalOcclusionDiagTex.SampleLevel(LinearSampler, AtlasUV, 0);

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
#	define ALBEDO_MULT 1.0  //1.9  //account for dark tex - can maybe remove later

#	define AO_SCALE 1
#	define MIN_AMBIENT_LUM 0.3  // without this the contrast at dawn and dusk is just too high

// todo: fix Albedo map - wrong scale
[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;

	//float2 settings.EnvRadianceTexSize = 1024;  // Fix me later
	//float2 InvEnvRadianceTexSize = 1 / settings.EnvRadianceTexSize;

	if (ThreadID.x >= (uint)settings.EnvRadianceTexSize.x || ThreadID.y >= (uint)settings.EnvRadianceTexSize.y)
		return;

	float2 CoordsUV = (ThreadID.xy + 0.5) * settings.InvEnvRadianceTexSize;

	const float2 atlasMin = settings.AtlasBounds.xy;
	const float2 atlasMax = settings.AtlasBounds.zw;
	const float2 AtlasUVPerWorldUnit = rcp(atlasMax - atlasMin);
	float3 WorldPos = float3(lerp(atlasMin, atlasMax, float2(CoordsUV.x, 1.0 - CoordsUV.y)), 0);

	float2 AtlasUV = LinearStep(atlasMin, atlasMax, WorldPos.xy);
	AtlasUV.y = 1 - AtlasUV.y;

	float3 NormalWS = NormalTex.SampleLevel(LinearSampler, AtlasUV, 0).xyz * 2.0 - 1.0;

	float3 Albedo = AlbedoTex.SampleLevel(LinearSampler, AtlasUV, 0).rgb;
	Albedo = Color::ColorToLinear(Albedo) * Color::VanillaDiffuseColorMult();
#	if defined(LOD_BLENDING)
	Albedo = pow(abs(Albedo), SharedData::lodBlendingSettings.LODTerrainGamma) * SharedData::lodBlendingSettings.LODTerrainBrightness;
#	endif
	Albedo = Color::IrradianceToLinear(Albedo) * ALBEDO_MULT;

	float4 BentNormal = BentNormalTex.SampleLevel(LinearSampler, AtlasUV, 0);
	//float3 BentNormalDir = BentNormal.xyz * 2.0 - 1.0;

	/*
	float WorldHeight = HeightTex.SampleLevel(LinearSampler, AtlasUV, 0);

	float SkyAO = pow(BentNormal.w, AO_SCALE);
	float3 SkySampleDir = float3(0, 0, 1);  // looks better than using bent normal

	//Albedo = Color::SkyrimGammaToLinear(Albedo) * Color::VanillaDiffuseColorMult(); // looks weird - surely lod isnt linear tho
	float3 EnvAlbedo = AlbedoTex.SampleLevel(LinearSampler, AtlasUV, 4) * ALBEDO_MULT;
	//EnvAlbedo = Color::SkyrimGammaToLinear(EnvAlbedo) * Color::VanillaDiffuseColorMult();

	// Ambient lighting //
	float NormalWeight = (1.0 + saturate(dot(NormalWS, SkySampleDir))) * 0.5;

	// Sky
	float SkyWeight = NormalWeight * SkyAO;

	float3 SkyRadiance = PhysSky::SampleSky(SkySampleDir, 0.0, LinearWrapSampler);
	float3 CloudRadiance = float3(0.5, 0.5, 0.5);  // no fast src for this yet
	float CloudShadow = 1;                         //CloudShadows::GetCloudShadowMult(WorldPos, LinearSampler);
	float3 SkyAmbient = lerp(CloudRadiance, SkyRadiance, CloudShadow);

	float3 SkyLight = SkyAmbient * SkyWeight;

	// Ground multi bounce approx
	float BounceWeight = 1.0;  //(1.0 + 1.0 - BentNormal.w);
	BounceWeight += 1.0 - NormalWeight;
	BounceWeight *= saturate(Color::RGBToLuminance(SkyRadiance * SkyAO * Math::PI));  // approx sky irradiance otherwise ground is always lit

	float3 GroundLight = EnvAlbedo * BounceWeight;

	float3 AmbientLighting = 0;  //SKY_MULT * SkyLight + GroundLight;
	float lum = Color::RGBToLuminance(AmbientLighting);
	float mult = MIN_AMBIENT_LUM / lum;
	//AmbientLighting *= max(1, mult);

	*/

	sh2vec3 SkyAverageSH = SH::UnpackSH2Vec3(IBLSkySHTex);
	float3 SkyIrradiance = BentNormal.w * max(0, SH::FuncProductIntegral(SkyAverageSH, SH::EvaluateCosineLobe(-NormalWS)));
	float3 AmbientLighting = SkyIrradiance * (1.0 / Math::PI);

	// Direct lighting //
	float SunShadow = GetDirOcclusion(AtlasUV.xy);
	float Shadow = SunShadow;  // * max(CloudShadow, 0.5);  // 0.5 lim so cloud doesn't stomp dir light
	float NdotL = saturate(dot(NormalWS, SharedData::DirLightDirection.xyz));

	float llDirLightMult = SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear && !SharedData::InInterior ? SharedData::linearLightingSettings.dirLightMult : 1.0;
	float3 DirLightColor = Color::DirectionalLight(SharedData::DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * llDirLightMult;
	DirLightColor = Color::IrradianceToLinear(DirLightColor);

	float3 DirLighting = NdotL * Shadow * DirLightColor * BRDF::Diffuse_Lambert();

	float3 Lighting = (AmbientLighting + DirLighting);
	Lighting *= Albedo;

	//Lighting = Color::LinearToSkyrimGamma(Lighting);

	TerrainRelight[ThreadID.xy] = float4(Lighting, 1);
}
#endif
