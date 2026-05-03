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

// Physically based volumetric clouds.
// Based on the Decima Nubis clouds rendering system.

#include "Common/SharedData.hlsli"
//#include "PhysicalSky/Common.hlsli"

#ifdef CLOUD_PS

struct CloudsParams
{
	//#ifdef USE_CLOUDS_DEPTH
	float4x4 viewProj;
	//#endif
	//#ifdef USE_CLOUDS_REPROJECTION
	float4x4 prevViewProj;
	uint2 fragCoord;
	uint2 bayerPos;
	//#endif
	float4x4 invViewProj;
	float3 cameraPos;
	float3 lightDir;
	float3 windDir;
	float3 lightColor;
	float3 ambientLight;
	float2 texCoords;
	float groundRadius;
	float atmTopRadius;
	float bottomRadius;
	float topRadius;
	float minDistance;
	float maxDistance;
	float currentTime;
	float cumulusCoverage;
	float cirrusCoverage;
	float temperatureDiff;
	float stepSizeFactor;
};

Texture2D DepthTex : register(t0);
Texture2D DisocculsionTex : register(t1);
Texture2D PrevFrameCloudTex : register(t2);
Texture2D PrevFrameCloudDepthTex : register(t3);

Texture2D DataFieldTex : register(t4);
Texture2D CirrusShapeTex : register(t5);
Texture2D VertProfileTex : register(t6);
Texture3D NoiseShapeTex : register(t7);

SamplerState LinearSampler : register(s0);
SamplerState LinearRepeatSampler : register(s1);

struct Ray
{
	float3 origin;
	float3 direction;
};

#	define FLOAT32_MAX 3.402823466e+38f
#	define FLOAT_EPS6 0.000001
#	define M_1_PI4 0.079577471545947667884441881686257181  // 1 / (pi * 4)

float LinearStep(float lo, float hi, float x)
{
	return saturate((x - lo) / (hi - lo));
}

float remap(float value, float oldMin, float oldMax, float newMin, float newMax)
{
	return lerp(newMin, newMax, LinearStep(oldMin, oldMax, value));
}

bool isIntersected(float2 raycastDists)
{
	// If tMax < 0.0f, ray is intersecting shape, but whole shape is behing us.
	return raycastDists.y > max(raycastDists.x, 0.0f);
}

float2 raycast2(float4 sphere, Ray ray)
{
	float3 l = ray.origin - sphere.xyz;
	float b = dot(ray.direction, l) * -2.0f;
	float c = dot(l, l) - sphere.w * sphere.w;
	float discriminant = mad(b, b, c * -4.0f);
	if (discriminant < 0.0f)
		return float2(-FLOAT32_MAX, -FLOAT32_MAX);

	float discrSqrt = sqrt(discriminant);
	return float2(b - discrSqrt, b + discrSqrt) * 0.5f;
}

float hgPhase(float anisotropy, float cosTheta)  // Henyey-Greenstein
{
	const float g2 = anisotropy * anisotropy;
	float i = rsqrt(saturate((g2 + 1.0f) - cosTheta * (anisotropy * 2.0f)));
	return (i * i * i) * ((1.0f - g2) * float(M_1_PI4));
}

float hgPhaseCloud(float cosTheta)  // Emulates the sun silver lining highlights.
{
	const float anisotropy = 0.6f, silverIntens = 0.6f, silverSpread = 0.2f;
	return max(hgPhase(anisotropy, cosTheta), hgPhase(0.99f - silverSpread, cosTheta) * silverIntens);
}

// HG makes far clouds too dark, clouds away from star direction need extra scattering.
float beerLambertCloud(float accumDensity, float cosTheta)
{
	float transmission = exp(-accumDensity);
	float modulated = max(transmission, exp(accumDensity * -0.25f) * 0.7f);
	return lerp(transmission, modulated, mad(cosTheta, -0.5f, 0.5f));
}

float calcMultiScattering(float hgMultiScatt, float stepSize, float3 cloudData, float relativeHeight, float cloudCoverage, float dimProfile, float transmittance)
{
	const float depthPower = 0.5f, heightPower = 0.5f;
	return hgMultiScatt * remap(dimProfile * stepSize * 1000.0f, 0.1f, 1.0f, 0.0f, 1.0f) * pow(cloudCoverage * cloudData.z, 0.25f) * pow(transmittance, depthPower) * pow(relativeHeight, heightPower);
}

float calcStepSize(float stepSizeFactor, float distance)
{
	const float nearStepSize = 0.003, farStepOffset = 0.06;
	return mad(farStepOffset * distance, stepSizeFactor / 8.192, nearStepSize);
}

float calcRelativeHeight(float bottomRadius, float coverage, float3 samplePos, float invThickness)
{
	float3 projPos = normalize(samplePos) * bottomRadius;
	float relativeHeight = mad(distance(samplePos, projPos), invThickness, max(coverage * 0.6, 0.2));
	return saturate(relativeHeight);
}

float calcCloudCoverage(float coverage, float3 cloudData) { return saturate(remap(cloudData.y, coverage, 1.0f, 0.0f, 1.0f)); }
float3 calcFieldWindDir(float3 windDir, float currentTime) { return windDir * (currentTime * 0.02f); }
float3 calcShapeWindDir(float3 windDir, float currentTime) { return windDir * (currentTime * -0.002f); }

float calcCloudMipLevel(float3 cameraPos, float3 samplePos, float scale, float offset)
{
	return log2(mad(max(distance(cameraPos, samplePos) + offset, 0.0f), scale, 1.0f));
}

float calcVerticalProfile(float temperatureDiff, float3 cloudData, float relativeHeight)
{
	float topProfile = VertProfileTex.SampleLevel(LinearSampler, float2(min(cloudData.z, temperatureDiff), relativeHeight), 0).x;
	float bottomProfile = VertProfileTex.SampleLevel(LinearSampler, float2(cloudData.x, relativeHeight), 0).y;

	return topProfile * bottomProfile;
}

float3 sampleDataFields(float3 cameraPos, float3 samplePos, float3 windDir, float posScale)
{
	float mipLod = calcCloudMipLevel(cameraPos, samplePos, 1.5, -15.0);
	samplePos += windDir;

	return DataFieldTex.SampleLevel(LinearRepeatSampler, samplePos.xy * posScale, mipLod).xyz;
}

float3 sampleCirrusShape(float3 cameraPos, float3 samplePos, float3 windDir)
{
	const float posScale = 0.25;
	float mipLod = calcCloudMipLevel(cameraPos, samplePos, 1.5, -5.0);
	samplePos += windDir;

	return CirrusShapeTex.SampleLevel(LinearRepeatSampler, samplePos.xy * posScale, mipLod).xyz;
}

float calcCloudDensity(float3 cameraPos, float3 samplePos, float3 cloudData, float dimProfile, float3 windDir)
{
	float mipLod = calcCloudMipLevel(cameraPos, samplePos, 0.5f, -15.0f);
	float4 noise = NoiseShapeTex.SampleLevel(LinearRepeatSampler, (samplePos.xzy + windDir) * 0.4f, mipLod);  // windDir needs mult
	float wispyNoise = lerp(noise.r, noise.g, dimProfile);
	float billowyNoise = lerp(noise.b * 0.3f, noise.a * 0.3f, pow(dimProfile, 0.25f));
	float noiseComposite = lerp(wispyNoise, billowyNoise, cloudData.z);
	return LinearStep(dimProfile, 1.0, noiseComposite);  // Erosion
}

float calcCirrusDensity(float3 cloudData, float3 cirrusShape, float cloudCoverage)
{
	float density = remap(cloudData.z, 0.5f, 1.0f, remap(cloudData.z, 0.0f, 0.5f, cirrusShape.x, cirrusShape.y), cirrusShape.z);
	density = pow(density, 1.0f - remap(cloudCoverage, 0.0f, 1.0f, -0.9f, 0.9f));
	return saturate(density * remap(pow(cloudCoverage, 3.0f), 0.0f, 0.5f, 0.0f, 1.0f));
}
#endif