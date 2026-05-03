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

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "PhysicalSky/CloudCommon.hlsli"

#define STEP_SIZE_FACTOR 1.0

cbuffer CloudDataCB : register(b0)
{
	float3 cameraPos;
	float groundRadius;
	float2 bayerPos;
	float atmTopRadius;
	float bottomRadius;
	float topRadius;
	float minDistance;
	float maxDistance;
	float currentTime;
	float cumulusCoverage;
	float cirrusCoverage;
	float temperatureDiff;
};

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

#	define USE_CAMERA_VOLUME
#	define USE_CLOUDS_DEPTH
//#define USE_CLOUDS_REPROJECTION

PixelOut main(VertexOut input)
{
	PixelOut output;

	CloudsParams clouds;
	clouds.fragCoord = (uint2)input.Position.xy;
	clouds.texCoords = input.TexCoord;

	clouds.viewProj = FrameBuffer::CameraViewProj[0];
	clouds.prevViewProj = FrameBuffer::CameraPreviousViewProjUnjittered[0];
	clouds.invViewProj = FrameBuffer::CameraViewProjInverse[0];

	clouds.lightDir = SharedData::DirLightDirection.xyz;  //float3(0,0,0); //-cc.lightDir;
	clouds.windDir = float3(0.2, 0.2, 0.2);               //cc.windDir;
	clouds.ambientLight = float3(1, 1, 1) * 3.0;          //cc.ambientLight;

	clouds.lightColor = float3(1, 1, 1) * 3.0;  // TODO
	clouds.stepSizeFactor = STEP_SIZE_FACTOR;

	clouds.cameraPos = cameraPos;  //game units
	clouds.bayerPos = bayerPos;
	clouds.groundRadius = groundRadius;  //km
	clouds.atmTopRadius = atmTopRadius;  //km
	clouds.bottomRadius = bottomRadius;  //km
	clouds.topRadius = topRadius;        //km
	clouds.minDistance = minDistance;    //km
	clouds.maxDistance = maxDistance;    //km
	clouds.currentTime = currentTime;
	clouds.cumulusCoverage = cumulusCoverage;
	clouds.cirrusCoverage = cirrusCoverage;
	clouds.temperatureDiff = temperatureDiff;

	//float depth;
	//float4 color;
	//evaluateClouds(clouds, color, depth);

	output.color = float4(0, 0, 0, 1);
	output.depth = 1.0;

	float GAME_UNIT_TO_KM = 1.428e-5;
	clouds.cameraPos = clouds.cameraPos * GAME_UNIT_TO_KM;
	clouds.cameraPos.z += groundRadius;

	//clouds.cameraPos.xyz = clouds.cameraPos.xzy;

	//float3 cam = clouds.cameraPos * GAME_UNIT_TO_KM;
	//clouds.cameraPos = float3(0, 0, groundRadius);

	//groundRadius 6360.0
	//bottomRadius 6361.50
	//topRadius 6364.0

	//
	Ray ray;
	ray.origin = clouds.cameraPos;
	//ray.direction = calcViewDirection(clouds.texCoords, clouds.invViewProj);

	float2 ndc = clouds.texCoords * float2(2.0, -2.0) - float2(1.0, -1.0);
	float4 worldPosition = mul(clouds.invViewProj, float4(ndc, 0.5, 1.0));  // why 0.5 depth?
	worldPosition.xyz /= worldPosition.w;
	ray.direction = normalize(worldPosition);

	//clouds.cameraPos = float3(0, 0, groundRadius);

	// .x = min ray dist, .y = max ray dist
	// .x = cloud entry, .y = cloud exit
	// used for cirrus layer
	float2 RayT = raycast2(float4((float3)0, clouds.topRadius), ray);
	if (!isIntersected(RayT)) {  // No atmosphere intersection so no clouds.
		output.color = (float4)0;
#	ifdef USE_CLOUDS_DEPTH
		output.depth = 1.0;
#	endif
		//output.color.xyz = float3(1,0,0);
		return output;
	}

	float cirrusT = RayT.x < 0.0 ? RayT.y : RayT.x;
	RayT.x = max(RayT.x, clouds.minDistance);

	// used for cumulus layer
	float2 InnerRay = raycast2(float4((float3)0.0, clouds.bottomRadius), ray);
	if (isIntersected(InnerRay)) {                                                                                    // Intersecting bottom clouds sphere.
		RayT = InnerRay.x < 0.0 ? float2(max(RayT.x, InnerRay.y), RayT.y) : float2(RayT.x, min(RayT.y, InnerRay.x));  // Is camera below bottom clouds level?
	}

#	ifdef USE_CLOUDS_DEPTH
	float MaxNDCDepth = 0.9999999999;
	//float hizDepth = MIN_HIZ(textureLod(hizBuffer, clouds.texCoords, 0.0f));
	float hizDepth = DepthTex.SampleLevel(LinearSampler, clouds.texCoords, 0);  // SAMPLER TYPE?
	float2 currNdcPos = clouds.texCoords * float2(2.0, -2.0) - float2(1.0, -1.0);
	float4 worldPos = mul(clouds.invViewProj, float4(currNdcPos, hizDepth, 1.0));
	worldPos.xyz = (worldPos.xyz / worldPos.w) * GAME_UNIT_TO_KM;
	//RayT.y = (hizDepth <= MaxNDCDepth) ? min(RayT.y, length((worldPos.xyz))) : RayT.y;
#	endif  // USE_CLOUDS_DEPTH

	// Skipping ray if whole planet is behind us.
	if (RayT.y <= RayT.x || RayT.x > clouds.maxDistance) {
		output.color = (float4)0;
#	ifdef USE_CLOUDS_DEPTH
		output.depth = 1.0;
#	endif
		//output.color.xyz = float3(0,0,1);
		return output;
	}

#	ifdef USE_CLOUDS_REPROJECTION
	float4 prevNdcPos = mul(clouds.prevViewProj, float4(normalize(worldPos.xyz), 0.0));
	float2 prevTexCoords = mad((prevNdcPos.xy / prevNdcPos.w) * GAME_UNIT_TO_KM, 0.5, 0.5);
	float disocclusion = DisocculsionTex.SampleLevel(LinearSampler, clouds.texCoords, 0.0).x;  // CLOUD BE WRONG SAMPLER TYPE

	if (disocclusion == 0.0 && clouds.bayerPos.x != -1.0 && all(prevTexCoords < 1.0 && prevTexCoords > 0.0)) {
		//float prevDepth = MAX_HIZ(textureLod(hizBuffer, prevTexCoords, 0.0f));
		float prevDepth = DepthTex.SampleLevel(LinearSampler, prevTexCoords, 0);  // SAMPLER TYPE?
		if (all(clouds.fragCoord % 4 != (uint2)clouds.bayerPos) && (prevDepth <= hizDepth)) {
			output.color = PrevFrameCloudTex.SampleLevel(LinearSampler, prevTexCoords, 0);
#		ifdef USE_CLOUDS_DEPTH
			output.depth = PrevFrameCloudDepthTex.SampleLevel(LinearSampler, prevTexCoords, 0).x;
#		endif
			return output;
		}
	}
#	endif  // USE_CLOUDS_REPROJECTION

	const float densityFactor = 5.0;
	float invThickness = 1.0 / (clouds.topRadius - clouds.bottomRadius);
	float3 fieldWindDir = calcFieldWindDir(clouds.windDir, clouds.currentTime);
	float3 shapeWindDir = calcShapeWindDir(clouds.windDir, clouds.currentTime);

	float cosTheta = dot(ray.direction, clouds.lightDir);
	float hgScattering = hgPhaseCloud(cosTheta), hgMultiScatt = hgPhase(0.3, cosTheta);
	float lightAbsorption = 0.0, directIntensity = 0.0, ambientIntensity = 0.0, distanceSum = 0.0;

	float stepMul = 3.0;
	uint missCount = 0;
	bool fastMarching = true;

	while (RayT.x < RayT.y && lightAbsorption < 1.0) {
		float stepSize = calcStepSize(clouds.stepSizeFactor, RayT.x) * stepMul;
		float3 samplePos = mad(ray.direction, RayT.x, clouds.cameraPos);
		float3 cloudData = sampleDataFields(clouds.cameraPos, samplePos, fieldWindDir, 0.02);
		float relativeHeight = calcRelativeHeight(clouds.bottomRadius, clouds.cumulusCoverage, samplePos, invThickness);
		float verticalProfile = calcVerticalProfile(clouds.temperatureDiff, cloudData, relativeHeight);
		float cloudCoverage = calcCloudCoverage(clouds.cumulusCoverage, cloudData);
		float dimProfile = verticalProfile * cloudCoverage;  // Dimensional profile

		if (dimProfile > 0.0) {
			missCount = 0;
			if (fastMarching) {      // Starting high resolution ray marching.
				RayT.x -= stepSize;  // Go back one step to not miss any high res samples.
				stepMul = 1.0;
				fastMarching = false;
				continue;
			}

			float cloudDensity = calcCloudDensity(clouds.cameraPos, samplePos, cloudData, dimProfile, shapeWindDir);
			if (cloudDensity < FLOAT_EPS6) {
				RayT.x += stepSize;
				continue;
			}

			cloudDensity *= clouds.temperatureDiff * (densityFactor * clouds.stepSizeFactor);
			float occlusion = cloudDensity * (1.0 - lightAbsorption);
			float attenuation = beerLambertCloud(cloudDensity, cosTheta);
			float ambientScattering = pow(1.0 - dimProfile, 0.5) * attenuation;
			ambientIntensity = mad(ambientScattering, occlusion, ambientIntensity);
			distanceSum = mad(RayT.x, occlusion, distanceSum);
			lightAbsorption += occlusion;

			float lightDensity = 0.0, lightStepSize = 0.006;
			for (uint step = 0; step < 10; step++) {  // 256 meters with 10 samples
				samplePos = mad(clouds.lightDir, lightStepSize, samplePos);
				float3 data = sampleDataFields(clouds.cameraPos, samplePos, fieldWindDir, 0.02);
				float height = calcRelativeHeight(clouds.bottomRadius, clouds.cumulusCoverage, samplePos, invThickness);
				float profile = calcVerticalProfile(clouds.temperatureDiff, data, height) * calcCloudCoverage(clouds.cumulusCoverage, data);
				lightDensity = mad(calcCloudDensity(clouds.cameraPos, samplePos, data, profile, shapeWindDir), lightStepSize, lightDensity);
				lightStepSize = mad(lightStepSize, 1.3, lightStepSize);
			}

			lightDensity *= densityFactor * 10.0;  // 10 samples
			float lightTransmittance = beerLambertCloud(lightDensity, cosTheta);
			float multiScattering = calcMultiScattering(hgMultiScatt, stepSize, cloudData, relativeHeight, cloudCoverage, dimProfile, lightTransmittance);
			float directScattering = mad(lightTransmittance, hgScattering, multiScattering);
			directIntensity = mad(directScattering, occlusion, directIntensity);
		} else if (!fastMarching) {
			if (missCount++ >= 10) {
				fastMarching = true;
				stepMul = 3.0;
			}
		}

		RayT.x += stepSize;
	}

	if (lightAbsorption < 1.0) {
		fieldWindDir = mad(fieldWindDir, 0.3, 16.0);
		shapeWindDir *= 2.0;
		float3 samplePos = mad(ray.direction, cirrusT, clouds.cameraPos);
		float3 cloudData = sampleDataFields(clouds.cameraPos, samplePos, fieldWindDir, 0.025);
		float3 cirrusShapeData = sampleCirrusShape(clouds.cameraPos, samplePos, shapeWindDir);
		float cloudCoverage = calcCloudCoverage(clouds.cirrusCoverage, cloudData);
		float cirrusDensity = calcCirrusDensity(cloudData, cirrusShapeData, cloudCoverage);

		if (cirrusDensity > FLOAT_EPS6) {
			float occlusion = cirrusDensity * (1.0 - lightAbsorption);
			float attenuation = beerLambertCloud(cirrusDensity, cosTheta) * densityFactor;
			float ambientScattering = pow(1.0 - cloudCoverage, 0.5) * attenuation;
			ambientIntensity = mad(ambientScattering, occlusion, ambientIntensity);
			distanceSum = mad(cirrusT, occlusion, distanceSum);
			lightAbsorption += occlusion;

			const float lightStepSize = 25.0;  // Based on the prev lightStep of 0.006f
			float lightDensity = 0.0;

			for (uint step = 0; step < 4; step++) {
				samplePos = mad(clouds.lightDir, lightStepSize, samplePos);
				float3 data = sampleDataFields(clouds.cameraPos, samplePos, fieldWindDir, 0.025);
				float3 shapeData = sampleCirrusShape(clouds.cameraPos, samplePos, shapeWindDir);
				float coverage = calcCloudCoverage(clouds.cirrusCoverage, data);
				lightDensity += calcCirrusDensity(data, shapeData, coverage);
			}

			const float directScattFactor = 0.5;  //
			float lightTransmittance = beerLambertCloud(lightDensity * directScattFactor, cosTheta);
			directIntensity = mad(lightTransmittance * hgScattering, occlusion, directIntensity);
		}
	}
	lightAbsorption = min(lightAbsorption, 1.0);

	// TODO: also support coloring by the storm lightnings.

	if (lightAbsorption == 0.0) {
		output.color = (float4)0.0;
#	ifdef USE_CLOUDS_DEPTH
		output.depth = 1.0;
#	endif
		return output;
	}

	float3 cloudCamPos = ray.direction * (distanceSum / lightAbsorption);

#	ifdef USE_CLOUDS_DEPTH
	float4 clipPos = mul(clouds.viewProj, float4(cloudCamPos / GAME_UNIT_TO_KM, 1.0));
	output.depth = saturate(clipPos.z / clipPos.w);
#	endif

	//float3 cloudWorldPos = cloudCamPos + float3(0.0f, clouds.groundRadius, 0.0f);
	float3 Transmittance = (float3)1;  //getTransmittance(transLUT, Ray(cloudWorldPos, clouds.lightDir), clouds.groundRadius, clouds.atmTopRadius);
	float3 directLight = clouds.lightColor * (Transmittance * directIntensity);
	float3 ambientLight = clouds.ambientLight * ambientIntensity * (1.0 / densityFactor);
	float3 lightEnergy = directLight + ambientLight;

#	ifdef USE_CAMERA_VOLUME
	//float4 ap = getAerialPerspLuminance(cameraVolume, clouds.texCoords, length(cloudCamPos));
	float4 ap = 0;  //SampleAP(normalize(cloudCamPos), clouds.fragCoord, length(cloudCamPos), LinearSampler); // CORRECT SAMPLER??
	lightEnergy = lerp(lightEnergy, ap.rgb, ap.a);
#	endif

	output.color = float4(min(lightEnergy, 65504.0), lightAbsorption);

	//output.color.xyz = ray.direction;
	//output.depth = float4(depth, 0, 0, 0);

	return output;
}
#endif

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
	if (depth > MaxNDCDepth)
		Cloud.a *= saturate(cloudDepth - depth) * 1000000.0;

	return Cloud;
}
#endif