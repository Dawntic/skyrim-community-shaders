#include "Common/SharedData.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

cbuffer ShadowVolumeBuffer : register(b0)
{
    float4 FrustumNearFar;
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
    float4x3 ShadowMapProj[2][3];
    float4x4 CameraViewProjInverse[2];
};

SamplerState Linear_Sampler : register(s10);
SamplerState Point_Sampler : register(s11);


#define EPSILON 1e-6

float4 FroxelWorldPosition(float3 Froxel)
{
    float2 CoordsNDC = (Froxel.xy / VolumeSize.xy) * 2.0 - 1.0;
	float Depth = exp2(Froxel.z / FrustumNearFar.w) / FrustumNearFar.z;

    float3 CoordsVS = float3(float2(CoordsNDC.x, -CoordsNDC.y) * Depth * float2(CameraProjInverse[0][0][0], CameraProjInverse[0][1][1]), Depth);
    float3 CoordsWS = mul(CameraViewInverse[0], float4(CoordsVS, 1.0)).xyz;

    float4 CoordsCS = mul(CameraViewProj[0], float4(CoordsWS, 1.0));
	float ClipZ = CoordsCS.z * rcp(CoordsCS.w);

	return float4(CoordsWS, ClipZ);
}


//// Scattering Compute Shader //////////////////////////////////////////////////////////

#ifdef SCATTER_COMPUTE

Texture3D ShadowVolume : register(t0);
Texture2DArray NoiseTex : register(t2);
Texture1D InvRepartition : register(t1);
Texture3D GameVLVolume : register(t3);
StructuredBuffer<ShadowDataStruct> ShadowDataSB : register(t5);
Texture2DArray ShadowMap : register(t6);

RWTexture3D<float4> ScatteringVolume : register(u0);

float LinearStep(float edge0, float edge1, float x){
    return saturate((x - edge0) / (edge1 - edge0));
}

#define BackScatterMin 0.0
#define BackScatterMax 1.0

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

float GetDirectionalShadow(float4 CoordsWS){
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
#define Weight1 0.5
#define Weight2 0.2
#define Anisotropy 0.2
#define Extinction 0.05
#define Lobes 2

// Stable 3D phase so each Froxel (and cascade) starts at a different point in the sequence.
uint STBNPhase3D(uint3 pos, uint CascadeIdx){
    uint h = (pos.x * 1973u) ^ (pos.y * 9277u) ^ (pos.z * 2663u) ^ (CascadeIdx * 811u);
    return h % max(1u, NoiseSize.z);
}

float3 SampleNoise(uint3 FroxelCoord)
{
    // Base XY in the blue/STBN tile
    uint2 BaseCoord = uint2(FroxelCoord.xy) % NoiseSize.xy;

    // Two distinct XY offsets (use odd primes; wrap by tile size)
    uint2 OffsetCoordA = uint2((BaseCoord.x + 37u) % NoiseSize.x,
                               (BaseCoord.y + 17u) % NoiseSize.y);
    uint2 OffsetCoordB = uint2((BaseCoord.x + 73u) % NoiseSize.x,
                               (BaseCoord.y + 29u) % NoiseSize.y);

    // Layer selection (frame-varying + per-froxel phase)
    uint LayerBase = (FrameCounter + STBNPhase3D(FroxelCoord, 1)) % NoiseSize.z;
    uint LayerAlt  = (LayerBase + 11u) % NoiseSize.z; // different Z to reduce correlation

    float Sample0 = NoiseTex.Load(int4(BaseCoord,    LayerBase, 0)).x;
    float Sample1 = NoiseTex.Load(int4(OffsetCoordA, LayerBase, 0)).x;
    float Sample2 = NoiseTex.Load(int4(OffsetCoordB, LayerBase,  0)).x;

    return float3(Sample0, Sample1, Sample2);
}

float GetShadowDepth(float3 positionWS, uint eyeIndex)
{
    float4 positionCSShifted = mul(CameraViewProj[eyeIndex], float4(positionWS, 1));
    return positionCSShifted.z / positionCSShifted.w;
}

float Get2DFilteredShadowCascade(float noise, float2x2 rotationMatrix, float sampleOffsetScale, float2 baseUV, float cascadeIndex, float compareValue, uint eyeIndex){
    const uint sampleCount = 16;
    float layerIndexRcp = rcp(1 + cascadeIndex);
    float visibility = 0.0;
    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        float2 sampleOffset = mul(Random::PoissonSampleOffsets16[sampleIndex], rotationMatrix);
        float2 sampleUV = layerIndexRcp * sampleOffset * sampleOffsetScale + baseUV;
        float4 depths = ShadowMap.GatherRed(Linear_Sampler, float3(saturate(sampleUV), cascadeIndex), 0);
        visibility += dot(depths > compareValue, 0.25);
    }

    return visibility * rcp((float)sampleCount);
}

float Get2DFilteredShadow(float noise, float2x2 rotationMatrix, float3 positionWS, uint eyeIndex)
{
    ShadowDataStruct sD = ShadowDataSB[0];
    float shadowMapDepth = GetShadowDepth(positionWS, eyeIndex);
    if (sD.EndSplitDistances.z >= shadowMapDepth) {
        float fadeFactor = 1 - pow(saturate(dot(positionWS.xyz, positionWS.xyz) / sD.ShadowLightParam.z), 8);
        float4x3 lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][0];
        float cascadeIndex = 0;
        if (sD.EndSplitDistances.x < shadowMapDepth) {
            lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][1];
            cascadeIndex = 1;
        }

        float3 positionLS = mul(transpose(lightProjectionMatrix), float4(positionWS.xyz, 1)).xyz;
        float shadowVisibility = Get2DFilteredShadowCascade(noise, rotationMatrix, sD.ShadowSampleParam.z, positionLS.xy, cascadeIndex, positionLS.z, eyeIndex);

        if (cascadeIndex < 1 && sD.StartSplitDistances.y < shadowMapDepth) {
            float3 cascade1PositionLS = mul(transpose(sD.ShadowMapProj[eyeIndex][1]), float4(positionWS.xyz, 1)).xyz;
            float cascade1ShadowVisibility = Get2DFilteredShadowCascade(noise, rotationMatrix, sD.ShadowSampleParam.z, cascade1PositionLS.xy, 1, cascade1PositionLS.z, eyeIndex);
            float cascade1BlendFactor = smoothstep(0, 1, (shadowMapDepth - sD.StartSplitDistances.y) / (sD.EndSplitDistances.x - sD.StartSplitDistances.y));
            shadowVisibility = lerp(shadowVisibility, cascade1ShadowVisibility, cascade1BlendFactor);
        }

        return lerp(1.0, shadowVisibility, fadeFactor);
    }
    return 1.0;
}

float GetLightingShadow(float noise, float3 worldPosition, uint eyeIndex)
{
    float2 rotation;
    sincos(Math::TAU * noise, rotation.y, rotation.x);
    float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);
    return Get2DFilteredShadow(noise, rotationMatrix, worldPosition, eyeIndex);
}

[numthreads(4, 4, 4)]
void main(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 Froxel = ThreadID;
    Froxel.z *= 2.0;
	Froxel.z += (((ThreadID.x + ThreadID.y) & 1) == (FrameCounter & 1)) ? 1.0 : 0.0;

    //Froxel += SampleNoise(ThreadID);

    float4 CoordsWS = FroxelWorldPosition(Froxel);
    float Shadow = GetDirectionalShadow(CoordsWS);

    float4 CameraPosWS = mul(CameraViewInverse[0], float4(0, 0, 0, 1));
    float3 IncomingDir = SharedData::DirLightDirection.xyz;
    float3 OutgoingDir = -normalize(CameraPosWS.xyz - CoordsWS.xyz);

    float3 Scattering = SharedData::DirLightColor.xyz * MLobePhaseFunction(IncomingDir, OutgoingDir, Anisotropy, Extinction, Weight1, Weight2, Lobes);

    //float Noise = NoiseTex.Load(int4(int2(Froxel.xy) & 63, FrameCounter & 31, 0)).x;
    //Shadow = GetLightingShadow(Noise, CoordsWS.xyz, 0);

    Scattering = Scattering * Shadow;

    ScatteringVolume[ThreadID] = float4(Scattering, Extinction);
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////



//// Filter Compute Shader //////////////////////////////////////////////////////////////

#ifdef FILTER_COMPUTE

Texture3D PrevFilterVolume : register(t0);
Texture1D InvRepartition : register(t1);
Texture1D Repartition : register(t2);
Texture2DArray NoiseTex : register(t3);
Texture3D ScatteringVolume : register(t4);

RWTexture3D<float4> FilterVolume : register(u0);

static const int2 Offsets[4] = { int2(0, 1), int2(-1, 0), int2(1, 0), int2(0, -1)};

#define UseHistory true
#define HistoryAlpha 0.2

float3 GetPreviousUVZ(float3 CoordsWS){
    float4 PrevCoordsVS = mul(PrevCameraView[0], float4(CoordsWS, 1.0));
    float4 PrevCoordsCS = mul(PrevCameraProj[0], PrevCoordsVS); //use unjittered?
    float Depth = saturate(log2(PrevCoordsVS.z * FrustumNearFar.z) * rcp(log2(FrustumNearFar.y * FrustumNearFar.z)));

    return float3((PrevCoordsCS.xy / PrevCoordsCS.w) * float2(0.5, -0.5) + 0.5, Depth);
}

[numthreads(4, 4, 4)]
void main(uint3 Froxel : SV_DispatchThreadID)
{
    float4 Output = float4(0.0, 0.0, 0.0, 0.0);

	bool checkerboardHole = (((Froxel.x + Froxel.y) & 1) == (FrameCounter & 1));
	checkerboardHole = (Froxel.z & 1) ? !checkerboardHole : checkerboardHole;

	if (!checkerboardHole)
		Output = ScatteringVolume.Load(uint4(Froxel.xy, Froxel.z / 2, 0));

    if(UseHistory){
        float3 CoordsWS = FroxelWorldPosition(float3(Froxel + 0.5)).xyz;
        float3 PrevCoordsUVZ = GetPreviousUVZ(CoordsWS);

        bool Valid = all(PrevCoordsUVZ >= 0.0 && PrevCoordsUVZ <= 1.0);

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
/////////////////////////////////////////////////////////////////////////////////////////



//// Slice March Compute Shader /////////////////////////////////////////////////////////

#ifdef MARCH_COMPUTE

Texture3D ScatteringVolume : register(t0);
Texture1D InvRepartition : register(t1);
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
    float3 PrevCoordsWS = FroxelWorldPosition(float3(Froxel.xy + 0.5, 0.0)).xyz;

    for(int Slice=0; Slice < VolumeSize.z; Slice++){
        float3 CoordsWS = FroxelWorldPosition(float3(Froxel.xy + 0.5, Slice + 1.0)).xyz;

        float StepLength = distance(PrevCoordsWS, CoordsWS);
              StepLength = GameUnitToMeter(StepLength);

        float4 ScatteredSlice = ScatteringVolume.Load(int4(Froxel.xy, Slice, 0));

        AccumulateScattering(Accumulation, ScatteredSlice, StepLength);

        PrevCoordsWS = CoordsWS;
        IntergrationVolume[uint3(Froxel.xy, Slice)] = saturate(Accumulation);
    }
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////