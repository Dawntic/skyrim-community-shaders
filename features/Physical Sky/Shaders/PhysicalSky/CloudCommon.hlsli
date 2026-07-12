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

//#include "Common/SharedData.hlsli"
//#include "PhysicalSky/Common.hlsli"

//#ifdef CLOUD_PS

cbuffer CloudDataCB : register(b0)
{
	float3 cameraPosIN;
	float groundRadius;
	float2 bayerPos;
	float atmTopRadius;
	float bottomRadius;

	float topRadius;
	float minDistance;
	float maxDistance;
	float Coverage2;

	float CloudCoverage;
	float HeightScale;
	float CloudType;
	float WeatherScale;
};

// Debug/verification seams for the cloud lighting chain. Uniform flow control
// on purpose: a single shader build supports the whole verification ladder.
cbuffer CloudDebugCB : register(b1)
{
	uint DebugSunTrMode;    // 0 live | 1 force white(=1) | 2 force orange | 3 A/B: sample GLOBAL Tr LUT instead
	uint DebugAmbientMode;  // 0 live | 1 DebugColor | 2 literal red | 3 red->blue height gradient
	float2 debugPad0;

	float3 DebugColor;
	float SunGain;  // 0 while validating ambient, 1 normally

	float AmbientGain;
	float SunMsGain;     // replaces CLOUD_MS_GAIN on the sun path only
	float cloudTrMuMin;  // windowed sun-transmittance LUT axes; radii in km
	float cloudTrMuMax;  // (this shader's scale -- the LUT was generated over
						 // the same normalized axes in game units)

	float cloudTrRBot;
	float cloudTrRTop;
	float OctaveAttenA;  // Wrenninge octave extinction attenuation (default 0.5)
	float OctaveAttenB;  // Wrenninge octave energy attenuation (default 0.6)
};

Texture2D DepthTex : register(t0);
Texture2D DisocculsionTex : register(t1);
Texture2D PrevFrameCloudTex : register(t2);
Texture2D PrevFrameCloudDepthTex : register(t3);

Texture2D DataFieldTex : register(t4);
Texture2D CirrusShapeTex : register(t5);
Texture2D VertProfileTex : register(t6);
Texture3D NoiseShapeTex : register(t7);

Texture3D CloudBaseTex : register(t8);
Texture3D CloudDetailTex : register(t9);
Texture2D CurlNoiseTex : register(t10);
Texture2D WeatherMapTex : register(t11);

Texture2D CloudPhaseLUT : register(t12);

Texture2D Base : register(t13);
Texture3D DetailOne : register(t14);
Texture3D DetailTwo : register(t15);
Texture3D Curl : register(t16);

// t17 is SharedData::DepthTexture -- do not reuse.
Texture2D<float4> TexCloudSunTr : register(t18);    // windowed sun transmittance (LUTGEN 4)
Texture2D<float4> TexCloudAmbient : register(t19);  // ambient endpoints, 2x1 (LUTGEN 5)

SamplerState LinearSampler : register(s0);
SamplerState LinearRepeatSampler : register(s1);

struct Ray
{
	float3 origin;
	float3 direction;
};

#define FLOAT32_MAX 3.402823466e+38f
#define FLOAT_EPS6 0.000001
#define M_1_PI4 0.079577471545947667884441881686257181  // 1 / (pi * 4)

// General ////////////////////////////////////////////////

float LinearStep(float lo, float hi, float x)
{
	return (x - lo) / (hi - lo);
}

float LerpLinearStep(float value, float oldMin, float oldMax, float newMin, float newMax)
{
	return lerp(newMin, newMax, LinearStep(oldMin, oldMax, value));
}

float LerpLinearStepClamped(float value, float oldMin, float oldMax, float newMin, float newMax)
{
	return lerp(newMin, newMax, saturate(LinearStep(oldMin, oldMax, value)));
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
	return max(hgPhase(anisotropy, cosTheta), hgPhase(0.99 - silverSpread, cosTheta) * silverIntens);
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
	return hgMultiScatt * LerpLinearStep(dimProfile * stepSize * 1000.0f, 0.1f, 1.0f, 0.0f, 1.0f) * pow(cloudCoverage * cloudData.z, 0.25f) * pow(transmittance, depthPower) * pow(relativeHeight, heightPower);
}
///////////////////////////////////////////////////////////

// Cloud General //////////////////////////////////////////

// Flat earth theory
float GetEnvelopeRelativeZ(float3 inPosition, float2 inCloudMinMax)
{
	return saturate(LinearStep(inCloudMinMax.x, inCloudMinMax.y, inPosition.z));
}

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
	int cloudtype = 1;
	if (cloudt < 0.1)
		cloudtype = 0;
	else if (cloudt > 0.9)
		cloudtype = 2;

	return GetGradientHeightFactor(relativeHeight, cloudtype);
}

float calcRelativeHeightA(float3 SamplePos, float PlanetCenterToEnvelopeBottom, float InvVertEnvelopeSize)
{
	float3 projPos = normalize(SamplePos) * PlanetCenterToEnvelopeBottom;
	return saturate(length(SamplePos - projPos) * InvVertEnvelopeSize);
}

float calcStepSize(float stepSizeFactor, float distance)
{
	const float nearStepSize = 0.003, farStepOffset = 0.06;
	return mad(farStepOffset * distance, stepSizeFactor / 8.192, nearStepSize);
}

float calcRelativeHeight(float bottomRadius, float coverage, float3 samplePos, float invThickness)
{
	float3 projPos = normalize(samplePos) * bottomRadius;
	float relativeHeight = distance(samplePos, projPos) * invThickness + max(coverage * 0.6, 0.2);
	return saturate(relativeHeight);
}

float3 calcFieldWindDir(float3 windDir, float Coverage2) { return windDir * (Coverage2 * 0.02f); }
float3 calcShapeWindDir(float3 windDir, float Coverage2) { return windDir * (Coverage2 * -0.002f); }
float calcCloudMipLevel(float3 cameraPos, float3 samplePos, float scale, float offset)
{
	return log2(mad(max(distance(cameraPos, samplePos) + offset, 0.0f), scale, 1.0f));
}

float calcCloudCoverage(float coverage, float3 cloudData)
{
	return saturate(LerpLinearStep(cloudData.y, coverage, 1.0f, 0.0f, 1.0f));
}

float calcVerticalProfile(float CloudType, float3 cloudData, float relativeHeight)
{
	float topProfile = VertProfileTex.SampleLevel(LinearSampler, float2(min(cloudData.z, CloudType), relativeHeight), 0).x;
	float bottomProfile = VertProfileTex.SampleLevel(LinearSampler, float2(cloudData.x, relativeHeight), 0).y;

	return topProfile * bottomProfile;
}

// perlin worley noise
float3 sampleDataFields(float3 cameraPos, float3 samplePos, float3 windDir, float posScale)
{
	float mipLod = calcCloudMipLevel(cameraPos, samplePos, 1.5, -15.0);
	samplePos += windDir;

	return DataFieldTex.SampleLevel(LinearRepeatSampler, samplePos.xy * posScale, mipLod).xyz;
}

float calcCloudDensity(float3 cameraPos, float3 samplePos, float3 cloudData, float dimProfile, float3 windDir)
{
	float mipLod = calcCloudMipLevel(cameraPos, samplePos, 0.5, -15.0);
	float4 noise = NoiseShapeTex.SampleLevel(LinearRepeatSampler, (samplePos.xzy + windDir) * 0.4, mipLod);  // windDir needs mult
	float wispyNoise = lerp(noise.r, noise.g, dimProfile);
	float billowyNoise = lerp(noise.b * 0.3, noise.a * 0.3, pow(dimProfile, 0.25));
	float noiseComposite = lerp(wispyNoise, billowyNoise, cloudData.z);
	return LinearStep(noiseComposite, 1.0, dimProfile);  // Erosion
}
///////////////////////////////////////////////////////////

float pow2(float input)
{
	return input * input;
}

float pow3(float input)
{
	return input * input * input;
}

struct CloudRenderParams
{
	float3 lightDir;
	float3 lightIrradiance;
	float cosLightTheta;
};

struct CloudParticpatingMedium
{
	float3 scattering;
	float3 extinction;
	float3 phase;
};

struct CloudRaymarchLayerParam
{
	CloudParticpatingMedium medium;
	float3 ambientIrradiance;
};

struct CloudRaymarchStepState
{
	float height;
	float4 rayStep;
	float3 upVector;
};

struct CloudRaymarchAccumState
{
	float3 totalInscattering;
	float totalTransmittance;
};

float Draine(float cos_theta, float g, float alpha)
{
	float scale = .25 * (1.0 / Math::PI);
	float g2 = g * g;
	float num = (1.0 - g2) * (1.0 + alpha * cos_theta * cos_theta);
	float denom = pow(abs(1.0 + g2 - 2.0 * g * cos_theta), 1.5) * (1.0 + alpha * (1.0 + 2.0 * g2) * 0.333333333333333);
	return scale * num / denom;
}

// Phase Method from alpha piscium
// gScale attenuates both lobes' eccentricity (1 = physical fit); the
// Wrenninge octave path passes 0.5^octave so deeply-scattered light goes
// isotropic.
float3 CloudPhase(float cosTheta, float gScale)
{
	float CLOUDS_CU_R_EFF = 5.77;  //////////
	// d: droplet diameter in µm (micrometers)
	float d = CLOUDS_CU_R_EFF * 2.0;
	float gHG = exp(-0.0990567 / (d - 1.67154)) * gScale;
	float gD = exp(-2.20679 / (d + 3.91029) - 0.428934) * gScale;
	float a = exp(3.62489 - 8.29288 / (d + 5.52825));
	float wD = exp(-0.599085 / (d - 0.641583) - 0.665888);
	// hgPhase's signature is (anisotropy, cosTheta) -- the previous call here
	// passed them swapped.
	float HGDraine = lerp(hgPhase(gHG, cosTheta), Draine(cosTheta, gD, a), wD);
	return HGDraine;
}

float3 CloudPhase(float cosTheta)
{
	return CloudPhase(cosTheta, 1.0);
}
//#endif