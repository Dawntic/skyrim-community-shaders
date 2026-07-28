#ifndef CLOUD_SHADOWS_HLSLI
#define CLOUD_SHADOWS_HLSLI

#ifndef CLOUD_SHADOW_REGISTER
#	define CLOUD_SHADOW_REGISTER t25
#endif

#ifndef CLOUD_SELFSHADOW_REGISTER
#	define CLOUD_SELFSHADOW_REGISTER t26
#endif

#ifndef CLOUD_VOL_SHADOW_REGISTER
#	define CLOUD_VOL_SHADOW_REGISTER t27
#endif

#include "Common/FrameBuffer.hlsli"
#include "Common/Game.hlsli"

namespace CloudShadows
{
	TextureCube<float> CloudShadowsTexture : register(CLOUD_SHADOW_REGISTER);
	TextureCube<float> CloudSelfShadowTexture : register(CLOUD_SELFSHADOW_REGISTER);

	// Beer-Lambert transmittance of the Physical Sky volumetric cloud layer,
	// projected onto a horizontal plane at the layer base. See
	// CloudShadows/VolumetricCloudShadowMapCS.hlsl for how it is built.
	Texture2D<float> VolumetricCloudShadowMap : register(CLOUD_VOL_SHADOW_REGISTER);

	const static float CloudHeight = (2e3f / GAME_UNIT_TO_M);
	const static float PlanetRadius = (6371e3f / GAME_UNIT_TO_M);
	const static float RcpHPlusR = (1.0 / (CloudHeight + PlanetRadius));

	float IntersectCloudDist(float3 rel_pos, float3 dir)
	{
		float r = PlanetRadius;
		float3 p = (rel_pos + float3(0, 0, r)) * RcpHPlusR;
		float dotprod = dot(p, dir);
		float lengthsqr = dot(p, p);
		if (lengthsqr > 1.0)
			return -1.0;

		return (-dotprod + sqrt(dotprod * dotprod - lengthsqr + 1.0)) * (r + CloudHeight);
	}

	float3 GetCloudShadowSampleDir(float3 rel_pos, float3 eye_to_sun)
	{
		float r = PlanetRadius;
		float3 p = (rel_pos + float3(0, 0, r)) * RcpHPlusR;
		float dotprod = dot(p, eye_to_sun);
		float t = -dotprod + sqrt(dotprod * dotprod - dot(p, p) + 1);
		float3 v = (p + eye_to_sun * t) * (r + CloudHeight) - float3(0, 0, r);
		return v;
	}

	// Volumetric (Physical Sky) path. `worldPosition` is camera-relative, in game
	// units, matching the cubemap path.
	float GetVolumetricCloudShadowMult(float3 worldPosition, SamplerState textureSampler)
	{
		const SharedData::CloudShadowsSettings shadowSettings = SharedData::cloudShadowsSettings;

		float3 posWorld = worldPosition + FrameBuffer::CameraPosAdjust.xyz;

		// Above the layer there is nothing left to occlude the light. (Skyrim's
		// terrain tops out far below a 1 km cloud base, but sky geometry and the
		// aerial-perspective march both feed positions up here.)
		if (posWorld.z >= shadowSettings.VolLayerTopZ)
			return 1.0;

		// Walk the light ray to the plane the map is built on. Receivers inside
		// the layer get the whole column's transmittance rather than the part
		// above them -- an over-estimate of the shadow, but the alternative is a
		// second march per receiver, and by the time anything is up there it is
		// already inside the cloud's own scattering.
		float t = max((shadowSettings.VolLayerBottomZ - posWorld.z) * shadowSettings.VolRcpLightZ, 0.0);
		float2 planeHit = posWorld.xy + shadowSettings.VolLightDirXY * t;

		float2 uv = (planeHit - shadowSettings.VolCenter) * shadowSettings.VolRcpExtent * 0.5 + 0.5;

		// Mip by view distance: one screen pixel covers VolMipScale shadow texels
		// per unit of distance, so this keeps the read at (or above) the footprint
		// the pixel actually integrates and stops the map aliasing into specks.
		float mip = clamp(log2(1.0 + length(worldPosition) * shadowSettings.VolMipScale) + shadowSettings.VolMipBias,
			0.0, shadowSettings.VolMaxMip);

		float shadow = VolumetricCloudShadowMap.SampleLevel(textureSampler, saturate(uv), mip);

		// The map only covers a finite square around the camera. Ramp back to
		// unshadowed across its border so the edge is not a visible line.
		float2 borderFade = saturate((0.5 - abs(uv - 0.5)) * shadowSettings.VolRcpBorderFade);
		shadow = lerp(1.0, shadow, min(borderFade.x, borderFade.y));

		// Opacity scales the optical depth, which keeps the result a Beer-Lambert
		// transmittance for any slider value (0 = unshadowed, 1 = physical).
		return pow(max(shadow, 1e-4), SharedData::cloudShadowsSettings.Opacity);
	}

	float GetCloudShadowMult(float3 worldPosition, SamplerState textureSampler)
	{
		[branch] if (SharedData::cloudShadowsSettings.VolumetricEnabled)
		{
			return GetVolumetricCloudShadowMult(worldPosition, textureSampler);
		}

		float3 cloudSampleDir = GetCloudShadowSampleDir(worldPosition, SharedData::DirLightDirection.xyz).xyz;
		float cloudCubeSample = CloudShadowsTexture.SampleLevel(textureSampler, cloudSampleDir, 0).x;
		return saturate(1.0 - cloudCubeSample * SharedData::cloudShadowsSettings.Opacity);
	}
}

#endif
