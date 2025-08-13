#include "Common/ShadowSampling.hlsli"

cbuffer LightCB : register(b1)
{
    float4x4 gLightViewProj[8];   // one per cascade
    float3   gLightDirWS;         // normalized
}

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

cbuffer ShadowVolumeBuffer : register(b1) // check over
{
    row_major float4x4 lightMat[2];
	float2 ShadowAtlasSize;
	uint ShadowAtlasBorderPx;
	float AmbientTerm;
	float CellJitterValue;
	float RayJitterValue;
	uint FrameIdx;
	uint ESM_Scale;
	uint ESM_EXP;
    float pad[3];
};

float4 EndSplitDist = ShadowSampling::ShadowData::EndSplitDistances;
float4 StartSplitDist = ShadowSampling::ShadowData::StartSplitDistances;

#define EPSILON 1e-6

#define GridDimensions float3(160, 88, 64)
#define Cascades 2

#define ShadowAtlasSize float2(256, 512)
#define AtlasBorderPx 10 // guard against gutters?

#define NoiseLayers 32
#define NoiseSize 64

#define EXP 60
#define ESM_SCALE 65000;

#define AmbientTerm //

Texture2D ShadowAtlas
Texture3D ShadowVolumePrevious
Texture2DArray Noise

#define CellJitterValue
#define RayJitterValue
#define FrameIdx //

//need previous view matrix, proj matrix, near far values
#define PrevViewPos
#define PrevCameraProjInverse
#define PrevCameraPlane








// Stable 3D phase so each Froxel (and cascade) starts at a different point in the sequence.
uint STBNPhase3D(uint3 pos, uint CascadeIdx)
{
    uint h = (pos.x * 1973u) ^ (pos.y * 9277u) ^ (pos.z * 2663u) ^ (CascadeIdx * 811u);
    return h % max(1u, NoiseLayers);
}

// Return two decorrelated scalars in [0,1) for XY jitter/etc.
float2 SampleNoise(uint3 Froxel, uint CascadeIdx)
{
    float2 Sample;

    uint2 Coord1 = uint2(Froxel.xy) % NoiseSize;
    uint2 Coord2 = uint2((Coord1.x + 37u) % NoiseSize, (Coord1.y + 17u) % NoiseSize);
    uint layer = (FrameIdx + STBNPhase3D(Froxel, CascadeIdx)) % NoiseLayers;

    Sample.x = Noise.Load(int4(Coord1.xy, layer, 0));
    Sample.y = Noise.Load(int4(Coord2.xy, layer, 0));

    return Sample;
}

float ViewZFromSlice(uint FroxelZ, float CameraRatioZ, float CameraNearZ)
{
    return CameraNearZ * pow(CameraRatioZ, (float(FroxelZ) + 0.5) / GridDimensions.z);
}

float3 FroxelViewPosition(uint3 Froxel, float2 Jitter)
{
    float2 InvCameraProjScale = float2(CameraProjInverse[0][0][0], CameraProjInverse[0][1][1]);

    float2 CoordsNDC = (float2(Froxel.xy) + 0.5 + Jitter.xy) / float2(GridDimensions.xy) * 2.0 - 1.0;
    CoordsNDC.y = -CoordsNDC.y;

    float DepthCenter = ViewZFromSlice(Froxel.z);
          DepthCenter += Jitter.z;

    return float3(CoordsNDC * DepthCenter * InvCameraProjScale, DepthCenter);
}


uint ChooseCascade(float viewZ)
{
    return (viewZ < EndSplitDist[0]) ? 0u : 1u;
}

float2 GetCascadeNearFar(uint i)
{
    float NearZ = StartSplitDist[i];
    float FarZ = EndSplitDist[i];
    return float2(nearZ, farZ);
}

float2 RemapAtlasUV_1xN(float2 CoordsUV, uint CascadeIdx)
{
    float  Height = 1.0f / EndSplitDist.z;
    float2 borderAtlas = AtlasBorderPx / ShadowAtlasSize;
    float2 Base = float2(0.0f, CascadeIdx * Height) + borderAtlas;
    float2 Scale = float2(1.0f, Height) - 2.0f * borderAtlas;

    return CoordsUV * Scale + Base;
}

float ESM_Reconstruct(float SampleValue, float ReceiverDepth)
{
    return saturate((SampleValue * (1.0 / ESM_SCALE)) * exp(EXP * (1.0 - ReceiverDepth)));
}

// Optional bleed reduction: remap [β..1] -> [0..1]
// β in [0..0.99] — raises the floor to kill soft bleed in penumbrae.
//float beta = saturate(gBleedReduction);
//vis = saturate( (vis - beta) / (1.0 - beta) );
float SampleESMShadow(float3 WorldPos, uint CascadeIdx)
{
    float4 LightClip = mul(float4(WorldPos, 1.0), LightViewProj[CascadeIdx]);
    float3 LightNDC = LightClip.xyz / LightClip.w;
    float2 CoordsUV = LightNDC.xy * 0.5 + 0.5;

    if (any(CoordsUV < 0.0) || any(CoordsUV > 1.0))
        return 1.0;

    float2 AtlasUV = RemapAtlasUV_1xN(CoordsUV, CascadeIdx);
    float SampleValue = ShadowAtlas.SampleLevel(Linear_Sampler, AtlasUV, 0.0);

    return ESM_Reconstruct(SampleValue, LightNDC.z);
}

bool FetchPreviousVisibility(float3 WorldPos, out float PreviousVis)
{
    //float4 CameraOffset = FrameBuffer::CameraPreviousPosAdjust; //what are these adjust values used for?
    float PrevCameraNearZ = CameraData.x - CameraData.z; //
    float PrevCameraRatioZ = CameraData.x / CameraNearZ; //

    float3 PrevView = mul(float4(WorldPos, 1.0f), PrevViewPos).xyz; //
    if (PrevView.z <= 0.0)
        return false; //behind camera

    float2 PrevCameraProjScale = float2(PrevCameraProjInverse[0][0][0], PrevCameraProjInverse[0][1][1]); //
    float2 PrevNDC = (PrevView.xy * PrevCameraProjScale.xy) / PrevView.z;

    float2 PrevUV = PrevNDC * 0.5 + 0.5;
    PrevUV.y = -PrevUV.y;

    PrevCoords = float3(PrevUV, saturate(log(PrevView.z / PrevCameraNearZ) / log(PrevCameraRatioZ)));

    // Clamp-to-neighborhood etc. can go here if you want “temporal filtering”
    if(all(PrevCoords >= 0.0) && all(PrevCoords <= 1.0f)){
        PreviousVis = ShadowVolumePrevious.Sample(Linear_Sampler, PrevCoords);
        return true;
    }

    PreviousVis = 0.0;
    return false;
}



// ========= Kernel =========
// Each thread writes one Froxel. (You can also 2×2×1 group if occupancy prefers.)
[numthreads(8, 8, 4)]
void CS_InjectESMToVolume(uint3 Froxel : SV_DispatchThreadID)
{
    if (any(Froxel >= GridDimensions))
        return;

    float4 CameraData = SharedData::CameraData;
    float CameraNearZ = CameraData.x - CameraData.z;
    float CameraRatioZ = CameraData.x / CameraNearZ;

    float FroxelViewZ = ViewZFromSlice(Froxel.z, CameraRatioZ, CameraNearZ);
    uint CascadeIdx = ChooseCascade(FroxelViewZ);

    float3 Jitter;
    // Jitter xy
    float2 NoiseValue = SampleNoise(Froxel, CascadeIdx);
    Jitter.xy = (NoiseValue - 0.5f) * CellJitterValue; //

    // Jitter along ray
    float2 NoiseValueZ = SampleNoise(Froxel + uint3(7,5,0), CascadeIdx);
    float JitterRay = (NoiseValueZ.x - 0.5f) * RayJitterValue; //

    float ViewZNear = CameraNearZ * pow(CameraRatioZ, (Froxel.z + 0.0f) / GridDimensions.z);
    float ViewZFar = CameraNearZ * pow(CameraRatioZ, (Froxel.z + 1.0f) / GridDimensions.z);
    float ViewSliceThickness = max(EPSILON, ViewZFar - ViewZNear);

    Jitter.z = clamp(JitterRay * ViewSliceThickness, -0.45 * ViewSliceThickness, +0.45 * ViewSliceThickness);

    // Froxel center in view space
    float3 FroxelViewPos = FroxelViewPosition(Froxel, Jitter);

     // Convert to worldspace
    float3 FroxelWorldPos = mul(float4(FroxelViewPos, 1.0f), FrameBuffer::CameraViewInverse[0]).xyz;

    // Single-point estimate at Froxel center
    float Visibility = SampleESMShadow(FroxelWorldPos, FroxelViewPos.z, CascadeIdx);

    float PrevVisibility;
    if(FetchPreviousVisibility(FroxelWorldPos, PrevVisibility))
        Visibility = lerp(Visibility, PrevVisibility, saturate(alpha)); // ??



    // (Optional) low-cost multi-sample along the light ray within the Froxel:
    // Improves stability in thick media at the cost of a few extra taps.
    // Uncomment if needed.
    /*
    const int S = 3;
    float  acc = 0.0f;
    // Approximate a small step length using view-space slice thickness
    float  z0 = gNearZ * pow(gLogZRatio, (tid.z + 0.0f) / GridDimensions.z);
    float  z1 = gNearZ * pow(gLogZRatio, (tid.z + 1.0f) / GridDimensions.z);
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

    ShadowVolume[Froxel] = vis;
}