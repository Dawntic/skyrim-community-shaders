#ifndef __SKYLIGHTING_DEPENDENCY_HLSL__
#define __SKYLIGHTING_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"
#include "Common/Shading.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonicsRGB.hlsli"

namespace Skylighting
{
#if defined(SKYLIGHTING_PROBE_REGISTER)
	Texture3D<sh2> SkylightingProbeArray : register(SKYLIGHTING_PROBE_REGISTER);
	Texture2DArray<float4> SparseProbeArray : register(t51);
	Texture2D<float4> BentNormalAtlas : register(t52);
	Texture2D<float4> BentNormalTile : register(t53);
#elif defined(PSHADER)
	Texture3D<sh2> SkylightingProbeArray : register(t50);
	Texture2DArray<float4> SparseProbeArray : register(t51);
	Texture2D<float4> BentNormalAtlas : register(t52);
	Texture2D<float4> BentNormalTile : register(t53);
#endif

	const static sh2 UNIT_SH = float4(sqrt(4.0 * Math::PI), 0, 0, 0);

	const static uint3 ARRAY_DIM = uint3(256, 256, 128);
	const static float3 ARRAY_SIZE = 10000.f * float3(1, 1, 0.5);
	const static float3 CELL_SIZE = ARRAY_SIZE / ARRAY_DIM;

	float GetFadeOutFactor(float3 positionMS)
	{
		float3 uvw = saturate(positionMS / ARRAY_SIZE + .5);
		float3 dists = min(uvw, 1 - uvw);
		float edgeDist = min(dists.x, min(dists.y, dists.z));
		return saturate(edgeDist * 20);
	}

	float MixDiffuse(float visibility)
	{
		return lerp(SharedData::skylightingSettings.MinDiffuseVisibility, 1.0, visibility);
	}

	float MixSpecular(float visibility)
	{
		return lerp(SharedData::skylightingSettings.MinSpecularVisibility, 1.0, visibility);
	}

	float EvaluateDiffuse(sh2 skylightingSH, float3 normal, float fadeOutFactor = 1.0)
	{
		float visibility = SH::FuncProductIntegral(skylightingSH, SH::EvaluateCosineLobe(normal)) / Math::PI;
		visibility = lerp(1.0, saturate(visibility), fadeOutFactor);
		return MixDiffuse(visibility);
	}

	float EvaluateSpecular(sh2 skylightingSH, sh2 specularLobe, float fadeOutFactor = 1.0)
	{
		float visibility = SH::FuncProductIntegral(skylightingSH, specularLobe);
		visibility = lerp(1.0, saturate(visibility), fadeOutFactor);
		return MixSpecular(visibility);
	}

#if defined(PSHADER) || defined(SKYLIGHTING_PROBE_REGISTER)
	void ApplySkylighting(inout float3 diffuseColor, inout float3 directionalAmbientColor, float3 albedo, float skylightingDiffuse)
	{
		float maxScale = 1.0;
		if (directionalAmbientColor.x > 0.0)
			maxScale = min(maxScale, diffuseColor.x / directionalAmbientColor.x);
		if (directionalAmbientColor.y > 0.0)
			maxScale = min(maxScale, diffuseColor.y / directionalAmbientColor.y);
		if (directionalAmbientColor.z > 0.0)
			maxScale = min(maxScale, diffuseColor.z / directionalAmbientColor.z);
		directionalAmbientColor *= maxScale;

		diffuseColor = max(0.0, diffuseColor - directionalAmbientColor);

		float3 linAmbient = Color::IrradianceToLinear(directionalAmbientColor);
		float3 multiBounceSkylighting = MultiBounceAO(albedo, skylightingDiffuse);
		directionalAmbientColor = Color::IrradianceToGamma(linAmbient * multiBounceSkylighting);

		diffuseColor += directionalAmbientColor;
	}
	//#endif

	float2 GetAtlasUV(float3 WorldPosition)
	{
		const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;

		const float2 atlasMin = settings.AtlasBounds.xy;
		const float2 atlasMax = settings.AtlasBounds.zw;
		WorldPosition += FrameBuffer::CameraPosAdjust.xyz;

		float2 CoordsUV = (WorldPosition.xy - atlasMin) / (atlasMax - atlasMin);
		CoordsUV.y = 1.0 - CoordsUV.y;

		return saturate(CoordsUV);
	}

	sh2 BentNormalToSH(float3 bentNormal, float visibility, bool test)
	{
		sh2 result;

		//if(test)
		//	return SH::Scale(SH::Evaluate(bentNormal), 4.0 * Math::PI * visibility);

		sh2 basis = SH::Evaluate(bentNormal);

		float solidAngle = 2.0 * Math::PI * visibility;
		result.x = basis.x * solidAngle;

		float cosTheta = 1.0 - visibility;
		float SinSq = 1.0 - cosTheta * cosTheta;
		result.yzw = basis.yzw * SinSq * Math::PI;

		return result;
	}

	sh2 SampleBentNormalSH(float3 positionCR, SamplerState samp, bool test)
	{
		const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;

		if (SharedData::InInterior)
			return UNIT_SH;

		const float2 positionWS = positionCR.xy + FrameBuffer::CameraPosAdjust.xy;

		// Inside the streamed tile? Its rows run south to north, so v needs no flip.
		if (settings.HasBentNormalTile) {
			const float2 tileMin = settings.BentNormalTileBounds.xy;
			const float2 tileMax = settings.BentNormalTileBounds.zw;

			if (all(positionWS >= tileMin) && all(positionWS < tileMax)) {
				const float2 uv = (positionWS - tileMin) / (tileMax - tileMin);
				float4 packed = BentNormalTile.SampleLevel(samp, uv, 0);
				packed.xyz = packed.xyz * 2.0 - 1.0;
				return BentNormalToSH(packed.xyz, packed.w, test);
			}
		}

		// Outside it, or with no tile resident: the atlas, which is stitched north up so v flips.
		if (settings.HasBentNormalAtlas) {
			const float2 atlasMin = settings.AtlasBounds.xy;
			const float2 atlasMax = settings.AtlasBounds.zw;

			if (all(positionWS >= atlasMin) && all(positionWS < atlasMax)) {
				float2 uv = (positionWS - atlasMin) / (atlasMax - atlasMin);
				uv.y = 1.0 - uv.y;
				float4 packed = BentNormalAtlas.SampleLevel(samp, uv, 0);
				packed.xyz = packed.xyz * 2.0 - 1.0;
				return BentNormalToSH(packed.xyz, packed.w, test);
			}
		}

		return UNIT_SH;
	}

	sh2vec3 SampleIrradianceProbe(float3 WorldPosition, SamplerState samp)
	{
		float2 CoordsUV = GetAtlasUV(WorldPosition);

		sh2vec3 probe;
		probe.x = SparseProbeArray.SampleLevel(samp, float3(CoordsUV, 0), 0);
		probe.y = SparseProbeArray.SampleLevel(samp, float3(CoordsUV, 1), 0);
		probe.z = SparseProbeArray.SampleLevel(samp, float3(CoordsUV, 2), 0);
		return probe;
	}

	float3 CalculateAmbientIrradiance(float3 WorldPosition, float3 worldNormal, sh2 skylightingSH, SamplerState Sampler)
	{
		const SharedData::SkylightingSettings sparseSettings = SharedData::skylightingSettings;

		sh2vec3 IrradianceProbe = SampleIrradianceProbe(WorldPosition, Sampler);

		skylightingSH = lerp(SH::UnitSH2(), skylightingSH, GetFadeOutFactor(WorldPosition));
		sh2 bentNormalSH = SampleBentNormalSH(WorldPosition, Sampler, 0);

		float SkyAO = SH::Unproject(skylightingSH, worldNormal);
		float BentAO = SH::Unproject(bentNormalSH, worldNormal);

		sh2 SkyLobe = SkyAO < BentAO ? skylightingSH : bentNormalSH;  //min(SkyAO, BentAO);
		SkyLobe = SH::LerpSH2(SharedData::skylightingSettings.MinDiffuseVisibility, 1.0, SkyLobe);
		SkyLobe = SH::UnitSH2();
		SkyLobe = SH::Product(SH::EvaluateCosineLobe(worldNormal), SkyLobe);

		float3 SkyIrradiance = SH::FuncProductIntegral(IrradianceProbe, SkyLobe);
		SkyIrradiance = max(SkyIrradiance / Math::PI, 0);

		return SkyIrradiance;
	}

	sh2 Sample(float3 positionMS, float3 normalWS)
	{
		sh2 scaledUnitSH = UNIT_SH;  // / 1e-10;

		if (SharedData::InInterior)
			return scaledUnitSH;

		positionMS.xyz += normalWS * CELL_SIZE * 0.5;  // Receiver normal bias

		float3 positionMSAdjusted = positionMS - SharedData::skylightingSettings.PosOffset.xyz;
		float3 uvw = positionMSAdjusted / ARRAY_SIZE + .5;

		if (any(uvw < 0) || any(uvw > 1))
			return scaledUnitSH;

		float3 cellVxCoord = uvw * ARRAY_DIM;
		int3 cell000 = floor(cellVxCoord - 0.5);
		float3 trilinearPos = cellVxCoord - 0.5 - cell000;

		sh2 sum = 0;
		float wsum = 0;
		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 2; j++)
				for (int k = 0; k < 2; k++) {
					int3 offset = int3(i, j, k);
					int3 cellID = cell000 + offset;

					if (any(cellID < 0) || any((uint3)cellID >= ARRAY_DIM))
						continue;

					float3 cellCentreMS = cellID + 0.5 - ARRAY_DIM / 2;
					cellCentreMS = cellCentreMS * CELL_SIZE;

					// https://handmade.network/p/75/monter/blog/p/7288-engine_work__global_illumination_with_irradiance_probes
					// basic tangent checks
					float tangentWeight = dot(normalize(cellCentreMS - positionMSAdjusted), normalWS) * 0.5 + 0.5;

					float3 trilinearWeights = 1 - abs(offset - trilinearPos);
					float w = trilinearWeights.x * trilinearWeights.y * trilinearWeights.z * tangentWeight;

					uint3 cellTexID = (cellID + SharedData::skylightingSettings.ArrayOrigin.xyz) % ARRAY_DIM;
					sh2 probe = SH::Scale(SkylightingProbeArray[cellTexID], w);

					sum = SH::Add(sum, probe);
					wsum += w;
				}

		sh2 result = SH::Scale(sum, rcp(wsum + EPSILON_WEIGHT_SUM));

		return result;
	}

	// Compute skylighting diffuse for a receiver biased to face upward (grass/foliage).
	// The result is pre-divided by vertexAO so that a subsequent multiply by vertexAO
	// yields min(skylightingDiffuse, vertexAO). Pass vertexAO = 1 to skip this compensation.
	float GetVertexSkylightingDiffuse(float3 positionMS, float3 normalWS, float vertexAO)
	{
		if (SharedData::InInterior)
			return 1.0;

		float fadeOutFactor = GetFadeOutFactor(positionMS);

		float3 biasedNormal = normalWS;
		biasedNormal.z = max(0.0, biasedNormal.z);
		biasedNormal = normalize(biasedNormal);

		sh2 skylightingSH = Sample(positionMS, normalWS);
		float skylightingDiffuse = EvaluateDiffuse(skylightingSH, biasedNormal, fadeOutFactor);

		return saturate(skylightingDiffuse / max(vertexAO, 1e-5));
	}

	sh2 SampleNoBias(float3 positionMS)
	{
		sh2 scaledUnitSH = UNIT_SH / 1e-10;

		if (SharedData::InInterior)
			return scaledUnitSH;

		float3 positionMSAdjusted = positionMS - SharedData::skylightingSettings.PosOffset.xyz;
		float3 uvw = positionMSAdjusted / ARRAY_SIZE + .5;

		if (any(uvw < 0) || any(uvw > 1))
			return scaledUnitSH;

		float3 cellVxCoord = uvw * ARRAY_DIM;
		int3 cell000 = floor(cellVxCoord - 0.5);
		float3 trilinearPos = cellVxCoord - 0.5 - cell000;

		sh2 sum = 0;
		float wsum = 0;
		[unroll] for (int i = 0; i < 2; i++)
			[unroll] for (int j = 0; j < 2; j++)
				[unroll] for (int k = 0; k < 2; k++)
		{
			int3 offset = int3(i, j, k);
			int3 cellID = cell000 + offset;

			if (any(cellID < 0) || any((uint3)cellID >= ARRAY_DIM))
				continue;

			float3 cellCentreMS = cellID + 0.5 - ARRAY_DIM / 2;
			cellCentreMS = cellCentreMS * CELL_SIZE;

			float3 trilinearWeights = 1 - abs(offset - trilinearPos);
			float w = trilinearWeights.x * trilinearWeights.y * trilinearWeights.z;

			uint3 cellTexID = (cellID + SharedData::skylightingSettings.ArrayOrigin.xyz) % ARRAY_DIM;
			sh2 probe = SH::Scale(SkylightingProbeArray[cellTexID], w);

			sum = SH::Add(sum, probe);
			wsum += w;
		}

		return SH::Scale(sum, rcp(wsum + EPSILON_WEIGHT_SUM));
	}
#endif

}
#endif
