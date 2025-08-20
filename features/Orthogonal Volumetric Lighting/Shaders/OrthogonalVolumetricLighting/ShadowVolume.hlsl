#include "Common/SharedData.hlsli"
#include "Common/Random.hlsli"
#include "Common/Math.hlsli"
#include "Common/FrameBuffer.hlsli"

#ifdef ShadowVolumeCompute

cbuffer ShadowVolumeBuffer : register(b0)
{
    row_major float4x4 Frustum;
    float4 FrustumNearFar;
    float4 CameraPosition;
    float4 VolumeSize;
	float4 NoiseSize;
    float4 PrevCameraData;
	float2 ShadowAtlasSize;
	float CellJitterValue;
	float RayJitterValue;
	uint ESM_Scale;
	uint ESM_EXP;
};

cbuffer PerFrame : register(b1)
{
    row_major float4x4 CameraView[1] : packoffset(c0);
    row_major float4x4 CameraProj[1] : packoffset(c4);
    row_major float4x4 CameraViewProj[1] : packoffset(c8);
    row_major float4x4 CameraViewProjUnjittered[1] : packoffset(c12);
    row_major float4x4 CameraPreviousViewProjUnjittered[1] : packoffset(c16);
    row_major float4x4 CameraProjUnjittered[1] : packoffset(c20);
    row_major float4x4 CameraProjUnjitteredInverse[1] : packoffset(c24);
    row_major float4x4 CameraViewInverse[1] : packoffset(c28);
    row_major float4x4 CameraViewProjInverse[1] : packoffset(c32);
    row_major float4x4 CameraProjInverse[1] : packoffset(c36);
    float4 CameraPosAdjust[1] : packoffset(c40);
    float4 CameraPreviousPosAdjust[1] : packoffset(c41);
    float4 FrameParams : packoffset(c42);
    float4 DynamicResolutionParams1 : packoffset(c43);
    float4 DynamicResolutionParams2 : packoffset(c44);
};

cbuffer PrevPerFrame : register(b2)
{
    row_major float4x4 PrevCameraView[1] : packoffset(c0);
    row_major float4x4 PrevCameraProj[1] : packoffset(c4);
    row_major float4x4 PrevCameraViewProj[1] : packoffset(c8);
    row_major float4x4 PrevCameraViewProjUnjittered[1] : packoffset(c12);
    row_major float4x4 PrevCameraPreviousViewProjUnjittered[1] : packoffset(c16);
    row_major float4x4 PrevCameraProjUnjittered[1] : packoffset(c20);
    row_major float4x4 PrevCameraProjUnjitteredInverse[1] : packoffset(c24);
    row_major float4x4 PrevCameraViewInverse[1] : packoffset(c28);
    row_major float4x4 PrevCameraViewProjInverse[1] : packoffset(c32);
    row_major float4x4 PrevCameraProjInverse[1] : packoffset(c36);
    float4 PrevCameraPosAdjust[1] : packoffset(c40);
    float4 PrevCameraPreviousPosAdjust[1] : packoffset(c41);
    float4 PrevFrameParams : packoffset(c42);
    float4 PrevDynamicResolutionParams1 : packoffset(c43);
    float4 PrevDynamicResolutionParams2 : packoffset(c44);
};

struct ShadowDataStruct
{
    float4 VPOSOffset;
    float4 ShadowSampleParam;    // fPoissonRadiusScale / iShadowMapResolution in z and w
    float4 EndSplitDistances;    // cascade end distances int xyz, cascade count int z
    float4 StartSplitDistances;  // cascade start ditances int xyz, 4 int z
    float4 FocusShadowFadeParam;
    float4 DebugColor;
    float4 PropertyColor;
    float4 AlphaTestRef;
    float4 ShadowLightParam;  // Falloff in x, ShadowDistance squared in z
    float4x3 FocusShadowMapProj[4];
    // Since ShadowData is passed between c++ and hlsl, can't have different defines due to strong typing
    float4x3 ShadowMapProj[2][3];
    float4x4 CameraViewProjInverse[2];
};

RWTexture3D<float> ShadowVolume : register(u0);
Texture3D<float> PrevShadowVolume : register(t0);
Texture2DArray NoiseTex : register(t1);
Texture2DArray ShadowAtlas : register(t2);
Texture2D TerrainHeight : register(t3);
Texture2D TerrainShadow : register(t4);
StructuredBuffer<ShadowDataStruct> ShadowDataSB : register(t5);
Texture2DArray ShadowMap : register(t6);
Texture2DArray ShadowMapVL : register(t7);
Texture1D InvRepartition : register(t8);
Texture1D Repartition : register(t9);

SamplerState Linear_Sampler : register(s10);
SamplerState Point_Sampler : register(s11);
SamplerComparisonState Depth_Sampler : register(s13);

#define EPSILON 1e-6

#define LinearSampler Linear_Sampler
#	include "Common/ShadowSampling.hlsli"


uint STBNPhase3D(uint3 pos){
    uint h = (pos.x * 1973u) ^ (pos.y * 9277u) ^ (pos.z * 2663u) ^ 0x9E3779B9u;
    return h % max(1u, NoiseSize.z);
}

float2 SampleNoise(uint3 Froxel){
    uint layer = (SharedData::FrameCountAlwaysActive + STBNPhase3D(Froxel)) % NoiseSize.z;

    uint2 coord1 = uint2(Froxel.xy) % NoiseSize.xy;
    uint2 coord2 = uint2((coord1.x + 37u) % NoiseSize.x, (coord1.y + 17u) % NoiseSize.y);

    float NoiseX = NoiseTex.Load(int4(coord1.xy, layer, 0)).x;
    float NoiseY = NoiseTex.Load(int4(coord2.xy, layer, 0)).x;

    return float2(NoiseX, NoiseY);
}

float SliceThicknessViewZ(float FroxelZ){
    float ZCoordUV = FroxelZ * rcp(VolumeSize.z);
    float ZCoordUV2 = (FroxelZ + 1) * rcp(VolumeSize.z);
    float depth = InvRepartition.SampleLevel(Linear_Sampler, ZCoordUV, 0).x;
    float depth2 = InvRepartition.SampleLevel(Linear_Sampler, ZCoordUV2, 0).x;

    return max(depth2 - depth, EPSILON);
}

float GetRayJitter(uint3 Froxel)
{
    float ZThickness = SliceThicknessViewZ((float)Froxel.z);
    float Limit = 0.45 * ZThickness;
    float Noise = SampleNoise(Froxel).x * 2.0 - 1.0;
          Noise = clamp(Noise * RayJitterValue * ZThickness, -Limit, +Limit);

    float ZCoordUVCenter = float(Froxel.z + 0.5) * rcp(VolumeSize.z);
    float ZCenter = InvRepartition.SampleLevel(Linear_Sampler, ZCoordUVCenter, 0).x;

    return ZCenter + Noise;
}

uint ChooseCascade(float viewZ){
    return (viewZ < ShadowDataSB[0].EndSplitDistances.x) ? 0u : 1u;
}

float2 AtlasFetch1x2(float2 uv, uint texNum){
    static const float2 pos[2] = {
        float2(0.0, 0.0), float2(0.0, 0.50)};
    return mad(uv, float2(1.0, 0.50), pos[texNum - 1]);
}

float ESM_Reconstruct(float SampleValue, float ReceiverDepth){
    return saturate((SampleValue * (1.0 / ESM_Scale)) * exp(ESM_EXP * (1.0 - ReceiverDepth)));
}

float SampleESMShadow(float3 CoordsLS, uint CascadeIdx)
{
    //float2 AtlasUV = GetAtlasUV(CoordsLS.xy, CascadeIdx);
    //float2 AtlasUV = AtlasFetch1x2(CoordsLS.xy, CascadeIdx);
    //float SampleValue = ShadowAtlas.SampleLevel(Linear_Sampler, AtlasUV, 0).x;

    return 0.0;//ESM_Reconstruct(SampleValue, CoordsLS.z);
}

float4 FroxelWorldPosition(float3 Froxel)
{
    float SliceZ = exp2(Froxel.z / FrustumNearFar.w) / FrustumNearFar.z;
    float2 CoordsUV = (Froxel.xy / VolumeSize.xy) * SliceZ;
    float4 CoordsWS = mul(Frustum, float4(CoordsUV.xy, SliceZ, 1.0));

    return float4(CoordsWS.xyz, SliceZ);
}

float3 FroxelLightPosition(float3 CoordsWS, uint CascadeIndex){
    ShadowDataStruct ShadowData = ShadowDataSB[0];
    float4x3 LightWorldShadowUV = ShadowData.ShadowMapProj[0][CascadeIndex];

    float3 CoordsLS = mul(transpose(LightWorldShadowUV), float4(CoordsWS, 1.0)).xyz;

    return CoordsLS;
}

float4 GetWorldCoords(float3 Froxel)
{
    float3 CoordsUV = Froxel.xyz * (1.0 / VolumeSize.xyz);

	float depth = InvRepartition.SampleLevel(Linear_Sampler, CoordsUV.z, 0).x;

	float4 CoordsNDC = float4(CoordsUV.xy * 2.0 - 1.0, depth, 1.0);
	CoordsNDC.y = -CoordsNDC.y;

	float4 CoordsWS = mul(CameraViewProjInverse[0], CoordsNDC);
	CoordsWS *= 1.0 / CoordsWS.w;

	float4 CoordsCS = mul(CameraViewProj[0], CoordsWS);
	CoordsCS *= 1.0 / CoordsCS.w;

    return float4(CoordsWS.xyz, CoordsCS.z);
}

float3 GetPrevWorldCoords(float3 CoordsWS)
{
    float4 PrevCS = mul(PrevCameraViewProj[0], float4(CoordsWS, 1.0));

    if (PrevCS.w <= 0.0)
        return float3(-1.0, -1.0, -1.0); //behind camera

    float2 PrevNDC = PrevCS.xy / PrevCS.w;
    float2 PrevUV = float2(PrevNDC.x, -PrevNDC.y) * 0.5 + 0.5;

    float PrevZ = PrevCS.z / PrevCS.w;

    float PrevDepth = Repartition.SampleLevel(Linear_Sampler, PrevZ, 0).x;

    float3 PrevTexCoord = float3(PrevUV, PrevDepth);

    if(all(PrevTexCoord >= 0.0 && PrevTexCoord <= 1.0))
        return PrevTexCoord;

    return float3(-1.0, -1.0, -1.0);
}





[numthreads(8, 8, 4)]
void main(uint3 Froxel : SV_DispatchThreadID)
{
    if (any(Froxel >= (uint3)VolumeSize.xyz))
        return;

    float3 Jitter = float3(0,0,0);
    Jitter.xy = (SampleNoise(Froxel) - 0.5) * CellJitterValue;
    Jitter.z = GetRayJitter(Froxel);

    float4 CoordsWS = GetWorldCoords(float3(Froxel.xy + 0.5 + Jitter.xy, Froxel.z + Jitter.z));
	float shadowMapDepth = CoordsWS.w;

	float Visibility = 0.0;
    ShadowDataStruct ShadowData = ShadowDataSB[0];
	if (ShadowData.EndSplitDistances.z >= shadowMapDepth) {
		uint cascadeIndex = (shadowMapDepth > ShadowData.EndSplitDistances.x) ? 1 : 0;

        float3 CoordsLS = FroxelLightPosition(CoordsWS.xyz, cascadeIndex);

		float Sample = ShadowMap.SampleLevel(Linear_Sampler, float3(CoordsLS.xy, cascadeIndex), 0).x;
        //float Sample = ShadowAtlas.SampleLevel(Linear_Sampler, float3(CoordsLS.xy, cascadeIndex), 0).x;
        //Sample = ESM_Reconstruct(Sample, CoordsLS.z);

		Visibility = Sample >= CoordsLS.z;
	}

    float3 PrevUVZ = GetPrevWorldCoords(CoordsWS.xyz);

    float PrevVisibility = PrevShadowVolume.SampleLevel(Linear_Sampler, PrevUVZ, 0);
    float Factor = 0.3 * (PrevUVZ.x >= 0.0);
    //Visibility = lerp(Visibility, PrevVisibility, Factor);

    if(Froxel.z < 1)
        Visibility = 0.0;

    ShadowVolume[Froxel] = Visibility;
}


//Visibility = lerp(CurrentShadow, PrevShadow, _347 * (1.0 - saturate((abs(CurrentShadow - PrevShadow) - SomeVar.y) / (1.0 - SomeVar.y))));


// Usage in your inject pass (after choosing cascade from *centerZ*):
//   SliceBounds b = GetSliceBounds(Froxel.z);
//   float zJit    = b.centerZ + JitterAlongRay(Froxel, RayJitterValue);


/*
float2 GetAtlasUV(float2 CoordsUV, uint CascadeIdx){
    float  Height = 1.0 / ShadowDataSB[0].EndSplitDistances.w;
    float2 Base = float2(0.0, CascadeIdx * Height);
    float2 Scale = float2(1.0, Height) - 2.0;

    return CoordsUV * Scale + Base;
}
*/



    //LH camera

    //CoordsUV.z = DepthFromLogT(CoordsUV.z, FrustumNearFar.x, FrustumNearFar.y); //camera near and far in yx
//CoordsUV.z = DepthFromLogT(CoordsUV.z, Camera.y, Camera.x); //camera near and far in yx

    // (Optional) low-cost multi-sample along the light ray within the Froxel:
    // Improves stability in thick media at the cost of a few extra taps.
    // Uncomment if needed.
    /*
    const int S = 3;
    float  acc = 0.0f;
    // Approximate a small step length using view-space slice thickness
    float  z0 = gNearZ * pow(gLogZRatio, (tid.z + 0.0f) / VolumeSize.z);
    float  z1 = gNearZ * pow(gLogZRatio, (tid.z + 1.0f) / VolumeSize.z);
    float  dz = max(1e-3f, z1 - z0);

    // Convert that to a world-space step scale near the center (cheap heuristic)
    float3 stepWS = normalize(gLightDirWS) * dz;

    [unroll]
    for (int s = 0; s < S; ++s)
    {
        float  a = ( (s + 0.5f) / S - 0.5f ); // centered samples
        acc += SampleESMShadow(Pworld + a * stepWS);
    }
    vis = acc / S;
    */

    /*
bool FetchPreviousVisibility(float3 WorldPos, out float PreviousVis)
{
    //float4 CameraOffset = FrameBuffer::CameraPreviousPosAdjust; //what are these adjust values used for?
    float2 PrevCameraProjScale = float2(PrevCameraProjUnjitteredInverse[0][0][0], PrevCameraProjUnjitteredInverse[0][1][1]); //

    float3 PrevView = mul(float4(WorldPos, 1.0f), PrevCameraView[0]).xyz; //
    if (PrevView.z <= 0.0)
        return false; //behind camera

    float2 PrevNDC = (PrevView.xy * PrevCameraProjScale.xy) / PrevView.z;

    float2 PrevUV = PrevNDC * 0.5 + 0.5;
    PrevUV.y = -PrevUV.y;

    float PrevCameraNearZ = PrevCameraData.x - PrevCameraData.z; //
    float PrevCameraRatioZ = PrevCameraData.x / PrevCameraNearZ; //

    float2 PrevCoords = float3(PrevUV, saturate(log(PrevView.z / PrevCameraNearZ) / log(PrevCameraRatioZ)));

    // Clamp-to-neighborhood etc. can go here if you want “temporal filtering”
    if(all(PrevCoords >= 0.0) && all(PrevCoords <= 1.0f)){
        PreviousVis = ShadowVolumePrevious.Sample(Linear_Sampler, PrevCoords);
        return true;
    }

    PreviousVis = 0.0;
    return false;
}


float2 GetCascadeNearFar(uint i)
{
    float NearZ = CascadeSplitStart[i];
    float FarZ = CascadeSplitEnd[i];
    return float2(nearZ, farZ);
}
*/


#endif

