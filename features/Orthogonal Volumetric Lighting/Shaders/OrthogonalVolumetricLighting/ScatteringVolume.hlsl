#include "Common/SharedData.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

cbuffer ShadowVolumeBuffer : register(b0)
{
    row_major float4x4 Frustum;
    float4 FrustumNearFar;
    float4 CameraPosition;
    float4 VolumeSize;
	float4 NoiseSize;
    float4 PrevCameraData;
	float2 ShadowAtlasSize;
	float CellJitterValue;
	float RayJitterValue;
	uint ESM_Scale;
	uint ESM_EXP;
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

cbuffer PrevPerFrame : register(b2)
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
    float4 EndSplitDistances;    // cascade end distances int xyz, cascade count int z
    float4 StartSplitDistances;  // cascade start ditances int xyz, 4 int z
    float4 FocusShadowFadeParam;
    float4 DebugColor;
    float4 PropertyColor;
    float4 AlphaTestRef;
    float4 ShadowLightParam;  // Falloff in x, ShadowDistance squared in z
    float4x3 FocusShadowMapProj[4];
    // Since ShadowData is passed between c++ and hlsl, can't have different defines due to strong typing
    float4x3 ShadowMapProj[2][3];
    float4x4 CameraViewProjInverse[2];
};

SamplerState Linear_Sampler : register(s10);







#ifdef ScatterVolumeCompute

Texture3D ShadowVolume : register(t0);
Texture2DArray NoiseTex : register(t2);
Texture1D InvRepartition : register(t1);
StructuredBuffer<ShadowDataStruct> ShadowDataSB : register(t5);
Texture2DArray ShadowMap : register(t6);

RWTexture3D<float4> ScatteringVolume : register(u0);



#define EPSILON 1e-6

float LinearStep(float edge0, float edge1, float x){
    return saturate((x - edge0) / (edge1 - edge0));}

//// Scattering ///////////////////////////////////////////////////////////////////////////////////
#define BackScatterMin 0.0
#define BackScatterMax 1.0

float4 FroxelWorldPosition(float3 Froxel)
{
    float SliceZ = exp2(Froxel.z / FrustumNearFar.w) / FrustumNearFar.z;
    float2 CoordsUV = (Froxel.xy / VolumeSize.xy) * SliceZ;
    float4 CoordsWS = mul(Frustum, float4(CoordsUV.xy, SliceZ, 1.0));

    return float4(CoordsWS.xyz, SliceZ);
}

float BackScattering(float phase, float Extinction){
	float v = 1.0 / Math::PI * LinearStep(BackScatterMax, BackScatterMin, Extinction);
	return max(phase, v);
}

float HenyeyGreensteinPhase(float ScatteringAngle, float Anisotropy){
    Anisotropy = clamp(Anisotropy, -0.999, 0.999);
	float AnisotropySq = Anisotropy * Anisotropy;
	float phase = max(1.0 + AnisotropySq - 2.0 * Anisotropy * ScatteringAngle, EPSILON);
    phase *= sqrt(phase);
    return (1.0 - AnisotropySq) / (4.0 * Math::PI * phase);
}

float MLobePhaseFunction(float3 IncidentDir, float3 CameraDir, float Anisotropy, float Extinction, float Weight1, float Weight2, int MLobes){
	float SecondaryLobe = 0.0;
    float secondAnisotropy = Anisotropy * (2.0 / 3.0);

    float ScatteringAngle = dot(IncidentDir, CameraDir);
    float PrimaryLobe = HenyeyGreensteinPhase(ScatteringAngle, Anisotropy) * Weight1;

    for (int j = 1; j <= MLobes; ++j)
        SecondaryLobe += HenyeyGreensteinPhase(ScatteringAngle, secondAnisotropy);
    SecondaryLobe = (Weight2 * Extinction / float(MLobes - 1)) * SecondaryLobe;

    return PrimaryLobe;// + SecondaryLobe;
}



uint STBNPhase3D(uint3 cell)
{
    uint h = (cell.x * 1973u) ^ (cell.y * 9277u) ^ (cell.z * 2663u) ^ 0x9E3779B9u;
    return h % max(1u, NoiseSize.z);
}

float3 SampleNoise(uint3 froxel)
{
    uint  spatialPhase = STBNPhase3D(froxel);
    uint  baseLayer = (SharedData::FrameCountAlwaysActive + spatialPhase) % max(1u, ceil(NoiseSize.z));

    uint2 baseXY = uint2(froxel.xy) % NoiseSize.xy;

    uint2 xy0 = baseXY;
    uint2 xy1 = uint2((baseXY.x + 37u) % NoiseSize.x, (baseXY.y + 17u) % NoiseSize.y);
    uint2 xy2 = uint2((baseXY.x + 11u) % NoiseSize.x, (baseXY.y + 29u) % NoiseSize.y);

    float n0 = NoiseTex.Load(int4(int2(xy0), (baseLayer +  0u) % NoiseSize.z, 0)).x;
    float n1 = NoiseTex.Load(int4(int2(xy1), (baseLayer +  7u) % NoiseSize.z, 0)).x;
    float n2 = NoiseTex.Load(int4(int2(xy2), (baseLayer + 13u) % NoiseSize.z, 0)).x;

    return float3(n0, n1, n2);
}


float SliceThicknessViewZ(float FroxelZ){
    float ZCoordUV = FroxelZ * rcp(VolumeSize.z);
    float ZCoordUV2 = (FroxelZ + 1) * rcp(VolumeSize.z);
    float depth = InvRepartition.SampleLevel(Linear_Sampler, ZCoordUV, 0).x;
    float depth2 = InvRepartition.SampleLevel(Linear_Sampler, ZCoordUV2, 0).x;

    return max(depth2 - depth, EPSILON);
}

float GetRayJitter(uint3 Froxel)
{
    float ZThickness = SliceThicknessViewZ((float)Froxel.z);
    float Limit = 0.45 * ZThickness;
    float Noise = SampleNoise(Froxel).x * 2.0 - 1.0;
          Noise = clamp(Noise * RayJitterValue * ZThickness, -Limit, +Limit);

    float ZCoordUVCenter = float(Froxel.z + 0.5) * rcp(VolumeSize.z);
    float ZCenter = InvRepartition.SampleLevel(Linear_Sampler, ZCoordUVCenter, 0).x;

    return ZCenter + Noise;
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

float GetDirectionalShadow(float4 CoordsWS)
{
    ShadowDataStruct ShadowData = ShadowDataSB[0];
	float Visibility = 0.0;
    float shadowMapDepth = CoordsWS.w;

	if (ShadowData.EndSplitDistances.z >= shadowMapDepth) {
		uint cascadeIndex = (shadowMapDepth > ShadowData.EndSplitDistances.x) ? 1 : 0;
        float4x3 LightWorldShadowUV = ShadowData.ShadowMapProj[0][cascadeIndex];

        float3 CoordsLS = mul(transpose(LightWorldShadowUV), float4(CoordsWS.xyz, 1.0)).xyz;
		float Sample = ShadowMap.SampleLevel(Linear_Sampler, float3(CoordsLS.xy, cascadeIndex), 0).x;
		Visibility = Sample >= CoordsLS.z;
	}

    return Visibility;
}



#define Weight1 0.2
#define Weight2 0.2
#define Anisotropy 0.2
#define Extinction 0.01 //0.002
#define Lobes 2

[numthreads(8, 8, 4)]
void main(uint3 Froxel : SV_DispatchThreadID)
{
    float3 TexCoord = Froxel;
    TexCoord.z *= 2.0;
	TexCoord.z += (((Froxel.x + Froxel.y) & 1) == (SharedData::FrameCountAlwaysActive & 1)) ? 1.0 : 0.0;

    //float3 Jitter = SampleNoise(Froxel);
    //Jitter.z = frac(Jitter.z);
	//TexCoord += Jitter.xyz;

    float4 CoordsWS = GetWorldCoords(TexCoord);
    float4 CameraPosWS = mul(CameraViewInverse[0], float4(0, 0, 0, 1));

    float3 IncomingDir = SharedData::DirLightDirection.xyz;
    float3 OutgoingDir = -normalize(CameraPosWS.xyz - CoordsWS.xyz);

    float3 Scattering = SharedData::DirLightColor.xyz * MLobePhaseFunction(IncomingDir, OutgoingDir, Anisotropy, Extinction, Weight1, Weight2, Lobes);

    float Shadow = GetDirectionalShadow(CoordsWS);
    Scattering = Scattering * Shadow;

    if(Froxel.z < 1)
        Scattering = float3(0, 0, 0);

    ScatteringVolume[Froxel] = float4(Scattering, Extinction);
}
#endif

#ifdef FilterVolumeCompute

Texture3D PrevFilterVolume : register(t0);
Texture1D InvRepartition : register(t1);
Texture1D Repartition : register(t2);
Texture2DArray NoiseTex : register(t3);
Texture3D ScatteringVolume : register(t4);

RWTexture3D<float4> FilterVolume : register(u0);


float4 GetWorldCoords(float3 Froxel)
{
    float3 CoordsUV = Froxel.xyz * (1.0 / VolumeSize.xyz);

	float depth = InvRepartition.SampleLevel(Linear_Sampler, CoordsUV.z, 0).x;

	float4 CoordsNDC = float4(CoordsUV.xy * 2.0 - 1.0, depth, 1.0);
	CoordsNDC.y = -CoordsNDC.y;

	float4 CoordsWS = mul(CameraViewProjInverse[0], CoordsNDC);
	CoordsWS *= 1.0 / CoordsWS.w;

	float4 CoordsCS = mul(CameraViewProj[0], CoordsWS);
	CoordsCS *= 1.0 / CoordsCS.w;

    return float4(CoordsWS.xyz, CoordsCS.z);
}

float3 GetPrevWorldCoords(float3 CoordsWS)
{
    float4 PrevCS = mul(PrevCameraViewProj[0], float4(CoordsWS, 1.0));

    if (PrevCS.w <= 0.0)
        return float3(-1.0, -1.0, -1.0); //behind camera

    float2 PrevNDC = PrevCS.xy / PrevCS.w;
    float2 PrevUV = float2(PrevNDC.x, -PrevNDC.y) * 0.5 + 0.5;

    float PrevZ = PrevCS.z / PrevCS.w;

    float PrevDepth = Repartition.SampleLevel(Linear_Sampler, PrevZ, 0).x;

    float3 PrevTexCoord = float3(PrevUV, PrevDepth);

    if(all(PrevTexCoord >= 0.0 && PrevTexCoord <= 1.0))
        return PrevTexCoord;

    return float3(-1.0, -1.0, -1.0);
}
static const int2 Offsets[4] = { int2(0, 1), int2(-1, 0), int2(1, 0), int2(0, -1)};



[numthreads(8, 8, 4)]
void main(uint3 Froxel : SV_DispatchThreadID)
{

    float4 Output = float4(0.0, 0.0, 0.0, 0.0);
    float HistoryAlpha = 0.2;
    bool UseHistory = true;

	bool checkerboardHole = (((Froxel.x + Froxel.y) & 1) == (SharedData::FrameCountAlwaysActive & 1));
	checkerboardHole = (Froxel.z & 1) ? !checkerboardHole : checkerboardHole;

	if (!checkerboardHole)
		Output = ScatteringVolume.Load(uint4(Froxel.xy, Froxel.z / 2, 0));

    float4 CoordsWS = GetWorldCoords(float3(Froxel));

    if(UseHistory){
        float3 PrevCoordsUVZ = GetPrevWorldCoords(CoordsWS.xyz);
        bool Valid = (PrevCoordsUVZ.x >= 0.0);

        if(Valid){
            float4 HistoryValue = PrevFilterVolume.SampleLevel(Linear_Sampler, PrevCoordsUVZ, 0);
            Output = checkerboardHole ? HistoryValue : lerp(HistoryValue, Output, HistoryAlpha);
        }

        if (!Valid && checkerboardHole){
            Output = 0.0;
            [unroll]for(int i=0; i<4; ++i)
                Output += ScatteringVolume.Load(uint4((int2)Froxel.xy + Offsets[i], Froxel.z / 2, 0));
            Output /= 4;
        }
    }

     FilterVolume[Froxel] = Output;
}
#endif