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
    //row_major float4x4 CamerView;
    row_major float4x4 InvCameraView;
    //row_major float4x4 FrustumInvViewProj;
    float4 FrustumNearFar;
    float4 frustumTL;
    float4 frustumTR;
    float4 frustumBL;
    float4 frustumBR;
    float4 CameraPosition;
    float4 CameraPositionTest;
    float4 VolumeSize;
	float4 NoiseSize;
	float2 ShadowAtlasSize;
	float CellJitterValue;
	float RayJitterValue;
	uint ESM_Scale;
	uint ESM_EXP;
    uint FrameCounter;
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
SamplerState Point_Sampler : register(s11);

#define EPSILON 1e-6




#ifdef MarchVolumeCompute

Texture3D ScatteringVolume : register(t0);
Texture1D InvRepartition : register(t1);

RWTexture3D<float4> IntergrationVolume : register(u0);

float4 FroxelWorldPosition(float3 Froxel)
{
    float SliceZ = exp2(Froxel.z / FrustumNearFar.w) / FrustumNearFar.z; //FrustumNearFar: 1.0 / nearPlane in z  volumeDepth / log2(farPlane / nearPlane) in w
    float2 CoordsUV = (Froxel.xy / VolumeSize.xy) * SliceZ;
    float4 CoordsWS = mul(Frustum, float4(CoordsUV.xy, SliceZ, 1.0));

    return float4(CoordsWS.xyz, SliceZ);
}

float3 calcWorldSpacePos(float3 Froxel){
    float2 invProjDiag = float2(CameraProjInverse[0][0][0], CameraProjInverse[0][1][1]);

    //float ZDepth = FrustumNearFar.x * exp2((Froxel.z + 0.5) * rcp(VolumeSize.z) * log2(FrustumNearFar.y / FrustumNearFar.x));
    float Depth = Froxel.z * rcp(VolumeSize.z);
	float ZPosition = FrustumNearFar.x * exp2(Depth * (log2(FrustumNearFar.y / FrustumNearFar.x)));

    float2 CoordsNDC = (Froxel.xy / VolumeSize.xy) * 2.0 - 1.0;
    CoordsNDC.y = -CoordsNDC.y;

    float3 posVS = float3(CoordsNDC * ZPosition * invProjDiag, ZPosition);
    float3 posWS = mul(CameraViewInverse[0], float4(posVS, 1.0)).xyz;  // world space

    //float2 CoordsUV = Froxel.xy / VolumeSize.xy;

	//float3 pos = lerp(frustumTL.xyz, frustumTR.xyz, CoordsUV.x);
	//pos = lerp(pos, lerp(frustumBL.xyz, frustumBR.xyz, CoordsUV.x), CoordsUV.y);

	//float Depth = Froxel.z * rcp(VolumeSize.z);
	//float ZPosition = FrustumNearFar.x * exp2(Depth * (log2(FrustumNearFar.y / FrustumNearFar.x)));
	//pos *= ZPosition / FrustumNearFar.y;

	//pos += CameraPosition.xyz;

	return posWS;
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
    float3 CoordsUV = Froxel.xyz * (1.0 / (VolumeSize.xyz));
    //CoordsUV.z = max(CoordsUV.z, 0.0002);
	float depth = InvRepartition.SampleLevel(Linear_Sampler, CoordsUV.z, 0).x;

	float4 CoordsNDC = float4(CoordsUV.xy * 2.0 - 1.0, depth, 1.0);
	CoordsNDC.y = -CoordsNDC.y;
	float4 CoordsWS = mul(CameraViewProjInverse[0], CoordsNDC);
	CoordsWS *= 1.0 / CoordsWS.w;
	float4 CoordsCS = mul(CameraViewProj[0], CoordsWS);
	CoordsCS *= 1.0 / CoordsCS.w;

    return float4(CoordsWS.xyz, CoordsCS.z);
}

float3 simpleTonemap(float3 color){
	float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
	return color / max(1.0 + luma, 1e-6);
}

float GameUnitToMeter(float input){
    return input * 0.0142875;
}

[numthreads(8, 8, 1)]
void main(uint3 Froxel : SV_DispatchThreadID)
{
    float4 Accumulation = float4(0.0, 0.0, 0.0, 1.0);
    float3 PrevCoordsWS = calcWorldSpacePos(float3(Froxel.xy + 0.5, 0.0)).xyz;
    //float3 PrevCoordsWS = GetWorldCoords(float3(Froxel.xy + 0.5, 0.0)).xyz;

    for(int Slice=0; Slice < VolumeSize.z; Slice++){
        float3 CoordsWS = calcWorldSpacePos(float3(Froxel.xy + 0.5, Slice + 1.0)).xyz;
        //float3 CoordsWS = GetWorldCoords(float3(Froxel.xy + 0.5, Slice + 1.0)).xyz;

        float StepLength = distance(PrevCoordsWS, CoordsWS);
              StepLength = GameUnitToMeter(StepLength);

        float4 ScatteredSlice = ScatteringVolume.Load(int4(Froxel.xy, Slice, 0));

        AccumulateScattering(Accumulation, ScatteredSlice, StepLength);

        PrevCoordsWS = CoordsWS;
        IntergrationVolume[uint3(Froxel.xy, Slice)] = saturate(Accumulation);
    }

    //float3 SliceStart = calcWorldSpacePos(float3(Froxel.xy + 0.5, 0.0)).xyz;
    //float3 SliceEnd = calcWorldSpacePos(float3(Froxel.xy + 0.5, VolumeSize.z - 1.0)).xyz;
    //float Dist = distance(SliceStart, SliceEnd);
    //Dist = GameUnitToMeter(Dist);
    //Dist = GameUnitToMeter(Dist);

    //IntergrationVolume[uint3(Froxel.xy, 0)] = float4(Dist,Dist,Dist,Dist);
}
#endif



#ifdef ApplyVolume

Texture3D IntergrationVolume : register(t0);
Texture2D DepthTex : register(t1);
Texture1D Repartition : register(t2);
Texture2DArray STBNoise : register(t3);
Texture3D STBNoiseVec : register(t51);

float4 FroxelWorldPosition(float3 Froxel)
{
    float SliceZ = exp2(Froxel.z / FrustumNearFar.w) / FrustumNearFar.z;
    float2 CoordsUV = (Froxel.xy / VolumeSize.xy) * SliceZ;
    float4 CoordsWS = mul(Frustum, float4(CoordsUV.xy, SliceZ, 1.0));

    return float4(CoordsWS.xyz, SliceZ);
}

float GetFroxelSlice(float Depth){
    float FroxelSlice = log(Depth / FrustumNearFar.x) / log(FrustumNearFar.y / FrustumNearFar.x);
    return FroxelSlice;
}

float LinearDepth(float depth){ ///CHECK THIS
    //return (SharedData::CameraData.y * SharedData::CameraData.x) / (SharedData::CameraData.x - depth * (SharedData::CameraData.x - SharedData::CameraData.y));
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
    //Depth = clamp(Depth, 0.0001, 0.9999);

          //Depth = clamp(Repartition.SampleLevel(Linear_Sampler, Depth, 0).x, 0.0, 0.99999);
          //Depth = max(0.0, Depth - TexelSize.z * 1.5);
    //Depth = Repartition.SampleLevel(Linear_Sampler, Depth, 0).x;

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



#ifdef OutputPixel

Texture2D VLResult : register(t0);
Texture3D Scattering : register(t1);
Texture3D Filtering : register(t2);
Texture3D SliceMarch : register(t3);


float4 main(VertexShaderOutput input) : SV_Target
{
    float3 Output = VLResult.Sample(Point_Sampler, input.TexCoord.xy).xyz;

    Output.x = ((asuint(Output.x) & 0x7fffffff) > 0x7f800000) ? 1.0 : Output.x;
    Output.y = ((asuint(Output.y) & 0x7fffffff) > 0x7f800000) ? 1.0 : Output.y;
    Output.z = ((asuint(Output.z) & 0x7fffffff) > 0x7f800000) ? 1.0 : Output.z;
    //Output = Scattering.SampleLevel(Linear_Sampler, float3(input.TexCoord.xy, 0.1), 0) *2;
    //Output = Filtering.Sample(Linear_Sampler, float3(input.TexCoord.xy, 0.1)) * 2;
    //Output = SliceMarch.Sample(Linear_Sampler, float3(input.TexCoord.xy, 0.3));

    return float4(Output, 1.0);
}
#endif