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
	uint FrameIdx;
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

Texture3D ShadowVolume : register(t0);
Texture1D InvRepartition : register(t1);

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

    return PrimaryLobe;// + SecondaryLobe;
}

float4 GetWorldCoords(uint3 Froxel)
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


[numthreads(8, 8, 4)]
void main(uint3 Froxel : SV_DispatchThreadID)
{
    if (any(Froxel >= (uint3)VolumeSize.xyz))
        return;

    float Weight1 = 0.1;
    float Weight2 = 1.0;
    float Anisotropy = 0.5; //higher = thicker media
    int Lobes = 2;

    float Extinction = 0.003; //0.001

    float3 NormCoords = float3(Froxel.xyz + 0.5) / VolumeSize.xyz;
    float Shadow = ShadowVolume.SampleLevel(Linear_Sampler, NormCoords, 0.0).x;

    //float3 CoordsWS = FroxelWorldPosition(CoordsUV).xyz;
    float4 CoordsWS = GetWorldCoords(Froxel);

    float3 IncomingDir = SharedData::DirLightDirection.xyz; //normalize(-SharedData::DirLightDirection.xyz);
    //float3 OutgoingDir = normalize(CameraPosition.xyz - CoordsWS.xyz);
    float3 OutgoingDir = normalize(CameraPosAdjust[0].xyz - CoordsWS.xyz);

    //float3 viewDirection = -normalize(input.WorldPosition.xyz);

    float3 ScatteringResult = SharedData::DirLightColor.xyz * MLobePhaseFunction(IncomingDir, OutgoingDir, Anisotropy, Extinction, Weight1, Weight2, Lobes);
    //float3 ScatteringResult = SharedData::DirLightColor.xyz * (1.0 / (4.0 * Math::PI));
    ScatteringResult = ScatteringResult * Shadow;// * Extinction;

    //if(Froxel.z < 20)
    //    ScatteringResult = float3(0,0,0);
   // else
       //ScatteringResult = float3(0.3, 0.3, 0.3);

    //ScatteringResult *= Shadow;
    //ScatteringResult = float3(0.01, 0.01, 0.01);

    ScatteringVolume[Froxel] = float4(ScatteringResult, Extinction);
}
#endif
