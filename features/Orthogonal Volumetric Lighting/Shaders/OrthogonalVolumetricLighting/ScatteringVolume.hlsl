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
Texture2DArray NoiseTexture : register(t2);
RWTexture3D<float> ShadowVolume : register(u0);

SamplerState AnisoX4Sampler : register(s13);

float ShadowVisibility(float4 Coords, float2 Thickness, float Noise){
    ShadowDataStruct ShadowData = ShadowDataSB[0];

    int FromBuffer = 0.05;

    float shadowMapThreshold = cascadeIndex == 0 ? 0.01f : 0.0f;
    noShadow = shadowMapValue >= positionLS.z - shadowMapThreshold;

    float Shadow = 0.0;
    int NSamples = 4;
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
        CoordsWS.xyz *= Thickness.x + Thickness.y * frac(Noise + NoiseOffset * 1.6180);

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

    float4 CoordsWS = FroxelWorldPosition(Froxel + 0.5);

    float Bound1 = exp2((Froxel.z-1) / FrustumNearFar.w) / FrustumNearFar.z;
    float Bound2 = exp2((Froxel.z-2) / FrustumNearFar.w) / FrustumNearFar.z;
    float Thickness = Bound2 - Bound1;

    float Noise = NoiseTexture.Load(int4(ThreadID.xy & 63, 0, 0)).x;

    float Shadow = ShadowVisibility(CoordsWS, float2(Bound1, Thickness), Noise);

    float ShadowHistory = GetHistoryValue(CoordsWS.xyz);

    Shadow = lerp(Shadow, ShadowHistory, 1.0 - UIHistoryAlpha);


    ShadowVolume[ThreadID] = Shadow;
}

#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Scattering Compute Shader //////////////////////////////////////////////////////////

#ifdef SCATTER_COMPUTE

Texture3D ShadowVolume : register(t0);
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
    float3 Scattering = Color * PhaseFunction(IncomingDir, OutgoingDir, UIAnisotropy, UIExtinction, UIWeight, UIWeight2, 2);

    float Shadow = ShadowVolume.Load(int4(Froxel, 0)).x;

    Scattering = Scattering * Shadow;

    ScatteringVolume[ThreadID] = float4(Scattering, UIExtinction);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Filter /////////////////////////////////////////////////////////////////////////////

Texture3D InputVolume : register(t0);
Texture3D Perlin : register(t1);
Texture3D BlueNoise : register(t2);
RWTexture3D<float4> FilterVolume : register(u0);

SamplerState AnisoWrapSampler : register(s13);
SamplerState AnisoClampSampler : register(s13);


float3 GetPreviousUV(float3 CoordsWS){
    float4 PrevCoordsVS = mul(PrevCameraView[0], float4(CoordsWS, 1.0));
    float4 PrevCoordsCS = mul(PrevCameraProj[0], PrevCoordsVS);
    float Depth = saturate(log2(PrevCoordsVS.z * FrustumNearFar.z) * rcp(log2(FrustumNearFar.y * FrustumNearFar.z)));

    return float3((PrevCoordsCS.xy / PrevCoordsCS.w) * float2(0.5, -0.5) + 0.5, Depth);
}


[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;

    float Bound1 = exp2((Froxel.z + 0.5) / FrustumNearFar.w) / FrustumNearFar.z;
    float Bound2 = exp2((Froxel.z + 1.0) / FrustumNearFar.w) / FrustumNearFar.z;
    float Thickness = Bound2 - Bound1;

    float3 PrevUV = GetPreviousUV(WorldCoords.xyz);

    float BNoise = BlueNoise.SampleLevel(AnisoClampSampler, PrevUV, 0.0);

    float ConfSample = 1.0;
    float Cofidence = any(abs(PrevNDC) > 1.0) ? 0.0 : 0.9 * (PrevClip.w > 0.0) * ConfSample;

    float BNoiseOffset = (Cofidence > 0) ? BNoise * ConfSample : BNoise;

    float OffsetZ = Bound1 + Thickness * BNoiseOffset;

    float4 CameraPosWS = mul(CameraViewInverse[0], float4(0, 0, 0, 1));
    float NoiseCoord = OffsetZ * CoordsWS.xyz - CameraPosWS.xyz;  //Jittered Z
          NoiseCoord *= rcp(32.0);

    float Perlin1 = Perlin.SampleLevel(AnisoWrapSampler, NoiseCoord, 0.0).x;
    float Perlin2 = Perlin.SampleLevel(AnisoWrapSampler, NoiseCoord * 4.0, 0.0).x;
    float Noise = saturate((((Perlin1 + Perlin2) * 0.5) - 0.39) * 5.42);

    float NoiseFinal = (Noise * Noise) * (3.0 - (2.0 * Noise));
          NoiseFinal = lerp(NoiseFinal, 1.0, saturate(OffsetZ * 0.0002));

    float Mult = FFF * NoiseFinal;
    FilterVolume[ThreadID] = Output;
}
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