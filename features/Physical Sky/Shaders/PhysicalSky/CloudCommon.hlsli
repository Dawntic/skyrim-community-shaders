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

#ifdef CLOUD_PS

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
	float currentTime;

	float cumulusCoverage;
	float cirrusCoverage;
	float temperatureDiff;
	float WeatherScale;

	float BaseScale;
	float CurlScale;
	float DetailScale;
	float Extinction;

	float3 Scattering;
	float Cone;

	float TopCurve;
	float BottomCurve;

	//float2 pad;

	//float2 Params[2];
	float4 Params[2];

	//float4 Params;
	//float4 ParamsTwo;
	//float2 padd;
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

float3 calcFieldWindDir(float3 windDir, float currentTime) { return windDir * (currentTime * 0.02f); }
float3 calcShapeWindDir(float3 windDir, float currentTime) { return windDir * (currentTime * -0.002f); }
float calcCloudMipLevel(float3 cameraPos, float3 samplePos, float scale, float offset)
{
	return log2(mad(max(distance(cameraPos, samplePos) + offset, 0.0f), scale, 1.0f));
}

float calcCloudCoverage(float coverage, float3 cloudData)
{
	return saturate(LerpLinearStep(cloudData.y, coverage, 1.0f, 0.0f, 1.0f));
}

float calcVerticalProfile(float temperatureDiff, float3 cloudData, float relativeHeight)
{
	float topProfile = VertProfileTex.SampleLevel(LinearSampler, float2(min(cloudData.z, temperatureDiff), relativeHeight), 0).x;
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

// Cirrus /////////////////////////////////////////////////

float3 sampleCirrusShape(float3 cameraPos, float3 samplePos, float3 windDir)
{
	const float posScale = 0.25;
	float mipLod = calcCloudMipLevel(cameraPos, samplePos, 1.5, -5.0);
	samplePos += windDir;

	return CirrusShapeTex.SampleLevel(LinearRepeatSampler, samplePos.xy * posScale, mipLod).xyz;
}

float calcCirrusDensity(float3 cloudData, float3 cirrusShape, float cloudCoverage)
{
	float density = LerpLinearStep(cloudData.z, 0.5, 1.0, LerpLinearStep(cloudData.z, 0.0, 0.5, cirrusShape.x, cirrusShape.y), cirrusShape.z);
	density = pow(density, 1.0 - LerpLinearStep(cloudCoverage, 0.0, 1.0, -0.9, 0.9));
	return saturate(density * LerpLinearStep(pow(cloudCoverage, 3.0), 0.0, 0.5, 0.0, 1.0));
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

/*
struct AtmosphereParameters {
    float bottom;
    float top;

    float mieHeight;

    float3 rayleighSctrCoeff;
    float3 rayleighExtinction;

    float3 mieSctrCoeff;
    float3 mieExtinction;

    float miePhaseG;
    float miePhaseE;

    float3 ozoneExtinction;
};
*/

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

// See https://www.desmos.com/calculator/yerfmyqpuh
float3 SamplePhaseLUT(float cosTheta, float type)
{
	float a0 = 0.672617934627;
	float a1 = -0.0713555761181;
	float a2 = 0.0299320735609;
	float b = 0.264767018876;
	float x1 = acos(cosTheta);
	float x2 = x1 * x1;
	float u = saturate((a0 + a1 * x1 + a2 * x2) * pow(x1, b));
	float v = (type + 0.5) / 3.0;

	// It was encoded from AP0
	return CloudPhaseLUT.SampleLevel(LinearSampler, float2(u, v), 0);
}

// Phase Method from alpha piscium
float3 CloudPhase(float cosTheta, float mixRatio)
{
	float CLOUDS_CU_R_EFF = 5.77;  //////////

	// d: droplet diameter in µm (micrometers)
	float d = CLOUDS_CU_R_EFF * 2.0;
	float gHG = exp(-0.0990567 / (d - 1.67154));
	float gD = exp(-2.20679 / (d + 3.91029) - 0.428934);
	float a = exp(3.62489 - 8.29288 / (d + 5.52825));
	float wD = exp(-0.599085 / (d - 0.641583) - 0.665888);
	float HGDraine = lerp(hgPhase(cosTheta, gHG), Draine(cosTheta, gD, a), wD);

	float phaseSample = SamplePhaseLUT(cosTheta, 1.0);

	return lerp(HGDraine, phaseSample, mixRatio);
}

// from ShortFuse (RenoDX)
float ApplyCurve(float x, float a, float b, float c, float d, float e, float f)
{
	return ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - e / f;
}
float3 ApplyCurve(float3 x, float a, float b, float c, float d, float e, float f)
{
	return ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - e / f;
}

static const float A = 0.22;  // Shoulder Strength  .22
static const float B = 0.30;  // Linear Strength
static const float C = 0.10;  // Linear Angle
static const float D = 0.20;  // Toe Strength  .20
static const float E = 0.01;  // Toe Numerator
static const float F = 0.30;  // Toe Denominator
static const float W = 11.2;  // Linear White

float3 Uncharted2Tonemap(float3 untonemapped, float linear_white = W)
{
	return ApplyCurve(untonemapped * 2.f, A, B, C, D, E, F) / ApplyCurve(linear_white, A, B, C, D, E, F);
}

float _clouds_cu_heightCurveWisp(float4 xs)
{
	// https://www.desmos.com/calculator/2c5574fcdc
	const float a0 = -1.755622;
	;
	const float4 as = float4(3.6801126, -163.09651, 320.59657, -163.29147);
	const float a5 = 0.01273534;
	return exp2(dot(as, xs) + a0) + a5;
}

float _clouds_cu_heightCurveBillowy(float4 xs)
{
	// https://www.desmos.com/calculator/2c5574fcdc
	const float a0 = -2.8940248;
	const float4 as = float4(20.938597, -83.765418, 114.27168, -53.99707);
	return exp2(dot(as, xs) + a0);
}

uint4 hash_pcg4d_44(uint4 v)
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.w;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v.w += v.y * v.z;
	v = v ^ (v >> 16u);
	v.x += v.y * v.w;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v.w += v.y * v.z;
	return v;
}
uint3 hash_33_q3(uint3 v)
{
	return hash_pcg4d_44(uint4(v, 2246822519)).xyz;
}

float hash_uintToFloat(uint v)
{
	return float(v) * (1.0 / float(0xffffffff));
}

float worleyNoise(float2 x, uint seed)
{
	uint2 centerCellID = floor(x).xx;  //uint2(int2(floor(x)));
	float2 centerOffset = frac(x);

	// Initialize results
	float f1 = 0.0;
	float f2 = 0.0;
	float m = 1.0;

	for (int ix = -1; ix <= 1; ++ix) {
		for (int iy = -1; iy <= 1; ++iy) {
			int2 idOffset = int2(ix, iy);
			uint2 cellID = centerCellID + (idOffset + 2);

			uint3 hashPos = uint3(cellID, seed);
			float3 hashValF = hash_uintToFloat(hash_33_q3(hashPos));
			float2 cellCenter = hashValF.xy + float2(idOffset);

			float cellDistance = distance(centerOffset, cellCenter);
			float v = pow2(hashValF.z) * saturate(1.0 - cellDistance);

			const float w = 0.35;
			const float wRcp = 1.4285714286;  // 1.0 / w * 0.5
			float h = hashValF.z * saturate((m - cellDistance) * wRcp + 0.5);
			float wh = w * h;
			m = lerp(m, cellDistance, h) - mad(wh, -h, wh) / (1.0 + 3.0 * w);

			if (f1 < v) {
				f2 = f1;
				f1 = v;
			} else if (f2 < v) {
				f2 = v;
			}
		}
	}

	return saturate(1.0 - m - f2);
}
/*
float detailNoiseB(float3 pos, float3 curl) {
    float3 lowFreqPos = pos + curl * exp2(Params[0].w - 1.0);
    lowFreqPos *= exp2(Params[0].z - 1.0);
    //float lowFreq = texture(usam_cumulusDetail1, lowFreqPos).x;
    float lowFreq = DetailOne.SampleLevel(LinearRepeatSampler, lowFreqPos, 0).x;
    float3 highFreqPos = pos + curl * exp2(Params[1].y);
    highFreqPos *= exp2(Params[1].x);
    //float highFreq = texture(usam_cumulusDetail2, highFreqPos).x;
   float highFreq = DetailTwo.SampleLevel(LinearRepeatSampler, highFreqPos, 0).x;
    return pow3(1.0 - lowFreq) * 0.6 + pow2(0.5 - highFreq) * 0.5;
}

float detailNoiseW(float3 pos) {
    pos *= 0.5;
    pos *= exp2(Params[1].z);
    return DetailTwo.SampleLevel(LinearRepeatSampler, pos, 0).x;
    //return texture(usam_cumulusDetail2, pos).x;
}

float coverageNoise(float2 pos) {
    pos *= exp2(Params[0].x);
    float higherOctave = Base.SampleLevel(LinearRepeatSampler, pos / 32.0, 0).x; //texture(usam_cumulusBase, pos / 32.0).x;
    const float freq = 0.35;
    float baseNoise = worleyNoise(pos * freq, 0x1919810);
    return baseNoise + higherOctave * 0.75;
}

float3 detailCurlNoise(float3 pos) {
    pos *= 0.5;
    pos *= exp2(Params[0].y);
    return Curl.SampleLevel(LinearRepeatSampler, pos, 0).xyz; //texture(usam_cumulusCurl, pos).xyz;
}

*/

float detailNoiseB(float3 pos, float3 curl)
{
	float _LOW_BILLOWY_CURL_STR = 0;
	float _LOW_BILLOWY_FREQ = 0;
	float _HIGH_BILLOWY_CURL_STR = 0;
	float _HIGH_BILLOWY_FREQ = 0;

	float3 lowFreqPos = pos + curl * exp2(_LOW_BILLOWY_CURL_STR - 1.0);
	lowFreqPos *= exp2(_LOW_BILLOWY_FREQ - 1.0);
	//float lowFreq = texture(usam_cumulusDetail1, lowFreqPos).x;
	float lowFreq = DetailOne.SampleLevel(LinearRepeatSampler, lowFreqPos, 0).x;
	float3 highFreqPos = pos + curl * exp2(_HIGH_BILLOWY_CURL_STR);
	highFreqPos *= exp2(_HIGH_BILLOWY_FREQ);
	//float highFreq = texture(usam_cumulusDetail2, highFreqPos).x;
	float highFreq = DetailTwo.SampleLevel(LinearRepeatSampler, highFreqPos, 0).x;
	return pow3(1.0 - lowFreq) * 0.6 + pow2(0.5 - highFreq) * 0.5;
}

float detailNoiseW(float3 pos)
{
	float _LOW_WISPS_FREQ = 0;

	pos *= 0.5;
	pos *= exp2(_LOW_WISPS_FREQ);
	return DetailTwo.SampleLevel(LinearRepeatSampler, pos, 0).x;
	//return texture(usam_cumulusDetail2, pos).x;
}

float coverageNoise(float3 pos)
{
	float _LOW_BASE_FREQ = 0;

	pos *= exp2(_LOW_BASE_FREQ);
	float higherOctave = Base.SampleLevel(LinearRepeatSampler, pos.xy / 32.0, 0).x;  //texture(usam_cumulusBase, pos / 32.0).x;
	const float freq = 0.35;
	float baseNoise = worleyNoise(pos.xy * freq, 0x1919810);

	//float tileScale2 = 0.001; //0.00002;
	float4 NoiseSample = CloudBaseTex.SampleLevel(LinearRepeatSampler, pos * BaseScale, 0);  // Perlin-Worley + 3 octives of worley
	float PerlinWorley = NoiseSample.x;
	float3 Worley = NoiseSample.yzw;

	// FBM from low freq worley noises
	baseNoise = dot(Worley, float3(0.625, 0.25, 0.125));  //float erosion = 0.625 * profileNoise.y + 0.25 * profileNoise.z + 0.125 * profileNoise.w;

	return baseNoise + higherOctave * 0.75;
}

float3 detailCurlNoise(float3 pos)
{
	float _LOW_CURL_FREQ = 0;

	pos *= 0.5;
	pos *= exp2(_LOW_CURL_FREQ);
	return Curl.SampleLevel(LinearRepeatSampler, pos, 0).xyz;  //texture(usam_cumulusCurl, pos).xyz;
}

#	define DETAIL_NOISE 0

bool clouds_cu_density(float3 rayPos, float heightFraction, bool detail, out float densityOut, out float densityLodOut)
{
	float SETTING_CLOUDS_CU_COVERAGE = 0.5;
	rayPos = rayPos.xzy;

	// Base
	// Sample base 2D coverage noise and apply user coverage setting
	// Higher coverage setting = more cloud, applied as power curve for perceptual linearity
	float baseCoverage = coverageNoise(rayPos.xzy);
	float COVERAGE = SETTING_CLOUDS_CU_COVERAGE;
	float COVERAGE_P = pow2(COVERAGE);
	float COVERAGE_SQRT = sqrt(COVERAGE);

	// Remap coverage so low coverage settings aggressively clear sky, high settings fill it
	baseCoverage = max(baseCoverage - (1.0 - COVERAGE_P) * 0.8, 0.0);
	densityOut = baseCoverage * (1.0 - pow2(1.0 - COVERAGE));
	//

	// Precompute height fraction powers for vertical profile polynomials below
	float x1 = heightFraction;
	float x2 = x1 * x1;
	float x3 = x1 * x2;
	float x4 = x1 * x3;
	float4 xs = float4(x1, x2, x3, x4);

	// float CONE_FACTOR = lerp(5.0, 0.1, Cone);
	// float CONE_TOP_FACTOR = lerp(4.0, 10.0, Cone);
	//  float TOP_CURVE_FACTOR = float(TopCurve);
	// float BOTTOM_CURVE_FACTOR = float(BottomCurve);

	float CONE_FACTOR = lerp(5.0f, 0.1f, 0.5f);
	float CONE_TOP_FACTOR = lerp(4.0f, 10.0f, 0.5f);
	float TOP_CURVE_FACTOR = float(48);
	float BOTTOM_CURVE_FACTOR = float(128);

	// Vertical profile shaping - sculpts the cloud into a cumuliform anvil/dome shape:
	densityOut *= 1.5 - pow(heightFraction, CONE_FACTOR);                           // Wider base, narrowing toward top (cone shape)
	densityOut *= exp2(-(heightFraction)*CONE_TOP_FACTOR);                          // Exponential falloff toward cloud top
	densityOut *= saturate(1.0 - exp2(TOP_CURVE_FACTOR * (heightFraction - 1.0)));  // Smooth cap at cloud top
	densityOut *= saturate(1.0 - exp2(-BOTTOM_CURVE_FACTOR * heightFraction));      // Smooth fade at cloud base
	densityLodOut = densityOut;                                                     // Store pre-detail density for LOD/shadow ray sampling
	//

	float CU_BASE_DENSITY_THRESHOLD = 0.02;

	if (densityOut > CU_BASE_DENSITY_THRESHOLD) {
		//#if !defined(SETTING_SCREENSHOT_MODE) && defined(SETTING_CLOUDS_CU_WIND)
		//rayPos += uval_cuDetailWind;  // Apply wind offset to detail sample position
		//#endif

		// Stage 1
		// Curl noise distorts the detail sample position to break up the uniform look
		// Stronger curl at higher altitudes to create wispy tops
		float3 curlPos = rayPos;
		curlPos.y *= 1.3;
		float3 detailCurl = detailCurlNoise(curlPos);
		detailCurl *= 0.2 + 0.3 * pow2(heightFraction);
		// Suppress curl near cloud edges to avoid over-eroding thin density regions
		detailCurl *= LinearStep(lerp(CU_BASE_DENSITY_THRESHOLD, 1.0, pow(1.0 - heightFraction, 5)), 0.0, densityOut);
		//

		// Stage 2
		// Billowy detail - large puffy shapes, dominant at cloud base and mid levels
		float detail1Billowy = detailNoiseB(rayPos, detailCurl);
		// Erode the base with billowy detail to create the characteristic cauliflower bottom
		float bottomDetail = 1.0 - detail1Billowy * smoothstep(0.1, 0.0, heightFraction) * 2.0;
		float hc3 = _clouds_cu_heightCurveBillowy(xs);
		detail1Billowy *= hc3;            // Weight billowy detail by its height curve
		detail1Billowy *= COVERAGE_SQRT;  // Scale with coverage so sparse clouds stay detailed

#	if DETAIL_NOISE
		// Subtract detail from density using LinearStep - erodes cloud edges without darkening interior
		densityOut = LinearStep(saturate(detail1Billowy), 1.0, densityOut);
#	endif
		//

		// Stage 3
		// Wispy detail - thin fibrous shapes, dominant at cloud top
		//float detail1Wisp = detailNoiseW(rayPos + detailCurl * 2.0 * exp2(Params[1].w));
		float _LOW_WISPS_CURL_STR = 0.5;
		float detail1Wisp = detailNoiseW(rayPos + detailCurl * 2.0 * exp2(_LOW_WISPS_CURL_STR));

		detail1Wisp = pow2(detail1Wisp);  // Sharpen wisp contrast
		detail1Wisp *= COVERAGE_SQRT;
		float hc2 = _clouds_cu_heightCurveWisp(xs);
		detail1Wisp *= hc2;  // Weight wisp detail by its height curve

#	if DETAIL_NOISE
		densityOut = LinearStep(saturate(detail1Wisp), 1.0, densityOut);
#	endif
		//

		// Stage 4
		// Soften hard density transitions at cloud base, tighten them higher up
		// Prevents unnaturally sharp edges at the bottom while keeping crisp tops
		float hardEdgeBlend = LinearStep(0.0, 0.3, heightFraction);
		float minDetailDensity = lerp(0.001, 0.02, hardEdgeBlend);
		float edgeDesnityRange = lerp(0.08, 0.0015, hardEdgeBlend);
		densityOut *= smoothstep(minDetailDensity, minDetailDensity + edgeDesnityRange, densityOut);

		densityOut *= 1.0 + heightFraction * 16.0;  // Boost density toward cloud top for a denser, more opaque anvil cap
		densityOut *= bottomDetail;                 // Multiply (not subtract) bottom detail to add shape variation without just eroding sides

		if (densityOut > 0.0)
			return true;
	}

	//densityOut = 0.0;
	return false;
}

void TESTDensity(float3 rayPos, float heightFraction, bool detail, out float densityOut, out float densityLodOut)
{
	float SETTING_CLOUDS_CU_COVERAGE = 0.9;
	//rayPos = rayPos.xzy;

	// Base
	// Sample base 2D coverage noise and apply user coverage setting
	// Higher coverage setting = more cloud, applied as power curve for perceptual linearity
	float baseCoverage = coverageNoise(rayPos.xyz);
	float COVERAGE = SETTING_CLOUDS_CU_COVERAGE;
	float COVERAGE_P = pow2(COVERAGE);
	float COVERAGE_SQRT = sqrt(COVERAGE);

	// Remap coverage so low coverage settings aggressively clear sky, high settings fill it
	baseCoverage = max(baseCoverage - (1.0 - COVERAGE_P) * 0.8, 0.0);
	densityOut = baseCoverage * (1.0 - pow2(1.0 - COVERAGE));
	//

	// Precompute height fraction powers for vertical profile polynomials below
	float x1 = heightFraction;
	float x2 = x1 * x1;
	float x3 = x1 * x2;
	float x4 = x1 * x3;
	float4 xs = float4(x1, x2, x3, x4);

	// float CONE_FACTOR = lerp(5.0, 0.1, Cone);
	// float CONE_TOP_FACTOR = lerp(4.0, 10.0, Cone);
	//  float TOP_CURVE_FACTOR = float(TopCurve);
	// float BOTTOM_CURVE_FACTOR = float(BottomCurve);

	float CONE_FACTOR = lerp(5.0f, 0.1f, 0.5f);
	float CONE_TOP_FACTOR = lerp(4.0f, 10.0f, 0.5f);
	float TOP_CURVE_FACTOR = float(48);
	float BOTTOM_CURVE_FACTOR = float(128);

	// Vertical profile shaping - sculpts the cloud into a cumuliform anvil/dome shape:
	densityOut *= 1.5 - pow(heightFraction, CONE_FACTOR);                           // Wider base, narrowing toward top (cone shape)
	densityOut *= exp2(-(heightFraction)*CONE_TOP_FACTOR);                          // Exponential falloff toward cloud top
	densityOut *= saturate(1.0 - exp2(TOP_CURVE_FACTOR * (heightFraction - 1.0)));  // Smooth cap at cloud top
	densityOut *= saturate(1.0 - exp2(-BOTTOM_CURVE_FACTOR * heightFraction));      // Smooth fade at cloud base
	densityLodOut = densityOut;                                                     // Store pre-detail density for LOD/shadow ray sampling
	//

	//return;

	float CU_BASE_DENSITY_THRESHOLD = 0.02;

	if (densityOut > CU_BASE_DENSITY_THRESHOLD) {
		//#if !defined(SETTING_SCREENSHOT_MODE) && defined(SETTING_CLOUDS_CU_WIND)
		//rayPos += uval_cuDetailWind;  // Apply wind offset to detail sample position
		//#endif

		// Stage 1
		// Curl noise distorts the detail sample position to break up the uniform look
		// Stronger curl at higher altitudes to create wispy tops
		float3 curlPos = rayPos;
		curlPos.y *= 1.3;
		float3 detailCurl = detailCurlNoise(curlPos);
		detailCurl *= 0.2 + 0.3 * pow2(heightFraction);
		// Suppress curl near cloud edges to avoid over-eroding thin density regions
		detailCurl *= LinearStep(lerp(CU_BASE_DENSITY_THRESHOLD, 1.0, pow(1.0 - heightFraction, 5)), 0.0, densityOut);
		//

		// Stage 2
		// Billowy detail - large puffy shapes, dominant at cloud base and mid levels
		float detail1Billowy = detailNoiseB(rayPos, detailCurl);
		// Erode the base with billowy detail to create the characteristic cauliflower bottom
		float bottomDetail = 1.0 - detail1Billowy * smoothstep(0.1, 0.0, heightFraction) * 2.0;
		float hc3 = _clouds_cu_heightCurveBillowy(xs);
		detail1Billowy *= hc3;            // Weight billowy detail by its height curve
		detail1Billowy *= COVERAGE_SQRT;  // Scale with coverage so sparse clouds stay detailed

#	if DETAIL_NOISE
		// Subtract detail from density using LinearStep - erodes cloud edges without darkening interior
		densityOut = LinearStep(saturate(detail1Billowy), 1.0, densityOut);
#	endif
		//

		// Stage 3
		// Wispy detail - thin fibrous shapes, dominant at cloud top
		//float detail1Wisp = detailNoiseW(rayPos + detailCurl * 2.0 * exp2(Params[1].w));
		float _LOW_WISPS_CURL_STR = 0.5;
		float detail1Wisp = detailNoiseW(rayPos + detailCurl * 2.0 * exp2(_LOW_WISPS_CURL_STR));

		detail1Wisp = pow2(detail1Wisp);  // Sharpen wisp contrast
		detail1Wisp *= COVERAGE_SQRT;
		float hc2 = _clouds_cu_heightCurveWisp(xs);
		detail1Wisp *= hc2;  // Weight wisp detail by its height curve

#	if DETAIL_NOISE
		densityOut = LinearStep(saturate(detail1Wisp), 1.0, densityOut);
#	endif
		//

		// Stage 4
		// Soften hard density transitions at cloud base, tighten them higher up
		// Prevents unnaturally sharp edges at the bottom while keeping crisp tops
		float hardEdgeBlend = LinearStep(0.0, 0.3, heightFraction);
		float minDetailDensity = lerp(0.001, 0.02, hardEdgeBlend);
		float edgeDesnityRange = lerp(0.08, 0.0015, hardEdgeBlend);
		//densityOut *= smoothstep(minDetailDensity, minDetailDensity + edgeDesnityRange, densityOut);

		// densityOut *= 1.0 + heightFraction * 16.0;   // Boost density toward cloud top for a denser, more opaque anvil cap
		//densityOut *= bottomDetail;         // Multiply (not subtract) bottom detail to add shape variation without just eroding sides
	}
}

float3 rand_sampleInCone(float3 center, float coneHalfAngle, float2 rand)
{
	// Random azimuth angle
	float phi = (Math::PI * 2) * rand.x;

	// Uniform sampling on spherical cap
	float cosTheta = cos(coneHalfAngle);
	float cosAlpha = lerp(1.0, cosTheta, rand.y);
	float sinAlpha = sqrt(1.0 - cosAlpha * cosAlpha);

	// Build orthonormal basis (u, v, center)
	float3 other = abs(center.x) < 0.9 ? float3(1, 0, 0) : float3(0, 1, 0);
	float3 u = normalize(cross(center, other));
	float3 v = cross(center, u);

	// Final direction
	return normalize(cosAlpha * center + sinAlpha * (cos(phi) * u + sin(phi) * v));
}

#endif
