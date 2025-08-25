#include "Common/SharedData.hlsli"
#include "Common/FrameBuffer.hlsli"

struct VertexShaderOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

cbuffer ShadowVolumeBuffer : register(b0)
{
    float4 FrustumNearFar;
    float4 VolumeSize;
	float4 NoiseSize;
	float2 ShadowAtlasSize;
	float CellJitterValue;
	float RayJitterValue;
	uint ESM_Scale;
	uint ESM_EXP;
    uint FrameCounter;
};
SamplerState Linear_Sampler : register(s10);
SamplerState Point_Sampler : register(s11);

#define EPSILON 1e-6


//// Apply Pixel Shader /////////////////////////////////////////////////////////////////

#ifdef APPLY_PIXEL

Texture3D IntergrationVolume : register(t0);
Texture2D DepthTex : register(t1);
Texture1D Repartition : register(t2);
Texture2DArray STBNoise : register(t3);
Texture3D STBNoiseVec : register(t51);

float GetFroxelSlice(float Depth){
    float FroxelSlice = log(Depth / FrustumNearFar.x) / log(FrustumNearFar.y / FrustumNearFar.x);
    return FroxelSlice;
}

float LinearDepth(float depth){
    return (SharedData::CameraData.w / (-depth * SharedData::CameraData.z + SharedData::CameraData.x));
}

float4 main(VertexShaderOutput input) : SV_Target
{
    float3 TexelSize = rcp(VolumeSize.xyz);

    float4 Noise;
    Noise.xyz = STBNoiseVec.Load(int4(int2(input.Position.xy) & 63, SharedData::FrameCountAlwaysActive & 63, 0)).xyz;
    Noise.w = STBNoise.Load(int4(int2(input.Position.xy) & 63, SharedData::FrameCountAlwaysActive & 31, 0)).x;
    Noise = (Noise * 2.0 - 1.0) * 1.5;

    float Depth = DepthTex.Sample(Point_Sampler, input.TexCoord.xy).x;
    Depth = GetFroxelSlice(LinearDepth(Depth));
    Depth = max(0.0, Depth - TexelSize.z * 1.5);

    float4 Output;
    for (int i=0; i<4; ++i){
        float3 TexCoord = float3(input.TexCoord.xy, Depth) + Noise.xyz * float3(1.0, 1.0, 0.5) * TexelSize;
        Output += IntergrationVolume.SampleLevel(Linear_Sampler, TexCoord.xyz, 0.0) / 4.0;
        Noise = Noise.yzwx;
    }
    Output = saturate(Output);

    return float4(Output.xyz, 1.0);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Output Pixel Shader ////////////////////////////////////////////////////////////////

#ifdef OUTPUT_PIXEL

Texture2D VLResult : register(t0);
Texture3D Scattering : register(t1);
Texture3D Filtering : register(t2);
Texture3D SliceMarch : register(t3);

float4 main(VertexShaderOutput input) : SV_Target
{
    float3 Output = VLResult.Sample(Point_Sampler, input.TexCoord.xy).xyz;
    //Output = Scattering.SampleLevel(Linear_Sampler, float3(input.TexCoord.xy, 0.1), 0) *2;
    //Output = Filtering.Sample(Linear_Sampler, float3(input.TexCoord.xy, 0.1)) * 2;
    //Output = SliceMarch.Sample(Linear_Sampler, float3(input.TexCoord.xy, 0.3));

    return float4(Output, 1.0);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////