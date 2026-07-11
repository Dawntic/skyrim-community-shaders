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
