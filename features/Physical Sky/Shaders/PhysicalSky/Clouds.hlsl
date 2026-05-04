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

// TODO:
// change transmittance/extinction to scalar

// Temp diff does weird things to cumulus
// Reprojection causes depth bug

//powder_sugar_effect = 1.0 - exp(-light_samples * 2.0);
//beers_law = exp(-light_samples);
//light_energy = 2.0 * beers_law * powder_sugar_effect;

//float mipmap_level = log2(1.0 + abs(inRaymarchInfo.mDistance * cVoxelFineDetailMipMapDistanceScale));

// Flat earth theory
float GetEnvelopeRelativeZ(float3 inPosition, float2 inCloudMinMax)
{
	return saturate(LinearStep(inCloudMinMax.x, inCloudMinMax.y, inPosition.z));
}

float calcRelativeHeightA(float3 SamplePos, float PlanetCenterToEnvelopeBottom, float InvVertEnvelopeSize)
{
	float3 projPos = normalize(SamplePos) * PlanetCenterToEnvelopeBottom;
	return saturate(length(SamplePos - projPos) * InvVertEnvelopeSize);
}

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

float GetGradientHeightFactor(float Height, int cloudtype)
{
	float timewidthup, starttimeup, starttimedown;
	// Stratus clouds
	if (cloudtype == 0) {
		timewidthup = 0.08;
		starttimeup = 0.08;
		starttimedown = 0.2;
	}  // Cumulus clouds
	else if (cloudtype == 1) {
		timewidthup = 0.14;
		starttimeup = 0.1;
		starttimedown = 0.5;
	}
	// Cumulunimbus clouds
	else if (cloudtype == 2) {
		timewidthup = 0.2;
		starttimeup = 0.10;
		starttimedown = 0.7;
	}

	float factor = 2.0 * Math::PI / (2.0 * timewidthup);

	float density_gradient = 0.0;
	// Gradient functions dependent on the cloudtype
	if (Height < starttimeup)
		density_gradient = 0.0;
	else if (Height < starttimeup + timewidthup)
		density_gradient = 0.5 * sin(factor * Height - Math::PI / 2.0 - factor * starttimeup) + 0.5;
	else if (Height < starttimedown)
		density_gradient = 1.0;
	else if (Height < starttimedown + timewidthup)
		density_gradient = 0.5 * sin(factor * Height - Math::PI / 2.0 - factor * (starttimedown + timewidthup)) + 0.5;
	else
		density_gradient = 0.0;

	return density_gradient;
}

float GetDensityHeightGradientForPoint(float3 pointT, float weather_data, float relativeHeight)
{
	float cloudt = weather_data;
	// Cloud type: {0: Stratus, 1: Cumulus, 2: Cumulonimbus}
	int cloudtype = 1;
	if (cloudt < 0.1)
		cloudtype = 0;
	else if (cloudt > 0.9)
		cloudtype = 2;

	//cloudtype = 2;
	// Relative Height from [0 - 1]
	//float relh = pointT.y;
	//float relativeHeight = GetRelativeHeightInAtmosphere(pointT, earthCenter);

	// Get gradient function defined for three type of clouds
	return GetGradientHeightFactor(relativeHeight, cloudtype);
}

/*
// Low freq base cloud shape
float GetBaseDensity(float3 pos, float Height) {

    // Weather map and type
	//tileScale = LerpLinearStep(tileScale, 0.0, 1.0, 0.0, 0.001); //
    float3 cloudInfo = WeatherMapTex.SampleLevel(LinearRepeatSampler, pos.xy * (WeatherScale + 1e-6f), 0).xyz;
    float cloudCover = cloudInfo.r;
    float cloudType = cloudInfo.b; // 0 = stratus, 1 = cumulus, .5 = stratocumulus

    // Sample low res shape
	//float tileScale2 = 0.001; //0.00002;
    float4 NoiseSample = CloudBaseTex.SampleLevel(LinearRepeatSampler, pos * BaseScale, 0);  // Perlin-Worley + 3 octives of worley
	float PerlinWorley = NoiseSample.x;
	float3 Worley = NoiseSample.yzw;

    // FBM from low freq worley noises
	float Erosion = dot(Worley, float3(0.625, 0.25, 0.125));     //float Erosion = 0.625 * profileNoise.y + 0.25 * profileNoise.z + 0.125 * profileNoise.w;

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
    float layerDensity = GetDensityHeightGradientForPoint(pos, cloudType, Height);
	BaseCloud *= layerDensity;

    // Add cloud coverage //
	// Only one of these is needed?

	// Method from GPU gems 7
	BaseCloud = LerpLinearStepClamped(BaseCloud, cloudCover, 1.0, 0.0, 1.0);
	BaseCloud *= cloudCover; // Mult so smaller clouds are lighter
	////

	//Method from alpha pisc
	// Remap coverage so low coverage settings aggressively clear sky, high settings fill it
    //float baseCoverage = max(BaseCloud - (1.0 - pow2(coverage)) * 0.8, 0.0);
    //BaseCloud = baseCoverage * (1.0 - pow2(1.0 - coverage));
	////

	//return BaseCloud;

	// Method from frost nova  - BEST BASE SO FAR
	float density = layerDensity * LerpLinearStepClamped(PerlinWorley, 0.3, 1.0, 0.0, 1.0);
	float baseCoverageA = pow(cloudCover, LerpLinearStep(Height, 0.7, 0.8, 1.0, 0.8));
	//baseCoverageA = pow(cloudCover, LerpLinearStep(Height, 0.7, 0.8, 1.0, lerp(1.0, 0.5, 0.9 )));
    Erosion = LerpLinearStepClamped(Erosion, baseCoverageA, 1.0, 0.0, 1.0);
    density = LerpLinearStepClamped(density, Erosion, 1.0, 0.0, 1.0);

	return density;
	////
}
*/

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
	float cloudType = cloudInfo.b;  // 0 = stratus, 1 = cumulus, .5 = stratocumulus

	float4 AlphaPi = Base.SampleLevel(LinearRepeatSampler, pos.xy * BaseScale, 0).xxxx;

	//float3 Scale = float3(Scale1, Scale2, Scale3);   //De-coupling coord scaling can give some good art styles
	float4 NoiseSample = CloudBaseTex.SampleLevel(LinearRepeatSampler, float3(pos.xy, 1) * BaseScale, 0);  // Perlin-Worley + 3 octaves of worley
	float PerlinWorley = lerp(AlphaPi.x, NoiseSample.x, temperatureDiff);                                  // Extrapolating here is very interesting
	float3 Worley = NoiseSample.yzw;

	float WorleyFBM = dot(Worley, float3(0.625, 0.25, 0.125));

	float layerDensity = GetDensityHeightGradientForPoint(pos, cloudType, Height);

	float Coverage = pow(cumulusCoverage, LerpLinearStep(Height, 0.7, 0.8, 1.0, 0.8));  // Anvil bias

	// Anvil top + densified base
	//float topBias    = LerpLinearStep(Height, 0.7, 0.8, 1.0, 0.8);
	//float bottomBias = LerpLinearStep(Height, 0.0, 0.15, 0.7, 1.0);  // tighter coverage at base
	//float Coverage = pow(cumulusCoverage, topBias * bottomBias);

	float Erosion = LerpLinearStepClamped(WorleyFBM, Coverage, 1.0, 0.0, 1.0);

	float Prof = GetVertProfile(Height);

	// Main
	//float Density = layerDensity * LerpLinearStep(PerlinWorley, 0.0, 1.0, 0.0, 1.0); // arg1 = bottom density
	// Density = LerpLinearStep(Density, Erosion, 1.0, 0.0, 1.0);

	// Intersting
	//float Density = cirrusCoverage * LerpLinearStep(PerlinWorley, 0.0, 1.0, 0.0, 1.0);
	//Density = LerpLinearStep(Density, Erosion, 1.0, 0.0, 1.0);

	float Density = Prof * cumulusCoverage * LerpLinearStep(PerlinWorley, 0.0, 1.0, 0.0, 1.0);  // arg1 = bottom density
																								//Density = LerpLinearStep(Density, Erosion, 1.0, 0.0, 1.0);

	// Test
	/*
	float TopGradient = pow(1.0 - Height, 1.5);
	float BottomGradient = pow(Height, 2.0);
	float EdgeGradient = LerpLinearStep(pos.z, 0.0, 35.0, 1.0, 0.0);
	float DimProfile = BottomGradient * TopGradient * EdgeGradient;

	//float DensityTest = LerpLinearStep(PerlinWorley, 0.0, 1.0, 0.0, 1.0); // arg1 = bottom density
	//DensityTest = LerpLinearStep(PerlinWorley, -(1.0 - WorleyFBM), 1.0, 0.0, 1.0);
	*/

	return Density;
}

/*
float GetBaseDensity(float3 pos, float Height)
{
	float4 NoiseSample = CloudBaseTex.SampleLevel(LinearRepeatSampler, float3(pos.xy, 10) * BaseScale, 0);  // Perlin-Worley + 3 octaves of worley
	float PerlinWorley = NoiseSample.x * 2;
	float3 Worley = NoiseSample.yzw;

	//float Density = LerpLinearStep(PerlinWorley, cumulusCoverage, 1.0, 0.0, 1.0) * cirrusCoverage; // higher cirr is softer
	//float Density = LerpLinearStep(PerlinWorley, cumulusCoverage, 1.0, 0.0, 1.0) * cirrusCoverage;
	float Density = LerpLinearStep(PerlinWorley, 0.0, 1.0, 0.0, 1.0);

	return Density;
}
*/

float BillowCloud(float3 pos, float3 curl)
{
	float _LOW_BILLOWY_CURL_STR = 0;
	float _LOW_BILLOWY_FREQ = 0;
	float _HIGH_BILLOWY_CURL_STR = 0;
	float _HIGH_BILLOWY_FREQ = 0;

	float4 densityNoise = CloudDetailTex.SampleLevel(LinearRepeatSampler, DetailScale * pos, 0);  // Worley noise in increasing freq
	//float HiFreqFBM = dot(densityNoise.xyz, float3(0.625, 0.25, 0.125));
	float lowFreq = densityNoise.x;
	float highFreq = densityNoise.w;

	float3 lowFreqPos = pos + curl * exp2(_LOW_BILLOWY_CURL_STR - 1.0);
	lowFreqPos *= exp2(_LOW_BILLOWY_FREQ - 1.0);

	//float lowFreq = DetailOne.SampleLevel(LinearRepeatSampler, lowFreqPos, 0).x;
	float3 highFreqPos = pos + curl * exp2(_HIGH_BILLOWY_CURL_STR);
	highFreqPos *= exp2(_HIGH_BILLOWY_FREQ);

	//float highFreq = DetailTwo.SampleLevel(LinearRepeatSampler, highFreqPos, 0).x;
	return pow3(1.0 - lowFreq) * 0.6 + pow2(0.5 - highFreq) * 0.5;
}

float GetDetailDensity(float3 pos, float BaseCloudShape, float Height, float curlStrength)
{
	// Curl noise on the bottom
	//float CurlCoordScale = 0.01; //0.0001;
	float3 curl = CurlNoiseTex.SampleLevel(LinearRepeatSampler, pos.xy * CurlScale, 0).xyz;
	curl = curl * 2.0 - 1.0;
	pos += 2.0 * curlStrength * curl;

	//float CoordScale = 0.02; //0.04; //0.0004;
	float4 densityNoise = CloudDetailTex.SampleLevel(LinearRepeatSampler, DetailScale * pos, 0);  // Worley noise in increasing freq
	float HiFreqFBM = dot(densityNoise.xyz, float3(0.625, 0.25, 0.125));
	float Erosion = lerp(HiFreqFBM, 1.0 - HiFreqFBM, saturate(Height * 10.0));

	float Scaler = 0.5;  //
	float Detail = LerpLinearStepClamped(BaseCloudShape, Erosion * Scaler, 1.0, 0.0, 1.0);

	return BaseCloudShape;

	// return Detail;

	// Alpha pisc
	// Erode the base with billowy detail to create the characteristic cauliflower bottom
	float detail1Billowy = BillowCloud(pos, curl);
	float bottomDetail = 1.0 - detail1Billowy * smoothstep(0.1, 0.0, Height) * 2.0;
	Detail = LinearStep(saturate(detail1Billowy), 1.0, BaseCloudShape) * bottomDetail;

	return Detail;

	// Add some t u r b u l e n c e t o bottoms o f c l o u d s .
	//p.xy += curl_noise.xy ∗ (1.0 − Height ) ;

	// Sample high−frequency noises.
	//float3 densityNoise = tex3Dlod ( Cloud3DNoiseTextureB , Cloud3DNoiseSamplerB , float4 ( p ∗ 0.1 , mip_level ) ). rgb ;

	// Transition from wispy shapes to billowy shapes over Height.
	//float modifer = lerp(HiFreqFBM, 1.0 - HiFreqFBM, saturate(Height * 10));

	// Erode the base cloud shape with the distorted
	// high−frequency Worley noises.
	//float final_cloud = LerpLinearStep(BaseCloudShape, modifer * 0.5, 1.0, 0.0, 1.0);

	//return final_cloud;
}

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

PixelOut main(VertexOut input)
{
	PixelOut output;

	float3 DirLightDirection = SharedData::DirLightDirection.xyz;

	output.color = float4(0, 0, 0, 0);
	output.depth = 1.0;

	float GAME_UNIT_TO_KM = 1.428e-5;
	float3 cameraPos = cameraPosIN.xyz * GAME_UNIT_TO_KM;
	cameraPos.z += groundRadius;
	//cameraPos = float3(0, 0, groundRadius);

	float2 CoordsNDC = input.TexCoord * 2.0 - 1.0;
	float4 worldPosition = mul(FrameBuffer::CameraViewProjInverse[0], float4(CoordsNDC.x, -CoordsNDC.y, 0.5, 1.0));

	Ray ray;
	ray.origin = cameraPos;
	ray.direction = normalize(worldPosition);

	// .x = min ray dist, .y = max ray dist
	// .x = cloud entry, .y = cloud exit
	// used for cirrus layer
	float2 RayT = raycast2(float4((float3)0, topRadius), ray);
	if (!isIntersected(RayT))  // No atmosphere intersection so no clouds.
		return output;

	float cirrusT = RayT.x < 0.0 ? RayT.y : RayT.x;
	RayT.x = max(RayT.x, minDistance);

	// used for cumulus layer
	float2 RayB = raycast2(float4((float3)0.0, bottomRadius), ray);
	// Check if intersecting bottom clouds sphere
	if (isIntersected(RayB))
		RayT = RayB.x < 0.0 ? float2(max(RayT.x, RayB.y), RayT.y) : float2(RayT.x, min(RayT.y, RayB.x));  // Is camera below bottom clouds level?

	float RayLen = RayT.y - RayT.x;

#	ifdef USE_CLOUDS_DEPTH
	float MaxNDCDepth = 0.9999999999;
	float sceneDepth = DepthTex.SampleLevel(LinearSampler, input.TexCoord, 0);
	float4 worldPos = mul(FrameBuffer::CameraViewProjInverse[0], float4(float2(CoordsNDC.x, -CoordsNDC.y), sceneDepth, 1.0));
	worldPos.xyz = worldPos.xyz / worldPos.w;
	float3 worldPosKM = worldPos.xyz * GAME_UNIT_TO_KM;
	//RayT.y = (sceneDepth <= MaxNDCDepth) ? min(RayT.y, length((worldPosKM))) : RayT.y; // needs fixing
#	endif  // USE_CLOUDS_DEPTH

	// Skipping ray if whole planet is behind us.
	if (RayT.y <= RayT.x || RayT.x > maxDistance)
		return output;

#	ifdef USE_CLOUDS_REPROJECTION
	float disocclusion = DisocculsionTex.SampleLevel(LinearSampler, input.TexCoord, 0.0).x;
	if (disocclusion == 0) {
		float4 prevNdcPos = mul(FrameBuffer::CameraPreviousViewProjUnjittered[0], float4(normalize(worldPos.xyz), 0.0));
		prevNdcPos.xyz = prevNdcPos.xyz / prevNdcPos.w;
		float2 prevTexCoords = float2(prevNdcPos.x, -prevNdcPos.y) * 0.5 + 0.5;

		if (bayerPos.x != -1.0 && all(prevTexCoords < 1.0 && prevTexCoords > 0.0)) {
			float prevDepth = DepthTex.SampleLevel(LinearSampler, prevTexCoords, 0);
			if (all((uint2)input.Position.xy % 4 != (uint2)bayerPos) && (prevDepth <= sceneDepth)) {
				output.color = PrevFrameCloudTex.SampleLevel(LinearSampler, prevTexCoords, 0);
#		ifdef USE_CLOUDS_DEPTH
				output.depth = PrevFrameCloudDepthTex.SampleLevel(LinearSampler, prevTexCoords, 0).x;
#		endif
				return output;
			}
		}
	}
#	endif  // USE_CLOUDS_REPROJECTION

	// Raymarching //////////////////////////////////

	// abs atmos pos
	float VertEnvelopeSize = topRadius - bottomRadius;
	float InvVertEnvelopeSize = rcp(VertEnvelopeSize);

	float cosTheta = dot(ray.direction, DirLightDirection);
	float hgScattering = hgPhaseCloud(cosTheta);
	float hgMultiScatt = hgPhase(0.3, cosTheta);
	float OpacityAccum = 0.0, directIntensity = 0.0, ambientIntensity = 0.0, distanceSum = 0.0;

	float stepMul = 3.0;
	uint missCount = 0;
	bool fastMarching = true;

	CloudRaymarchAccumState accum;
	accum.totalInscattering = float3(0, 0, 0);
	accum.totalTransmittance = 1.0;

	CloudParticpatingMedium medium;
	medium.scattering = 10;                     //float3(0.18067349140471628, 0.18215551958414714, 0.19358579492341665);
	medium.extinction = 25;                     //float3(0.18067367051236774, 0.18215559145291857, 0.19358580629452912);
	float SETTING_CLOUDS_CU_PHASE_RATIO = 0.1;  /////////////////////
	medium.phase = CloudPhase(cosTheta, SETTING_CLOUDS_CU_PHASE_RATIO);

	float ATMOSPHERE_THICKNESS = topRadius - bottomRadius;  //100.0;
	while (RayT.x < RayT.y && OpacityAccum < 1.0) {
		float stepSize = 0.01 * ATMOSPHERE_THICKNESS;

		float3 SamplePos = ray.direction * RayT.x + cameraPos;
		float EnvelopeZ = GetEnvelopeRelativeZ(SamplePos, float2(bottomRadius, topRadius));

		float cuMinHeight = bottomRadius;
		float cuMaxHeight = topRadius;
		float lightSampleHeight = length(SamplePos);
		//EnvelopeZ = LinearStep(cuMinHeight, cuMaxHeight, lightSampleHeight);

		float StepBaseDensity = GetBaseDensity(SamplePos, EnvelopeZ);

		float3 cloudData = sampleDataFields(cameraPos, SamplePos, float3(0, 0, 0), cirrusCoverage);
		float verticalProfile = calcVerticalProfile(temperatureDiff, cloudData, EnvelopeZ);
		float cloudCoverage = calcCloudCoverage(cumulusCoverage, cloudData);
		StepBaseDensity = verticalProfile * cloudCoverage;  // Dimensional profile

		if (StepBaseDensity > 0.0) {  // Ray has hit cloud outline
			missCount = 0;
			// Walk back one step and switch to high rez ray march
			if (fastMarching) {
				RayT.x -= stepSize;
				stepMul = 1.0;
				fastMarching = false;
				continue;
			}

			// only use low freq clouds while debugging
			//float StepDetailDensity = GetDetailDensity(SamplePos, StepBaseDensity, EnvelopeZ, stepSize); // STEP SIZE PROBS WRONG
			float StepDetailDensity = StepBaseDensity;

			if (StepDetailDensity < 1e-6) {
				RayT.x += stepSize;
				continue;
			}

			// Calculate cloud detail density, marching toward dir light
			uint SAMPLES = 10;
			const float LIGHT_RCP = 1.0 / float(SAMPLES);
			const float HalfRcpSamples = 0.5 * LIGHT_RCP;
			float lightRayLen = 2.0;  // 2 km — covers any reasonable cloud thickness

			float POS_SCALE = cirrusCoverage;  //0.02;

			float StepLightDensity = 0.0;
			float lightStepSize = 0.01;
			//float3 LightSamplePos = SamplePos;
			for (uint step = 0; step < SAMPLES; step++) {  // 256 meters with 10 samples
				float x = (float(step) + 0.5) * LIGHT_RCP;
				float3 LightSamplePos = SamplePos + DirLightDirection * lightRayLen * pow2(x);
				float Height = GetEnvelopeRelativeZ(LightSamplePos, float2(bottomRadius, topRadius));
				if (Height > 1.0)
					break;  // exited cloud top — no more occlusion
				//Height = LinearStep(cuMinHeight, cuMaxHeight, length(LightSamplePos));
				//float loDensity = GetBaseDensity(LightSamplePos, Height);

				float3 data = sampleDataFields(cameraPos, LightSamplePos, float3(0, 0, 0), POS_SCALE);
				float height = calcRelativeHeight(bottomRadius, cumulusCoverage, LightSamplePos, rcp(topRadius - bottomRadius));
				float profile = calcVerticalProfile(temperatureDiff, data, Height) * calcCloudCoverage(cumulusCoverage, data);
				float loDensity = profile;  //calcCloudDensity(cameraPos, LightSamplePos, data, profile, float3(0,0,0));

				if (loDensity > 0.0) {  // only use low freq clouds while debugging
					float lightStepLen = 4.0 * x * HalfRcpSamples * lightRayLen;
					StepLightDensity += loDensity * lightStepLen;
					//StepLightDensity += GetDetailDensity(LightSamplePos, loDensity, Height, stepSize, clouds);// * lightStepSize; // STEP SIZE PROBS WRONG
				}
				lightStepSize *= 1.5;
			}
			//StepLightDensity *= densityFactor * (float)SAMPLES;  // 10 samples

			//StepBaseDensity *= clouds.temperatureDiff * (densityFactor * clouds.stepSizeFactor);

			float cloudTransmittance = (1.0 - OpacityAccum);
			float StepContrib = StepBaseDensity * cloudTransmittance;

			CloudRaymarchStepState state;
			state.height = EnvelopeZ;
			state.rayStep = float4(0, 0, 0, stepSize);  // only .w used
			state.upVector = SamplePos / EnvelopeZ;

			ComputeLighting(StepDetailDensity, StepLightDensity, cosTheta, state, medium, accum);

			float StepContrib = StepDetailDensity * accum.totalTransmittance;
			distanceSum += RayT.x * StepContrib;
		} else if (!fastMarching) {
			// No cloud found - switch to low rez march after 10 misses
			if (missCount++ >= 10) {
				fastMarching = true;
				stepMul = 3.0;
			}
		}
		RayT.x += stepSize;
	}
	/////////////////////////////////////////////

	OpacityAccum = clamp(1.0 - accum.totalTransmittance, 1e-6, 1.0);

	float3 cloudCamPos = ray.direction * (distanceSum / OpacityAccum);
#	ifdef USE_CAMERA_VOLUME
	//#define PHYSICAL_SKY
	float4 ap = (float4)0;
	//ap = PhysSky::SampleAp(normalize(cloudCamPos), input.Position.xy, length(cloudCamPos), LinearSampler); // looks cooked
	//accum.totalInscattering = lerp(accum.totalInscattering, ap.rgb, ap.a);
#	endif

	output.color = float4(Color::LinearToGamma(accum.totalInscattering), OpacityAccum);

#	ifdef USE_CLOUDS_DEPTH
	float4 clipPos = mul(FrameBuffer::CameraViewProj[0], float4(cloudCamPos / GAME_UNIT_TO_KM, 1.0));
	output.depth = saturate(clipPos.z / clipPos.w);
#	endif

	return output;
}
#endif
/////////////////////////////////////////////////////////////////////////

//float test = 0;
//float lightSampleDensityLod = 0;//
//TESTDensity(SamplePos, Height, true, test, lightSampleDensityLod);
//if(test > 0)
//	StepLightDensity += test;

//float cloudTransmittance = (1.0 - OpacityAccum);
//float StepContrib = StepDetailDensity * cloudTransmittance;
//float lightTransmittance = beerLambertCloud(StepLightDensity, cosTheta);
//float multiScattering = 0.2;//calcMultiScattering(hgMultiScatt, stepSize, cloudData, EnvelopeZ, cloudCoverage, StepBaseDensity, lightTransmittance);
//float directScattering = mad(lightTransmittance, hgScattering, multiScattering);
//directIntensity += directScattering * StepContrib;
//float StepTr = beerLambertCloud(StepDetailDensity, cosTheta);
//float ambientScattering = pow(1.0 - StepBaseDensity, 0.5) * StepTr;
//ambientIntensity += ambientScattering * StepContrib;

//lightEnergy = directIntensity * float(1,1,1);
//lightEnergy = Uncharted2Tonemap(lightEnergy);
//float3 cloudWorldPos = cloudCamPos + float3(0.0f, groundRadius, 0.0f);
//float3 TransmittanceA = PhysSky::SampleTr(DirLightDirection, LinearSampler);
//float3 directLight = LightColor * (TransmittanceA * directIntensity);
//float3 ambientLight = AmbientLightingColor * ambientIntensity * 0.2;// * (1.0 / densityFactor);
//float3 lightEnergy = saturate(directLight + ambientLight);

////////////////////////////////////////////////////////////////////////
#ifdef CLOUD_BLEND_PS

Texture2D CloudColorTex : register(t0);
Texture2D CloudDepthTex : register(t1);
Texture2D DepthTex : register(t2);

SamplerState LinearSampler : register(s0);

float4 main(VertexOut input) : SV_TARGET0
{
	float depth = DepthTex.SampleLevel(LinearSampler, input.TexCoord, 0).x;
	float cloudDepth = CloudDepthTex.SampleLevel(LinearSampler, input.TexCoord, 0).x;
	if (depth < cloudDepth)
		discard;

	float4 Cloud = CloudColorTex.SampleLevel(LinearSampler, input.TexCoord, 0);
	float MaxNDCDepth = 1;  //0.9999999999;
	//if (depth > MaxNDCDepth)
	//	Cloud.a *= saturate(cloudDepth - depth) * 1000000.0;

	return Cloud;
}
#endif

/*
	float CLOUDS_CU_MAX_RAY_LENGTH = 50; //km
	float SETTING_CLOUDS_LOW_STEP_MAX = 128;
	float SETTING_CLOUDS_LOW_STEP_MIN = 1;

	// Adaptive step count - shorter rays get fewer steps to save performance
	float cuRayLen = min(RayLen, CLOUDS_CU_MAX_RAY_LENGTH);
	float cuRaySteps = cuRayLen / CLOUDS_CU_MAX_RAY_LENGTH * float(SETTING_CLOUDS_LOW_STEP_MAX);
	uint cuRayStepsI = uint(max(cuRaySteps, SETTING_CLOUDS_LOW_STEP_MIN));

	// Raymarch ///////////////////
	for (uint stepIndex = 1; stepIndex < cuRayStepsI; ++stepIndex) {
		//if (stepState.position.w > cuRayLen) break;
		float3 StepStatePosition = ray.direction * stepIndex + cameraPos;
		float SampleHeight = length(StepStatePosition);
		float upVector = StepStatePosition / SampleHeight;

		// Sample cloud density at current position
		float cuMinHeight = bottomRadius;
		float cuMaxHeight = topRadius;
		float HeightFraction = LinearStep(cuMinHeight, cuMaxHeight, SampleHeight);

		float SETTING_CLOUDS_CU_DENSITY = 1.0;
		float CLOUDS_CU_DENSITYAAA = 256.0 * SETTING_CLOUDS_CU_DENSITY;
		float CLOUDS_CU_LIGHT_RAYMARCH_STEP = 8;
		float CLOUDS_CU_LIGHT_RAYMARCH_STEP_RCP = rcp(CLOUDS_CU_LIGHT_RAYMARCH_STEP);
		float C = 0.5 * CLOUDS_CU_LIGHT_RAYMARCH_STEP_RCP;

		//float SETTING_CLOUDS_CU_THICKNESS = 1.0;

		float2 lightRayDirJitterRand = Random::R2Sequence(stepIndex);

		float SUN_ANGULAR_RADIUS = atan((695700.0f * 1.0f) / (1.495978707E+8f * 1.0f));

		SharedData::DirLightDirection.xyz = rand_sampleInCone(SharedData::DirLightDirection.xyz, SUN_ANGULAR_RADIUS * 4.0, lightRayDirJitterRand);

		float sampleDensity = 0.0;
		float sampleDensityLod = 0.0;
		// Cumulus /////////////
		float EnvelopeZ = GetEnvelopeRelativeZ(StepStatePosition, float2(bottomRadius, topRadius));  // CHANGED
		//sampleDensity = GetBaseDensity(StepStatePosition, EnvelopeZ, cumulusCoverage, cirrusCoverage); // CHANGED

		//if(sampleDensity > 1e-6)  // CHANGED
		if (clouds_cu_density(StepStatePosition, HeightFraction, true, sampleDensity, sampleDensityLod))
		{
			sampleDensity *= CLOUDS_CU_DENSITYAAA;
			sampleDensityLod *= CLOUDS_CU_DENSITYAAA;

			// Shadow ray - march toward light to compute optical depth (how much light is blocked)
			// Uses x^2 sample distribution to concentrate samples near the cloud surface
			float lightRayTotalDensity = 0.0;

			// Ray Position
			// camera offset

			float lightRayLen = 2.0; //SETTING_CLOUDS_CU_THICKNESS * 1.0;
			float3 lightRayTotalDelta = SharedData::DirLightDirection.xyz * lightRayLen;

			for (uint lightStepIndex = 0; lightStepIndex < CLOUDS_CU_LIGHT_RAYMARCH_STEP; ++lightStepIndex) {
				// Quadratic distribution: more samples near start, fewer toward light source
				float lightRayJitterRand = 0; /////
				float x = (float(lightStepIndex) + lightRayJitterRand) * CLOUDS_CU_LIGHT_RAYMARCH_STEP_RCP;

				float3 lightRaySamplePos = StepStatePosition + lightRayTotalDelta * pow2(x);
				float lightSampleHeight = length(lightRaySamplePos);
				float lightHeightFraction = LinearStep(cuMinHeight, cuMaxHeight, lightSampleHeight);

				if (lightSampleHeight > cuMaxHeight)
					break;  // Exited cloud layer, no more occlusion

				float lightSampleDensity = 0.0;
				float lightSampleDensityLod = 0.0;

				EnvelopeZ = GetEnvelopeRelativeZ(lightRaySamplePos, float2(bottomRadius, topRadius)); // CHANGED
				//lightSampleDensity = GetBaseDensity(lightRaySamplePos, EnvelopeZ, cumulusCoverage, cirrusCoverage); // CHANGED

				//if(lightSampleDensity > 1e-6){
				if (clouds_cu_density(lightRaySamplePos, lightHeightFraction, true, lightSampleDensity, lightSampleDensityLod)) {
					float x = (float(lightStepIndex) + 0.5) * CLOUDS_CU_LIGHT_RAYMARCH_STEP_RCP; 					// Compute step length from quadratic spacing: (x+c)^2 - (x-c)^2 = 4xc
					float lightRayStepLength = 4.0 * x * C * lightRayLen;
					lightRayTotalDensity += lightSampleDensity * lightRayStepLength;
				}
			}

			// Convert accumulated density to optical depth and compute transmittance along light ray
			lightRayTotalDensity *= CLOUDS_CU_DENSITYAAA;

			float3 lightRayOpticalDepth = m.extinction * lightRayTotalDensity;
			float3 TrSample = (float3)0.85;//PhysSky::SampleTr(DirLightDirection, LinearSampler);

			CloudRaymarchStepState s;
			s.Height = SampleHeight;
			s.rayStep = float4(0,0,0, cuRayLen) * (float)rcp(cuRayStepsI + 1);
			s.upVector = upVector;

			float LODValue = 0; // add later
			clouds_computeLighting(r, l, s, lightRayTotalDensity, LODValue, lightRayOpticalDepth, TrSample, a);

			////
		}
		// END Cumulus /////////////

		// Early exit when cloud is fully opaque - no light can penetrate further
		if (a.totalTransmittance.y < 1e-6f) {
			break;
		}
	}
*/
