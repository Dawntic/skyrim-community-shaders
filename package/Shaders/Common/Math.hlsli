#ifndef __MATH_DEPENDENCY_HLSL__
#define __MATH_DEPENDENCY_HLSL__

#define EPSILON_SSS_ALBEDO 1e-3f    // For albedo clamping in SSS calculations
#define EPSILON_SKIN_ALBEDO 0.001f  // Minimum per-channel skin base color to prevent SSS division explosion
#define EPSILON_DOT_CLAMP 1e-5f     // For dot product clamping
#define EPSILON_DEPTH_SKY 1e-5f     // Depth threshold for sky/unrendered pixel detection (raw reversed-Z near zero)
#define EPSILON_DIVISION 1e-6f      // For division to avoid division by zero
#define EPSILON_GLINTS 1e-8f        // For glints calculations
#define EPSILON_WEIGHT_SUM 1e-10f   // For weight normalization
#define EPSILON_LENGTH_SQ 1e-20f    // Minimum dot(v,v) before rsqrt to avoid inf on degenerate vectors

#define DEPTH_SKY_SENTINEL 999999.0f  // Linearized depth sentinel for sky/unmapped pixels (beyond any real geometry)

// GetWaterData returns .w = INT_MIN (~-2.147e9) when the tile is out of the 5x5 grid.
// Use this threshold to test for "no water body present": waterHeight > WATER_HEIGHT_NO_TILE_SENTINEL.
#define WATER_HEIGHT_NO_TILE_SENTINEL -1e9f

namespace Math
{
	static const float4x4 IdentityMatrix = {
		{ 1, 0, 0, 0 },
		{ 0, 1, 0, 0 },
		{ 0, 0, 1, 0 },
		{ 0, 0, 0, 1 }
	};

	static const float PI = 3.1415926535897932384626433832795f;  // PI
	static const float HALF_PI = PI * 0.5f;                      // PI / 2
	static const float TAU = PI * 2.0f;                          // PI * 2
	static const float INV_PI = 1.0f / PI;                       // 1 / PI

	static const float GOLDEN_ANGLE = 2.39996322972865332;  // PI * (3 - sqrt(5))

	// Uniform (area-weighted) hemisphere/sphere sampler, stratified via Hammersley.
	// ap == 1 -> hemisphere, solid angle 2PI.  ap == 2 -> full sphere.
	// domain: xy[-1,1], z[1-ap, 1].
	float3 UniformSphereSample(float i, float n, float ap = 2.0)
	{
		// radical inverse base 2 (van der Corput) for the second dimension
		uint bits = uint(i);
		bits = (bits << 16) | (bits >> 16);
		bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
		bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
		bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
		bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
		float u2 = float(bits) * 2.3283064365386963e-10;  // / 2^32

		float u1 = (i + 0.5) / n;  // stratified first dim

		float cosT = lerp(1.0, 1.0 - ap, u2);  // uniform in z -> area-uniform
		float phi = u1 * Math::PI * 2;

		float3 Out;
		sincos(phi, Out.y, Out.x);
		Out.xy *= sqrt(saturate(1.0 - cosT * cosT));
		Out.z = cosT;
		return Out;
	}

	// Fibonacci sample direction over hemisphere
	// gives solid angle 2PI at input ap == 1
	// domain: xy[-1, 1] z[1-ap, 1]
	float3 FibonacciHemisphere(float i, float n, float ap)
	{
		float cosT = lerp(1.0, 1 - ap, (i + 0.5) / n);  // ap == 2 gives full sphere
		float3 Out = float3(0, 0, cosT);
		sincos(i * GOLDEN_ANGLE, Out.y, Out.x);
		Out.xy *= sqrt(saturate(1.0 - cosT * cosT));
		return Out;
	}

	//  orthonormal basis (Frisvad, branchless)
	float3x3 BuildTBN(float3 dir)
	{
		float sign = dir.z >= 0.0 ? 1.0 : -1.0;
		float a = -1.0 / (sign + dir.z);
		float bb = dir.x * dir.y * a;
		float3 T = float3(1.0 + sign * dir.x * dir.x * a, sign * bb, -sign * dir.x);
		float3 B = float3(bb, sign + dir.y * dir.y * a, -dir.y);
		return float3x3(T, B, dir);
	}

}

#endif  //__MATH_DEPENDENCY_HLSL__