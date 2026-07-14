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

// This shader works in planet-center-relative KILOMETERS; the sky LUT code
// (physSkyData) works in game units. Keep this in sync with
// Util::Units::GAME_UNIT_TO_KM -- the C++ side asserts the scales agree.
static const float GAME_UNIT_TO_KM = 1.428e-5;

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

// Per-octave phase eccentricity attenuation for the sun light march
// (g *= 0.5 per octave, so deeply-scattered light goes isotropic).
// Default on per the cloud lighting upgrade plan.
#	ifndef CLOUD_SUN_OCTAVE_PHASE
#		define CLOUD_SUN_OCTAVE_PHASE 1
#	endif

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

// Optical depth toward the sun: 5 exponential steps, ~1.5 km total, coarse
// density only (erosion skipped). sunDir points TOWARD the sun, which at
// sunset goes DOWNWARD through the layer -- exiting through the layer BASE is
// the unoccluded case, exactly like exiting through the top. The atmosphere
// beyond the exit is already accounted for by the windowed sun-transmittance
// LUT; the two are independent path segments composed by multiplication.
float SunOpticalDepth(float3 samplePos, float3 sunDir, float extinction)
{
	float od = 0.0;
	float t = 0.0, dt = 0.05;  // km
	[unroll] for (int s = 0; s < 5; ++s)
	{
		t += dt;
		float3 p = samplePos + sunDir * t;
		// Unsaturated height: the exit test must see out-of-layer values
		// (GetEnvelopeRelativeZ saturates and would never trigger it).
		float h = LinearStep(bottomRadius, topRadius, p.z);
		if (h < 0.0 || h > 1.0)
			break;  // exited layer (through base OR top) -> unoccluded beyond
		od += GetCloudProfile(p, h) * dt * extinction;
		dt *= 2.0;
	}
	return od;
}

// Wrenninge multi-scatter octaves: fakes deep multiple scattering of the
// (gray, spectrally neutral) droplet medium.
float SunVisibilityMS(float od)
{
	float vis = 0.0;
	float a = 1.0, b = 1.0;
	[unroll] for (int o = 0; o < 3; ++o)
	{
		vis += b * exp(-a * od);
		a *= OctaveAttenA;
		b *= OctaveAttenB;
	}
	return vis;
}

#	if CLOUD_SUN_OCTAVE_PHASE
// Octave sum with per-octave phase folded in:
// sum_o b^o * exp(-a^o * od) * phase(g * 0.5^o).
float SunVisibilityMSPhased(float od, float3 phaseOctaves)
{
	float vis = 0.0;
	float a = 1.0, b = 1.0;
	[unroll] for (int o = 0; o < 3; ++o)
	{
		vis += b * exp(-a * od) * phaseOctaves[o];
		a *= OctaveAttenA;
		b *= OctaveAttenB;
	}
	return vis;
}
#	endif

static const float3 CLOUD_AMBIENT = float3(0.4, 0.45, 0.5);  // flat skylight fill into the cloud
static const float CLOUD_MS_GAIN = 1.8;                      // flat multiple-scatter boost
// Kept for reference; superseded by ComputeLightingV3.
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

// Direct sun transmittance at a raymarch sample, from the per-frame windowed
// LUT (LUTGEN 4), parameterized by the sample's actual altitude and sun zenith
// cosine -- including mu < 0 (afterglow/underlighting). Debug overrides sit
// AFTER the UV remap decision so remap bugs are excluded from
// application-side tests.
float3 SampleCloudSunTr(float3 posPlanetRel)
{
	if (DebugSunTrMode == 1)
		return 1.0;
	if (DebugSunTrMode == 2)
		return float3(1.0, 0.35, 0.08);
	if (DebugSunTrMode == 3) {
		// A/B: sample the GLOBAL Tr LUT through its real remap instead. The
		// global LUT works in game units, unlike this shader.
		float2 uvGlobal = PhysSky::TrLutUvPlanet(posPlanetRel / GAME_UNIT_TO_KM, SharedData::physSkyData.sunDir);
		return PhysSky::TexTrLut.SampleLevel(LinearSampler, uvGlobal, 0).rgb;
	}
	float r = length(posPlanetRel);
	float mu = dot(posPlanetRel / r, SharedData::physSkyData.sunDir);
	float2 uv = float2((mu - cloudTrMuMin) / (cloudTrMuMax - cloudTrMuMin),
		(r - cloudTrRBot) / (cloudTrRTop - cloudTrRBot));
	return TexCloudSunTr.SampleLevel(LinearSampler, saturate(uv), 0).rgb;
}

// Pre-integrated ambient (LUTGEN 5 endpoints), lerped by in-layer height.
float3 EvalCloudAmbient(float heightFrac, float3 ambBottom, float3 ambTop)
{
	switch (DebugAmbientMode) {
	case 1:
		return DebugColor;
	case 2:
		return float3(1, 0, 0);
	case 3:
		return lerp(float3(1, 0, 0), float3(0, 0, 1), heightFrac);
	}
	return lerp(ambBottom, ambTop, heightFrac);
}

// sunPhaseVis is the phase-weighted sun visibility from the in-cloud light
// march (Wrenninge octave sum, with the phase folded in per octave when
// CLOUD_SUN_OCTAVE_PHASE is on).
void ComputeLightingV3(float density, float stepLength, float3 sunPhaseVis, float heightFrac,
	float3 sunTr, float3 ambBottom, float3 ambTop,
	CloudParticpatingMedium medium,
	inout float3 Inscattering, inout float Transmittance)
{
	float3 albedo = medium.scattering / medium.extinction;
	float extinction = medium.extinction.x * density;
	float tr = exp(-extinction * stepLength);

	// physSkyData.sunlightColor is the constant TOA illuminance (white); ALL
	// time-of-day color enters through sunTr. SharedData::DirLightColor is
	// artist-tinted at sunset and would double-tint. The sun is never clamped
	// here -- the Tr LUT decides when light stops arriving.
	float3 sunRad = SharedData::physSkyData.sunlightColor * sunTr * sunPhaseVis * SunGain * SunMsGain;
	float3 ambRad = EvalCloudAmbient(heightFrac, ambBottom, ambTop) * AmbientGain;  // no phase: pre-integrated

	float3 inscatter = (sunRad + ambRad) * albedo * (1.0 - tr);

	Inscattering += inscatter * Transmittance;
	Transmittance *= tr;
}

PixelOut main(VertexOut input)
{
	PixelOut output;

	output.color = float4(0, 0, 0, 0);
	output.depth = 1.0;

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

	// The sun path is coherent around the real sun (physSkyData.sunDir):
	// phase, windowed transmittance and (later) the light march all use the
	// same vector. DirLightDirection may be a moon at night.
	float cosTheta = dot(ray.direction, SharedData::physSkyData.sunDir);
	float StepLength = (RayT.y - RayT.x) / RAY_SAMPLES;

	float3 Inscattering = float3(0, 0, 0);
	float Transmittance = 1.0;

	// Albedo ~0.996 (24.9 / 25): a 0.4 albedo makes cloud interiors charcoal
	// and kills any twilight glow penetration.
	CloudParticpatingMedium medium;
	medium.scattering = CloudScattering;
	medium.extinction = CloudExtinction;
	medium.phase = CloudPhase(cosTheta);

#	if CLOUD_SUN_OCTAVE_PHASE
	// View-constant per-octave phases, eccentricity halved each octave.
	float3 phaseOctaves = float3(
		CloudPhase(cosTheta, 1.0).x,
		CloudPhase(cosTheta, 0.5).x,
		CloudPhase(cosTheta, 0.25).x);
#	endif

	float3 ambBottom = TexCloudAmbient.Load(int3(0, 0, 0)).rgb;
	float3 ambTop = TexCloudAmbient.Load(int3(1, 0, 0)).rgb;

	float TrDepthSum = 0.0;
	float TrSum = 0.0;

	for (int i = 0; i < RAY_SAMPLES; i++) {
		float3 SamplePos = ray.direction * (RayT.x + i * StepLength) + cameraPos;
		float EnvelopeZ = GetEnvelopeRelativeZ(SamplePos, float2(bottomRadius, topRadius));

		float CloudDensity = GetCloudProfile(SamplePos, EnvelopeZ);
		if (CloudDensity <= 0.0)
			continue;

		TrDepthSum += Transmittance * (RayT.x + i * StepLength);
		TrSum += Transmittance;

		CloudRaymarchStepState state;
		state.height = EnvelopeZ;
		state.rayStep = float4(0, 0, 0, StepLength);
		state.upVector = normalize(SamplePos);

		float3 sunTr = SampleCloudSunTr(SamplePos);

		// In-cloud sun light march + multi-scatter octaves. Skipped once the
		// view transmittance no longer matters (the ambient term still
		// accumulates for the step).
		float3 sunPhaseVis = 0;
		if (Transmittance >= 0.01) {
			float od = SunOpticalDepth(SamplePos, SharedData::physSkyData.sunDir, medium.extinction.x);
#	if CLOUD_SUN_OCTAVE_PHASE
			sunPhaseVis = SunVisibilityMSPhased(od, phaseOctaves);
#	else
			sunPhaseVis = SunVisibilityMS(od) * medium.phase;
#	endif
		}
		ComputeLightingV3(CloudDensity, StepLength, sunPhaseVis, EnvelopeZ, sunTr, ambBottom, ambTop, medium, Inscattering, Transmittance);
	}
	/////////////////////////////////////////////

	// TODO: gamma placement. Encoding here is only valid if the composite
	// consumes gamma; if clouds ever blend with the linear-HDR sky
	// pre-tonemap, this shifts hues exactly at twilight, where the
	// channel ratios are extreme. Revisit when the compose path is settled.
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
#ifdef CLOUD_DEBUG_BLIT_PS

Texture2D<float4> BlitTex : register(t0);

// Debug overlay blit: nearest-fetches the bound LUT into a screen-corner
// viewport rect (the windowed sun-Tr LUT scaled up, and the 2x1 ambient
// endpoints as two swatches).
float4 main(VertexOut input) : SV_TARGET0
{
	uint2 dims;
	BlitTex.GetDimensions(dims.x, dims.y);
	uint2 coord = min(uint2(input.TexCoord * dims), dims - 1);
	float3 blitColor = BlitTex.Load(int3(coord, 0)).rgb;
	// The main RT holds gamma-encoded values at this point in the frame.
	return float4(Color::LLLinearToGamma(blitColor), 1.0);
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
