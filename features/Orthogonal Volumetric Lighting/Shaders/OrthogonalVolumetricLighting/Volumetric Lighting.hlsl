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
    float4 FogMapData;
    float4 FogMapColor;
    float BlendOpp;
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


float3 FroxelWorldPosition(float3 Froxel)
{
    float2 Diag = float2(CameraProjInverse[0][0][0], CameraProjInverse[0][1][1]);
    float3x3 InvViewRot = (float3x3)CameraViewInverse[0];

    float3 c0 = mul(InvViewRot, float3(Diag.x, 0.0, 0.0)).xyz;
    float3 c1 = mul(InvViewRot, float3(0.0, Diag.y, 0.0)).xyz;
    float3 c2 = mul(InvViewRot, float3(0.0, 0.0, 1.0)).xyz;
    float3x3 Matrix = transpose(float3x3(c0, c1, c2));

    float3 CoordsNDC = float3(((Froxel.xy + 0.5) / VolumeSize.xy) * 2.0 - 1.0, 1.0);
           CoordsNDC.y = -CoordsNDC.y;

    float3 RayDirection = mul(Matrix, CoordsNDC).xyz;

    return RayDirection;
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

float GetCascadeShadow(float4 CoordsWS, float3 RayDirection, float2 SliceZ, float Noise){
    ShadowDataStruct ShadowData = ShadowDataSB[0];

    float BiasVal = 0.0005;
    float2 Values = float2(8388608.0, -8388608.0);
    int NSamples = 1; //i think adding more than 1 sample breaks things

    float Result = 0.0;
    for (int i = 0; i < NSamples; ++i){
        //uint CascadeIndex = (CoordsWS.w < ShadowData.EndSplits.y) ? 0 : (CoordsWS.w < ShadowData.EndSplits.x) ? 1 : (CoordsWS.w < ShadowData.EndSplits.z) ? 2 : 3;
        uint CascadeIndex = (CoordsWS.w < ShadowData.EndSplits.x) ? 0 : 1;
        //if(CascadeIndex > 1)
            //return 0.5;
        float3 CoordsLS = mul(ShadowCascadeMatrix[CascadeIndex], float4(CoordsWS.xyz, 1.0)).xyz;

        float Reconstruct = exp(UIExponent * (saturate(CoordsLS.z) * 2.0 - 1.0)) + 9.9e-5;
        float2 Moments = EVSMCascade.SampleLevel(Linear_Sampler, float3(CoordsLS.xy, CascadeIndex), 0).xy + float2(9.9e-5, 9.9e-9);
        float VarianceBias = BiasVal * Reconstruct;
        float Variance = max(Moments.y - Moments.x * Moments.x, VarianceBias * VarianceBias);

        float Delta = Reconstruct - Moments.x;

        float Visibility = Variance / (Variance + Delta * Delta);
              Visibility = (Reconstruct <= Moments.x) ? 1.0 : saturate(Visibility * Values.x + Values.y);

        Result += Visibility;

        float NoiseOffset = frac(Noise + (((FrameCounter & 31) + i + 1) * kPhi));
        CoordsWS.xyz = RayDirection * (SliceZ.x + (SliceZ.y * NoiseOffset));
    }
    Result /= NSamples;

    return Result;
}


[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;

    float3 RayDirection = FroxelWorldPosition(Froxel);


    float CoordZ = exp2(max(Froxel.z - 2, 0.1) / FrustumNearFar.w) / FrustumNearFar.z;
    float Bound2 = exp2(max(Froxel.z - 1, 0.1) / FrustumNearFar.w) / FrustumNearFar.z;
    float ThicknessZ = Bound2 - CoordZ;


    float BNoise = BlueNoise.Load(int4(ThreadID.xy & 63, 0, 0)).x;
    float BNoise2 = frac(BNoise + (FrameCounter & 31) * kPhi);

    float3 CameraWS = mul(CameraViewInverse[0], float4(0, 0, 0, 1)).xyz;

    float3 OffsetCoords = RayDirection * (CoordZ + ThicknessZ * BNoise2);
           OffsetCoords += CameraWS; //??

    float4 CoordsCS = mul(CameraViewProj[0], float4(OffsetCoords, 1.0));
    float ClipZ = CoordsCS.z / CoordsCS.w;

    float Shadow = GetCascadeShadow(float4(OffsetCoords, ClipZ), RayDirection, float2(CoordZ, ThicknessZ), BNoise);

    //Shadow += GetTerrainShadow(OffsetCoords, Point_Sampler);


    float2 Confidence;
    float SliceCenter = exp2(max(Froxel.z + 0.5, 0.1) / FrustumNearFar.w) / FrustumNearFar.z;
    float3 PrevCoordWS = CameraWS + RayDirection * SliceCenter;
    float3 PrevCoordsUV = GetHistoryValue(PrevCoordWS, Confidence);

    float ShadowHistory = HistoryVolume.SampleLevel(AnisoClampSampler, PrevCoordsUV, 0).x;

    float Threshold = 1.0;
    float ConfidenceValue = abs(Shadow - ShadowHistory) - Threshold;
          ConfidenceValue = 1.0 - saturate(ConfidenceValue / (1.0 - Threshold + EPSILON));
          ConfidenceValue *= Confidence.y;

    float BaseValue = 0.85;
    Shadow = lerp(Shadow, ShadowHistory, BaseValue);


     //Shadow = lerp(Shadow, ShadowHistory, (1.0 - UIHistoryAlpha) * Confidence);

    ShadowVolume[ThreadID] = Shadow;
}

#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Filter /////////////////////////////////////////////////////////////////////////////

#ifdef MEDIA_COMPUTE

Texture3D HistoryVolume : register(t0);
Texture3D Perlin : register(t1);
Texture2DArray BlueNoise : register(t2);
Texture2D FogMap : register(t3);
RWTexture3D<float4> MediaVolume : register(u0);

float GameUnitToMeter(float input){
    return input * 0.0142875;
}

float MeterToGameUnit(float input){
    return input / 0.0142875;
}

float GetHeightFog(float StartAltitude, float MaxAltitude, float DecayRate, float cosZenith, float Extinction){
    float Zenith = rcp(max(abs(cosZenith), EPSILON));
    float MinHeight = (cosZenith >= 0) ? StartAltitude : -1e6;
    float Fog = exp(-max(MinHeight - MaxAltitude, 0) * DecayRate) * (Zenith * rcp(DecayRate));
    float OpticalDepth = Extinction * (max((MaxAltitude - MinHeight) * Zenith, 0) + Fog);

	return OpticalDepth; //exp(-OpticalDepth);
}


[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;

    float3 RayDirection = FroxelWorldPosition(Froxel);

    float CoordZ = exp2((Froxel.z + 0.5) / FrustumNearFar.w) / FrustumNearFar.z;
    float ThicknessZ = exp2((Froxel.z + 1.5) / FrustumNearFar.w) / FrustumNearFar.z - CoordZ;

    float BNoise = BlueNoise.Load(int4(ThreadID.xy & 63, 0, 0)).x;
          BNoise = frac(BNoise + (FrameCounter & 31) * kPhi);

    float3 CameraWS = mul(CameraViewInverse[0], float4(0, 0, 0, 1)).xyz;

    float OffsetZ = ThicknessZ * BNoise + CoordZ;
    float3 OffsetWS = RayDirection * OffsetZ - CameraWS; //camera?


    float2 Confidence;
    float3 PrevCoordWS = RayDirection * CoordZ - CameraWS;
    float3 PrevCoordsUV = GetHistoryValue(PrevCoordWS, Confidence);
    float MediaHistory = HistoryVolume.SampleLevel(AnisoClampSampler, PrevCoordsUV, 0).x;

    float FadeRate = 0.0002;
    float Threshold = 0.4;
    float Gain = 5.42;
    float NoiseScale = 1.0 / MeterToGameUnit(25.0);

    float3 NoiseCoord = OffsetWS * NoiseScale;
    float Perlin1 = Perlin.SampleLevel(AnisoWrapSampler, NoiseCoord, 0.0).x;
    float Perlin2 = Perlin.SampleLevel(AnisoWrapSampler, NoiseCoord * 4.0, 0.0).x;
    float Noise = saturate((((Perlin1 + Perlin2) * 0.5) - Threshold) * Gain);
          Noise = smoothstep(0.0, 1.0, Noise);
          Noise = lerp(Noise, 1.0, saturate(OffsetZ * FadeRate));


    float3 UpWS = float3(0,0,1);
    float3 FogBaseWS = float3(0,0,TerrainPosRange.x);
    float Bound1 = exp2((Froxel.z) / FrustumNearFar.w) / FrustumNearFar.z;
    float Bound2 = exp2((Froxel.z + 1.0) / FrustumNearFar.w) / FrustumNearFar.z;
    float3 PointA = RayDirection * Bound1 + CameraWS;
    float3 PointB  = RayDirection * Bound2 + CameraWS;
    float StartAltitude = dot(PointA - FogBaseWS, UpWS);
    float EndAltitude = dot(PointB - FogBaseWS, UpWS);

    float DecayRate = rcp(MeterToGameUnit(600.0));
    //float DecayRate = rcp(TerrainPosRange.y);
    float cosZenith = dot(-normalize(RayDirection), UpWS);
    float Extinction = UIExtinction;
    //float HeightFog = GetHeightFog(StartAltitude, EndAltitude, DecayRate, cosZenith, Extinction);


    float4 FogClip = mul(FogViewProjMatrix, float4(OffsetWS, 1.0));
    FogClip.xyz /= FogClip.w;

    float3 FogCoords = float3(FogClip.xy * float2(0.5, -0.5) + 0.5, 1.0); //

    float3 Fog = FogMap.SampleLevel(AnisoClampSampler, float3(FogCoords.xy, 0.0), 0.0).xyz;

    float LocalFog = 1.0 - min(1.0, abs((FogCoords.z - Fog.x) / Fog.y)); //coord Z
          LocalFog = LocalFog * LocalFog * Fog.z;

    //float GlobalFog = 1.0 - clamp((OffsetWS.z - UIBaseHeight) * UIInverseFalloff, 0.0, 1.0);
    //      GlobalFog = GlobalFog * GlobalFog * GlobalFog * UIFogVisibility;

    float HeightFog = LocalFog; //+ GlobalFog;

    //float4 Output = float4(1.0, 1.0, 1.0, HeightFog) * Noise;

    float BaseValue = 0.90;
    //Output = lerp(Output, MediaHistory, BaseValue);

     float4 Output = float4(Noise,Noise,Noise,Noise);


    MediaVolume[ThreadID] = Output;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



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

    float3 RayDirection = FroxelWorldPosition(Froxel);

    float4 Media = MediaVolume.Load(int4(Froxel, 0));
    float Shadow = ShadowVolume.Load(int4(Froxel, 0)).x;

    float3 IncomingDir = SharedData::DirLightDirection.xyz;
    float3 OutgoingDir = -normalize(RayDirection);

    float3 Color = lerp(float3(1.0, 1.0, 1.0), SharedData::DirLightColor.xyz, UISaturation);
    float3 Scattering = Color * PhaseFunction(IncomingDir, OutgoingDir, UIAnisotropy, Media.w, UIWeight, UIWeight2, 2) * 10;
    //float3 Scattering = Color * PhaseFunction(IncomingDir, OutgoingDir, UIAnisotropy, UIExtinction, UIWeight, UIWeight2, 2) * 10;


    Scattering = Scattering * Shadow * Media.xyz;

    ScatteringVolume[ThreadID] = float4(Scattering, Media.w);
    //ScatteringVolume[ThreadID] = float4(Scattering, UIExtinction);
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
//viewSpaceToMetersFactor = 0.01428222656;

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

static const int2 Offsets[4] = { int2(-1,-1), int2( 1,-1), int2(-1, 1), int2( 1, 1) };
static const float4 ONE = float4(1.0, 1.0, 1.0, 1.0);

[numthreads(16, 16, 1)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    bool ForceEnable = false;
    float3 Coords = float3((float2(ThreadID.xy) + 0.5) / EVSMData.xy, ThreadID.z);

    float3 Result = float3(0.0, 0.0, 0.0);
    [unroll] for(int i = 0; i < 4; ++i){
        float4 Sample = CSM.GatherRed(Point_Sampler, Coords, Offsets[i]);
        float4 Value = ForceEnable ? ONE : saturate(sign(ONE - Sample));
        float4 ExpValue = Value * exp((Sample * 2.0 - 1.0) * 5.0);
        Result += float3(dot(ExpValue, ONE), dot(ExpValue, ExpValue), dot(Value, ONE));
    }
    float2 Output = (Result.z > 0) ? Result.xy / Result.z : EVSMData.zw;

    EVSM[ThreadID] = Output;
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

    float3 SamplePosition = float3(input.TexCoord.xy + (rcp(VolumeSize.xy) * Noise), Depth);

    float4 Output = IntergrationVolume.SampleLevel(Linear_Sampler, SamplePosition, 0.0);


    return float4(Output.xyz, 1.0);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Fog Map ////////////////////////////////////////////////////////////////////////////

#ifdef DRAW_FOGMAP

RWTexture2D<float4> FogMap : register(u0);

float4 SatAddBlend(float4 dst, float4 src){
    return float4(dst.rgb + src.rgb * src.a, saturate(dst.a + src.a));
}

float4 SatSubBlend(float4 dst, float4 src){
    float3 outRgb = max(0.0, dst.rgb - src.rgb * src.a);
    return float4(outRgb, max(0.0, dst.a - src.a));
}

float4 AbsSubBlend(float4 dst, float srcA){
    return float4(dst.rgb, dst.a) * (1.0 - srcA);
}

[numthreads(1, 1, 1)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float2 Coords = float2(ThreadID.xy);
    float Radius = FogMapData.z;

    if(length(Coords - FogMapData.xy) - Radius > 0.0)
        return;

    float4 Curr = FogMap[ThreadID.xy];

    float Dist = distance(Coords, FogMapData.xy);
    float Feather = (1.0 - FogMapData.w) * (Radius * 0.5);
    float Mask = 1.0 - smoothstep(Radius - Feather, Radius, Dist);

    float Density = (BlendOpp != -1) ? Curr.w + FogMapColor.w : Curr.w - FogMapColor.w;
          Density = saturate(Density);

    float4 Output = float4(FogMapColor.xyz, Density);
           //Output *= Mask;

    FogMap[ThreadID.xy] = Output;
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




