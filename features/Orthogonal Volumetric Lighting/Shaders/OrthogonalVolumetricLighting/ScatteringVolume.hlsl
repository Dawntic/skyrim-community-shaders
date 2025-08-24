#include "Common/SharedData.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

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
SamplerState Point_Sampler : register(s11);

#ifdef ScatterVolumeCompute

Texture3D ShadowVolume : register(t0);
Texture2DArray NoiseTex : register(t2);
Texture1D InvRepartition : register(t1);
Texture3D GameVLVolume : register(t3);
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
    float ViewZ = exp2(Froxel.z / FrustumNearFar.w) / FrustumNearFar.z;
    float2 CoordsUV = (Froxel.xy / VolumeSize.xy) * ViewZ;
    float3 CoordsWS = mul(Frustum, float4(CoordsUV.xy, ViewZ, 1.0)).xyz;

    float ClipZ = CameraProj[0][2][2] * ViewZ + CameraProj[0][2][3];

    return float4(CoordsWS, ClipZ);
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
#define Anisotropy 0.10
#define Extinction 0.004 //0.002
#define Lobes 2

//float3 Jitter = SampleNoise(Froxel);
//TexCoord += Jitter.xyz;

[numthreads(4, 4, 4)]
void main(uint3 Froxel : SV_DispatchThreadID)
{
    float3 TexCoord = Froxel;
    //TexCoord.z *= 2.0;
	//TexCoord.z += (((Froxel.x + Froxel.y) & 1) == (FrameCounter & 1)) ? 1.0 : 0.0;

    //float4 CoordsWS = GetWorldCoords(TexCoord);
    //float Shadow = GetDirectionalShadow(CoordsWS);

    float3 CoordsWS = calcWorldSpacePos(TexCoord);

    float4 CoordsCS = mul(CameraViewProj[0], float4(CoordsWS, 1.0));
	float ZClip = CoordsCS.z * (1.0 / CoordsCS.w);
    float Shadow = GetDirectionalShadow(float4(CoordsWS, ZClip));

    //float3 Scattering = float3(1,1,1) * rcp(4 * Math::PI)  * 0.1;
    //float3 ShadowCoord = Froxel.xyz * (1.0 / (VolumeSize.xyz));
    //float Shadow = GameVLVolume.SampleLevel(Point_Sampler, ShadowCoord, 0).x;

    float4 CameraPosWS = mul(CameraViewInverse[0], float4(0, 0, 0, 1));
    float3 IncomingDir = SharedData::DirLightDirection.xyz;
    float3 OutgoingDir = -normalize(CameraPosWS.xyz - CoordsWS.xyz);

    float3 Scattering = SharedData::DirLightColor.xyz * MLobePhaseFunction(IncomingDir, OutgoingDir, Anisotropy, Extinction, Weight1, Weight2, Lobes);

    //float Noise = NoiseTex.Load(int4(int2(Froxel.xy) & 63, SharedData::FrameCountAlwaysActive & 31, 0)).x;
    //Shadow = GetLightingShadow(Noise, CoordsWS.xyz, 0);

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

static const int2 Offsets[4] = { int2(0, 1), int2(-1, 0), int2(1, 0), int2(0, -1)};

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

float3 GetPrevUVCoords(float3 CoordsWS)
{
    float4 PrevCoordsVS = mul(PrevCameraView[0], float4(CoordsWS, 1.0));
    float4 PrevCoordsCS = mul(PrevCameraProj[0], PrevCoordsVS);
    PrevCoordsCS /= PrevCoordsCS.w;

    float Depth = Repartition.SampleLevel(Point_Sampler, saturate(PrevCoordsCS.z), 0).x;
    float3 PrevCoordsUVZ = float3((PrevCoordsCS.xy) * float2(0.5, -0.5) + 0.5, Depth);

    return PrevCoordsUVZ;
}


#define UseHistory true
#define HistoryAlpha 0.2

[numthreads(4, 4, 4)]
void main(uint3 Froxel : SV_DispatchThreadID)
{
    float4 Output = float4(0.0, 0.0, 0.0, 0.0);

	bool checkerboardHole = (((Froxel.x + Froxel.y) & 1) == (FrameCounter & 1));
	checkerboardHole = (Froxel.z & 1) ? !checkerboardHole : checkerboardHole;

	if (!checkerboardHole)
		Output = ScatteringVolume.Load(uint4(Froxel.xy, Froxel.z / 2, 0));

    if(UseHistory){
        float4 PrevCoordsVS = mul(PrevCameraView[0], float4(GetWorldCoords(float3(Froxel + 0.5)).xyz, 1.0));
        float4 PrevCoordsCS = mul(PrevCameraProj[0], PrevCoordsVS); //use unjittered?
        PrevCoordsCS /= PrevCoordsCS.w; // * 1 / w

        float Depth = Repartition.SampleLevel(Point_Sampler, saturate(PrevCoordsCS.z), 0).x;
        float3 PrevCoordsUVZ = float3((PrevCoordsCS.xy) * float2(0.5, -0.5) + 0.5, Depth);

        bool Valid = all(PrevCoordsUVZ >= 0.0 && PrevCoordsUVZ <= 1.0); //setting to false makes it go away

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