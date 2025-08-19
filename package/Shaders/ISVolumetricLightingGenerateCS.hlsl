#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/VR.hlsli"

#if defined(CSHADER)
SamplerState ShadowmapSampler : register(s0);
SamplerState ShadowmapVLSampler : register(s1);
SamplerState InverseRepartitionSampler : register(s2);
SamplerState NoiseSampler : register(s3);

Texture2DArray<float> ShadowmapTex : register(t0);
Texture2DArray<float> ShadowmapVLTex : register(t1);
Texture1D<float> InverseRepartitionTex : register(t2);
Texture3D<float> NoiseTex : register(t3);

RWTexture3D<float> DensityRW : register(u0);

#	define LinearSampler ShadowmapSampler

#	include "Common/Framebuffer.hlsli"
#	include "Common/SharedData.hlsli"

#	if defined(TERRAIN_SHADOWS)
#		include "TerrainShadows/TerrainShadows.hlsli"
#	endif

#	if defined(CLOUD_SHADOWS)
#		include "CloudShadows/CloudShadows.hlsli"
#	endif

#	include "Common/ShadowSampling.hlsli"

cbuffer PerTechnique : register(b0)

{
#	ifndef VR
	row_major float4x4 CameraViewProj[1] : packoffset(c0);
	row_major float4x4 CameraViewProjInverse[1] : packoffset(c4);
	float4x3 ShadowMapProj[1][3] : packoffset(c8);
	float3 EndSplitDistances : packoffset(c17.x);
	float ShadowMapCount : packoffset(c17.w);
	float EnableShadowCasting : packoffset(c18);
	float3 DirLightDirection : packoffset(c19);
	float3 TextureDimensions : packoffset(c20);
	float3 WindInput[1] : packoffset(c21);
	float InverseDensityScale : packoffset(c21.w);
	float3 PosAdjust[1] : packoffset(c22);
	float IterationIndex : packoffset(c22.w);
	float PhaseContribution : packoffset(c23.x);
	float PhaseScattering : packoffset(c23.y);
	float DensityContribution : packoffset(c23.z);
#	else
	row_major float4x4 CameraViewProj[2] : packoffset(c0);
	row_major float4x4 CameraViewProjInverse[2] : packoffset(c8);
	float4x3 ShadowMapProj[2][3] : packoffset(c16);
	float3 EndSplitDistances : packoffset(c34.x);
	float ShadowMapCount : packoffset(c34.w);
	float EnableShadowCasting : packoffset(c35.x);
	float3 DirLightDirection : packoffset(c36);
	float3 TextureDimensions : packoffset(c37);
	float3 WindInput[2] : packoffset(c38);
	float InverseDensityScale : packoffset(c39.w);
	float3 PosAdjust[2] : packoffset(c40);
	float IterationIndex : packoffset(c41.w);
	float PhaseContribution : packoffset(c42.x);
	float PhaseScattering : packoffset(c42.y);
	float DensityContribution : packoffset(c42.z);
#	endif
}

// LH perspective camera, D3D depth in [0,1]
float ViewZToDepth01_PerspLH(float z, float nearZ, float farZ)
{
    // depth = f/(f-n) - (f*n)/((f-n)*z)
    float gap = farZ - nearZ;
    float depth = (farZ / gap) - (farZ * nearZ) / max(1e-6f, gap * z);
    return saturate(depth);
}


// Pure log repartition (view-distance)
float DepthFromLogT(float t, float nearZ, float farZ)
{
    float ratio = max(farZ / max(nearZ, 1e-6f), 1.0001f);
    float zView = nearZ * pow(ratio, t);                 // inverse repartition: t -> viewZ
    return ViewZToDepth01_PerspLH(zView, nearZ, farZ);   // -> camera NDC depth in [0,1]
}

float DepthFromHybridT(float t, float nearZ, float farZ, float lambda)
{
    float zLin  = lerp(nearZ, farZ, t);
    float ratio = max(farZ / max(nearZ, 1e-6f), 1.0001f);
    float zLog  = nearZ * pow(ratio, t);
    float zView = lerp(zLin, zLog, saturate(lambda));
    return ViewZToDepth01_PerspLH(zView, nearZ, farZ);
}


[numthreads(32, 32, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	float3 CoordsUV = dispatchID.xyz * (1.0 / TextureDimensions.xyz);

	float depth = InverseRepartitionTex.SampleLevel(InverseRepartitionSampler, CoordsUV.z, 0);

	//float4 Camera = SharedData::CameraData;
	//depth = DepthFromLogT(CoordsUV.z, Camera.y, Camera.x); //camera near and far in yx
	//depth = DepthFromHybridT(CoordsUV.z, Camera.y, Camera.x, 1);

	float4 positionCS = float4(CoordsUV.xy * 2.0 - 1.0, depth, 1.0);
	positionCS.y = -positionCS.y;

	float4 CoordsWS = mul(CameraViewProjInverse[0], positionCS);
	CoordsWS *= 1.0 / CoordsWS.w;

	float4 CoordsCS = mul(CameraViewProj[0], CoordsWS);
	CoordsCS *= 1.0 / CoordsCS.w;

	float shadowMapDepth = CoordsCS.z;

	bool noShadow = true;
	if (EndSplitDistances.z >= shadowMapDepth) {
		uint cascadeIndex = (shadowMapDepth > EndSplitDistances.x) ? 1 : 0;

		float4x3 lightProjectionMatrix = ShadowMapProj[0][cascadeIndex];

		float3 CoordsLS = mul(transpose(lightProjectionMatrix), float4(CoordsWS.xyz, 1)).xyz;
		float Visibility = ShadowmapTex.SampleLevel(ShadowmapSampler, float3(CoordsLS.xy, cascadeIndex), 0);
		noShadow = Visibility >= CoordsLS.z;
	}
	//noShadow = 0;

	DensityRW[dispatchID.xyz] = noShadow;
}
#endif






