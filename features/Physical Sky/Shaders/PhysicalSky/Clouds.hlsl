#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "PhysicalSky/CloudCommon.hlsli"

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

	float3 sunRad = SharedData::physSkyData.sunlightColor * sunTr * sunPhaseVis * SunGain;
	float3 ambRad = EvalCloudAmbient(heightFrac, ambBottom, ambTop) * AmbientGain;  // no phase - pre-integrated

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

	float cosTheta = dot(ray.direction, SharedData::physSkyData.sunDir);
	float StepLength = (RayT.y - RayT.x) / RAY_SAMPLES;

	float3 Inscattering = float3(0, 0, 0);
	float Transmittance = 1.0;

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

	float3 ambBottom = TexCloudAmbient.Load(int3(0, 0, 0)).xyz;
	float3 ambTop = TexCloudAmbient.Load(int3(1, 0, 0)).xyz;

	float TrDepthSum = 0.0;
	float TrSum = 0.0;

	for (int i = 0; i < RAY_SAMPLES; i++) {
		float3 SamplePos = ray.direction * (RayT.x + i * StepLength) + cameraPos;
		float EnvelopeZ = GetEnvelopeRelativeZ(SamplePos, float2(bottomRadius, topRadius));

		float CloudDensity = GetCloudProfile(SamplePos, EnvelopeZ);
		if (CloudDensity <= 0.0)
			continue;

		// Sculpt billows/wisps into the base shape (view march only).
		CloudDensity = ApplyCloudDetail(CloudDensity, SamplePos, EnvelopeZ, RayT.x + i * StepLength);
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
			float SunOpticalDepth = GetOpticalDepth(SamplePos, SharedData::physSkyData.sunDir, medium.extinction.x);
#	if CLOUD_SUN_OCTAVE_PHASE
			sunPhaseVis = SunVisibilityMSPhased(SunOpticalDepth, phaseOctaves);
#	else
			sunPhaseVis = SunVisibilityMS(SunOpticalDepth) * medium.phase;
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
	float3 blitColor = BlitTex.Load(int3(coord, 0)).xyz;
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
