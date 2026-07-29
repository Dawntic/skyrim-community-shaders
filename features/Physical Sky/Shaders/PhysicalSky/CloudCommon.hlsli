// PhysSky namespace + SharedData/Math/Color, needed by the cloud helpers below
// (SampleCloudSunTr samples the global Tr LUT). Guarded, so re-including it from
// the translation units that also pull it in directly is a no-op.
#include "PhysicalSky/Common.hlsli"

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
	float Scroll;  // z offset of the noise lookups; drag/animate to evolve the pattern

	// Detail sculpting pass (billows/wisps), see ApplyCloudDetail
	float DetailFrequency;     // detail noise frequency, as a multiple of the base noise scale
	float DetailStrength;      // max fraction of the density range the detail may erode (0 = off)
	float DetailCurlScale;     // curl lookup frequency, as a multiple of the base noise scale
	float DetailCurlStrength;  // km of turbulent lookup distortion at the cloud base

	float DetailFadeStart;  // km -- detail fades out over [start, end]; the 32^3
	float DetailFadeEnd;    // texture has no mips, so it goes subpixel past this
	float2 cloudDataPad;
};

// Debug/verification seams for the cloud lighting chain. Uniform flow control
// on purpose: a single shader build supports the whole verification ladder.
cbuffer CloudDebugCB : register(b1)
{
	uint DebugSunTrMode;    // 0 live | 1 force white(=1) | 2 force orange | 3 A/B: sample GLOBAL Tr LUT instead
	uint DebugAmbientMode;  // 0 live | 1 DebugColor | 2 literal red | 3 red->blue height gradient
	float2 debugPad0;

	float3 DebugColor;
	float SunGain;  // sun path gain; 0 while validating ambient

	float AmbientGain;
	float cloudTrMuMin;  // windowed sun-transmittance LUT axes; radii in km
	float cloudTrMuMax;  // (this shader's scale -- the LUT was generated over
						 // the same normalized axes in game units)
	float cloudTrRBot;

	float cloudTrRTop;
	float OctaveAttenA;  // Wrenninge octave extinction attenuation
	// Cloud droplets are spectrally neutral with albedo ~0.996; no color may
	// be injected in the medium itself.
	float CloudScattering;  // km^-1 (default 24.9)
	float CloudExtinction;  // km^-1 (default 25)
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
#define GAME_UNIT_TO_KM 1.428e-5

// General ////////////////////////////////////////////////

float LinearStep(float lo, float hi, float x)
{
	return (x - lo) / (hi - lo);
}
float2 LinearStep(float2 lo, float2 hi, float2 x)
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
///////////////////////////////////////////////////////////

struct CloudParticpatingMedium
{
	float3 scattering;
	float3 extinction;
	float3 phase;
};

struct CloudRaymarchStepState
{
	float height;
	float4 rayStep;
	float3 upVector;
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

// Cloud lighting march ///////////////////////////////////
// Shared by the view raymarch (Clouds.hlsl) and the skylighting probe march
// (UpdateProbesCS.hlsl).

float GetCloudProfile(float3 SamplePos, float Height)
{
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

float ApplyCloudDetail(float BaseDensity, float3 SamplePos, float Height, float ViewDistance)
{
	float fade = 1.0 - LerpLinearStepClamped(ViewDistance, DetailFadeStart, DetailFadeEnd, 0.0, 1.0);
	float strength = DetailStrength * fade;
	if (strength <= 0.0)
		return BaseDensity;

	// Turbulent advection: curl-distort the detail lookup (2D field, strongest
	// at the cloud base where wind shear lives).
	float2 curl = CurlNoiseTex.SampleLevel(LinearRepeatSampler, SamplePos.xy * (HeightScale * DetailCurlScale), 0).xy * 2.0 - 1.0;
	float3 detailPos = SamplePos + float3(curl * ((1.0 - Height) * DetailCurlStrength), Scroll);

	float3 worley = CloudDetailTex.SampleLevel(LinearRepeatSampler, detailPos * (HeightScale * DetailFrequency), 0).xyz;
	float detailFBM = dot(worley, float3(0.625, 0.25, 0.125));

	// Wispy at the base, billowy at the top.
	float erosion = lerp(detailFBM, 1.0 - detailFBM, saturate(Height * 10.0)) * strength;

	return LerpLinearStepClamped(BaseDensity, erosion, 1.0, 0.0, 1.0);
}

// Optical depth toward the sun: 5 exponential steps, ~1.5 km total, coarse
// density only (erosion skipped). sunDir points TOWARD the sun, which at
// sunset goes DOWNWARD through the layer -- exiting through the layer BASE is
// the unoccluded case, exactly like exiting through the top. The atmosphere
// beyond the exit is already accounted for by the windowed sun-transmittance
// LUT; the two are independent path segments composed by multiplication.
float GetOpticalDepth(float3 Pos, float3 Dir, float extinction)
{
	float OpticalDepth = 0.0;
	float t = 0.0, dt = 0.05;  // km
	[unroll] for (int i = 0; i < 5; ++i)
	{
		t += dt;
		float3 SamplePos = Pos + Dir * t;
		// Unsaturated height: the exit test must see out-of-layer values
		float Height = LinearStep(bottomRadius, topRadius, SamplePos.z);
		if (Height < 0.0 || Height > 1.0)
			break;
		OpticalDepth += GetCloudProfile(SamplePos, Height) * dt * extinction;
		dt *= 2.0;
	}
	return OpticalDepth;
}

static const float OCTAVE_ENERGY_ATTEN = 0.6;

// Wrenninge multi-scatter octaves: fakes deep multiple scattering of spectrally neutral droplet medium.
float SunVisibilityMS(float SunOpticalDepth)
{
	float vis = 0.0;
	float a = 1.0, b = 1.0;
	[unroll] for (int i = 0; i < 3; ++i)
	{
		vis += b * exp(-a * SunOpticalDepth);
		a *= OctaveAttenA;
		b *= OCTAVE_ENERGY_ATTEN;
	}
	return vis;
}

// Per-octave phase eccentricity attenuation for the sun light march
// (g *= 0.5 per octave, so deeply-scattered light goes isotropic).
// Default on per the cloud lighting upgrade plan.
#ifndef CLOUD_SUN_OCTAVE_PHASE
#	define CLOUD_SUN_OCTAVE_PHASE 1
#endif

#if CLOUD_SUN_OCTAVE_PHASE
// Octave sum with per-octave phase
float SunVisibilityMSPhased(float SunOpticalDepth, float3 phaseOctaves)
{
	float vis = 0.0;
	float a = 1.0, b = 1.0;
	[unroll] for (int i = 0; i < 3; ++i)
	{
		vis += b * exp(-a * SunOpticalDepth) * phaseOctaves[i];
		a *= OctaveAttenA;
		b *= OCTAVE_ENERGY_ATTEN;
	}
	return vis;
}
#endif

// Direct sun transmittance at a raymarch sample, from the per-frame windowed
// LUT (LUTGEN 4), parameterized by the sample'i actual altitude and sun zenith
// cosine -- including mu < 0 (afterglow/underlighting). Debug overrides sit
// AFTER the UV remap decision so remap bugs are excluded from
// application-side tests.
static const float REFRACTION_MU_BIAS = 0.009;

float3 SampleCloudSunTr(float3 posPlanetRel)
{
	if (DebugSunTrMode == 1)
		return 1.0;
	if (DebugSunTrMode == 2)
		return float3(1.0, 0.35, 0.08);
	if (DebugSunTrMode == 3) {
		float2 uvGlobal = PhysSky::TrLutUvPlanet(posPlanetRel / GAME_UNIT_TO_KM, SharedData::physSkyData.sunDir);
		return PhysSky::TexTrLut.SampleLevel(LinearSampler, uvGlobal, 0).xyz;
	}
	float r = length(posPlanetRel);
	float mu = dot(posPlanetRel / r, SharedData::physSkyData.sunDir);
	// Atmospheric refraction lifts the apparent sun ~0.5 deg at the horizon extending the underlighting window
	mu += REFRACTION_MU_BIAS * (mu < 0.0);
	float2 uv = float2(LinearStep(cloudTrMuMin, cloudTrMuMax, mu), LinearStep(cloudTrRBot, cloudTrRTop, r));
	return TexCloudSunTr.SampleLevel(LinearSampler, saturate(uv), 0).xyz;
}
///////////////////////////////////////////////////////////
