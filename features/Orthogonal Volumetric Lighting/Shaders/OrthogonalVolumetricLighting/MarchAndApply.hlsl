#include "Common/SharedData.hlsli"
#include "Common/FrameBuffer.hlsli"

struct VertexShaderOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

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

SamplerState Linear_Sampler : register(s10);

#define EPSILON 1e-6


#ifdef MarchVolumeCompute

Texture3D ScatteringVolume : register(t0);
Texture1D InvRepartition : register(t1);

RWTexture3D<float4> IntergrationVolume : register(u0);

float4 FroxelWorldPosition(float3 Froxel)
{
    float SliceZ = exp2(Froxel.z / FrustumNearFar.w) / FrustumNearFar.z;
    float2 CoordsUV = (Froxel.xy / VolumeSize.xy) * SliceZ;
    float4 CoordsWS = mul(Frustum, float4(CoordsUV.xy, SliceZ, 1.0));

    return float4(CoordsWS.xyz, SliceZ);
}

void AccumulateScattering(inout float4 Accumulation, float4 ScatteringSlice, float StepLength){
    float Extinction = max(ScatteringSlice.w, EPSILON);

    float Transmittance = exp(-Extinction * StepLength);

    float3 InScatterIntegral = (-ScatteringSlice.xyz * Transmittance + ScatteringSlice.xyz) * rcp(Extinction);

    Accumulation.xyz += InScatterIntegral * Accumulation.w;
    Accumulation.w *= Transmittance;
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


[numthreads(8, 8, 1)]
void main(uint3 Froxel : SV_DispatchThreadID)
{
    if (any(Froxel >= (uint3)VolumeSize.xyz))
        return;

    float4 Accumulation = float4(0.0, 0.0, 0.0, 1.0);

    float3 PrevCoordsWS = GetWorldCoords(float3(Froxel.xy + 0.5, 0.0)).xyz;

    for(int Slice=0; Slice < VolumeSize.z; Slice++){
        float3 CoordsWS = GetWorldCoords(float3(Froxel.xy + 0.5, Slice + 1)).xyz;
        float StepLength = distance(PrevCoordsWS, CoordsWS);

        float4 ScatteredSlice = ScatteringVolume.Load(int4(Froxel.xy, Slice, 0));

        AccumulateScattering(Accumulation, ScatteredSlice, StepLength);

        PrevCoordsWS = CoordsWS;
        IntergrationVolume[uint3(Froxel.xy, Slice)] = Accumulation;
    }
}
#endif




#ifdef ApplyVolume

Texture3D Volume : register(t0);
Texture2D DepthTex : register(t1);
Texture1D Repartition : register(t2);

float LinearDepth(float Depth, float Near, float Far){
    return (Near * Far) / (Far - Depth * (Far - Near));
}

float GetFroxelSlice(float LinDepth){ ////////////////
    float FroxelSlice = log(LinDepth / FrustumNearFar.x) / log(FrustumNearFar.y / FrustumNearFar.x);
    return FroxelSlice;
}


float4 main(VertexShaderOutput input) : SV_Target
{
    float DepthSample = DepthTex.Sample(Linear_Sampler, input.TexCoord.xy).x;

    //float2 CameraNearFar = float2(SharedData::CameraData.y, SharedData::CameraData.x);

    float depth = clamp(Repartition.SampleLevel(Linear_Sampler, DepthSample, 0).x, 0.0, 0.999999);

    float3 Output = Volume.SampleLevel(Linear_Sampler, float3(input.TexCoord.xy, depth), 0).xyz;

    //float LinDepth = LinearDepth(DepthSample, CameraNearFar.x, CameraNearFar.y);
    //float FroxelSlice = GetFroxelSlice(LinDepth);

	//float TexelSize = 1.0 / VolumeSize.z;
	//FroxelSlice = max(0.0, FroxelSlice - TexelSize * 1.5);

	//float3 Output = Volume.SampleLevel(Linear_Sampler, float3(input.TexCoord.xy, FroxelSlice), 0.0).xyz;
	//Output = min(Output, float3(0.5, 0.5, 0.5));

    return float4(Output, 1.0);
}
#endif




#ifdef OutputPixel

Texture2D OutputTex : register(t0);


float4 main(VertexShaderOutput input) : SV_Target
{
    return OutputTex.Sample(Linear_Sampler, input.TexCoord.xy);
}
#endif