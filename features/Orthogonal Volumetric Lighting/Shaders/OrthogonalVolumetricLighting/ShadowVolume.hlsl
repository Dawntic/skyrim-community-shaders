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
	uint FrameIdx;
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
Texture3D<float> ShadowVolumePrevious : register(t0);
Texture2DArray Noise : register(t1);
Texture2D ShadowAtlas : register(t2);
Texture2D TerrainHeight : register(t3);
Texture2D TerrainShadow : register(t4);
StructuredBuffer<ShadowDataStruct> ShadowDataSB : register(t5);
Texture2DArray ShadowMap : register(t6);
Texture2DArray ShadowMapVL : register(t7);
Texture1D InvRepartition : register(t8);

SamplerState Linear_Sampler : register(s10);
SamplerState Point_Sampler : register(s11);
SamplerComparisonState Depth_Sampler : register(s13);

#define EPSILON 1e-6

#define LinearSampler Linear_Sampler
#	include "Common/ShadowSampling.hlsli"


// Stable 3D phase so each Froxel (and cascade) starts at a different point in the sequence.
uint STBNPhase3D(uint3 pos, uint CascadeIdx){
    uint h = (pos.x * 1973u) ^ (pos.y * 9277u) ^ (pos.z * 2663u) ^ (CascadeIdx * 811u);
    return h % max(1u, NoiseSize.z);
}

// Return two decorrelated scalars in [0,1) for XY jitter/etc.
float2 SampleNoise(uint3 Froxel, uint CascadeIdx){
    uint2 Coord1 = uint2(Froxel.xy) % NoiseSize.xy;
    uint2 Coord2 = uint2((Coord1.x + 37u) % NoiseSize.x, (Coord1.y + 17u) % NoiseSize.y);
    uint layer = (FrameIdx + STBNPhase3D(Froxel, CascadeIdx)) % NoiseSize.z;

    float2 Sample;
    Sample.x = Noise.Load(int4(Coord1.xy, layer, 0)).x;
    Sample.y = Noise.Load(int4(Coord2.xy, layer, 0)).x;

    return Sample;
}

uint ChooseCascade(float viewZ){
    return (viewZ < ShadowDataSB[0].EndSplitDistances.x) ? 0u : 1u;
}

float2 AtlasFetch1x2(float2 uv, uint texNum){
    static const float2 pos[2] = {
        float2(0.0, 0.0), float2(0.0, 0.50)};
    return mad(uv, float2(1.0, 0.50), pos[texNum - 1]);
}

float2 GetAtlasUV(float2 CoordsUV, uint CascadeIdx){
    float  Height = 1.0 / ShadowDataSB[0].EndSplitDistances.w;
    float2 Base = float2(0.0, CascadeIdx * Height);
    float2 Scale = float2(1.0, Height) - 2.0;

    return CoordsUV * Scale + Base;
}

float ESM_Reconstruct(float SampleValue, float ReceiverDepth){
    return saturate((SampleValue * (1.0 / ESM_Scale)) * exp(40 * (1.0 - ReceiverDepth))); /////////
}

float SampleESMShadow(float3 CoordsLS, uint CascadeIdx)
{
    //float2 AtlasUV = GetAtlasUV(CoordsLS.xy, CascadeIdx);
    float2 AtlasUV = AtlasFetch1x2(CoordsLS.xy, CascadeIdx);

    float SampleValue = ShadowAtlas.SampleLevel(Linear_Sampler, AtlasUV, 0.0).x;

    return ESM_Reconstruct(SampleValue, CoordsLS.z);
}

/*
float GetRayJitter(uint3 Froxel, FroxelViewZ){
    float NoiseValueZ = SampleNoise(Froxel + uint3(7,5,0), CascadeIdx).x;
    float JitterRay = (NoiseValueZ - 0.5) * RayJitterValue; //

    float ViewZNear = CameraNearZ * pow(CameraRatioZ, (Froxel.z + 0.0) / VolumeSize.z);
    float ViewZFar = CameraNearZ * pow(CameraRatioZ, (Froxel.z + 1.0) / VolumeSize.z);
    float ViewSliceThickness = max(EPSILON, ViewZFar - ViewZNear);

    float SplitDist = 1e9;
    [unroll] for (uint i = 0; i < Cascades-1; ++i)
        SplitDist = min(SplitDist, abs(FroxelViewZ - EndSplitDistances[i]));

    Jitter.z = clamp(JitterRay * ViewSliceThickness, -0.45 * ViewSliceThickness, +0.45 * ViewSliceThickness);
    Jitter.z *= saturate((SplitDist - 0.5 * ViewSliceThickness) / (0.5 * ViewSliceThickness));
}
*/

float4 FroxelWorldPosition(float3 Froxel)
{
    float SliceZ = exp2(Froxel.z / FrustumNearFar.w) / FrustumNearFar.z;
    float2 CoordsUV = (Froxel.xy / VolumeSize.xy) * SliceZ;
    float4 CoordsWS = mul(Frustum, float4(CoordsUV.xy, SliceZ, 1.0));

    return float4(CoordsWS.xyz, SliceZ);
}

float3 FroxelLightPosition(float3 CoordsWS, uint CascadeIndex){
    ShadowDataStruct ShadowData = ShadowDataSB[0]; //non VR structured buffer
    float4x3 LightWorldShadowUV = ShadowData.ShadowMapProj[0][CascadeIndex]; // world to shadow UV //don't use VR

    float3 CoordsLS = mul(transpose(LightWorldShadowUV), float4(CoordsWS, 1.0)).xyz; //this probably assumes default map size?

    return CoordsLS;
}

float SampleShadow(float3 CoordsLS, uint cascadeIndex)
{
	float Visibility = ShadowMap.SampleLevel(Linear_Sampler, float3(CoordsLS.xy, cascadeIndex), 0).x;
	Visibility = Visibility >= CoordsLS.z;

    return Visibility;
}

float4 GetWorldCoords(uint3 Froxel)
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

[numthreads(8, 8, 4)]
void main(uint3 Froxel : SV_DispatchThreadID)
{
    if (any(Froxel >= (uint3)VolumeSize.xyz))
        return;

    float4 CoordsWS = GetWorldCoords(Froxel);
	float shadowMapDepth = CoordsWS.w;

     ShadowDataStruct ShadowData = ShadowDataSB[0];

	bool noShadow = true;
	if (ShadowData.EndSplitDistances.z >= shadowMapDepth) {
		uint cascadeIndex = (shadowMapDepth > ShadowData.EndSplitDistances.x) ? 1 : 0;

		float4x3 lightProjectionMatrix = ShadowData.ShadowMapProj[0][cascadeIndex];

		float3 CoordsLS = mul(transpose(lightProjectionMatrix), float4(CoordsWS.xyz, 1)).xyz;

		float Visibility = ShadowMap.SampleLevel(Linear_Sampler, float3(CoordsLS.xy, cascadeIndex), 0).x;
		noShadow = Visibility >= CoordsLS.z;
	}
    //noShadow = 0;

    if(Froxel.z < 3)
        noShadow = false;

    ShadowVolume[Froxel] = float(noShadow);
}

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

