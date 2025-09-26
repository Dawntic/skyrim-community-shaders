#include "Common/SharedData.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

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


cbuffer VolumeBuffer : register(b0)
{
    row_major float4x4 ShadowCascadeMatrix[4];
    row_major float4x4 CloudShadowMatrix;
    row_major float4x4 FogViewProjMatrix;
    float4 EVSMData;
    float4 FrustumNearFar;
    float4 CameraWS;
    float4 VolumeSize;
	float4 NoiseSize;
    float4 Jitter;
    uint FrameCounter;
    uint BoardCond;
};

cbuffer SettingsBuffer : register(b1)
{
    uint UIUseHistory;
    uint CheckerBoard;
    float UIHistoryAlpha;
    float UIWeight;
    float UIWeight2;
    float UIAnisotropy;
    float UIExtinction;
    float UISaturation;
    float UIShadowThreshold;
    uint UIExponent;
    float BlendOpp;
    float4 FogMapData;
    float4 FogMapColor;
}

cbuffer PerFrame : register(b2)
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

cbuffer PrevPerFrame : register(b3)
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

cbuffer ShadowUpdateCB : register(b4)
{
	float2 LightPxDir : packoffset(c0.x);   // direction on which light descends, from one pixel to next via dda
	float2 LightDeltaZ : packoffset(c0.z);  // per lightUVDir, normalised, [upper, lower] penumbra, should be negative
	uint StartPxCoord : packoffset(c1.x);
	float2 PxSize : packoffset(c1.y);
	float pad : packoffset(c1.w);
	float2 TerrainPosRange : packoffset(c2.x);
	float2 TerrainZRange : packoffset(c2.z);
}

struct ShadowDataStruct
{
    float4 VPOSOffset;
    float4 ShadowSampleParam;
    float4 EndSplits;
    float4 StartSplitDistances;
    float4 FocusShadowFadeParam;
    float4 DebugColor;
    float4 PropertyColor;
    float4 AlphaTestRef;
    float4 ShadowLightParam;
    float4x3 FocusShadowMapProj[4];
    float4x3 ShadowMapProj[2][3];
    float4x4 CameraViewProjInverseA[2];
};

SamplerState Linear_Sampler : register(s10);
SamplerState Point_Sampler : register(s11);
SamplerState AnisoClampSampler : register(s13);
SamplerState AnisoWrapSampler : register(s14);

StructuredBuffer<ShadowDataStruct> ShadowDataSB : register(t10);

#define EPSILON 1e-6
#define kPhi 1.61803398875



float3 FroxelWorldDirection(float3 Froxel, float ViewZ)
{
    float3 CoordsNDC = float3(((Froxel.xy + 0.5) / VolumeSize.xy) * 2.0 - 1.0, 1);
           CoordsNDC = float3(CoordsNDC.xy * float2(1.0, -1.0), CameraProj[0][2][2] + CameraProj[0][2][3] / ViewZ);

    float4 CoordsVS = mul(CameraProjInverse[0], float4(CoordsNDC, 1.0));
    float3 CoordsWS = mul((float3x3)CameraViewInverse[0], CoordsVS.xyz / CoordsVS.z);

    return CoordsWS;
}


float3 GetHistoryValue(float3 CoordsWS, out float2 Confidence)
{
    float4 PrevClip = mul(PrevCameraViewProj[0], float4(CoordsWS, 1.0));
    float3 PrevNDC = PrevClip.xyz / PrevClip.w;
    float2 PrevUV = PrevNDC.xy * float2(0.5, -0.5) + 0.5;
    float PrevZ = log2(PrevClip.w * FrustumNearFar.z) * rcp(log2(FrustumNearFar.y * FrustumNearFar.z));

    float ValidClip = float(PrevClip.w > 0.0);
    float ValidDepth = float(PrevZ >= 0.0 && PrevZ <= 1.0);

    Confidence = (any(abs(PrevNDC.xyz) > 1.0)) ? float2(0.0, 0.0) : float2(ValidClip, ValidClip * ValidDepth);

    return float3(PrevUV, PrevZ);
}


#ifdef SHADOW_COMPUTE

Texture3D HistoryVolume : register(t0);
Texture2DArray EVSMCascade : register(t1);
Texture2DArray BlueNoise : register(t2);
RWTexture3D<float> ShadowVolume : register(u0);


float GetCascadeShadow(float4 RayDirection){
    float2 Values = float2(8388608.0, -8388608.0);

    float ViewSplit = CameraProj[0][2][3] / (ShadowDataSB[0].EndSplits.x - CameraProj[0][2][2]); //move to cpu

    uint CascadeIndex = (RayDirection.w < ViewSplit) ? 0 : 1;
    float3 CoordsLS = mul(ShadowCascadeMatrix[CascadeIndex], float4(RayDirection.xyz, 1.0)).xyz;

    float Reconstruct = exp(UIExponent * (CoordsLS.z * 2.0 - 1.0)) + 9.9e-5;
    float2 Moments = EVSMCascade.SampleLevel(Linear_Sampler, float3(CoordsLS.xy, CascadeIndex), 0).xy + float2(9.9e-5, 9.9e-9);
    float Variance = max(Moments.y - Moments.x * Moments.x, Reconstruct * Reconstruct);
    float Delta = Reconstruct - Moments.x;

    float Visibility = Variance / (Variance + Delta * Delta);
          Visibility = (Reconstruct <= Moments.x) ? 1.0 : saturate(Visibility * Values.x + Values.y);

    return Visibility;
}


[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;

    float CoordZ = exp2(max(Froxel.z - 2, 0.1) / FrustumNearFar.w) / FrustumNearFar.z;
    float Bound2 = exp2(max(Froxel.z - 1, 0.1) / FrustumNearFar.w) / FrustumNearFar.z;
    float ThicknessZ = Bound2 - CoordZ;

    float BNoise = BlueNoise.Load(int4(ThreadID.xy & 63, 0, 0)).x;
    float BNoise2 = frac(BNoise + (FrameCounter & 31) * kPhi);

    float ViewZ = CoordZ + ThicknessZ * BNoise2;
    float3 RayPosition = FroxelWorldDirection(Froxel, ViewZ) * ViewZ;

    float Shadow = GetCascadeShadow(float4(RayPosition, ViewZ));

/*
    float2 Confidence;
    float SliceCenter = exp2(max(Froxel.z + 0.5, 0.1) / FrustumNearFar.w) / FrustumNearFar.z;
    float3 PrevCoordWS = RayVector * SliceCenter; // + CameraWS;
    float3 PrevCoordsUV = GetHistoryValue(PrevCoordWS, Confidence);
    float ShadowHistory = HistoryVolume.SampleLevel(AnisoClampSampler, PrevCoordsUV, 0).x;
    float BaseValue = 0.0;//0.85;
    Shadow = lerp(Shadow, ShadowHistory, BaseValue);
*/


    ShadowVolume[ThreadID] = Shadow;
}

#endif
/////////////////////////////////////////////////////////////////////////////////////////

//Player at slice 20 - 23
//Higher is further

//// Filter /////////////////////////////////////////////////////////////////////////////

#ifdef MEDIA_COMPUTE

Texture3D HistoryVolume : register(t0);
Texture3D Perlin : register(t1);
Texture2DArray BlueNoise : register(t2);
Texture2D FogMap : register(t3);
RWTexture3D<float4> MediaVolume : register(u0);
//RWTexture2D<float4> FogMap : register(u1);


float4 GetLocalFog(float3 CoordsWS){
    float MapCameraDepth = -249920.0;

    row_major float4x4 MapViewProj = float4x4(
    float4(1.19175, 1.01186E-07, -0.00029, 0.00),
    float4(0.00, 2.11867, 0.00065, 0.00),
    float4(0.00, 0.00035, -1.00036, -128.04633),
    float4(0.00, 0.00035, -1.00, 0.00));

    float4 MapCoordsNDC = mul(MapViewProj, float4(CoordsWS.xy, CoordsWS.z + MapCameraDepth, 1.0));
    float2 MapCoordsUV = (MapCoordsNDC.xy / MapCoordsNDC.w) * float2(0.5, -0.5) + 0.5;

    return FogMap.SampleLevel(Point_Sampler, MapCoordsUV, 0);
}

float2 MapRange(float x, float oldMin, float oldMax, float newMin, float newMax){
    return newMin + ((x - oldMin) / max(oldMax - oldMin, EPSILON_DIVISION)) * (newMax - newMin);
}


[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;

    float ViewZ = exp2((Froxel.z + 0.5) / FrustumNearFar.w) / FrustumNearFar.z;
    float ThicknessZ = exp2((Froxel.z + 1.5) / FrustumNearFar.w) / FrustumNearFar.z - ViewZ;

    float3 RayPosition = FroxelWorldDirection(Froxel, ViewZ) * ViewZ;

    float BNoise = BlueNoise.Load(int4(ThreadID.xy & 63, 0, 0)).x;
          BNoise = frac(BNoise + (FrameCounter & 31) * kPhi);


    float3 WorldPosition = RayPosition + CameraWS.xyz;
    float FroxelHeight = WorldPosition.z;

    float4 FogValue = GetLocalFog(WorldPosition);
    float Extinction = 1.0 - MapRange(FogValue.w, 0.0, 1.0, 0.0, 0.2);

    Extinction = MapRange(UIExtinction, 0.0, 0.1, 0.0, 0.1);


    float4 Output = float4(1.0, 1.0, 1.0, Extinction);
    //Output.xyz = WorldPosition;

/*
    float2 Confidence;
    float3 PrevCoordWS = RayDirection * ViewZ + CameraWS.xyz;
    float3 PrevCoordsUV = GetHistoryValue(PrevCoordWS, Confidence);
    float4 MediaHistory = HistoryVolume.SampleLevel(AnisoClampSampler, PrevCoordsUV, 0);
    float BaseValue = 0.90;
    Output = lerp(Output, MediaHistory, BaseValue);
*/


    MediaVolume[ThreadID] = Output;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////

//10

//// Scattering Compute Shader //////////////////////////////////////////////////////////

#ifdef SCATTER_COMPUTE

Texture3D ShadowVolume : register(t0);
Texture3D MediaVolume : register(t1);
RWTexture3D<float4> ScatteringVolume : register(u0);

float HenyeyGreensteinPhase(float ScatteringAngle, float Anisotropy){
    Anisotropy = clamp(Anisotropy, -0.999, 0.999);
	float AnisotropySq = Anisotropy * Anisotropy;
	float phase = max(1.0 + AnisotropySq - 2.0 * Anisotropy * ScatteringAngle, EPSILON);
    phase *= sqrt(phase);
    return (1.0 - AnisotropySq) / (4.0 * Math::PI * phase);
}

float PhaseFunction(float3 IncidentDir, float3 CameraDir, float Anisotropy, float Extinction, float Weight1, float Weight2, int MLobes){
	float SecondaryLobe = 0.0;
    float secondAnisotropy = Anisotropy * (2.0 / 3.0);

    float ScatteringAngle = dot(IncidentDir, CameraDir);
    float PrimaryLobe = HenyeyGreensteinPhase(ScatteringAngle, Anisotropy) * Weight1;

    for (int j = 1; j <= MLobes; ++j)
        SecondaryLobe += HenyeyGreensteinPhase(ScatteringAngle, secondAnisotropy);
    SecondaryLobe = (Weight2 * Extinction / float(MLobes - 1)) * SecondaryLobe;

    return PrimaryLobe + SecondaryLobe;
}
//float3 Scattering = Color * PhaseFunction(IncomingDir, OutgoingDir, UIAnisotropy, Media.w, UIWeight, UIWeight2, 2);


[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;

    float4 Media = MediaVolume.Load(int4(Froxel, 0));
    float Shadow = ShadowVolume.Load(int4(Froxel, 0)).x;

    float ViewZ = exp2((Froxel.z + 0.5) / FrustumNearFar.w) / FrustumNearFar.z;
    float3 RayPosition = FroxelWorldDirection(Froxel, ViewZ) * ViewZ;

    float3 IncomingDir = SharedData::DirLightDirection.xyz;
    float3 OutgoingDir = normalize(RayPosition);
    float ScatteringAngle = dot(IncomingDir, OutgoingDir);

    float3 Scattering = HenyeyGreensteinPhase(ScatteringAngle, UIAnisotropy).xxx;
           Scattering *= lerp(float3(1.0, 1.0, 1.0), SharedData::DirLightColor.xyz, UISaturation);
           Scattering = Scattering * Media.xyz * Shadow;


    ScatteringVolume[ThreadID] = float4(Scattering, Media.w);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Slice March Compute Shader /////////////////////////////////////////////////////////

#ifdef MARCH_COMPUTE

Texture3D ScatterVolume : register(t0);
RWTexture3D<float4> IntergrationVolume : register(u0);

void AccumulateScattering(inout float4 Accumulation, float4 ScatteringSlice, float StepLength){
    float Extinction = max(ScatteringSlice.w, EPSILON);
    float Transmittance = exp(-Extinction * StepLength);

    float3 InScatterIntegral = (-ScatteringSlice.xyz * Transmittance + ScatteringSlice.xyz) * rcp(Extinction);

    Accumulation.xyz += InScatterIntegral * Accumulation.w;
    Accumulation.w *= Transmittance;
}

float GameUnitToMeter(float input){
    return input * 0.01428222656;
}

[numthreads(8, 8, 1)]
void main(uint3 Froxel : SV_DispatchThreadID)
{
    float4 Accumulation = float4(0.0, 0.0, 0.0, 1.0);
    float PrevDepth = 0.0;

    for(int Slice=0; Slice < VolumeSize.z; Slice++){
        float4 ScatteredSlice = ScatterVolume.Load(int4(Froxel.xy, Slice, 0));

        float CurrDepth = exp2((Slice + 1.0) / FrustumNearFar.w) / FrustumNearFar.z;
        float StepLength = GameUnitToMeter(CurrDepth - PrevDepth);

        AccumulateScattering(Accumulation, ScatteredSlice, StepLength);

        PrevDepth = CurrDepth;
        IntergrationVolume[uint3(Froxel.xy, Slice)] = Accumulation;
    }
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Raymarch ///////////////////////////////////////////////////////////////////////////

#ifdef RAYMARCH_COMPUTE


[numthreads(8, 8, 1)]
void main(uint3 Froxel : SV_DispatchThreadID)
{

}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Apply //////////////////////////////////////////////////////////////////////////////

#ifdef APPLY_PIXEL

Texture3D IntergrationVolume : register(t0);
Texture2D DepthTex : register(t1);
Texture2DArray STBNoise : register(t2);

float GetFroxelSlice(float Depth){
    float FroxelSlice = log(Depth / FrustumNearFar.x) / log(FrustumNearFar.y / FrustumNearFar.x);
    return FroxelSlice;
}

float DepthVS(float depth){
    return (SharedData::CameraData.w / (-depth * SharedData::CameraData.z + SharedData::CameraData.x));
}

float4 main(VertexShaderOutput input) : SV_Target
{
    float2 Noise;
    Noise.x = STBNoise.Load(int4(int2(input.Position.xy) & 63, 0, 0)).x;
    Noise.y = STBNoise.Load(int4(int2(input.Position.yx) & 63, 0, 0)).x;
    Noise = frac(Noise + (float(FrameCounter & 16) * kPhi)) * 2.0 - 1.0;

    float Depth = DepthTex.Sample(Point_Sampler, input.TexCoord.xy).x;
    float FroxelDepth = GetFroxelSlice(DepthVS(Depth));

    float3 SamplePosition = float3(input.TexCoord.xy + (rcp(VolumeSize.xy) * Noise), FroxelDepth);

    float4 Output = IntergrationVolume.SampleLevel(Linear_Sampler, SamplePosition, 0.0);
    Output = saturate(Output);

    return float4(Output.xyz, 0.0);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Create EVSM ////////////////////////////////////////////////////////////////////////

#ifdef EVSM_COMPUTE

Texture2DArray CSM : register(t0);
RWTexture2DArray<float2> EVSM : register(u0);

static const int2 Offsets[8] = { int2(-1,-1), int2( 1,-1), int2(-1, 1), int2( 1, 1),
                                 int2(-2,-2), int2( 2,-2), int2(-2, 2), int2( 2, 2) };

static const float4 ONE = float4(1.0, 1.0, 1.0, 1.0);

[numthreads(16, 16, 1)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Coords = float3((float2(ThreadID.xy) + 0.5) / EVSMData.xy, ThreadID.z);

    float DeltaLim = 0.0; //??
    float3 Result = float3(0.0, 0.0, 0.0);
    [unroll] for(int i = 0; i < 4; ++i){
        float4 Sample = CSM.GatherRed(Point_Sampler, Coords, Offsets[i]);
        float4 Valid = (Sample < (1.0 - DeltaLim)) ? ONE : 0.0;
        float4 ExpValue = Valid * exp((Sample * 2.0 - 1.0) * UIExponent);
        Result += float3(dot(ExpValue, ONE), dot(ExpValue, ExpValue), dot(Valid, ONE));
    }
    float2 Output = (Result.z > 0) ? Result.xy / Result.z : EVSMData.zw;

    EVSM[ThreadID] = Output;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////

#ifdef EVSMBLUR_COMPUTE

Texture2DArray EVSM : register(t0);
RWTexture2DArray<float2> BlurOutput : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float2 Result = 3.4e+38;
    int SearchRadius = 2;
    [loop] for (int dy = -SearchRadius; dy <= SearchRadius; ++dy){
        [loop] for (int dx = -SearchRadius; dx <= SearchRadius; ++dx){
            Result = min(Result, EVSM.Load(int4(ThreadID.xy + int2(dx, dy), ThreadID.z, 0)).xy);
        }
    }
    Result = EVSM.Load(int4(ThreadID.xyz, 0)).xy;

    BlurOutput[ThreadID.xyz] = Result;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Fog Map ////////////////////////////////////////////////////////////////////////////

#ifdef DRAW_FOGMAP

RWTexture2D<float4> FogMap : register(u0);
Texture2D WorldMap : register(t0);

[numthreads(1, 1, 1)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float2 Coords = float2(ThreadID.xy);
    float Radius = FogMapData.z;

    float CameraDepth = -249920.0;

    row_major float4x4 MapViewProj = float4x4(
    float4(1.19175, 1.01186E-07, -0.00029, 0.00),
    float4(0.00, 2.11867, 0.00065, 0.00),
    float4(0.00, 0.00035, -1.00036, -128.04633),
    float4(0.00, 0.00035, -1.00, 0.00));

    float3 PlayerWSPosition = CameraWS.xyz;
           PlayerWSPosition.z += CameraDepth;

    float4 MapCoordsNDC = mul(MapViewProj, float4(PlayerWSPosition, 1.0));
    float2 MapCoordsUV = (MapCoordsNDC.xy / MapCoordsNDC.w) * float2(0.5, -0.5) + 0.5;
    float2 MapCoordsSS = MapCoordsUV * float2(2560.0, 1440.0);

     //if(length(Coords - MapCoordsSS) - Radius < 0.0){
     //   FogMap[ThreadID.xy] = float4(1.0, 0, 0, 1.0);
     //}

    float4 Output = 0.0;
    float4 CurrValue = FogMap[ThreadID.xy];

    if(length(Coords - FogMapData.xy) - Radius < 0.0){
        float Density = (BlendOpp != -1) ? CurrValue.w + FogMapColor.w : CurrValue.w - FogMapColor.w;
              Density = saturate(Density);

        float4 Output = float4(FogMapColor.xyz, Density);

        FogMap[ThreadID.xy] = Output;
    }

}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Noise //////////////////////////////////////////////////////////////////////////////

//https://github.com/Bubblebird-Studio/NoiseGenerator

#ifdef PERLIN_COMPUTE

#define VolumeSize float3(32.0, 32.0, 32.0)
#define perlinSize 0.16
#define perlinOctaves 2
#define perlinLacunarity 2.0
#define seed 346

RWTexture3D<float> PerlinVolume : register(u0);

uint hash_uint(uint x){
    uint h = x;
    h ^= (h >> 16);
    h *= 0x85EBCA6Bu;
    h ^= (h >> 13);
    h *= 0xC2B2AE35u;
    h ^= (h >> 16);
    return h;
}

uint hash_int3(int3 v){
    uint x = asuint(v.x);
    uint y = asuint(v.y);
    uint z = asuint(v.z);

    uint h = 0xDEADBEEFu;
    h ^= x + 0x9E3779B9u + (h << 6) + (h >> 2);
    h ^= y + 0x9E3779B9u + (h << 6) + (h >> 2);
    h ^= z + 0x9E3779B9u + (h << 6) + (h >> 2);

    h ^= (h >> 16);
    h *= 0x85EBCA6Bu;
    h ^= (h >> 13);
    h *= 0xC2B2AE35u;
    h ^= (h >> 16);

    return h;
}

float3 rand_vector(uint h){
    uint x = hash_uint(h ^ 0xA53C9A1Fu);
    uint y = hash_uint(h ^ 0xC2B2AE35u);
    uint z = hash_uint(h ^ 0x27D4EB2Fu);

    const float invU32 = 1.0f / 4294967296.0f; // 2^-32
    return float3((float)x * invU32, (float)y * invU32, (float)z * invU32);
}

float3 fade(float3 t){
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float grad(uint h, float3 p){
    uint hh = (h & 15u);
    float3 g = rand_vector(hh) * 2.0 - 1.0;
    return dot(g, p);
}

[numthreads(8, 8, 8)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float grid_resolution = floor(1.0 / perlinSize);
    float3 position = float3(ThreadID) / VolumeSize * grid_resolution;

    float Output = 0.0;
    [loop] for(uint i = 0u; i < perlinOctaves; ++i){
        uint Sample = seed + i;

        float attenuationF = floor(pow(perlinLacunarity, (float)i));
        float3 p = position * attenuationF;

        int3  pi = (int3)floor(p);
        float3 pf = frac(p);
        float3 f = fade(pf);

        int3 period = (grid_resolution * (int)attenuationF).xxx;

        float n000 = grad(hash_int3(( (pi + int3(0,0,0)) % period )) + Sample, pf - float3(0,0,0));
        float n001 = grad(hash_int3(( (pi + int3(0,0,1)) % period )) + Sample, pf - float3(0,0,1));
        float n010 = grad(hash_int3(( (pi + int3(0,1,0)) % period )) + Sample, pf - float3(0,1,0));
        float n011 = grad(hash_int3(( (pi + int3(0,1,1)) % period )) + Sample, pf - float3(0,1,1));
        float n100 = grad(hash_int3(( (pi + int3(1,0,0)) % period )) + Sample, pf - float3(1,0,0));
        float n101 = grad(hash_int3(( (pi + int3(1,0,1)) % period )) + Sample, pf - float3(1,0,1));
        float n110 = grad(hash_int3(( (pi + int3(1,1,0)) % period )) + Sample, pf - float3(1,1,0));
        float n111 = grad(hash_int3(( (pi + int3(1,1,1)) % period )) + Sample, pf - float3(1,1,1));

        float x00 = lerp(n000, n100, f.x);
        float x01 = lerp(n001, n101, f.x);
        float x10 = lerp(n010, n110, f.x);
        float x11 = lerp(n011, n111, f.x);

        float y0 = lerp(x00, x10, f.y);
        float y1 = lerp(x01, x11, f.y);

        Output += lerp(y0, y1, f.z) / max(attenuationF, 1.0);
    }

    PerlinVolume[ThreadID] = Output * 0.5 + 0.5;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



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




