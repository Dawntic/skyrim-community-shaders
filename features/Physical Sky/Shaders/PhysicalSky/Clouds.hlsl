// Copyright 2022-2026 Nikita Fediuchin. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "PhysicalSky/CloudCommon.hlsli"

#define STEP_SIZE_FACTOR 1.0

struct VertexOut
{
	noperspective float2 TexCoord: TEXCOORD0;
	float4 Position: SV_POSITION;
};

struct PixelOut
{
	float4 color: SV_TARGET0;
	float4 depth: SV_TARGET1;
};

#ifdef CLOUD_VS

VertexOut main(uint vertexID : SV_VertexID)
{
	VertexOut output;
	output.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
	output.Position = float4(output.TexCoord * float2(2, -2) - float2(1, -1), 0, 1);
	return output;
}
#endif

#ifdef CLOUD_PS

#	include "PhysicalSky/Common.hlsli"

#	define USE_CAMERA_VOLUME
#	define USE_CLOUDS_DEPTH
#	define USE_CLOUDS_REPROJECTION

#	define BaseScale 0.001
#	define DetailScale 0.001
// TODO:
// change transmittance/extinction to scalar

// Temp diff does weird things to cumulus
// Reprojection causes depth bug

//powder_sugar_effect = 1.0 - exp(-light_samples * 2.0);
//beers_law = exp(-light_samples);
//light_energy = 2.0 * beers_law * powder_sugar_effect;

//float mipmap_level = log2(1.0 + abs(inRaymarchInfo.mDistance * cVoxelFineDetailMipMapDistanceScale));

float GetCloudLayerDensity(float EnvelopeZ, float cloudType)
{
	EnvelopeZ = saturate(EnvelopeZ);

	float cumulus = max(0.0, LerpLinearStep(EnvelopeZ, 0.0, 0.2, 0.0, 1.0) * LerpLinearStep(EnvelopeZ, 0.7, 0.9, 1.0, 0.0));
	float stratocumulus = max(0.0, LerpLinearStep(EnvelopeZ, 0.0, 0.2, 0.0, 1.0) * LerpLinearStep(EnvelopeZ, 0.2, 0.7, 1.0, 0.0));
	float stratus = max(0.0, LerpLinearStep(EnvelopeZ, 0.0, 0.1, 0.0, 1.0) * LerpLinearStep(EnvelopeZ, 0.2, 0.3, 1.0, 0.0));

	float d1 = lerp(stratus, stratocumulus, saturate(cloudType * 2.0));
	float d2 = lerp(stratocumulus, cumulus, saturate((cloudType - 0.5) * 2.0));
	return lerp(d1, d2, cloudType);
}

// Base scale increases with roughness using method gpu gems 7
// alpha pisc method can scale density

// Low freq base cloud shape
float GetBaseDensity(float3 pos, float Height)
{
	// Weather map and type
	//tileScale = LerpLinearStep(tileScale, 0.0, 1.0, 0.0, 0.001); //
	float3 cloudInfo = WeatherMapTex.SampleLevel(LinearRepeatSampler, pos.xy * (WeatherScale + 1e-6f), 0).xyz;
	float cloudCover = cloudInfo.r;
	float cloudType = cloudInfo.b;  // 0 = stratus, 1 = cumulus, .5 = stratocumulus

	// Sample low res shape
	//float tileScale2 = 0.001; //0.00002;
	float4 NoiseSample = CloudBaseTex.SampleLevel(LinearRepeatSampler, pos * BaseScale, 0);  // Perlin-Worley + 3 octives of worley
	float PerlinWorley = NoiseSample.x;
	float3 Worley = NoiseSample.yzw;

	// FBM from low freq worley noises
	float Erosion = dot(Worley, float3(0.625, 0.25, 0.125));  //float Erosion = 0.625 * profileNoise.y + 0.25 * profileNoise.z + 0.125 * profileNoise.w;

	// Create base cloud shape by dilating it with the Worley FBM
	float BaseCloud = LerpLinearStep(PerlinWorley, -(1.0 - Erosion), 1.0, 0.0, 1.0);

	// NEED ACTUAL DENSITY CONTROL LIKE OLD SHADER

	// Method from realtime-volumetric-cloudscapes
	//BaseCloud = LerpLinearStep(PerlinWorley, (Erosion - 0.9), 1.0, 0.0, 1.0);
	//float base_cloud_with_coverage = remapClampedBeforeAndAfter ( baseCloud, cloud_coverage, 1.0, 0.0, 1.0);
	//base_cloud_with_coverage *= cloud_coverage;
	//return base_cloud_with_coverage;

	//Height = 0.5;
	// Get the density−Height gradient using the density Height
	// function explained in Section 4.3.2.
	//float layerDensity = GetCloudLayerDensity(Height, cloudType);
	//float layerDensity = GetDensityHeightGradientForPoint(pos, cloudType, Height);
	//BaseCloud *= layerDensity;

	// Add cloud coverage //
	// Only one of these is needed?

	// Method from GPU gems 7
	BaseCloud = LerpLinearStepClamped(BaseCloud, cloudCover, 1.0, 0.0, 1.0);
	BaseCloud *= cloudCover;  // Mult so smaller clouds are lighter
	////

	//Method from alpha pisc
	// Remap coverage so low coverage settings aggressively clear sky, high settings fill it
	//float baseCoverage = max(BaseCloud - (1.0 - pow2(coverage)) * 0.8, 0.0);
	//BaseCloud = baseCoverage * (1.0 - pow2(1.0 - coverage));
	////

	//return BaseCloud;

	// Method from frost nova  - BEST BASE SO FAR
	//float density = layerDensity * LerpLinearStepClamped(PerlinWorley, 0.3, 1.0, 0.0, 1.0);
	//float baseCoverageA = pow(cloudCover, LerpLinearStep(Height, 0.7, 0.8, 1.0, 0.8));
	//baseCoverageA = pow(cloudCover, LerpLinearStep(Height, 0.7, 0.8, 1.0, lerp(1.0, 0.5, 0.9 )));
	//Erosion = LerpLinearStepClamped(Erosion, baseCoverageA, 1.0, 0.0, 1.0);
	// density = LerpLinearStepClamped(density, Erosion, 1.0, 0.0, 1.0);

	return BaseCloud;  //density;
					   ////
}

/*
float GetVertProfile(float Height)
{
	float top = VertProfileTex.SampleLevel(LinearRepeatSampler, float2(Scattering.x, Height), 0);
	float bottom = VertProfileTex.SampleLevel(LinearRepeatSampler, float2(Scattering.y, Height), 0);
	return top * bottom;
}

// Create base cloud shape by dilating Worley FBM
float GetBaseDensity(float3 pos, float Height)
{
    float3 cloudInfo = WeatherMapTex.SampleLevel(LinearRepeatSampler, pos.xy * (WeatherScale + 1e-6f), 0).xyz;
    //float cloudCover = cloudInfo.r; // This is dog shit
    float cloudType = cloudInfo.b; // 0 = stratus, 1 = cumulus, .5 = stratocumulus

	float4 AlphaPi = Base.SampleLevel(LinearRepeatSampler, pos.xy * BaseScale, 0).xxxx;

	//float3 Scale = float3(Scale1, Scale2, Scale3);   //De-coupling coord scaling can give some good art styles
    float4 NoiseSample = CloudBaseTex.SampleLevel(LinearRepeatSampler, float3(pos.xy, 1) * BaseScale, 0);  // Perlin-Worley + 3 octaves of worley
	float PerlinWorley = lerp(AlphaPi.x, NoiseSample.x, CloudType); // Extrapolating here is very interesting
	float3 Worley = NoiseSample.yzw;

	float WorleyFBM = dot(Worley, float3(0.625, 0.25, 0.125));

	float layerDensity = GetDensityHeightGradientForPoint(pos, cloudType, Height);

	float Coverage = pow(CloudCoverage, LerpLinearStep(Height, 0.7, 0.8, 1.0, 0.8)); // Anvil bias

	// Anvil top + densified base
	//float topBias    = LerpLinearStep(Height, 0.7, 0.8, 1.0, 0.8);
	//float bottomBias = LerpLinearStep(Height, 0.0, 0.15, 0.7, 1.0);  // tighter coverage at base
	//float Coverage = pow(CloudCoverage, topBias * bottomBias);

	float Erosion = LerpLinearStepClamped(WorleyFBM, Coverage, 1.0, 0.0, 1.0);

	float Prof = GetVertProfile(Height);

	// Main
	//float Density = layerDensity * LerpLinearStep(PerlinWorley, 0.0, 1.0, 0.0, 1.0); // arg1 = bottom density
   // Density = LerpLinearStep(Density, Erosion, 1.0, 0.0, 1.0);

	// Intersting
	//float Density = HeightScale * LerpLinearStep(PerlinWorley, 0.0, 1.0, 0.0, 1.0);
	//Density = LerpLinearStep(Density, Erosion, 1.0, 0.0, 1.0);

	float Density = Prof * CloudCoverage * LerpLinearStep(PerlinWorley, 0.0, 1.0, 0.0, 1.0); // arg1 = bottom density
    //Density = LerpLinearStep(Density, Erosion, 1.0, 0.0, 1.0);


	// Test

	float TopGradient = pow(1.0 - Height, 1.5);
	float BottomGradient = pow(Height, 2.0);
	float EdgeGradient = LerpLinearStep(pos.z, 0.0, 35.0, 1.0, 0.0);
	float DimProfile = BottomGradient * TopGradient * EdgeGradient;
	//float DensityTest = LerpLinearStep(PerlinWorley, 0.0, 1.0, 0.0, 1.0); // arg1 = bottom density
	//DensityTest = LerpLinearStep(PerlinWorley, -(1.0 - WorleyFBM), 1.0, 0.0, 1.0);


	return Density;
}



float GetBaseDensity(float3 pos, float Height)
{
	float4 NoiseSample = CloudBaseTex.SampleLevel(LinearRepeatSampler, float3(pos.xy, 10) * BaseScale, 0);  // Perlin-Worley + 3 octaves of worley
	float PerlinWorley = NoiseSample.x * 2;
	float3 Worley = NoiseSample.yzw;

	//float Density = LerpLinearStep(PerlinWorley, CloudCoverage, 1.0, 0.0, 1.0) * HeightScale; // higher cirr is softer
	//float Density = LerpLinearStep(PerlinWorley, CloudCoverage, 1.0, 0.0, 1.0) * HeightScale;
	float Density = LerpLinearStep(PerlinWorley, 0.0, 1.0, 0.0, 1.0);

	return Density;
}
*/
//PowderedSugar = 1.0 - exp(-light_samples * 2.0);
//beers_law = exp(-light_samples);
//light_energy = 2.0 * beers_law * PowderedSugar;

//float PowderedSugar = 1.0 - exp(-lightOpticalDepth * 2.0);
//LightTransmittance = 2.0 * LightTransmittance * PowderedSugar;

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
	float3 sampleInSctrInt = (sampleInSctr - sampleInSctr * CloudTransmittance) / Extinction;
	accumState.totalInscattering += sampleInSctrInt * accumState.totalTransmittance;
	accumState.totalTransmittance *= CloudTransmittance;
}

/*
// Vertical profiling method from Nubis^3: https://d3d3g8mu99pzk9.cloudfront.net/AndrewSchneider/Nubis%20Cubed.pdf
float GetCloudProfile(float3 SamplePos, float Height)
{
	float3 cloudData = DataFieldTex.SampleLevel(LinearRepeatSampler, SamplePos.xy * HeightScale, 0).xyz;
	float2 TopBottomType = cloudData.zx;
	float Cover = cloudData.y;

	float topProfile = VertProfileTex.SampleLevel(LinearSampler, float2(min(TopBottomType.x, CloudType), Height), 0).x;
	float bottomProfile = VertProfileTex.SampleLevel(LinearSampler, float2(TopBottomType.y, Height), 0).y;

	float VertProfile = topProfile * bottomProfile;

	float Coverage = saturate(LerpLinearStep(Cover, CloudCoverage, 1.0f, 0.0f, 1.0f));

	return VertProfile * Coverage;
}
*/

//Worley noise if inverted and used in a FBM approximates a nice fractal
//billowing pattern. It can also be used to add detail to the low-density regions of
//the low-frequency Perlin noise. (See Figure 4.7, left and center.) We do this by
//remapping the Perlin noise using the Worley noise FBM as the minimum value
//from the original range:   LerpLinearStep(Perlin, WorleyFBM, 1.0, 0.0, 1.0);

// Todo in future:
// Add height density gradient
// Get high rez detail

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

PixelOut main(VertexOut input)
{
	PixelOut output;

	output.color = float4(0, 0, 0, 0);
	output.depth = 1.0;

	float GAME_UNIT_TO_KM = 1.428e-5;
	float3 cameraPos = cameraPosIN.xyz * GAME_UNIT_TO_KM;
	cameraPos.z += groundRadius;
	//cameraPos = float3(0,0, groundRadius);

	float2 CoordsNDC = input.TexCoord * 2.0 - 1.0;
	float4 worldPosition = mul(FrameBuffer::CameraViewProjInverse, float4(CoordsNDC.x, -CoordsNDC.y, 0.5, 1.0));

	Ray ray;
	ray.origin = cameraPos;
	ray.direction = normalize(worldPosition);

	// .x = cloud entry, .y = cloud exit
	float2 RayT = raycast2(float4((float3)0, topRadius), ray);
	if (!isIntersected(RayT))  // No atmosphere intersection so no clouds.
		return output;
	RayT.x = max(RayT.x, minDistance);

	float2 RayB = raycast2(float4((float3)0.0, bottomRadius), ray);
	if (isIntersected(RayB))                                                                              // Check if intersecting bottom clouds sphere
		RayT = RayB.x < 0.0 ? float2(max(RayT.x, RayB.y), RayT.y) : float2(RayT.x, min(RayT.y, RayB.x));  // Is camera below bottom clouds level?

	// Skipping ray if whole planet is behind us.
	if (RayT.y <= RayT.x || RayT.x > maxDistance)
		return output;

	// clip march to opaque scene
	float sceneDepth = DepthTex.SampleLevel(LinearSampler, input.TexCoord, 0).x;
	float4 worldPos = mul(FrameBuffer::CameraViewProjInverse, float4(float2(CoordsNDC.x, -CoordsNDC.y), sceneDepth, 1.0));
	worldPos.xyz = worldPos.xyz / worldPos.w;
	float sceneDistKm = length(worldPos.xyz) * GAME_UNIT_TO_KM;
	if (sceneDepth < 1.0) {
		RayT.y = min(RayT.y, sceneDistKm);
		if (RayT.y <= RayT.x)
			return output;
	}

	float disocclusion = DisocculsionTex.SampleLevel(LinearSampler, input.TexCoord, 0.0).x;
	if (disocclusion == 0) {
		float4 prevNdcPos = mul(FrameBuffer::CameraPreviousViewProjUnjittered, float4(normalize(worldPos.xyz), 0.0));
		prevNdcPos.xyz = prevNdcPos.xyz / prevNdcPos.w;
		float2 prevTexCoords = float2(prevNdcPos.x, -prevNdcPos.y) * 0.5 + 0.5;
		if (bayerPos.x != -1.0 && all(prevTexCoords < 1.0 && prevTexCoords > 0.0)) {
			float prevDepth = DepthTex.SampleLevel(LinearSampler, prevTexCoords, 0);
			if (all((uint2)input.Position.xy % 4 != (uint2)bayerPos) && (prevDepth <= sceneDepth)) {
				output.color = PrevFrameCloudTex.SampleLevel(LinearSampler, prevTexCoords, 0);
				output.depth = PrevFrameCloudDepthTex.SampleLevel(LinearSampler, prevTexCoords, 0).x;
				return output;
			}
		}
	}

	//// Raymarching ////////////////////////

#	define RAY_SAMPLES 512  //128

	float cosTheta = dot(ray.direction, SharedData::DirLightDirection.xyz);
	float StepLength = (RayT.y - RayT.x) / RAY_SAMPLES;

	float3 Inscattering = float3(0, 0, 0);
	float Transmittance = 1.0;

	CloudParticpatingMedium medium;
	medium.scattering = 10;
	medium.extinction = 25;
	medium.phase = CloudPhase(cosTheta);

	float TrDepthSum = 0.0;
	float TrSum = 0.0;

	for (int i = 0; i < RAY_SAMPLES; i++) {
		float3 SamplePos = ray.direction * (RayT.x + i * StepLength) + cameraPos;
		float EnvelopeZ = GetEnvelopeRelativeZ(SamplePos, float2(bottomRadius, topRadius));

		float CloudDensity = GetCloudProfile(SamplePos, EnvelopeZ);
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
	/////////////////////////////////////////////

	output.color = float4(Color::LLLinearToGamma(Inscattering), max(1.0 - Transmittance, 1e-6));

	//float cloudDistance = (TrSum > 0.0) ? TrDepthSum / TrSum : RayT.y;
	//float3 cloudCamPos  = ray.direction * cloudDistance;
	//float4 clipPos = mul(FrameBuffer::CameraViewProj, float4(cloudCamPos / GAME_UNIT_TO_KM, 1.0));
	//output.depth = saturate(clipPos.z / clipPos.w);

	return output;
}
#endif
/////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
#ifdef CLOUD_BLEND_PS

Texture2D CloudColorTex : register(t0);
Texture2D CloudDepthTex : register(t1);
Texture2D DepthTexA : register(t2);

SamplerState LinearSamplerA : register(s0);

float4 main(VertexOut input) : SV_TARGET0
{
	//float depth = DepthTexA.SampleLevel(LinearSamplerA, input.TexCoord, 0).x;
	//float cloudDepth = CloudDepthTex.SampleLevel(LinearSamplerA, input.TexCoord, 0).x;
	//if (depth < cloudDepth){
	//	discard;
	//}

	float4 Cloud = CloudColorTex.SampleLevel(LinearSamplerA, input.TexCoord, 0);

	return Cloud;
}
#endif
