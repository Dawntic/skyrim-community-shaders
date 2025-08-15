#include "Common/SharedData.hlsli"
#include "Common/Random.hlsli"
#include "Common/Math.hlsli"

#ifdef ShadowVolumeCompute

cbuffer ShadowVolumeBuffer : register(b0) // check over
{
    float4 VolumeSize;
	float4 NoiseSize;
    float4 PrevCameraData;
	float2 ShadowAtlasSize;
	float CellJitterValue;
	float RayJitterValue;
	uint FrameIdx;
	uint ESM_Scale;
	uint ESM_EXP;
    //uint ShadowAtlasBorderPx;
	//float AmbientTerm;
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
Texture3D<float> ShadowVolumePrevious : register(t0);
Texture2DArray Noise : register(t1);
Texture2D ShadowAtlas : register(t2);
Texture2D TerrainHeight : register(t3);
Texture2D TerrainShadow : register(t4);
StructuredBuffer<ShadowDataStruct> ShadowData : register(t5);
Texture2DArray ShadowMap : register(t6);
Texture2DArray ShadowMapVL : register(t7);

SamplerState Linear_Sampler : register(s10);
SamplerState Point_Sampler : register(s11);
SamplerComparisonState Depth_Sampler : register(s13);

#define EPSILON 1e-6
#define AtlasBorderPx 2
#define NumCascades 2

#define GridScale float2(0.1, 160)

// Not sure about:
// light matrix
// cascade split, some for VL maps?


//Matrix used
//PrevCameraProjInverse
//CameraProjInverse


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

float2 GetAtlasUV(float2 CoordsUV, uint CascadeIdx){
    float  Height = 1.0 / NumCascades;
    float2 borderAtlas = AtlasBorderPx / ShadowAtlasSize;
    float2 Base = float2(0.0, CascadeIdx * Height) + borderAtlas;
    float2 Scale = float2(1.0, Height) - 2.0 * borderAtlas;

    return CoordsUV * Scale + Base;
}

float ViewZFromSlice(uint FroxelZ, float CameraRatio, float CameraNear){
    return CameraNear * pow(CameraRatio, (float(FroxelZ) + 0.5) / VolumeSize.z);
}

uint ChooseCascade(float viewZ){
    return (viewZ < ShadowData[0].EndSplitDistances.x) ? 0u : 1u;
}

float ESM_Reconstruct(float SampleValue, float ReceiverDepth){
    return saturate((SampleValue * (1.0 / ESM_Scale)) * exp(ESM_EXP * (1.0 - ReceiverDepth)));
}

float SampleESMShadow(float3 WorldPos, uint CascadeIdx)
{
    float4x3 lightProj = ShadowData[0].ShadowMapProj[0][CascadeIdx];
    float3 positionLS = mul(transpose(lightProj), float4(WorldPos, 1.0)).xyz; // should take us direct to uv

    float2 AtlasUV = GetAtlasUV(positionLS.xy, CascadeIdx);
    float SampleValue = ShadowAtlas.SampleLevel(Linear_Sampler, AtlasUV, 0.0).x;

    return ESM_Reconstruct(SampleValue, positionLS.z);
}

float3 FroxelWorldPosition(uint2 Froxel, float FroxelViewZIn, float3 Jitter)
{
    row_major float4x4 InvCameraProjNoJitter = CameraProjUnjitteredInverse[0];
    float2 InvCameraProjScale = float2(InvCameraProjNoJitter[0][0], InvCameraProjNoJitter[1][1]);

    float2 CoordsNDC = (float2(Froxel.xy) + 0.5 + Jitter.xy) / float2(VolumeSize.xy) * 2.0 - 1.0;
    CoordsNDC.y = -CoordsNDC.y;

    float ForxelViewZ = FroxelViewZIn + Jitter.z;
    float3 FroxelViewPosition = float3(CoordsNDC * ForxelViewZ * InvCameraProjScale, ForxelViewZ);
    float3 FroxelWorldPos = mul(float4(FroxelViewPosition, 1.0), CameraViewInverse[0]).xyz;

    return FroxelWorldPos;
}



float GetShadowDepth(float3 positionWS, uint eyeIndex){
    float4 positionCSShifted = mul(CameraViewProj[eyeIndex], float4(positionWS, 1));
    return positionCSShifted.z / positionCSShifted.w;
}

float Get2DFilteredShadowCascade(float noise, float2x2 rotationMatrix, float sampleOffsetScale, float2 baseUV, float cascadeIndex, float compareValue, uint eyeIndex){
    const uint sampleCount = 16;
    float visibility = 0.0;

    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        float2 sampleOffset = mul(Random::PoissonSampleOffsets16[sampleIndex], rotationMatrix);
        float2 sampleUV = rcp(1 + cascadeIndex) * sampleOffset * sampleOffsetScale + baseUV;
        float4 depths = ShadowMap.GatherRed(Linear_Sampler, float3(saturate(sampleUV), cascadeIndex), 0);
        visibility += dot(depths > compareValue, 0.25);
    }

    return visibility * rcp((float)sampleCount);
}

float Get2DFilteredShadow(float noise, float2x2 rotationMatrix, float3 positionWS, uint eyeIndex)
{
    ShadowDataStruct ShadowDataF = ShadowData[0];
    float shadowMapDepth = GetShadowDepth(positionWS, eyeIndex);

    if (ShadowDataF.EndSplitDistances.z >= shadowMapDepth) {
        float fadeFactor = 1.0 - pow(saturate(dot(positionWS.xyz, positionWS.xyz) / ShadowDataF.ShadowLightParam.z), 8);

        float4x3 lightProjectionMatrix = ShadowDataF.ShadowMapProj[eyeIndex][0];
        float cascadeIndex = 0;

        if (ShadowDataF.EndSplitDistances.x < shadowMapDepth) {
            lightProjectionMatrix = ShadowDataF.ShadowMapProj[eyeIndex][1];
            cascadeIndex = 1;
        }

        float3 positionLS = mul(transpose(lightProjectionMatrix), float4(positionWS.xyz, 1)).xyz;

        float shadowVisibility = Get2DFilteredShadowCascade(noise, rotationMatrix, ShadowDataF.ShadowSampleParam.z, positionLS.xy, cascadeIndex, positionLS.z, eyeIndex);

        if (cascadeIndex < 1 && ShadowDataF.StartSplitDistances.y < shadowMapDepth) {
            float3 cascade1PositionLS = mul(transpose(ShadowDataF.ShadowMapProj[eyeIndex][1]), float4(positionWS.xyz, 1)).xyz;

            float cascade1ShadowVisibility = Get2DFilteredShadowCascade(noise, rotationMatrix, ShadowDataF.ShadowSampleParam.z, cascade1PositionLS.xy, 1, cascade1PositionLS.z, eyeIndex);

            float cascade1BlendFactor = smoothstep(0, 1, (shadowMapDepth - ShadowDataF.StartSplitDistances.y) / (ShadowDataF.EndSplitDistances.x - ShadowDataF.StartSplitDistances.y));
            shadowVisibility = lerp(shadowVisibility, cascade1ShadowVisibility, cascade1BlendFactor);
        }

        return lerp(1.0, shadowVisibility, fadeFactor);
    }

    return 1.0;
}

float GetLightingShadow(float noise, float3 worldPosition, uint eyeIndex){
    float2 rotation;
    sincos(Math::TAU * noise, rotation.y, rotation.x);
    float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);
    return Get2DFilteredShadow(noise, rotationMatrix, worldPosition, eyeIndex);
}


[numthreads(8, 8, 4)]
void main(uint3 Froxel : SV_DispatchThreadID)
{
    if (any(Froxel >= (uint3)VolumeSize.xyz))
        return;

     float3 Jitter = float3(0.0, 0.0, 0.0); // leave 0 for the sake of debugging


    float FroxelViewZ = ViewZFromSlice(Froxel.z, GridScale.y / GridScale.x, GridScale.x);

    float3 FroxelWorldPos = FroxelWorldPosition(Froxel.xy, FroxelViewZ, Jitter);

    float Visibility = GetLightingShadow(0.0, FroxelWorldPos, 0);


    //uint CascadeIdx = ChooseCascade(FroxelViewZ);
    //float Visibility = SampleESMShadow(FroxelWorldPos, CascadeIdx);

    // Jitter xy
    //float2 NoiseValue = SampleNoise(Froxel, CascadeIdx);
    //Jitter.xy = (NoiseValue - 0.5f) * CellJitterValue;

    // Jitter along ray
    //float2 NoiseValueZ = SampleNoise(Froxel + uint3(7,5,0), CascadeIdx);
    //float JitterRay = (NoiseValueZ.x - 0.5f) * RayJitterValue; //

    //float ViewZNear = CameraNearZ * pow(CameraRatioZ, (Froxel.z + 0.0f) / VolumeSize.z);
    //float ViewZFar = CameraNearZ * pow(CameraRatioZ, (Froxel.z + 1.0f) / VolumeSize.z);
    //float ViewSliceThickness = max(EPSILON, ViewZFar - ViewZNear);

    //float SplitDist = 1e9;
   // [unroll] for (uint i = 0; i < Cascades-1; ++i)
     //   SplitDist = min(SplitDist, abs(FroxelViewZ - EndSplitDistances[i]));

    //Jitter.z = clamp(JitterRay * ViewSliceThickness, -0.45 * ViewSliceThickness, +0.45 * ViewSliceThickness);
    //Jitter.z *= saturate((SplitDist - 0.5 * ViewSliceThickness) / (0.5 * ViewSliceThickness));

    //float PrevVisibility;
    //if(FetchPreviousVisibility(FroxelWorldPos, PrevVisibility))
    //    Visibility = lerp(Visibility, PrevVisibility, saturate(alpha)); // ??

   // float4x3 lightProj = ShadowData[0].ShadowMapProj[0][0];
   // float3 positionLS = mul(transpose(lightProj), float4(FroxelWorldPos, 1.0)).xyz;
   // float test = min(min(positionLS.x, positionLS.y), positionLS.z);

    ShadowVolume[Froxel] = Visibility;
}




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

