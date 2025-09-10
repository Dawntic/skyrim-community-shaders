#include "Common/SharedData.hlsli"
#include "Common/FrameBuffer.hlsli"

struct VertexShaderOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

cbuffer VolumeBuffer : register(b0)
{
    row_major float4x3 ShadowCascadeMatrix[4];
    row_major float4x4 CloudShadowMatrix;
    float4 EVSMData;
    float4 FrustumNearFar;
    float4 VolumeSize;
	float4 NoiseSize;
    float4 Jitter;
    uint FrameCounter;
    uint BoardCond;
};

SamplerState Linear_Sampler : register(s10);
SamplerState Point_Sampler : register(s11);

#define EPSILON 1e-6


//// Apply Pixel Shader /////////////////////////////////////////////////////////////////

#ifdef APPLY_PIXEL

Texture3D IntergrationVolume : register(t0);
Texture2D DepthTex : register(t1);
Texture2DArray STBNoise : register(t2);
Texture3D Filtering : register(t3);
Texture3D SliceMarch : register(t4);
Texture3D STBNoiseVec : register(t51);

#define kPhi 1.61803398875

float GetFroxelSlice(float Depth){
    float FroxelSlice = log(Depth / FrustumNearFar.x) / log(FrustumNearFar.y / FrustumNearFar.x);
    return FroxelSlice;
}

float LinearDepth(float depth){
    return (SharedData::CameraData.w / (-depth * SharedData::CameraData.z + SharedData::CameraData.x));
}

float4 main(VertexShaderOutput input) : SV_Target
{
    float2 Noise;
    Noise.x = STBNoise.Load(int4(int2(input.Position.xy) & 63, 0, 0)).x;
    Noise.y = STBNoise.Load(int4(int2(input.Position.yx) & 63, 0, 0)).x;
    Noise = frac(Noise + (float(FrameCounter & 16) * kPhi)) * 2.0 - 1.0;

    float Depth = DepthTex.Sample(Point_Sampler, input.TexCoord.xy).x;
          Depth = GetFroxelSlice(LinearDepth(Depth));

    float3 SamplePosition float3(input.TexCoord.xy + (rcp(VolumeSize.xy) * Noise), Depth);

    float4 Output = IntergrationVolume.SampleLevel(Linear_Sampler, SamplePosition, 0.0);


    return float4(Output.xyz, 1.0);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////


