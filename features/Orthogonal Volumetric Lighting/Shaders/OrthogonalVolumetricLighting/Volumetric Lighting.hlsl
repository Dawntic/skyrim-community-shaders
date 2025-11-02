#include "Common/SharedData.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/Color.hlsli"


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
    row_major float4x4 DirectionalShadowCascadeMatrix[4];
    row_major float4x4 LocalShadowCascadeMatrix[16];
    row_major float4x4 FogViewProjMatrix;
    float4 ShadowCascadeEndSplit;
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
    float UIExtinction;
    float UIAnisotropy;
    uint UIEVSMExponent;
    float UIScatterRatio;
    float UISaturation;

    float UIGlobalFogDensity;
    float UIGlobalFogStartHeight;
    float UIGobalFogFalloffHeight;

    float FogMapBlendOpp;
    float4 FogMapData;
    float4 FogMapColor;

    uint CheckerBoard;
}

cbuffer PerFrame : register(b10)
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
SamplerState LinearMinSampler : register(s15);

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
    float PrevZ = log(PrevClip.w / FrustumNearFar.x) / log(FrustumNearFar.y / FrustumNearFar.x);

    float ValidViewZ = float(PrevClip.w > 0.0);
    float ValidDepth = float(PrevZ >= 0.0 && PrevZ <= 1.0);

    Confidence = (any(abs(PrevNDC.xyz) > 1.0)) ? float2(0.0, 0.0) : float2(ValidViewZ, ValidViewZ * ValidDepth);

    return float3(PrevUV, PrevZ);
}


#ifdef SHADOW_COMPUTE

#include "TerrainShadows/TerrainShadows.hlsli"
#include "CloudShadows/CloudShadows.hlsli"

Texture3D HistoryVolume : register(t0);
Texture2DArray EVSMCascade : register(t1);
Texture2DArray BlueNoise : register(t2);
Texture2DArray CSMCascade : register(t3);
RWTexture3D<float> ShadowVolume : register(u0);
Texture2DArray STBNFloat3 : register(t4);

float GetFroxelSlice(float Depth){
    float FroxelSlice = log(Depth / FrustumNearFar.x) / log(FrustumNearFar.y / FrustumNearFar.x);
    return FroxelSlice;
}

float DepthVS(float depth){
    return (SharedData::CameraData.w / (-depth * SharedData::CameraData.z + SharedData::CameraData.x));
}

float EVSM_Visibility(float3 CoordsLS, float2 Moments){
    float Offset = 8388888;
    float BiasVal = 0.0005;

    float Depth = exp(UIEVSMExponent * CoordsLS.z);
    float DepthBias = BiasVal * Depth;
    float Delta = Depth - Moments.x;

    float Variance = max(Moments.y - Moments.x * Moments.x, DepthBias * DepthBias);
    float Visibility = Variance / (Variance + Delta * Delta);

    return (Depth <= Moments.x) ? 1.0 : saturate(Visibility * Offset + -Offset);
}

float GetCascadeShadow(float3 RayPosition, float ViewZ, float CoordZ, float ThicknessZ, float BNoise){

    int Samples = 4;

    float Result = 0;
    for(int i=0; i<Samples; i++){
        float3 RaySampleCoords = RayPosition * ViewZ;

        uint CascadeIndex = (ViewZ < ShadowCascadeEndSplit.x) ? 0 : 1;
        float3 CoordsLS = mul(DirectionalShadowCascadeMatrix[CascadeIndex], float4(RaySampleCoords, 1.0)).xyz;
        float2 Moments = EVSMCascade.SampleLevel(Linear_Sampler, float3(CoordsLS.xy, CascadeIndex), 0).xy;
        float Visibility = EVSM_Visibility(CoordsLS, Moments);

        Result += Visibility;

        float ViewZNoise = frac(BNoise + (FrameCounter * (i+2)) * kPhi);
        ViewZ = CoordZ + ThicknessZ * ViewZNoise;
    }
    Result /= Samples;

    return Result;
}


[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;

    float CoordZ = exp2(max(Froxel.z - 2, 0.1) / FrustumNearFar.w) / FrustumNearFar.z;
    float ThicknessZ = exp2(max(Froxel.z - 1, 0.1) / FrustumNearFar.w) / FrustumNearFar.z - CoordZ;

    float BNoise = BlueNoise.Load(int4(ThreadID.xy & 63, 0, 0)).x; //true vec4 STBN from SSGI
    float ViewZNoise = frac(BNoise + FrameCounter * kPhi);

    float ViewZ = CoordZ + ThicknessZ * ViewZNoise;
    float3 RayPosition = FroxelWorldDirection(Froxel, ViewZ);

    float3 WorldPosition = RayPosition * ViewZ + CameraWS.xyz;

    float CascadeShadow = GetCascadeShadow(RayPosition, ViewZ, CoordZ, ThicknessZ, BNoise);

    float UICloudShadowContrib = 1.0;
    float CloudShadow = CloudShadows::GetCloudShadowMult(WorldPosition, Linear_Sampler) * UICloudShadowContrib;

    float TerrainShadow = TerrainShadows::GetTerrainShadow(WorldPosition, Linear_Sampler);

    float Shadow = CascadeShadow;// + CloudShadow + TerrainShadow;


    float2 Confidence;
    float ViewZCenter = exp2(max(Froxel.z + 0.5, 0.1) / FrustumNearFar.w) / FrustumNearFar.z;
    float3 PrevCoordsUV = GetHistoryValue(RayPosition * ViewZCenter, Confidence);

    float BaseValue = 0.85;
    float ShadowHistory = HistoryVolume.SampleLevel(AnisoClampSampler, PrevCoordsUV, 0).x;
    Shadow = lerp(Shadow, ShadowHistory, BaseValue);

    ShadowVolume[ThreadID] = Shadow;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////

/*
    //Shadow = min(Shadow, 0.12);

    float2 Splits = float2(0.98704, 0.99777);
    float ViewSplit = GetFroxelSlice(DepthVS(Splits.x));
          ViewSplit = exp2((ViewSplit * VolumeSize.z) / FrustumNearFar.w) / FrustumNearFar.z;
    uint cascadeIndex = (ViewZ < ViewSplit) ? 0 : 1;
    float3 positionLS = mul(DirectionalShadowCascadeMatrix[cascadeIndex], float4(RayPosition * ViewZ, 1.0)).xyz;
    float shadowMapValue = CSMCascade.SampleLevel(Linear_Sampler, float3(positionLS.xy, cascadeIndex), 0).x;
    float shadowMapThreshold = cascadeIndex == 0 ? 0.01 : 0.0;
    Shadow = float(shadowMapValue >= positionLS.z - shadowMapThreshold);

    Shadow = GetLightingShadow(0, RayPosition * ViewZ, 0, ViewZ);
*/

//// Create EVSM ////////////////////////////////////////////////////////////////////////

#ifdef EVSM_COMPUTE

Texture2DArray CSM : register(t0);
RWTexture2DArray<float4> EVSM : register(u0);

float sum4(float4 value){ return value.x + value.y + value.z + value.w; }

static const int2 Offsets[4] = { int2(-1,-1), int2( 1,-1), int2(-1, 1), int2( 1, 1) };


[numthreads(16, 16, 1)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float2 Coords = (float2(ThreadID.xy) + 0.5) / EVSMData.xy;

    float DeltaLim = 0.3; //add to UI?
    float ValidSamples = 0;
    float2 Output = 0.0;
    for(int i=0; i<4; i++){
        float4 Sample = CSM.GatherRed(Linear_Sampler, float3(Coords, ThreadID.z), Offsets[i]);
        float4 Valid = (Sample < (1.0 - DeltaLim)) ? 1 : 0;
        float4 ExpValue = Valid * exp(UIEVSMExponent * Sample);
        Output += float2(sum4(ExpValue), sum4(ExpValue * ExpValue));
        ValidSamples += sum4(Valid);
    }
    Output = (ValidSamples > 0) ? Output.xy / ValidSamples : EVSMData.zw;

    EVSM[ThreadID.xyz] = Output.xyxy;
}
#endif

/////////////////////////////////////////////////////////////////////////////////////////

#ifdef EVSMBLUR_COMPUTE

Texture2DArray EVSM : register(t0);
RWTexture2DArray<float4> BlurOutput : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float2 Result = 1e+10;
    int SearchRadius = 2;
    [loop] for (int dy = -1; dy <= SearchRadius; ++dy){
        [loop] for (int dx = -1; dx <= SearchRadius; ++dx){
            int2 Coords = clamp(int2(ThreadID.xy) + int2(dx, dy), int2(0,0), int2(EVSMData.xy - 1));
            float4 Sample = EVSM.SampleLevel(Linear_Sampler, float3(Coords / EVSMData.xy, ThreadID.z), 0);
            Result = (Result.x < Sample.x) ? Result.xy : Sample.xy;
            //float2 MinTest = (Sample.x < Sample.z) ? Sample.xy : Sample.zw;
            //Result = (Result.x < MinTest.x) ? Result.xy : MinTest.xy;
        }
    }
    Result.xy = EVSM.SampleLevel(Linear_Sampler, float3(ThreadID.xy / EVSMData.xy, ThreadID.z), 0).xy;

    //float4 Sample = EVSM.Load(int4(ThreadID.xyz, 0));
    //Result = (Sample.x < Sample.z) ? Sample.xy : Sample.zw;

    BlurOutput[ThreadID.xyz] = Result.xyxy;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////

//Player at slice 20 - 23
//Higher is further

//// Media Volume ///////////////////////////////////////////////////////////////////////

#ifdef MEDIA_COMPUTE

Texture3D HistoryVolume : register(t0);
Texture3D Perlin : register(t1);
Texture2DArray BlueNoise : register(t2);
Texture2D FogMap : register(t3);
RWTexture3D<float4> MediaVolume : register(u0);
//RWTexture2D<float4> FogMap : register(u1);

//Maybe:
//Abledo
//Base height
//Falloff


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

float GetGlobalHeightFog(float FroxelWorldHeight){
    //method 1
    float GlobalFog = clamp((FroxelWorldHeight - UIGlobalFogStartHeight) * rcp(UIGobalFogFalloffHeight), 0.0, 1.0);
    GlobalFog = GlobalFog * GlobalFog * GlobalFog * UIGlobalFogDensity;

    //method 2
	//GlobalFog = exp(-(FroxelWorldHeight - UIGlobalFogStartHeight) * UIGobalFogFalloffHeight);
    return GlobalFog;
}

[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;

    float ViewZ = exp2((Froxel.z + 0.5) / FrustumNearFar.w) / FrustumNearFar.z;
    //float ThicknessZ = exp2((Froxel.z + 1.5) / FrustumNearFar.w) / FrustumNearFar.z - ViewZ;
    float3 RayPosition = FroxelWorldDirection(Froxel, ViewZ) * ViewZ;

    //float BNoise = BlueNoise.Load(int4(ThreadID.xy & 63, 0, 0)).x;
    //      BNoise = frac(BNoise + (FrameCounter & 31) * kPhi);

    float3 WorldPosition = RayPosition + CameraWS.xyz;

    float4 LocalFogData = GetLocalFog(WorldPosition);
    float LocalFog = 0.0;

    float GlobalFog = GetGlobalHeightFog(WorldPosition.z);

    float DensityAtFroxel = GlobalFog + LocalFog;
    DensityAtFroxel = 1.0;

    float MediaExt = UIExtinction * DensityAtFroxel;
    float3 MediaScat = UIScatterRatio.xxx;

    float4 Output = float4(MediaScat, MediaExt);
    //Output.xyz = WorldPosition;

//// Wind Vectoring
/*
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
*/

//// Reprojection
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



//// Scattering Compute Shader //////////////////////////////////////////////////////////

#ifdef SCATTER_COMPUTE

#	if defined(LIGHT_LIMIT_FIX)
#		include "LightLimitFix/LightLimitFix.hlsli"
#	endif

#	if defined(ISL) && defined(LIGHT_LIMIT_FIX)
#		include "InverseSquareLighting/InverseSquareLighting.hlsli"
#	endif

Texture3D ShadowVolume : register(t0);
Texture3D MediaVolume : register(t1);
RWTexture3D<float4> ScatteringVolume : register(u0);

float HenyeyGreensteinPhase(float ScatteringAngle, float Anisotropy){
    Anisotropy = clamp(Anisotropy, -0.999, 0.999);
	float AnisotropySq = Anisotropy * Anisotropy;
	float phase = max(1.0 + AnisotropySq - 2.0 * Anisotropy * ScatteringAngle, EPSILON);

    return (1.0 - AnisotropySq) / (4.0 * Math::PI * (phase * sqrt(phase)));
}

float HenyeyGreensteinPhase(float3 RayDirection, float3 LightDirection, float Anisotropy){
    float ScatteringAngle = dot(LightDirection, RayDirection);
    Anisotropy = clamp(Anisotropy, -0.999, 0.999);
	float AnisotropySq = Anisotropy * Anisotropy;
	float phase = max(1.0 + AnisotropySq - 2.0 * Anisotropy * ScatteringAngle, EPSILON);

    return (1.0 - AnisotropySq) / (4.0 * Math::PI * (phase * sqrt(phase)));
}

/*
void GetLocalLights(float3 RayDirection, float Phase)
{
    float Lighting = 0.0;
    uint clusterIndex = 0;
    uint lightCount = 0;
    if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) { //fill
        lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
        uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;

        [loop] for (uint i = 0; i < lightCount; i++){
            uint LightIndex = LightLimitFix::lightList[lightOffset + i];
            LightLimitFix::Light Light = LightLimitFix::lights[LightIndex];
            float3 LightPosition = Light.positionWS[0].xyz - input.WorldPosition.xyz; //fill

            //ISL
            float Attenuation;
            #if defined(ISL)
                Attenuation = InverseSquareLighting::GetAttenuation(length(LightPosition), light);
                if (Attenuation < 1e-5) continue;
            #else
                float intensityFactor = saturate(length(LightPosition) / light.radius);
                if (intensityFactor == 1) continue;
                Attenuation = 1 - intensityFactor * intensityFactor;
            #endif

            //Point Lights
            if (Light.lightFlags & LightLimitFix::LightFlags::Simple) {
                float3 Radiance = Light.color * Attenuation;
                Lighting += Radiance * HenyeyGreensteinPhase(RayDirection, normalize(LightPosition), Phase);
            }

            //Shadow Point Lights   //Needs Matrices
            if (Light.lightFlags & LightLimitFix::LightFlags::Shadow) {
                //SampleLocalShadowMap();
            }

            //Phase
            //float3 IncomingDir = normalize(Direction);
            //float3 OutgoingDir = normalize(RayPosition);
            //float Phase = henyeyGreenstein(dot(IncomingDir, OutgoingDir), phase);

            //Sum
            //float3 LightColor = Light.color.xyz * Intensity;
            //Lighting += LightColor * Phase;
        }
    }
}
*/

[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;

    float3 RayPosition = FroxelWorldDirection(Froxel, 15.7);

    float3 IncomingDir = normalize(SharedData::DirLightDirection.xyz); //Eye to sun
    float3 OutgoingDir = normalize(RayPosition);
    float ScatterCos = dot(IncomingDir, OutgoingDir);

    float Phase = HenyeyGreensteinPhase(ScatterCos, UIAnisotropy);

    float Shadow = ShadowVolume.Load(int4(Froxel, 0)).x;

    float4 Scattering_Extinction = MediaVolume.Load(int4(Froxel, 0));
    float3 MediaScattering = Scattering_Extinction.xyz;
    float MediaExtinction = Scattering_Extinction.w;


    float3 Lighting = float3(0,0,0);
    //float3 Ambient = rcp(Math::PI) / (4.0 * Math::PI);
    //Lighting += Ambient;

    float3 DirLight = SharedData::DirLightColor.xyz * Phase * Shadow;
    Lighting += DirLight;

    Lighting *= MediaScattering;


    ScatteringVolume[ThreadID] = float4(Lighting, MediaExtinction);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////
    //float3 Ambient = Color::GammaToLinear(SharedData::DirectionalAmbient._14_24_34);
    //float3 ambientLight = (1.0 / Math::PI);
    //float Directional_Light_Radiance =
/*
    float TransMult = 50.0;
    float BaseTransmittance = exp(-Scattering_Extinction.w * TransMult);
    float ShadowTransmittance = Shadow * BaseTransmittance;

    float3 RadianceMult = float3(20000, 11100, 3400) * 0.01;
    float RandomMult = 1500.0;
    RandomMult = 1;

    float3 FillAmbient = float3(100, 200, 350) * 0.0;

    float3 MultTermFirst = RandomMult * RadianceMult * ShadowTransmittance;
    float3 MultTermSecond = FillAmbient * ShadowTransmittance;

    //Phase = Phase * MultTermFirst + MultTermSecond;

    //float3 Scattering = Color * Phase * MediaScattering_EScattering_Extinctionxtinction.xyz * Shadow;// * 30; //* Shadow
    //Scattering += ambientLight / (4.0 * Math::PI);

     //float3 Color = lerp(float3(1.0, 1.0, 1.0), SharedData::DirLightColor.xyz, UISaturation); //need to preserve power
*/


//// Slicemarch Compute Shader /////////////////////////////////////////////////////////

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
Texture3D ShadowVolume : register(t3);
//Texture2D EVSMCascade : register(t4);

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

    //Output.xyz = ShadowVolume.SampleLevel(Linear_Sampler, float3(input.TexCoord.xy, FroxelDepth), 0.0).xxx;
    //Output.xy = EVSM.SampleLevel(Linear_Sampler, input.TexCoord.xy, 0.0).xy;
    //Output.z = 0;

    return float4(Output.xyz, 0.0);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Cloud Shadow Map ///////////////////////////////////////////////////////////////////

struct VertexShaderInputV
{
	float4 Position : POSITION0;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR0;
};

struct VertexShaderOutputV
{
	float4 Position : SV_POSITION0;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR0;
};


#ifdef CLOUD_ESM_VETEX

cbuffer PerGeometry : register(b2)
{
	row_major float4x4 WorldViewProj[1] : packoffset(c0);
	row_major float4x4 World[1] : packoffset(c4);
	row_major float4x4 PreviousWorld[1] : packoffset(c8);
	float3 EyePosition[1] : packoffset(c12);
	float VParams : packoffset(c12.w);
	float4 BlendColor[3] : packoffset(c13);
	float2 TexCoordOff : packoffset(c16);
};



VertexShaderOutputV main(VertexShaderInputV input)
{
    VertexShaderOutputV output;
    output.TexCoord = input.TexCoord + TexCoordOff;
    output.Color = float4(1.0, 1.0, 1.0, BlendColor[0].w * input.Color.w);

    float3 WorldPosition = mul(World[0], float4(input.Position.xyz, 1.0));
    output.Position = mul(CloudShadowViewProj, float4(WorldPosition, 1.0));

    return output;
}
#endif

/////////////////////////////////////////////////////////////////////////////////////////

#ifdef CLOUD_ESM_PIXEL

Texture2D CloudTexture : register(t0);
Texture2D TexDepthSampler : register(t17);

float4 main(VertexShaderOutputV input) : SV_Target
{
    float4 Color = CloudTexture.Sample(Point_Sampler, input.TexCoord.xy);
    Color.w = input.Color.w * Color.w;

    Color.xyz = float3(dot(float3(1.0, 1.0, 1.0), Color.xyz) * 0.33, 0.0, 0.0);

    return Color;
}
#endif



/////////////////////////////////////////////////////////////////////////////////////////

#ifdef CLOUD_ESM_COMPUTE

Texture2D CloudShadowMap : register(t0);
RWTexture2D<float4> CloudESM : register(u0);


// Reconstruct world-space ray from UV
void BuildRay(float2 UV, out float3 RayOrigin, out float3 RayDirection)
{
    float2 ndc = float2(UV.x * 2 - 1, 1 - UV.y * 2); // D3D Y flip
    float4 nearH = mul(ViewProjInverse, float4(ndc, 0, 1));
    float4 farH  = mul(ViewProjInverse, float4(ndc, 1, 1));
    float3 Pn = nearH.xyz / nearH.w;
    float3 Pf = farH.xyz  / farH.w;
    RayOrigin    = CameraWS;
    RayDirection = normalize(Pf - Pn);
}


[numthreads(16, 16, 1)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Coords = float3((float2(ThreadID.xy) + 0.5) / EVSMData.xy, ThreadID.z);

    float Output = CloudShadowMap.Load(int4(ThreadID.xyz, 0)).x;


    float3 RayOrigin, RayDir;
    BuildRay(In.UV, RayOrigin, RayDir);

    float RayDist = 0.0;
    float Transmittance = 1.0;

    int MaxSteps = 2
    [loop]for (int StepIndex = 0; StepIndex < MaxSteps && RayDist < MaxDistance; ++StepIndex)
    {
        float3 SamplePositionWS = RayOrigin + RayDir * RayDist;

        float StepLength = GameUnitToMeter(CurrDepth - PrevDepth);

        float Extinction = max(ScatteringSlice.w, EPSILON);
        Transmittance = exp(-Extinction * StepLength);


        RayT += DistanceToSurface;
    }


    CloudESM[ThreadID.xy] = Output;
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
        float Density = (FogMapBlendOpp != -1) ? CurrValue.w + FogMapColor.w : CurrValue.w - FogMapColor.w;
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





