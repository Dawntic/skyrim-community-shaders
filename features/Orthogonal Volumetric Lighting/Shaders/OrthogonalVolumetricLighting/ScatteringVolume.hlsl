#include "Common/SharedData.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"


#ifdef ScatterVolumeCompute

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

Texture3D ShadowVolume : register(t0);
Texture1D InvRepartition : register(t1);
Texture2DArray NoiseTex : register(t2);

StructuredBuffer<ShadowDataStruct> ShadowDataSB : register(t5);
Texture2DArray ShadowMap : register(t6);

RWTexture3D<float4> ScatteringVolume : register(u0);

SamplerState Linear_Sampler : register(s10);

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

    return PrimaryLobe + SecondaryLobe;
}

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


uint STBNPhase3D(uint3 pos){
    uint h = (pos.x * 1973u) ^ (pos.y * 9277u) ^ (pos.z * 2663u) ^ 0x9E3779B9u;
    return h % max(1u, NoiseSize.z);
}

float2 SampleNoise(uint3 Froxel){
    uint layer = (SharedData::FrameCountAlwaysActive + STBNPhase3D(Froxel)) % NoiseSize.z;

    uint2 coord1 = uint2(Froxel.xy) % NoiseSize.xy;
    uint2 coord2 = uint2((coord1.x + 37u) % NoiseSize.x, (coord1.y + 17u) % NoiseSize.y);

    float NoiseX = NoiseTex.Load(int4(coord1.xy, layer, 0)).x;
    float NoiseY = NoiseTex.Load(int4(coord2.xy, layer, 0)).x;

    return float2(NoiseX, NoiseY);
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

float GetDirectionalShadow(float4 CoordsWS)
{
    float shadowMapDepth = CoordsWS.w;
	float Visibility = 0.0;
    ShadowDataStruct ShadowData = ShadowDataSB[0];
	if (ShadowData.EndSplitDistances.z >= shadowMapDepth) {
		uint cascadeIndex = (shadowMapDepth > ShadowData.EndSplitDistances.x) ? 1 : 0;
        float4x3 LightWorldShadowUV = ShadowData.ShadowMapProj[0][cascadeIndex];

        float3 CoordsLS = mul(transpose(LightWorldShadowUV), float4(CoordsWS.xyz, 1.0)).xyz;
		float Sample = ShadowMap.SampleLevel(Linear_Sampler, float3(CoordsLS.xy, cascadeIndex), 0).x;
		Visibility = Sample >= CoordsLS.z;
	}

    return Visibility;
}

[numthreads(8, 8, 4)]
void main(uint3 Froxel : SV_DispatchThreadID)
{
    if (any(Froxel >= (uint3)VolumeSize.xyz))
        return;

    float3 Jitter = float3(0,0,0);
    Jitter.xy = (SampleNoise(Froxel) - 0.5) * CellJitterValue;
    Jitter.z = GetRayJitter(Froxel);

    float3 TexCoord = Froxel;
    TexCoord.z *= 2.0;
	TexCoord.z += (((Froxel.x + Froxel.y) & 1) == (SharedData::FrameCountAlwaysActive & 1)) ? 1.0 : 0.0;
	TexCoord += Jitter.xyz;

    float Weight1 = 0.1;
    float Weight2 = 1.0;
    float Anisotropy = 0.5;
    float Extinction = 0.002; //0.001
    int Lobes = 2;

    //float3 NormCoords = float3(Froxel.xyz + 0.5) / VolumeSize.xyz;
    //float Shadow = ShadowVolume.SampleLevel(Linear_Sampler, NormCoords, 0.0).x;

    float4 CoordsWS = GetWorldCoords(TexCoord);

    float3 IncomingDir = SharedData::DirLightDirection.xyz;
    float3 OutgoingDir = normalize(CameraPosition.xyz - CoordsWS.xyz);

    float3 Scattering = SharedData::DirLightColor.xyz * MLobePhaseFunction(IncomingDir, OutgoingDir, Anisotropy, Extinction, Weight1, Weight2, Lobes);

    float Shadow = GetDirectionalShadow(CoordsWS);
    Scattering = Scattering * Shadow;

    if(Froxel.z < 1)
        Scattering = float3(0, 0, 0);

    ScatteringVolume[Froxel] = float4(Scattering, Extinction);
}
#endif

