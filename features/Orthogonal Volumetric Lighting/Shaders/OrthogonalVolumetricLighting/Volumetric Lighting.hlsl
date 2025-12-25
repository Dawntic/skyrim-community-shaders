#include "Common/SharedData.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/Color.hlsli"
#include "Common/Game.hlsli"


//https://bartwronski.com/wp-content/uploads/2014/08/bwronski_volumetric_fog_siggraph2014.pdf
//https://doerriest.github.io/publication/master/master.pdf
//https://publications.scss.tcd.ie/theses/diss/2022/TCD-SCSS-DISSERTATION-2022-060.pdf
//https://advances.realtimerendering.com/s2019/slides_public_release.pptx
//https://books.google.com.au/books?hl=en&lr=&id=30ZOCgAAQBAJ&oi=fnd&pg=PA217&dq=gpu+pro+6+volumetric+wronski&ots=2ZfubWDDFI&sig=P611iciYxczkBTD5LDngvBYPN10&redir_esc=y#v=onepage&q=gpu%20pro%206%20volumetric%20wronski&f=false
//https://renderwonk.com/publications/gdc-2002/rolsirt.pdf

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

struct ShadowLightTransform
{
    row_major float4x4 ShadowMatrix;
    uint ShadowMapIndex;
};


cbuffer ShadowBuffer : register(b0)
{
    row_major float4x4 DirectionalShadowCascadeMatrix[4];
    ShadowLightTransform ShadowLightData[4];
    float4 ShadowCascadeEndSplit;
    float4 EVSMData;
};

cbuffer FroxelBuffer : register(b1)
{
    row_major float4x4 CameraView;
    row_major float4x4 CameraProj;
    row_major float4x4 CameraViewInverse;
    row_major float4x4 CameraProjInverse;
    row_major float4x4 PrevCameraViewProj;
    row_major float4x4 CameraViewProjInverse;
    float4 CameraPosition;
    float4 CameraData;
    float4 VolumeSize;
    float4 InverseVolumeSize;
    float4 FrustumNearFar;
    float4 LightDirection;
    float4 FrameParams;
    uint4 LightGridClusterSize;
};

cbuffer GeneralBuffer : register(b2)
{
    row_major float4x4 FogViewProjMatrix;
    float4 HeightMapParams;
    float4 HeightMapZRange;
    float4 NoiseSize;
    float4 FogParam; //near / far, 1.0 / far, power, max
    float4 FogMapSize;
};

cbuffer SettingsBuffer : register(b3)
{
    uint UIEnableVL;
    uint UIUseWeatherFog;
    uint UIUseHistory;
    uint UIEnableLocalLights;

    float4 UIScatteringRatio;

    float UIDirLightMultipler;
    float UIAnisotropy;
    float UISaturation;
    float UIExposure;

    float UILocalLightAnisotropy;
    float UILocalLightMultiplier;
    float UILocalLightsSaturation;

    float UIAmibentLightingMultiplier;
    float UISkyAmbientContribution;
    float UISceneAmbientContribution;

    float UIExtinction;
    float UIGlobalFogFalloff;
    float UIGlobalFogBaseHeight;
    float UIDistantHazeExtinction;

    uint UIEVSMExponent;
    float UIEVSMSearchSize;
    float UIDisocclutionThreshold;

    float UIDistanceFadeIn;

    float FogMapBlendOpp;

    float4 FogMapData;
    float4 UILocalFogMapInput;
};

SamplerState Linear_Sampler : register(s10);
SamplerState Point_Sampler : register(s11);
SamplerState AnisoClampSampler : register(s13);
SamplerState AnisoWrapSampler : register(s14);

#define kPhi 1.61803398875

float2 R2Sequence(uint n) {
    const float g = 1.32471795724474602596; // Plastic constant
    const float a1 = 1.0 / g;
    const float a2 = 1.0 / (g * g);
    return frac(0.5 + float2(a1, a2) * n);
}

//CameraData = Far, Near, Far - Near, Far * Near
//frustumNearFar = nearPlane, farPlane, farPlane / nearPlane, lambda

float GameUnitToMeter(float input){
    return input * 0.01428222656;
}

float LinearStep(float edge0, float edge1, float x){
    return saturate((x - edge0) / (edge1 - edge0));}

float4 LinearStep(float4 edge0, float4 edge1, float4 x){
return saturate((x - edge0) / (edge1 - edge0));}

float NDCDepthToView(float depth){
    return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

float ViewDepthToUV(float ViewDepth){
    return pow(abs(log(ViewDepth / FrustumNearFar.x) / log(FrustumNearFar.z)), 1.0 / FrustumNearFar.w);
}
float UVToViewDepth(float UV){
    return FrustumNearFar.x * pow(abs(FrustumNearFar.z), pow(UV, FrustumNearFar.w));
}

// Higher slice numbers are further in front of the camera
// Higher slices are exp further away from each other
float FroxelDepthToView(float Froxel){
    return FrustumNearFar.x * pow(abs(FrustumNearFar.z), pow(abs(Froxel / VolumeSize.z), FrustumNearFar.w));
}
float ViewDepthToFroxel(float ViewDepth){
    return pow(abs(log(ViewDepth / FrustumNearFar.x) / log(FrustumNearFar.z)), 1.0 / FrustumNearFar.w) * VolumeSize.z;
}
float ViewDepthToLinear(float ViewZ){
    return (ViewZ - FrustumNearFar.x) / (FrustumNearFar.y - FrustumNearFar.x);
}

float3 FroxelWorldDirection(float3 Froxel)
{
    float2 CoordsNDC = Froxel.xy * InverseVolumeSize.xy * 2.0 - 1.0;
    float3 CoordsWS = mul(CameraViewProjInverse, float4(CoordsNDC.x, -CoordsNDC.y, 0.0, 1.0)).xyz;

    return CoordsWS;
}

float3 GetHistoryUV(float3 CoordsWS, out float Confidence)
{
    float4 PrevClip = mul(PrevCameraViewProj, float4(CoordsWS, 1.0));
    float3 PrevNDC = PrevClip.xyz / PrevClip.w;
    float3 PrevUV = float3(PrevNDC.xy * float2(0.5, -0.5) + 0.5, ViewDepthToUV(PrevClip.w));

    Confidence = all(abs(PrevNDC.xy) <= 1.0) && PrevClip.w > 0.0 && PrevUV.z >= 0.0 && PrevUV.z <= 1.0;

    return PrevUV;
}

bool GetClusterIndex(in float2 CoordsUV, in float ViewZ, inout uint clusterIndex){
   // const uint3 clusterSize = SharedData::lightLimitFixSettings.ClusterSize.xyz; //uint3(40,23,32);
   //if (!FrameParams.y) // Fix first person lights ///////////////////////////////////////////
    //uv = 0.5;

    ViewZ = max(ViewZ, CameraData.y);
    uint clusterZ = log(ViewZ / CameraData.y) * LightGridClusterSize.z / log(CameraData.x / CameraData.y);
    uint3 cluster = uint3(uint2(CoordsUV * LightGridClusterSize.xy), clusterZ);

    if (any(cluster >= LightGridClusterSize.xyz))
        return false;

    clusterIndex = cluster.x + (LightGridClusterSize.x * cluster.y) + (LightGridClusterSize.x * LightGridClusterSize.y * cluster.z);
    return true;
}

/*
float GetAnalyticOpticalDepth(float WorldUp, float StartHeight, float EndHeight)
{
    float WorldStartHeight = StartHeight + CameraPosition.z - UIGlobalFogBaseHeight; //Start at previous froxel world height
    float WorldEndHeight = EndHeight + CameraPosition.z - UIGlobalFogBaseHeight; //End at current froxel world height

    float InverseFalloff = rcp(max(UIGlobalFogFalloff, EPSILON_DIVISION));
    float Density = exp(-WorldStartHeight * InverseFalloff) - exp(-WorldEndHeight * InverseFalloff);

    float OpticalDepth = UIGlobalFogDensity * UIExtinction * UIGlobalFogFalloff * Density * rcp(WorldUp);

    return max(OpticalDepth, EPSILON_DIVISION);
}
*/
//WorldUp = (abs(WorldUp) < 1e-4) ? sign(WorldUp) * 0.001 : clamp(WorldUp, -1.0, 1.0);
float GetAnalyticOpticalDepth(float WorldUp, float StartHeight, float StepLength, float InFogBaseHeight, float InFogFalloff, float InExtinction)
{
    float InverseFalloff = max(rcp(InFogFalloff), 1e-8); //CPU

    StartHeight += CameraPosition.z;

     float OpticalDepth;
     if(abs(WorldUp) < 1e-6){
        float DensityAtHeight = exp(-max(StartHeight - InFogBaseHeight, 0) * InverseFalloff);
        OpticalDepth = StepLength * DensityAtHeight * InExtinction;
    } else{
        float EndHeight = StartHeight + StepLength * WorldUp;
        float BaseHeight = min(StartHeight, EndHeight);

        WorldUp = abs(WorldUp);
        float InverseWorldUp = rcp(WorldUp);

        float FogBase = clamp((InFogBaseHeight - BaseHeight) * InverseWorldUp, 0, StepLength);
        float DensityBase = exp(-max(BaseHeight - InFogBaseHeight, 0) * InverseFalloff);
        float DensityDelta = 1.0 - exp(-(StepLength - FogBase) * WorldUp * InverseFalloff);

        float VerticalCorrection = InFogFalloff * InverseWorldUp;
        OpticalDepth = (VerticalCorrection * DensityBase * DensityDelta + FogBase) * InExtinction;
    }

    return max(OpticalDepth, EPSILON_DIVISION);
}

float GetHomogeneousOpticalDepth(float Extinction, float StepLength)
{
    return Extinction * StepLength;
}

float GetWeatherBasedFog(float LinearDepth)
{
    float FogDistFactor = saturate(LinearDepth * FogParam.y - FogParam.x);
    return min(0.99, pow(FogDistFactor, FogParam.z));
}


#ifdef SHADOW_COMPUTE

#include "TerrainShadows/TerrainShadows.hlsli"
#include "CloudShadows/CloudShadows.hlsli"
#include "LightLimitFix/LightLimitFix.hlsli"

RWTexture3D<float> ShadowVolume : register(u0);

Texture3D ShadowHistoryVolume : register(t0);
Texture2DArray BlueNoise : register(t1);
Texture2DArray EVSMCascade : register(t2);
Texture2DArray ParaboloidShadowMaps : register(t3);
StructuredBuffer<LightLimitFix::Light> lights : register(t4);
StructuredBuffer<uint> lightList : register(t5);
StructuredBuffer<LightLimitFix::LightGrid> lightGrid : register(t6);


float EVSM_VisibilityV2(float3 CoordsLS, float2 Moments)
{
    CoordsLS.z = exp(UIEVSMExponent * CoordsLS.z);

    float Bias = 0.001;
	float Variance = Moments.y - (Moments.x * Moments.x);
	      Variance = max(Variance, Bias * CoordsLS.z * CoordsLS.z);

	float Delta = CoordsLS.z - Moments.x;
	float pMax = Variance / (Variance + (Delta * Delta));

	return (CoordsLS.z <= Moments.x) ? 1.0 : pMax;
}

float EVSM_Visibility(float3 CoordsLS, float2 Moments)
{
    float MinVariance = 0.0;

    float Depth = exp(UIEVSMExponent * CoordsLS.z);
    float Delta = Depth - Moments.x;

    float Variance = max(Moments.y - Moments.x * Moments.x, MinVariance);
    float Visibility = Variance / (Variance + Delta * Delta);

    return (Depth <= Moments.x) ? 1.0 : Visibility;
}

float GetCascadeShadow(float3 RayDirection, float ViewZ, float CoordZ, float ThicknessZ, float BNoise)
{
    int Samples = 8;

    float Result = 0;
    for(int i=0; i<Samples; i++){
        float3 RaySampleCoords = RayDirection * ViewZ;

        uint CascadeIndex = (ViewZ < ShadowCascadeEndSplit.x) ? 0 : 1;
        if(ViewZ > ShadowCascadeEndSplit.y)
            return 1.0;
        float3 CoordsLS = mul(DirectionalShadowCascadeMatrix[CascadeIndex], float4(RaySampleCoords, 1.0)).xyz;
        float2 Moments = EVSMCascade.SampleLevel(Linear_Sampler, float3(CoordsLS.xy, CascadeIndex), 0).xy;
        float Visibility = EVSM_Visibility(CoordsLS, Moments);

        Result += Visibility;

        float Value = SharedData::FrameCountAlwaysActive % 16;//((SharedData::FrameCountAlwaysActive + 17) % 33);
        float ViewZNoise = frac(BNoise + ((Value + i + 1)) * kPhi);

        ViewZ = CoordZ + ThicknessZ * ViewZNoise;
    }
    Result /= Samples;

    return Result;
}

float GetLocalLightShadow(float3 WorldPosition, float ViewZ, float2 CoordsUV)
{
    float Visibility = 1.0;
    uint clusterIdx = 0;
    uint lightCount = 0;

    if (GetClusterIndex(CoordsUV, ViewZ, clusterIdx) && UIEnableLocalLights){
        uint lightCount = lightGrid[clusterIdx].lightCount;
        uint lightOffset = lightGrid[clusterIdx].offset;

        [loop] for(uint i = 0; i < lightCount; i++){
            uint Index = lightList[lightOffset + i];
            LightLimitFix::Light light = lights[Index];

            if (light.lightFlags & LightLimitFix::LightFlags::Shadow) {
                float4 CoordsLS = mul(ShadowLightData[light.shadowLightIndex].ShadowMatrix, float4(WorldPosition, 1.0));

                bool lowerHalf = CoordsLS.z < 0;
                float3 PosOffset = float3(0, 0, 1.0 - 2 * lowerHalf);
                float3 lightDirection = normalize(normalize(CoordsLS.xyz) + PosOffset);
                float2 ShadowUV = lightDirection.xy / lightDirection.z * 0.5 + 0.5;
                ShadowUV.y = lowerHalf ? 1 - 0.5 * ShadowUV.y : 0.5 * ShadowUV.y;

                float Shadow = ParaboloidShadowMaps.SampleLevel(Linear_Sampler, float3(ShadowUV.xy, ShadowLightData[light.shadowLightIndex].ShadowMapIndex), 0).x;

                float shadowMapCompareValue = saturate(length(CoordsLS.xyz) / light.radius); //- 0.00638;
                if (Shadow <= shadowMapCompareValue)
                    Visibility = min(Visibility, Shadow);
            }
        }
    }
    return Visibility;
}

#define MaxHistory 0.85

[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;
    float2 CoordsUV = (Froxel.xy + 0.5) * InverseVolumeSize.xy;

    float CoordZ = FroxelDepthToView(Froxel.z - 2); // Bias to avoid leaks
    float ThicknessZ = FroxelDepthToView(Froxel.z - 1) - CoordZ;

    float Noise = BlueNoise.Load(int4(ThreadID.xy & 63, 0, 0)).x;
    float RayJitter = frac(Noise + (SharedData::FrameCountAlwaysActive % 16) * kPhi);

    float ViewZ = CoordZ + ThicknessZ * RayJitter;
    float3 RayDirection = FroxelWorldDirection(Froxel);
    float3 RayPosition = RayDirection * ViewZ;
    float3 WorldPosition = RayPosition + CameraPosition.xyz;

    float CascadeShadow = GetCascadeShadow(RayDirection, ViewZ, CoordZ, ThicknessZ, Noise);

    float LocalShadow = GetLocalLightShadow(RayPosition, ViewZ, CoordsUV);

    float UICloudShadowContrib = 1.0;
    float CloudShadow = CloudShadows::GetCloudShadowMult(WorldPosition, Linear_Sampler) * UICloudShadowContrib;
    float TerrainShadow = TerrainShadows::GetTerrainShadow(WorldPosition, Linear_Sampler);

    float Shadow = LocalShadow * CascadeShadow; //min(LocalShadow, CascadeShadow);// * TerrainShadow;// * CloudShadow;

    float Confidence;
    float ViewZCenter = FroxelDepthToView(Froxel.z + 0.5);
    float3 PrevCoordsUV = GetHistoryUV(RayDirection * ViewZCenter, Confidence);
    //float3 PrevCoordsUV = GetHistoryUV(RayDirection * (ViewZCenter + ThicknessZ * RayJitter), Confidence);
    float ShadowHistory = ShadowHistoryVolume.SampleLevel(Linear_Sampler, PrevCoordsUV, 0).x;

    float DeltaLimit = max(UIDisocclutionThreshold, EPSILON_DIVISION); //Shadow diff below which max history will be used
    float ReprojectionValue = 1.0 - LinearStep(DeltaLimit, 1.0, abs(Shadow - ShadowHistory));
          ReprojectionValue = Confidence * min(ReprojectionValue, MaxHistory) * UIUseHistory;

    //Shadow = lerp(Shadow, ShadowHistory, ReprojectionValue);
    Shadow = Shadow * 0.15;
    //Shadow = min(Shadow, 0.15);

    ShadowVolume[ThreadID] = Shadow;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////

    //float3 BoxCoords = WorldPosition - float3(500, -600, -5500);
    //float3 Box = abs(BoxCoords) - float3(1000,20,100);
    //float BoxSDF = length(max(Box, 0.0)) + min(max(Box.x, max(Box.y, Box.z)), 0.0);
    //Shadow = 1.0 - smoothstep(0.0, 0.01, saturate(BoxSDF));

//// Create EVSM ////////////////////////////////////////////////////////////////////////

#ifdef EVSM_COMPUTE

Texture2DArray CSM : register(t0);
RWTexture2DArray<float4> EVSM : register(u0);

float sum4(float4 value){ return value.x + value.y + value.z + value.w; }

static const int2 Offsets[4] = { int2(-1,-1), int2( 1,-1), int2(-1, 1), int2( 1, 1) };


[numthreads(16, 16, 1)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float2 Coords = (ThreadID.xy + 0.5) * rcp(EVSMData.xy);

    float2 Result = 0.0;
    for(int i=0; i<4; i++){
        float4 Sample = CSM.GatherRed(Linear_Sampler, float3(Coords, ThreadID.z), Offsets[i]);
        float4 ExpValue = exp(UIEVSMExponent * Sample);
        Result += float2(sum4(ExpValue), sum4(ExpValue * ExpValue));
    }
    Result /= 16;

    EVSM[ThreadID.xyz] = Result.xyxy;
}
#endif

/////////////////////////////////////////////////////////////////////////////////////////

#ifdef EVSMBLUR_COMPUTE

Texture2DArray EVSM : register(t0);
RWTexture2DArray<float4> BlurOutput : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float2 Coords = (ThreadID.xy + 0.5) / EVSMData.xy;

    float2 Result = 1e+10;
    int SearchRadius = UIEVSMSearchSize;
    [loop] for (int dy = -SearchRadius; dy <= SearchRadius; ++dy){
        [loop] for (int dx = -SearchRadius; dx <= SearchRadius; ++dx){
            int2 SampleCoords = clamp(int2(ThreadID.xy) + int2(dx, dy), int2(0, 0), int2(EVSMData.xy) - 1);
            float4 Sample = EVSM.Load(int4(SampleCoords, ThreadID.z, 0));

            Result = (Result.x < Sample.x) ? Result.xy : Sample.xy;
        }
    }
    //Result.xy = EVSM.SampleLevel(Linear_Sampler, float3(Coords, ThreadID.z), 0).xy;

    BlurOutput[ThreadID.xyz] = Result.xyxy;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Media Volume ///////////////////////////////////////////////////////////////////////

#ifdef MEDIA_COMPUTE

Texture3D HistoryVolume : register(t0);
Texture3D Perlin : register(t1);
Texture2DArray BlueNoise : register(t2);
Texture2D FogMap : register(t3);
RWTexture3D<float4> MediaVolume : register(u0);
//RWTexture2D<float4> FogMap : register(u1);


float4 GetLocalFogData(float3 CoordsWS, float RayJitter){
    float MapCameraDepth = 249920.0;
    float4 MapCoordsNDC = mul(FogViewProjMatrix, float4(CoordsWS.xy, CoordsWS.z - MapCameraDepth, 1.0));
    float2 MapCoordsUV = (MapCoordsNDC.xy / MapCoordsNDC.w) * float2(0.5, -0.5) + 0.5;

    float2 FogMapSize = float2(2560, 1440);// - (RayJitter*2);
    return FogMap.Load(int3(MapCoordsUV * FogMapSize.xy, 0)); //returns fog height, falloff and extinction
}


[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;

    float ViewZ = FroxelDepthToView(Froxel.z);
    float ThicknessZ = FroxelDepthToView(Froxel.z + 1.0) - ViewZ;

    float RayJitter = BlueNoise.Load(int4(ThreadID.xy & 63, 0, 0)).x;
          RayJitter = frac(RayJitter + (SharedData::FrameCountAlwaysActive % 16) * kPhi);

    ViewZ += ThicknessZ * RayJitter;
    float3 RayDirection = FroxelWorldDirection(Froxel);
    float3 RayPosition = RayDirection * ViewZ;

    float PrevViewZ = FroxelDepthToView(max(Froxel.z - 1.0, 1.0)); // + RayJitter;
    float3 PrevRayPosition = RayDirection * PrevViewZ;
    float StepLength = max(distance(PrevRayPosition, RayPosition), 1.0);

    float OpticalDepth = 0.0;
    //OpticalDepth = GetAnalyticOpticalDepth(RayDirection.z, PrevRayPosition.z, StepLength, UIGlobalFogBaseHeight, UIGlobalFogFalloff, UIExtinction);


    //float CoordsZ = Froxel.z * InverseVolumeSize.z;
    //CoordsZ += (RayJitter * 6.0 - 0.0) * InverseVolumeSize.z;
    //CoordsZ *= VolumeSize.z;

    //float ViewZ_2 = FroxelDepthToView(CoordsZ);
    //float UVZ = ViewZ_2 * InverseVolumeSize.z;
   //UVZ += InverseVolumeSize.z * (RayJitter);
    // CoordsUV + InverseVolumeSize.xy * Jitter
    //ViewZ_2 = UVZ * VolumeSize
    //float3 PosTest = RayDirection * ViewZ_2 + CameraPosition.xyz;

    //float4 LocalFogData = 0;
   // for(int j=-1; j<=1;j++){
   //     ViewZ = FroxelDepthToView(Froxel.z + 0.5 * j);
   //     RayPosition = RayDirection * ViewZ;
   //     LocalFogData += GetLocalFogData(RayPosition + CameraPosition.xyz, 0.0);
   // }
   // LocalFogData *= 0.33;

    float4 LocalFogData = GetLocalFogData(RayPosition + CameraPosition.xyz, RayJitter);
    OpticalDepth += GetAnalyticOpticalDepth(RayDirection.z, PrevRayPosition.z, StepLength, LocalFogData.y, LocalFogData.z, LocalFogData.w);


    float WeatherFog = GetWeatherBasedFog(ViewZ) * 0.002;
    float HomogeneousOpticalDepth = GetHomogeneousOpticalDepth(WeatherFog, StepLength);
    OpticalDepth = lerp(OpticalDepth, HomogeneousOpticalDepth, FogParam.w * UIUseWeatherFog);

    float Extinction = OpticalDepth * rcp(StepLength); //max(OpticalDepth * rcp(max(StepLength, EPSILON_DIVISION)), EPSILON_DIVISION);
    float3 ScatteringAlbedo = UIScatteringRatio.xyz * Extinction;

    float4 Output = float4(ScatteringAlbedo, OpticalDepth);


    //float DensityAtFroxel = 1.0;
    //float MediaExtinction = UIExtinction * DensityAtFroxel;
    //float3 MediaScattering = UIScatteringRatio.xyz * MediaExtinction;
    //float3 MediaScattering = UIScatteringRatio.xyz * DensityAtFroxel;
    //Output = float4(MediaScattering, MediaExtinction);


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

    float Confidence;
    float CenterViewZ = FroxelDepthToView(Froxel.z);
     float ThicknessZ2 = FroxelDepthToView(Froxel.z + 1.0) - CenterViewZ;
     //CenterViewZ += ThicknessZ2 * RayJitter;

    float3 PrevCoordsUV = GetHistoryUV(RayDirection * CenterViewZ, Confidence);

    float4 MediaHistory = HistoryVolume.SampleLevel(Linear_Sampler, PrevCoordsUV, 0);
    float BaseValue = 0.90;
    //float ReprojectionValue = BaseValue;// * Confidence;

    float DeltaLimit = max(UIDisocclutionThreshold, EPSILON_DIVISION); //Shadow diff below which max history will be used
    float ReprojectionValue = 1.0 - LinearStep(DeltaLimit.xxxx, float4(1,1,1,1), abs(Output - MediaHistory));
          ReprojectionValue = Confidence * min(ReprojectionValue, BaseValue) * UIUseHistory;

    ReprojectionValue = 0.60;
    //Output = lerp(Output, MediaHistory, ReprojectionValue);


    MediaVolume[ThreadID] = Output;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Scattering Compute Shader //////////////////////////////////////////////////////////

#ifdef SCATTER_COMPUTE

#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "Skylighting/Skylighting.hlsli"
#include "LightLimitFix/LightLimitFix.hlsli"
#include "InverseSquareLighting/InverseSquareLighting.hlsli"
#include "IBL/IBL.hlsli"

Texture3D ShadowVolume : register(t0);
Texture3D MediaVolume : register(t1);
Texture2DArray BlueNoise : register(t2);
Texture3D<sh2> SkylightingProbeArray : register(t3);
Texture2D<sh2> DiffuseIBLTexture : register(t76);
Texture2D<sh2> DiffuseSkyIBLTexture : register(t77);
StructuredBuffer<LightLimitFix::Light> lights : register(t4);
StructuredBuffer<uint> lightList : register(t5);
StructuredBuffer<LightLimitFix::LightGrid> lightGrid : register(t6);
Texture3D<float4> ParaboloidShadowMaps : register(t7);

RWTexture3D<float4> ScatteringVolume : register(u0);

// strength * polarization * normalizationFactor / angularDistributionLobe
float CSPhase(float ScatterCos, float Anisotropy)
{
    float AnisotropySquared = Anisotropy * Anisotropy;
    float Polarization = 1.0 + ScatterCos * ScatterCos;
    float Normalization = 3.0 * rcp(2.0 * (2.0 + AnisotropySquared));
    float ADLobe = 1.0 + AnisotropySquared - 2.0 * Anisotropy * ScatterCos;

    float Phase = (1.0 - AnisotropySquared) * Polarization * Normalization * rcp(ADLobe * sqrt(ADLobe));

    return Phase;
}

float HGPhase(float cosTheta, float anisotropy){
    anisotropy = clamp(anisotropy, -0.999f, 0.999f);
	float anisotropy2 = anisotropy * anisotropy;
	float phase = max(1.0 + anisotropy2 - 2.0 * anisotropy * cosTheta, 1e-6);
    phase *= sqrt(phase);
    return (1.0 - anisotropy2) / (4.0 * Math::PI * phase);
}

float3 ClampLuminance(float3 Color, float Max){
    float Luminance = Color::RGBToLuminance(Color);
    if (Luminance > Max)
        return Color * (Max / Luminance);

    return Color;
}

float3 GetLocalLighting(float3 WorldPosition, float2 CoordsUV, float ViewZ, float3 RayToEye, float ShadowA)
{
    float3 Lighting = float3(0,0,0);
    uint clusterIdx = 0;

    if (GetClusterIndex(CoordsUV, ViewZ, clusterIdx)){
        uint lightOffset = lightGrid[clusterIdx].offset;

        [loop] for(uint i = 0; i < lightGrid[clusterIdx].lightCount; i++){
            LightLimitFix::Light light = lights[lightList[lightOffset + i]];
            float3 LightPosition = WorldPosition.xyz - light.positionWS[0].xyz;

            float Attenuation;
            Attenuation = InverseSquareLighting::GetAttenuation(length(LightPosition), light);
            if (Attenuation < 1e-5) continue;

            float3 Radiance = Color::Saturation(ClampLuminance(Color::GammaToTrueLinear(light.color.xyz), 10.0), UILocalLightsSaturation) * Attenuation;

            // local shadows should not attenuate dir light
            // local shadows should not attenuate other local light sources
            float Visibility = 1.0;
            if (light.lightFlags & LightLimitFix::LightFlags::Shadow) {
                Radiance *= ShadowA;

                float4 CoordsLS = mul(ShadowLightData[light.shadowLightIndex].ShadowMatrix, float4(WorldPosition, 1.0));

                bool lowerHalf = CoordsLS.z < 0;
                float3 PosOffset = float3(0, 0, 1.0 - 2 * lowerHalf);
                float3 lightDirection = normalize(normalize(CoordsLS.xyz) + PosOffset);
                float2 ShadowUV = lightDirection.xy / lightDirection.z * 0.5 + 0.5;
                ShadowUV.y = lowerHalf ? 1 - 0.5 * ShadowUV.y : 0.5 * ShadowUV.y;

                float Shadow = ParaboloidShadowMaps.SampleLevel(Linear_Sampler, float3(ShadowUV.xy, ShadowLightData[light.shadowLightIndex].ShadowMapIndex), 0).x;

                float shadowMapCompareValue = saturate(length(CoordsLS.xyz) / light.radius); //- 0.00638;
                if (Shadow <= shadowMapCompareValue)
                    Visibility = 0.0;
            }
            //else{Radiance *= 0.0;}

            //Radiance *= Visibility;

            float3 LightToRay = normalize(LightPosition);
            float ScatterCos = dot(LightToRay, RayToEye);
            Lighting += Radiance * CSPhase(ScatterCos, UILocalLightAnisotropy);
        }
    }
    return Lighting;
}

float3 GetSceneVolumetricDiffuse(float3 LightToSurface){
    float3 DALC = float3(
        SphericalHarmonics::Unproject(DiffuseIBLTexture.Load(int3(0, 0, 0)), LightToSurface),
        SphericalHarmonics::Unproject(DiffuseIBLTexture.Load(int3(1, 0, 0)), LightToSurface),
        SphericalHarmonics::Unproject(DiffuseIBLTexture.Load(int3(2, 0, 0)), LightToSurface));
    return max(0.0, DALC);
}
float3 GetSkyVolumetricDiffuse(float3 LightToSurface){
    float3 Sky_Ambient = float3(
        SphericalHarmonics::Unproject(DiffuseSkyIBLTexture.Load(int3(0, 0, 0)), LightToSurface),
        SphericalHarmonics::Unproject(DiffuseSkyIBLTexture.Load(int3(1, 0, 0)), LightToSurface),
        SphericalHarmonics::Unproject(DiffuseSkyIBLTexture.Load(int3(2, 0, 0)), LightToSurface));
    return max(0.0, Sky_Ambient);
}

float3 GetAmbientLighting(float3 WorldPosition, float3 DirLightToEye, float3 RayToEye)
{
    sh2 SkyLightVisibility = Skylighting::sampleNoBias(SharedData::skylightingSettings, SkylightingProbeArray, WorldPosition); // Note the use of Skylightings settings

    float SkyDiffuse = SphericalHarmonics::FuncProductIntegral(SkyLightVisibility, float4(0.282095, 0, 0, 0)); // Omnidir average
          SkyDiffuse = lerp(1.0, saturate(SkyDiffuse), Skylighting::getFadeOutFactor(WorldPosition));

    float3 AmbientLight;

    //Quick Amibent term
    //float3 SceneAmbientA = GetSceneVolumetricDiffuse(RayToEye) * UISceneAmbientContribution;
    float3 SkyAmbientA = GetSkyVolumetricDiffuse(float3(0, 0, -1)) * UISkyAmbientContribution; // Should weight this dir by phase func or just do light dir maybe? but then its not ambient anymore or is it?

    float3 AmbientColor = SkyAmbientA;// + SceneAmbientA;
    float4 RotatedZonalSH = float4(1.0, RayToEye) * float4(1.0, UIAnisotropy, UIAnisotropy, UIAnisotropy); //RayToEye
    float4 SkyIrradiance = float4(0.282095 * AmbientColor.x, 0, 0, 0);
    float4 SkyIrradiance1 = float4(0.282095 * AmbientColor.y, 0, 0, 0);
    float4 SkyIrradiance2 = float4(0.282095 * AmbientColor.z, 0, 0, 0);
    float AmbientR = dot(SkyIrradiance, RotatedZonalSH);
    float AmbientG = dot(SkyIrradiance1, RotatedZonalSH);
    float AmbientB = dot(SkyIrradiance2, RotatedZonalSH);

    AmbientLight = float3(AmbientR, AmbientG, AmbientB);


    sh2 PhaseZonalSH = SphericalHarmonics::EvaluatePhaseHG(RayToEye, UIAnisotropy); //PhaseZonalSH = float4(0.282095, 0, 0, 0);

    float3 SkyAmbient;
    sh2 SkyIrradianceR = DiffuseSkyIBLTexture.Load(int3(0, 0, 0)); // Red channel SH
    sh2 SkyIrradianceG = DiffuseSkyIBLTexture.Load(int3(1, 0, 0)); // Green channel SH
    sh2 SkyIrradianceB = DiffuseSkyIBLTexture.Load(int3(2, 0, 0)); // Blue channel SH
    SkyAmbient.x = SphericalHarmonics::FuncProductIntegral(SkyIrradianceR, PhaseZonalSH);
    SkyAmbient.y = SphericalHarmonics::FuncProductIntegral(SkyIrradianceG, PhaseZonalSH);
    SkyAmbient.z = SphericalHarmonics::FuncProductIntegral(SkyIrradianceB, PhaseZonalSH);

    float3 SceneAmbient; //This method stops ambient jumping
    sh2 SceneIrradianceR = DiffuseIBLTexture.Load(int3(0, 0, 0)); // Red channel SH
    sh2 SceneIrradianceG = DiffuseIBLTexture.Load(int3(1, 0, 0)); // Green channel SH
    sh2 SceneIrradianceB = DiffuseIBLTexture.Load(int3(2, 0, 0)); // Blue channel SH

    SceneAmbient.x = SphericalHarmonics::FuncProductIntegral(SceneIrradianceR, PhaseZonalSH);
    SceneAmbient.y = SphericalHarmonics::FuncProductIntegral(SceneIrradianceG, PhaseZonalSH);
    SceneAmbient.z = SphericalHarmonics::FuncProductIntegral(SceneIrradianceB, PhaseZonalSH);

    //AmbientLight = SkyAmbient * UISkyAmbientContribution; // * 0.25 + SceneAmbient * UISceneAmbientContribution * 0.25;

    AmbientLight += SceneAmbient * UISceneAmbientContribution; // * 0.25; // * SkyDiffuse;

    AmbientLight *= SkyDiffuse;


    return AmbientLight;
}


[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;
    float2 CoordsUV = Froxel.xy * InverseVolumeSize.xy;

    float RayJitter = BlueNoise.Load(int4(ThreadID.xy & 63, 0, 0)).x;
          RayJitter = frac(RayJitter + (SharedData::FrameCountAlwaysActive % 16) * kPhi);

    float ViewZ = FroxelDepthToView(Froxel.z + 0.5);
    float ThicknessZ = FroxelDepthToView(Froxel.z + 1.5) - ViewZ;

    ViewZ += ThicknessZ * RayJitter;
    float3 RayDirection = FroxelWorldDirection(Froxel);
    float3 RayPosition = RayDirection * ViewZ;

    float Shadow = ShadowVolume.Load(int4(Froxel, 0)).x;

    float4 Scattering_Extinction = MediaVolume.Load(int4(Froxel, 0));
    float3 MediaScattering = Scattering_Extinction.xyz;
    float OpticalDepth = Scattering_Extinction.w;

    float3 RayToEye = -normalize(RayDirection);
    float3 DirLightToEye = LightDirection.xyz;

    //float Extinction = MediaScattering.x / UIScatteringRatio.x;
    //float Tranmittance = exp(-OpticalDepth);

    float3 JitteredWorldPos = RayPosition;//RayDirection * (ViewZ + RayJitter * (Skylighting::CELL_SIZE * 0.5));

    float3 Lighting = float3(0,0,0);
    Lighting += GetAmbientLighting(JitteredWorldPos, DirLightToEye, RayToEye) * UIAmibentLightingMultiplier;
    //Lighting = 0.5;

    float DirLightCosTheta = dot(DirLightToEye, RayToEye);
    float Phase = CSPhase(DirLightCosTheta, UIAnisotropy);
    //float Phase = HGPhase(DirLightCosTheta, UIAnisotropy) * UILocalLightAnisotropy;
    //Phase += HGPhase(DirLightCosTheta, UIAnisotropy * (2.0 / 3.0)) * UILocalLightsSaturation;

    //float3 DirLightRadiance = Color::Saturation(SharedData::DirLightColor.xyz + 20, UISaturation+20) * Shadow * UIDirLightMultipler;
    float3 DirLightRadiance = Color::Saturation(Color::GammaToTrueLinear(SharedData::DirLightColor.xyz), UISaturation);
           DirLightRadiance = DirLightRadiance * Shadow * Phase * UIDirLightMultipler;
    Lighting += DirLightRadiance;

    float3 LocalLight = 0;
    for(int j=-1; j<=1;j++){ // Reduce light noise (for full coverage use +-2 and 0.25)
        ViewZ = FroxelDepthToView(Froxel.z + 0.5 * j);
        RayPosition = RayDirection * ViewZ;
        LocalLight += GetLocalLighting(RayPosition, CoordsUV, ViewZ, RayToEye, Shadow) * UIEnableLocalLights * UILocalLightMultiplier;
    }
    Lighting += LocalLight * 0.33;
    //Lighting += GetLocalLighting(RayPosition, CoordsUV, ViewZ, RayToEye, Shadow) * UIEnableLocalLights * UILocalLightMultiplier;

    //MediaScattering = 0.0063;
    //OpticalDepth = 0.002;

    Lighting = Lighting * MediaScattering;

    //if(UIUseHistory)
    //    Lighting = Shadow.xxx;

    ScatteringVolume[ThreadID] = float4(Lighting, OpticalDepth);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Slicemarch Compute Shader /////////////////////////////////////////////////////////

#ifdef MARCH_COMPUTE

Texture3D ScatterVolume : register(t0);
RWTexture3D<float4> IntergrationVolume : register(u0);


void AccumulateScattering(inout float4 Accumulation, float4 ScatteringSample, float OpticalDepth, float StepLength, float FadeIn)
{
    float Transmittance = exp(-OpticalDepth);
    float Extinction = max(OpticalDepth * rcp(max(EPSILON_DIVISION, StepLength)), EPSILON_DIVISION);

    float3 InScatteredLight = FadeIn * ScatteringSample.xyz * (1.0 - Transmittance) * rcp(Extinction) * Accumulation.w;

    Accumulation.xyz += InScatteredLight;
    Accumulation.w *= Transmittance;
}

[numthreads(8, 8, 1)]
void main(uint3 Froxel : SV_DispatchThreadID)
{
    float4 Accumulation = float4(0.0, 0.0, 0.0, 1.0);
    float3 PrevWorldPosition = FroxelWorldDirection(Froxel) * FrustumNearFar.x;

    //float Depth = 0;
    for(int Slice=0; Slice < VolumeSize.z; Slice++){
        float4 ScatteringSample = ScatterVolume.Load(int4(Froxel.xy, Slice, 0));
        float OpticalDepth = ScatteringSample.w;

        float ViewZ = FroxelDepthToView(Slice + 1.0);
        float3 WorldDirection = FroxelWorldDirection(Froxel);
        float3 WorldPosition = WorldDirection * ViewZ;
        float StepLength = max(distance(PrevWorldPosition, WorldPosition), 1.0);

		float FadeIn = saturate(ViewZ * rcp(UIDistanceFadeIn)); // CPU

        AccumulateScattering(Accumulation, ScatteringSample, OpticalDepth, StepLength, FadeIn);
        PrevWorldPosition = WorldPosition;

        //IntergrationVolume[uint3(Froxel.xy, Slice)] = float4(Accumulation.xyz * rcp(max(1.0 - Accumulation.w, EPSILON_DIVISION)), Accumulation.w); // Encode output as normalized radiance for density anti aliasing
        IntergrationVolume[uint3(Froxel.xy, Slice)] = Accumulation;
    }
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Apply //////////////////////////////////////////////////////////////////////////////

//Density Anti Aliasing from: https://advances.realtimerendering.com/s2021/jpatry_advances2021.pdf

// Tri cubic B spline: https://developer.nvidia.com/gpugems/gpugems2/part-iii-high-quality-rendering/chapter-20-fast-third-order-texture-filtering
// Quadratic polynomial approximation: https://advances.realtimerendering.com/s2021/jpatry_advances2021.pdf
// Function Graph: https://www.desmos.com/calculator/udlencsh7k

//#include "Common/DisplayMapping.hlsli"
//float2 Jitter = frac(Noise + ((SharedData::FrameCountAlwaysActive + 17) % 33) * kPhi) * 2.0 - 1.0;

#ifdef APPLY_PIXEL

 #include "TerrainShadows/TerrainShadows.hlsli"

Texture2D DepthTex : register(t0);
Texture3D IntergrationVolume : register(t1);
Texture2DArray BlueNoise : register(t2);
Texture3D ShadowVolume : register(t3);
Texture2D MainScene : register(t4);

// from ShortFuse (RenoDX)
float ApplyCurve(float x, float a, float b, float c, float d, float e, float f){
	return ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - e / f;}
float3 ApplyCurve(float3 x, float a, float b, float c, float d, float e, float f){
	return ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - e / f;}

static const float A = 0.22;  // Shoulder Strength  .22
static const float B = 0.30;  // Linear Strength
static const float C = 0.10;  // Linear Angle
static const float D = 0.20;  // Toe Strength  .20
static const float E = 0.01;  // Toe Numerator
static const float F = 0.30;  // Toe Denominator
static const float W = 11.2;  // Linear White

float3 Uncharted2Tonemap(float3 untonemapped, float linear_white = W){
	return ApplyCurve(untonemapped * 2.f, A, B, C, D, E, F) / ApplyCurve(linear_white, A, B, C, D, E, F);
}

float2 GetJitteredUVCoords(float2 CoordsSS, float2 CoordsUV){
    float2 Noise = BlueNoise.Load(int4(int2(CoordsSS.xy) & 63, 0, 0)).xx;
           Noise.y = BlueNoise.Load(int4(int2(CoordsSS.yx) & 63, 0, 0)).x;
    float2 Jitter = frac(Noise + (SharedData::FrameCountAlwaysActive % 16) * kPhi) * 4.0 - 2.0;

    return CoordsUV + InverseVolumeSize.xy * Jitter;
}

float3 Uncharted2TonemapA(float3 x)
{
    // http://www.gdcvault.com/play/1012459/Uncharted_2__HDR_Lighting
    // http://filmicgames.com/archives/75 - the coefficients are from here
    float A = 0.15; // Shoulder Strength
    float B = 0.50; // Linear Strength
    float C = 0.10; // Linear Angle
    float D = 0.20; // Toe Strength
    float E = 0.02; // Toe Numerator
    float F = 0.30; // Toe Denominator
    return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F; // E/F = Toe Angle
}


float4 main(VertexShaderOutput input) : SV_Target
{
    float Depth = DepthTex.Sample(Point_Sampler, input.TexCoord.xy).x;
    float PixelViewZ = NDCDepthToView(Depth);
    float NearPlaneBias = 10.0;
    float FroxelDepth = saturate(ViewDepthToUV(max(FrustumNearFar.x + NearPlaneBias, PixelViewZ)));

    float4 Scene = MainScene.SampleLevel(Point_Sampler, input.TexCoord.xy, 0);

   float2 CoordsUV = GetJitteredUVCoords(input.Position.xy, input.TexCoord.xy);

    float4 NormalizedRadiance = IntergrationVolume.SampleLevel(Linear_Sampler, float3(CoordsUV, FroxelDepth), 0);
    float Transmittance = NormalizedRadiance.w;

    float2 CoordsNDC = input.TexCoord.xy * 2.0 - 1.0;
    float3 PixelDirectionWS = mul(CameraViewProjInverse, float4(CoordsNDC.x, -CoordsNDC.y, 0.0, 1.0)).xyz;

    //Given our analyical optical depth function, is there any way to account for multiple medias(different heights and extinctions) without recalculating another optical depth

    float OpticalDepth = GetAnalyticOpticalDepth(PixelDirectionWS.z, 0.0, PixelViewZ, UIGlobalFogBaseHeight, UIGlobalFogFalloff, UIExtinction);
    if(Depth < 0.999999) // maybe use transmittance and multiply with ^^
        OpticalDepth = max(OpticalDepth, GetHomogeneousOpticalDepth(UIDistantHazeExtinction, PixelViewZ)); // Dont apply this to... and scale by total radiance(if radiance sample is too high then this over exposes)

    // DAA
    //Transmittance = exp(-OpticalDepth);
    //NormalizedRadiance.xyz = NormalizedRadiance.xyz * (1.0 - Transmittance); // * TerrainShadow;

    NormalizedRadiance.xyz = Uncharted2Tonemap(NormalizedRadiance.xyz);

    float3 OutputColor = Color::GammaToTrueLinear(Scene.xyz);
           OutputColor = Uncharted2Tonemap(OutputColor);
    if(UIEnableVL){
        OutputColor = OutputColor * Transmittance + NormalizedRadiance.xyz;
    }
    OutputColor = Color::TrueLinearToGamma(OutputColor);

    return float4(OutputColor, 1.0);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////

//float OpticalDepth = GetAnalyticOpticalDepth_2(float3(0,0,0), PixelDirectionWS * PixelViewZ, UIGlobalFogFalloff);
//float3 WorldPosition = PixelDirectionWS * PixelViewZ + CameraPosition.xyz;
//float TerrainShadow = TerrainShadows::GetTerrainShadow(WorldPosition, Linear_Sampler);


//// Fog Map ////////////////////////////////////////////////////////////////////////////

#ifdef DRAW_FOGMAP

RWTexture2D<float4> UIFogMap : register(u0);
RWTexture2D<float4> FogMap : register(u1);
Texture2D WorldMap : register(t0);
Texture2D HeightMap : register(t1);



float3 GetMapSSFromWorldPos(float3 CoordsWS){
    float MapCameraDepth = 249920.0;

    row_major float4x4 MapViewProj = float4x4(
    float4(1.19175, 1.01186E-07, -0.00029, 0.00),
    float4(0.00, 2.11867, 0.00065, 0.00),
    float4(0.00, 0.00035, -1.00036, -128.04633),
    float4(0.00, 0.00035, -1.00, 0.00));

    row_major float4x4 MapViewProjTest = float4x4(
    float4( 1.19175, 0.00, 0.00, 0.00),
    float4(0.00, 2.11867, 0.00073, 0.00),
    float4(0.00, 0.00035, -1.00036, -128.04633),
    float4(0.00, 0.00035, -1.00, 0.00));

    float4 MapCoordsNDC = mul(MapViewProjTest, float4(CoordsWS.xy, CoordsWS.z - MapCameraDepth, 1.0));
    float2 MapCoordsUV = (MapCoordsNDC.xy / MapCoordsNDC.w) * float2(0.5, -0.5) + 0.5;

    return float3(MapCoordsUV * float2(2560.0, 1440.0), MapCoordsNDC.z / MapCoordsNDC.w);
}

float3 GetWorldPosFromMapSS(float2 CoordsSS){
    float MapCameraDepth = 249920.0;

    row_major float4x4 MapViewProjInverse = float4x4(
    float4(0.8391, 2.81308E-15, 0.00, -0.00025),
    float4(0.00, 0.47199, 0.00, 0.00031),
    float4(0.00, 0.00016, 0.00, -1.00),
    float4(0.00, 0.00, -0.00781, 0.00781));

    row_major float4x4 MapViewProjInverseTest = float4x4(
    float4(0.8391, 0.00, 0.00, 0.00),
    float4(0.00, 0.47199, 0.00, 0.00035),
    float4(0.00, 0.00016, 0.00, -1.00),
    float4(0.00, 0.00, -0.00781, 0.00781));

    row_major float4x4 MapProj = float4x4(
    float4(1.19175, 0.00, 0.00029, 0.00),
    float4(0.00, 2.11867, 0.00008, 0.00),
    float4(0.00, 0.00, 1.00036, -128.04633),
    float4(0.00, 0.00, 1.00, 0.00));

    float NDCDepth = MapProj[2][2] + MapProj[2][3] / MapCameraDepth;
    NDCDepth = 0.99952;

    float3 CoordsNDC = float3(((CoordsSS.xy+0.5) / float2(2560.0, 1440.0)) * 2.0 - 1.0, 1);
           CoordsNDC = float3(CoordsNDC.xy * float2(1.0, -1.0), NDCDepth);

    float4 CoordsWS = mul(MapViewProjInverseTest, float4(CoordsNDC, 1.0));
    CoordsWS /= CoordsWS.w;

    return CoordsWS.xyz;
}


float MapRange(float x, float oldMin, float oldMax, float newMin, float newMax){
    return newMin + ((x - oldMin) / (oldMax - oldMin)) * (newMax - newMin);}

float GetWorldHeight(float2 CoordsUV){
    float Height = HeightMap.SampleLevel(Point_Sampler, CoordsUV, 0.0).x;
    return lerp(HeightMapZRange.x, HeightMapZRange.y, Height);
}

//Map UV(or NDC) -> worldspace point on map
//worldspace pos -> use pos to sample terrain height map

[numthreads(1, 1, 1)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float2 Coords = float2(ThreadID.xy);
    float RadiusPx = FogMapData.z;

    float CurrExtinction = FogMap[ThreadID.xy].w;
    if(length(Coords - FogMapData.xy) - RadiusPx < 0.0){
        float Extinction = (FogMapBlendOpp != -1) ? CurrExtinction + UILocalFogMapInput.w : CurrExtinction - UILocalFogMapInput.w;
              Extinction = saturate(Extinction);

        //float3 UIOutputColor = float3(1.0 - UILocalFogMapInput.xy * Extinction, 1 * Extinction);
        //float3 WorldPos = GetWorldPosFromMapSS(Coords);
        //float2 HeightUV = WorldPos.xy * HeightMapParams.xy + HeightMapParams.zw;
        //float GroundHeight = GetWorldHeight(HeightUV);

        //float FogGroundHeightBais = UILocalFogMapInput.x; //MapRange(UILocalFogMapInput.x, 0.0, 1.0, 0.0, 35000.0);
        //float FogBaseHeight = UILocalFogMapInput.y;
        //float FogHeight = FogBaseHeight + GroundHeight;//GroundHeight + FogBaseHeight;
        //float FogFalloff = UILocalFogMapInput.z;
        //FogMap[ThreadID.xy] = float4(FogHeight, FogFalloff, 1.0, Extinction);


        UIFogMap[ThreadID.xy] = float4(1.0, 1.0, 1.0, Extinction); //This target is an overlay fitted onto the world map image displayed to the user to visualize their changes

        FogMap[ThreadID.xy] = float4(1.0, UILocalFogMapInput.y, UILocalFogMapInput.z, Extinction); // This target is used as a lookup in the media compute stage
        //FogMap[ThreadID.xy] = float4(1.0, UIGlobalFogBaseHeight, UIGlobalFogFalloff, Extinction);
    }
    else{
        if(CurrExtinction == 0.0)
            FogMap[ThreadID.xy] = float4(WorldMap.Load(int3(ThreadID.xy, 0.0)).xyz, 0.0); // This is just to debug in renderDoc
        }

     //UIFogMap[ThreadID.xy] = float4(GetWorldHeight(HeightUV).xxx, 1);
     //FogMap[ThreadID.xy] = float4(0,0,0,0);
     //UIFogMap[ThreadID.xy] = float4(0,0,0,0);
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

#pragma warning(push)
#pragma warning(disable : 3556) // Disable integer modulus speed warning since this is a run once shader

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
#pragma warning(pop)
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


