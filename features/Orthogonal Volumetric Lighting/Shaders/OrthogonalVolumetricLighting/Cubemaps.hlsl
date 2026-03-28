
cbuffer ShadowBuffer : register(b0)
{
	int2 BentNormalWriteCoords;
	int2 BentNormalTexSize;
	float4 CubemapParams;  //size in xy, face in z
};

#ifdef BENT_NORMAL_CS

Texture2DArray<float> DepthCubeTex : register(t0);
RWTexture2D<float4> OutputBentNormalMap : register(u0);

float3 CubemapDirection(float2 CoordsNDC, uint face)
{
	float3 dir = float3(0, 0, 0);
	switch (face) {
	case 0:
		dir = float3(1, -CoordsNDC.y, -CoordsNDC.x);
		break;  // +X
	case 1:
		dir = float3(-1, -CoordsNDC.y, CoordsNDC.x);
		break;  // -X
	case 2:
		dir = float3(CoordsNDC.x, 1, CoordsNDC.y);
		break;  // +Y
	case 3:
		dir = float3(CoordsNDC.x, -1, -CoordsNDC.y);
		break;  // -Y
	case 4:
		dir = float3(CoordsNDC.x, -CoordsNDC.y, 1);
		break;  // +Z
	case 5:
		dir = float3(-CoordsNDC.x, -CoordsNDC.y, -1);
		break;  // -Z
	}
	return normalize(dir);
}

float GetSolidAngleWeight(float2 Coords)
{
	// texels near cubemap corners subtend less solid angle
	float lenSq = 1.0 + dot(Coords, Coords);
	return rcp((lenSq * sqrt(lenSq)));
}
groupshared float3 gs_BentSum[256];
groupshared float gs_VisWeight[256];
groupshared float gs_TotalWeight[256];

[numthreads(256, 1, 1)] void main(uint threadIdx : SV_GroupIndex) {
	float3 localBent = 0;
	float localVis = 0;
	float localTotal = 0;

	float CubePxSize = CubemapParams.x;
	// each thread processes multiple texels
	uint totalTexels = CubePxSize * CubePxSize * 6;
	for (uint i = threadIdx; i < totalTexels; i += 256) {
		uint face = i / (CubePxSize * CubePxSize);
		uint rem = i % (CubePxSize * CubePxSize);
		uint x = rem % CubePxSize;
		uint y = rem / CubePxSize;

		float2 CoordsNDC = (float2(x, y) + 0.5) * rcp(CubePxSize) * 2.0 - 1.0;
		float3 Direction = CubemapDirection(CoordsNDC, face);
		if (Direction.z <= 0)
			continue;

		float Weight = Direction.z * GetSolidAngleWeight(CoordsNDC);
		float depth = DepthCubeTex.Load(int4(x, y, face, 0));
		bool visible = depth > 0.9999999;

		if (visible) {
			localBent += Direction * Weight;
			localVis += Weight;
		}
		localTotal += Weight;
	}

	gs_BentSum[threadIdx] = localBent;
	gs_VisWeight[threadIdx] = localVis;
	gs_TotalWeight[threadIdx] = localTotal;

	GroupMemoryBarrierWithGroupSync();

	for (uint s = 128; s > 0; s >>= 1) {
		if (threadIdx < s) {
			gs_BentSum[threadIdx] += gs_BentSum[threadIdx + s];
			gs_VisWeight[threadIdx] += gs_VisWeight[threadIdx + s];
			gs_TotalWeight[threadIdx] += gs_TotalWeight[threadIdx + s];
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (threadIdx == 0) {
		float3 bent = (length(gs_BentSum[0]) > 0.001) ? normalize(gs_BentSum[0]) : float3(0, 0, 1);
		float AO = (gs_TotalWeight[0] > 0) ? gs_VisWeight[0] / gs_TotalWeight[0] : 1.0;

		OutputBentNormalMap[BentNormalWriteCoords.xy] = float4(bent * 0.5 + 0.5, AO);
	}
}
#endif

#ifdef COPY_DEPTH_MAP

Texture2D<float> SourceDepth : register(t0);
RWTexture2DArray<float> DestCubemap : register(u0);

[numthreads(8, 8, 1)] void main(uint2 ThreadID : SV_DispatchThreadID) {
	if (any(ThreadID >= (uint2)CubemapParams.xy))
		return;

	float depth = SourceDepth.Load(int3(ThreadID, 0));

	DestCubemap[uint3(ThreadID, CubemapParams.z)] = depth;
}
#endif