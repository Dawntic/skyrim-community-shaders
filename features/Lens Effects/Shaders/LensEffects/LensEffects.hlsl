#include "LensEffects/LensEffectHelper.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Math.hlsli"

//// Structs ////////////////////////////////////////////////////////////////////////////

struct VertexShaderInput
{
    float4 Position : POSITION;
    float2 TexCoord : TEXCOORD;
};

struct VertexShaderOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

/////////////////////////////////////////////////////////////////////////////////////////



//// Resources //////////////////////////////////////////////////////////////////////////

cbuffer Settings : register(b1)
{
    uint slice;
    uint KernalWidth;
    uint EXP;
    uint ESM_SCALE;
    float2 SrcSize;
    float2 InvSrcSize;
    float2 DstSize;
    //float2 FilterDir;
};

SamplerState Linear_Sampler : register(s10);
SamplerState Point_Sampler : register(s11);
SamplerComparisonState Depth_Sampler : register(s13);

/////////////////////////////////////////////////////////////////////////////////////////

#define EPSILON 1e-6

//// Create ESM /////////////////////////////////////////////////////////////////////////

#ifdef DownSample

Texture2DArray ShadowMap : register(t0);

float main(VertexShaderOutput input) : SV_Target
{
    float4 accum = 0.0;
    float4 Sample = 0.0;

    float3 samplingPos = float3(((input.TexCoord.xy * DstSize * 4.0 + 1.0) / SrcSize), slice);

    Sample = ShadowMap.GatherRed(Point_Sampler, samplingPos, int2(0, 0));
    accum += clamp(exp(EXP * (Sample - 1.0)), EPSILON, 1.0);
    Sample = ShadowMap.GatherRed(Point_Sampler, samplingPos, int2(2, 0));
    accum += clamp(exp(EXP * (Sample - 1.0)), EPSILON, 1.0);
    Sample = ShadowMap.GatherRed(Point_Sampler, samplingPos, int2(0, 2));
    accum += clamp(exp(EXP * (Sample - 1.0)), EPSILON, 1.0);
    Sample = ShadowMap.GatherRed(Point_Sampler, samplingPos, int2(2, 2));
    accum += clamp(exp(EXP * (Sample - 1.0)), EPSILON, 1.0);

    float output = sum4(accum) * (1 / (float(KernalWidth) * float(KernalWidth))) * ESM_SCALE;

    return output;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Minify /////////////////////////////////////////////////////////////////////////////

#ifdef Minify

Texture2D DownSampled : register(t0);

float main(VertexShaderOutput input) : SV_Target
{
    float4 accum = 0.0;

   // float2 samplingPos = (input.TexCoord.xy * DstSize * 4.0 + 1.0) / SrcSize;
    //float2 samplingPos = (float2((uint2)input.Position.xy * (uint)KernalWidth) + 0.5) / SrcSize;

    float2 samplingPos = (input.Position.xy * 4.0 + 1.0) / SrcSize;

    accum += DownSampled.GatherRed(Point_Sampler, samplingPos, int2(0, 0));
    accum += DownSampled.GatherRed(Point_Sampler, samplingPos, int2(2, 0));
    accum += DownSampled.GatherRed(Point_Sampler, samplingPos, int2(0, 2));
    accum += DownSampled.GatherRed(Point_Sampler, samplingPos, int2(2, 2));

    float output = sum4(accum) *  (1 / (float(KernalWidth) * float(KernalWidth)));

    return output;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Blur ///////////////////////////////////////////////////////////////////////////////

#ifdef Filter

Texture2D ESM : register(t0);

float main(VertexShaderOutput input) : SV_Target
{
    /*
    float accum = 0.0;
    float Radius = (KernalWidth - 1) / 2;

    [loop] for (int i = -Radius; i <= Radius; ++i){
        float2 Offset = i * (InvSrcSize * FilterDir);
        accum += ESM.Sample(Point_Sampler, input.TexCoord + Offset);
    }
    float output = accum * (1.0 / KernalWidth);

    if(output == 1.0 && FilterDir.x == 0.0)
        output = 0.0;

    return output;
*/
    return 1;
}
#endif
//// Blur ///////////////////////////////////////////////////////////////////////////////



//// Bypass VS //////////////////////////////////////////////////////////////////////////

#ifdef BYPASS_VSSHADER


VertexShaderOutput main(VertexShaderInput input)
{
    VertexShaderOutput output;
    output.TexCoord = input.TexCoord;
    output.Position = float4(input.Position.xy, 0.0, 1.0);

    return output;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////


