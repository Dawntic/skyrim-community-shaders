
cbuffer CacheGenBuffer : register(b0)
{
	float4 CubemapParams;  // Dimension, 1.0 / Dimension,  Dimension^2, Dimension^2 * Sides Used
	int2 BentNormalWriteCoords;
};

#define GROUP_SIZE 256
#define REDUCTION (GROUP_SIZE / 2)

#ifdef BENT_NORMAL_COMPUTE

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
	float lenSq = 1.0 + dot(Coords, Coords);
	return rsqrt(lenSq) * rcp(lenSq);
}

groupshared float3 gs_DirSum[GROUP_SIZE];
groupshared float gs_VisWeight[GROUP_SIZE];
groupshared float gs_WeightTotal[GROUP_SIZE];

[numthreads(GROUP_SIZE, 1, 1)] void main(uint ThreadID : SV_GroupIndex) {
	float3 localBent = 0;
	float localVis = 0;
	float localTotal = 0;

	for (uint t = ThreadID; t < (uint)CubemapParams.w; t += GROUP_SIZE) {
		uint Face = t / CubemapParams.z;
		uint r = t % CubemapParams.z;
		int2 CoordsFace = int2(r % CubemapParams.x, r / CubemapParams.x);

		float2 CoordsNDC = (float2(CoordsFace.x, CoordsFace.y) + 0.5) * CubemapParams.y * 2.0 - 1.0;
		float3 Direction = CubemapDirection(CoordsNDC, Face);

		if (Direction.z <= 0)
			continue;

		float Weight = GetSolidAngleWeight(CoordsNDC) * Direction.z;

		float depth = DepthCubeTex[int3(CoordsFace.xy, Face)];
		float visible = float(depth > 0.9999999);

		localBent += Direction * Weight * visible;
		localVis += Weight * visible;
		localTotal += Weight;
	}

	gs_DirSum[ThreadID] = localBent;
	gs_VisWeight[ThreadID] = localVis;
	gs_WeightTotal[ThreadID] = localTotal;

	GroupMemoryBarrierWithGroupSync();

	for (uint s = REDUCTION; s > 0; s >>= 1) {
		if (ThreadID < s) {
			gs_DirSum[ThreadID] += gs_DirSum[ThreadID + s];
			gs_VisWeight[ThreadID] += gs_VisWeight[ThreadID + s];
			gs_WeightTotal[ThreadID] += gs_WeightTotal[ThreadID + s];
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (ThreadID == 0) {
		float3 BentNormal = (length(gs_DirSum[0]) > 1e-6) ? normalize(gs_DirSum[0]) : float3(0, 0, 1);
		float AO = gs_VisWeight[0] / max(gs_WeightTotal[0], 1e-6);

		OutputBentNormalMap[BentNormalWriteCoords.xy] = float4(BentNormal * 0.5 + 0.5, AO);
	}
}
#endif