#include "Common/SharedData.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

cbuffer VolumeBuffer : register(b0)
{
    row_major float4x4 ShadowCascadeMatrix[4];
    row_major float4x4 CloudShadowMatrix;
    float4 EVSMData;
    float4 FrustumNearFar;
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

struct ShadowDataStruct
{
    float4 VPOSOffset;
    float4 ShadowSampleParam;    // fPoissonRadiusScale / iShadowMapResolution in z and w
    float4 EndSplits;    // cascade end distances int xyz, cascade count int z
    float4 StartSplitDistances;  // cascade start ditances int xyz, 4 int z
    float4 FocusShadowFadeParam;
    float4 DebugColor;
    float4 PropertyColor;
    float4 AlphaTestRef;
    float4 ShadowLightParam;  // Falloff in x, ShadowDistance squared in z
    float4x3 FocusShadowMapProj[4];
    float4x3 ShadowMapProj[2][3];
    float4x4 CameraViewProjInverse[2];
};

SamplerState Linear_Sampler : register(s10);
SamplerState Point_Sampler : register(s11);

StructuredBuffer<ShadowDataStruct> ShadowDataSB : register(t10);

#define EPSILON 1e-6

float4 FroxelWorldPosition(float3 Froxel)
{
    float2 CoordsNDC = (Froxel.xy / VolumeSize.xy) * 2.0 - 1.0;
	float Depth = exp2(Froxel.z / FrustumNearFar.w) / FrustumNearFar.z;

    float3 CoordsVS = float3(float2(CoordsNDC.x, -CoordsNDC.y) * Depth * float2(CameraProjInverse[0][0][0], CameraProjInverse[0][1][1]), Depth);
    float3 CoordsWS = mul(CameraViewInverse[0], float4(CoordsVS, 1.0)).xyz;

    float4 CoordsCS = mul(CameraViewProj[0], float4(CoordsWS, 1.0));
    float ClipZ = CoordsCS.z / CoordsCS.w;

	return float4(CoordsWS, ClipZ);
}



#ifdef SHADOW_COMPUTE

Texture3D HistoryVolume : register(t0);
Texture2DArray EVSMCascade : register(t1);
Texture2DArray BlueNoise : register(t2);
RWTexture3D<float> ShadowVolume : register(u0);

SamplerState AnisoX4Sampler : register(s13);

#define kPhi 1.61803398875

float ShadowVisibility(float4 Coords, float2 ThicknessZ, float Noise){
    ShadowDataStruct ShadowData = ShadowDataSB[0];

    int FromBuffer = 0.05;

    //float shadowMapThreshold = cascadeIndex == 0 ? 0.01f : 0.0f;
    //noShadow = shadowMapValue >= positionLS.z - shadowMapThreshold;

    float Shadow = 0.0;
    int NSamples = 1;
    float passed = 0.00001;
    float4 CoordsWS = Coords;
    for (int i = 0; i < NSamples; ++i){
        uint CascadeIndex = (CoordsWS.w < ShadowData.EndSplits.x) ? 0 : (CoordsWS.w < ShadowData.EndSplits.y) ? 1 : (CoordsWS.w < ShadowData.EndSplits.z) ? 2 : 3;
        float3 CoordsLS = mul(ShadowCascadeMatrix[CascadeIndex], float4(CoordsWS.xyz, 1.0)).xyz;
        CoordsLS.z = CoordsLS.z * 2.0 - 1.0;

        if (abs(CoordsLS.z) > 1.0)
            break;

        float2 Moments = EVSMCascade.SampleLevel(Linear_Sampler, float3(CoordsLS.xy, CascadeIndex), 0).xy;
        float Variance = max(Moments.y - Moments.x * Moments.x, 1e-6); //sharpness?
        float Receiver = exp(CoordsLS.z * UIExponent);

        float Delta = Receiver - Moments.x;
        float Visibility = Variance / (Variance + Delta * Delta);
        Visibility = (Receiver <= Moments.x) ? 1.0 : Visibility;

        Shadow += Visibility;

        float NoiseOffset = (FromBuffer + i + 1) & 15;
        CoordsWS.xyz *= ThicknessZ.x + ThicknessZ.y * frac(Noise + NoiseOffset * 1.6180);

        passed += 1;
    }

    return (Shadow / passed);
}


float GetHistoryValue(float3 CoordsWS)
{
    float4 PrevCoordsVS = mul(PrevCameraView[0], float4(CoordsWS, 1.0));
    float4 PrevCoordsCS = mul(PrevCameraProj[0], PrevCoordsVS);

    float2 PrevUVCoords = (PrevCoordsCS.xy / PrevCoordsCS.w) * float2(0.5, -0.5) + float2(0.5, 0.5);
    float PrevUVDepth = saturate(log2(PrevCoordsVS.z * FrustumNearFar.z) * rcp(log2(FrustumNearFar.y * FrustumNearFar.z)));

    float HistoryValue = HistoryVolume.SampleLevel(AnisoX4Sampler, float3(PrevUVCoords, PrevUVDepth), 0.0).x;

    return HistoryValue;
}

[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;

    float2 Diag = float2(CameraProjInverse[0][0][0], CameraProjInverse[0][1][1]);
    float3x3 InvViewRot = (float3x3)CameraViewInverse[0];

    float3 c0 = mul(InvViewRot, float3(Diag.x, 0.0, 0.0)).xyz;
    float3 c1 = mul(InvViewRot, float3(0.0, Diag.y, 0.0)).xyz;
    float3 c2 = mul(InvViewRot, float3(0.0, 0.0, 1.0)).xyz;
    float3x3 Matrix = transpose(float3x3(c0, c1, c2));

    float3 CoordsNDC = float3(((Froxel.xy + 0.5) / VolumeSize.xy) * 2.0 - 1.0, 1.0);
           CoordsNDC.y = -CoordsNDC.y;

    float3 RayDirection = mul(Matrix, CoordsNDC).xyz;

    float CoordZ = exp2(max(Froxel.z - 2, 0.1) / FrustumNearFar.w) / FrustumNearFar.z;
    float Bound2 = exp2(max(Froxel.z - 1, 0.1) / FrustumNearFar.w) / FrustumNearFar.z;

    float ThicknessZ = Bound2 - CoordZ;

    float BNoise = BlueNoise.Load(int4(ThreadID.xy & 63, 0, 0)).x;
          BNoise = frac(BNoise + (FrameCounter & 31) * kPhi);

    float3 OffsetCoords = RayDirection * (CoordZ + ThicknessZ * BNoise);

    float3 CameraWS = mul(CameraViewInverse[0], float4(0, 0, 0, 1)).xyz;
    OffsetCoords += CameraWS;

    float4 CoordsCS = mul(CameraViewProj[0], float4(OffsetCoords, 1.0));
    float ClipZ = CoordsCS.z / CoordsCS.w;

    float Shadow = ShadowVisibility(float4(OffsetCoords, ClipZ), float2(CoordZ, ThicknessZ), BNoise);

    //float ShadowHistory = GetHistoryValue(CoordsWS.xyz);

    //Shadow = lerp(Shadow, ShadowHistory, 1.0 - UIHistoryAlpha);


    ShadowVolume[ThreadID] = Shadow;
}

#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Filter /////////////////////////////////////////////////////////////////////////////

#ifdef MEDIA_COMPUTE

Texture3D PrevVolume : register(t0);
Texture3D Perlin : register(t1);
Texture2DArray BlueNoise : register(t2);
RWTexture3D<float4> MediaVolume : register(u0);

SamplerState AnisoWrapSampler : register(s14);


float3 GetPreviousUV(float3 CoordsWS){
    float4 PrevCoordsVS = mul(PrevCameraView[0], float4(CoordsWS, 1.0));
    float4 PrevCoordsCS = mul(PrevCameraProj[0], PrevCoordsVS);
    float Depth = saturate(log2(PrevCoordsVS.z * FrustumNearFar.z) * rcp(log2(FrustumNearFar.y * FrustumNearFar.z)));

    return float3((PrevCoordsCS.xy / PrevCoordsCS.w) * float2(0.5, -0.5) + 0.5, Depth);
}

float GameUnitToMeter(float input){
    return input * 0.0142875;
}

float MeterToGameUnit(float input) {
    return input / 0.0142875;
}

#define kPhi 1.61803398875


[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;

    float2 Diag = float2(CameraProjInverse[0][0][0], CameraProjInverse[0][1][1]);
    float3x3 InvViewRot = (float3x3)CameraViewInverse[0];

    float3 c0 = mul(InvViewRot, float3(Diag.x, 0.0, 0.0)).xyz;
    float3 c1 = mul(InvViewRot, float3(0.0, Diag.y, 0.0)).xyz;
    float3 c2 = mul(InvViewRot, float3(0.0, 0.0, 1.0)).xyz;
    float3x3 Matrix = transpose(float3x3(c0, c1, c2));

    float3 CoordsNDC = float3(((Froxel.xy + 0.5) / VolumeSize.xy) * 2.0 - 1.0, 1.0);
           CoordsNDC.y = -CoordsNDC.y;
    float3 RayDirection = mul(Matrix, CoordsNDC).xyz;

    float CoordZ = exp2((Froxel.z + 0.5) / FrustumNearFar.w) / FrustumNearFar.z;
    float Bound2 = exp2((Froxel.z + 1.5) / FrustumNearFar.w) / FrustumNearFar.z;
    float ThicknessZ = Bound2 - CoordZ;


    float BNoise = BlueNoise.Load(int4(ThreadID.xy & 63, 0, 0)).x;
          BNoise = frac(BNoise + (FrameCounter & 31) * kPhi);


    float OffsetZ = ThicknessZ * BNoise + CoordZ;
    float3 OffsetWS = RayDirection * OffsetZ - mul(CameraViewInverse[0], float4(0, 0, 0, 1)).xyz;


    float Threshold = 0.4;
    float Gain = 5.42;
    float NoiseScale = 1.0 / MeterToGameUnit(25.0);
    float3 NoiseCoord = OffsetWS * NoiseScale;
    float Perlin1 = Perlin.SampleLevel(AnisoWrapSampler, NoiseCoord, 0.0).x;
    float Perlin2 = Perlin.SampleLevel(AnisoWrapSampler, NoiseCoord * 4.0, 0.0).x;
    float Noise = saturate((((Perlin1 + Perlin2) * 0.5) - Threshold) * Gain);

    float FadeRate = 0.0002;
    Noise = smoothstep(0.0, 1.0, Noise);
    Noise = lerp(Noise, 1.0, saturate(OffsetZ * FadeRate));

    Noise *= 0.002;

    Noise *= 50;

    MediaVolume[ThreadID] =  Noise.xxxx;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////

//BufferOne._m18.w = 0.0002 — per-meter fade rate to “no modulation.”
//At OffsetZ ≈ 1 / 0.0002 = 5000 units, the factor hits 1.0, and you fully blend to 1.0 (i.e., the noise stops modulating the quantity). This avoids far-distance shimmer.

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


[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;

    float4 CoordsWS = FroxelWorldPosition(Froxel);

    float4 CameraPosWS = mul(CameraViewInverse[0], float4(0, 0, 0, 1));
    float3 IncomingDir = SharedData::DirLightDirection.xyz;
    float3 OutgoingDir = -normalize(CameraPosWS.xyz - CoordsWS.xyz);

    float3 Color = lerp(float3(1.0, 1.0, 1.0), SharedData::DirLightColor.xyz, UISaturation);
    float3 Scattering = Color * PhaseFunction(IncomingDir, OutgoingDir, UIAnisotropy, UIExtinction, UIWeight, UIWeight2, 2) * 10;

    float Shadow = ShadowVolume.Load(int4(Froxel, 0)).x;
    float4 Media = MediaVolume.Load(int4(Froxel, 0));

    Scattering = Scattering * Shadow * Media.xyz;

    ScatteringVolume[ThreadID] = float4(Scattering, UIExtinction);
    //ScatteringVolume[ThreadID] = FilterVolume.Load(int4(ThreadID, 0.0));
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
    return input * 0.0142875;
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



//// Create EVSM ////////////////////////////////////////////////////////////////////////

#ifdef EVSM_COMPUTE

Texture2DArray CSM : register(t0);
RWTexture2DArray<float2> EVSM : register(u0);

static const int2 Offset[4] = { int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1) };

[numthreads(16, 16, 1)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float2 Result;
    uint Samples;

    float3 SamplePosition = float3((ThreadID.xy + 0.5) / EVSMData.xy, ThreadID.z);

    for(int i=0; i<4; i++){
        float4 Sample = CSM.GatherRed(Point_Sampler, SamplePosition, Offset[i]);
        uint IsValid = (uint)any(Sample < 1.0);
        float4 ExpValue = exp((Sample * 2.0 - 1.0) * UIExponent);
        Result += float2(dot(ExpValue, float4(1,1,1,1)), dot(ExpValue, ExpValue)) * IsValid;
        Samples += 4 * IsValid;
    }
    float2 Output = (Samples != 0) ? Result * rcp(float(Samples)) : float2(EVSMData.z, EVSMData.w);

    EVSM[ThreadID] = Output;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////

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
        uint s = seed + i;

        float attenuationF = floor(pow(perlinLacunarity, (float)i));
        float3 p = position * attenuationF;

        int3  pi = (int3)floor(p);
        float3 pf = frac(p);
        float3 f = fade(pf);

        int3 period = (grid_resolution * (int)attenuationF).xxx;

        float n000 = grad(hash_int3(( (pi + int3(0,0,0)) % period )) + s, pf - float3(0,0,0));
        float n001 = grad(hash_int3(( (pi + int3(0,0,1)) % period )) + s, pf - float3(0,0,1));
        float n010 = grad(hash_int3(( (pi + int3(0,1,0)) % period )) + s, pf - float3(0,1,0));
        float n011 = grad(hash_int3(( (pi + int3(0,1,1)) % period )) + s, pf - float3(0,1,1));
        float n100 = grad(hash_int3(( (pi + int3(1,0,0)) % period )) + s, pf - float3(1,0,0));
        float n101 = grad(hash_int3(( (pi + int3(1,0,1)) % period )) + s, pf - float3(1,0,1));
        float n110 = grad(hash_int3(( (pi + int3(1,1,0)) % period )) + s, pf - float3(1,1,0));
        float n111 = grad(hash_int3(( (pi + int3(1,1,1)) % period )) + s, pf - float3(1,1,1));

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