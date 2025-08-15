#include "Common/SharedData.hlsli"

struct VertexShaderOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

#ifdef ApplyVolume

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

Texture3D Volume : register(t0);
//Texture2D SceneTex : register(t1);
Texture2D DepthTex : register(t2);

SamplerState Linear_Sampler : register(s0);

#define EPSILON 1e-6

float DepthToViewZ(float depth){
    float ViewZ = SharedData::CameraData.w / max(EPSILON, SharedData::CameraData.x - depth * SharedData::CameraData.z);
    return clamp(ViewZ, SharedData::CameraData.y, SharedData::CameraData.x);
}

float2 LocalToFroxelUV(float2 UVCoords){
    float2 minUV = 0.5 / VolumeSize.xy;
    return clamp((floor(UVCoords * VolumeSize.xy) + 0.5) / VolumeSize.xy, minUV, 1.0 - minUV); //sub TAA jitter here
}

float ViewZToFroxelZ(float ViewZ) {
    float NormDepth = log(max(ViewZ, SharedData::CameraData.y + EPSILON) / SharedData::CameraData.y) / log(SharedData::CameraData.x / SharedData::CameraData.y);
    float CenterDepth = clamp(NormDepth * VolumeSize.z - 0.5, 0.0, VolumeSize.z - 1.0);
    return (CenterDepth + 0.5) / VolumeSize.z;
}

float4 main(VertexShaderOutput input) : SV_Target
{
    //float3 Scene = SceneTex.Sample(Linear_Sampler, input.TexCoord).xyz;
    float Depth = DepthTex.Sample(Linear_Sampler, input.TexCoord).x;

    float ViewZ = DepthToViewZ(Depth);
    float3 FroxelCoords = float3(LocalToFroxelUV(input.TexCoord), ViewZToFroxelZ(ViewZ));

    float3 output = Volume.Sample(Linear_Sampler, FroxelCoords).xxx;

    return float4(output, 1.0);
}
#endif




