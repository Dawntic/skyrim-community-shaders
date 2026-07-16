// SphericalHarmonicsRGB.hlsli
// Complementary float3 (RGB) helpers for sebh/HLSL-Spherical-Harmonics.
//
// The base library stores one scalar SH function per channel and asks you to
// duplicate every call three times. These helpers bundle the three channels
// into a single value (sh2RGB / sh3RGB) and reopen the SphericalHarmonics
// namespace so the RGB versions overload the existing names by argument type.
//
// Naming follows the base library: order-2 = no suffix, order-3 = "SH3".
// Constructors that take no arguments can't overload on return type, so they
// carry an explicit "2RGB" / "3RGB" tag.
//
// Adjust the include path below to match your project layout.

#ifndef __SPHERICAL_HARMONICS_RGB_HLSL__
#define __SPHERICAL_HARMONICS_RGB_HLSL__

#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

struct sh2RGB
{
	sh2 r;
	sh2 g;
	sh2 b;
};

struct sh3RGB
{
	sh3 r;
	sh3 g;
	sh3 b;
};

namespace SH
{
	// ------------------------------------------------------------------ //
	// Order 2 (sh2RGB)                                                    //
	// ------------------------------------------------------------------ //

	sh2RGB Zero2RGB()
	{
		sh2RGB result;
		result.r = ZeroSH2();
		result.g = ZeroSH2();
		result.b = ZeroSH2();
		return result;
	}

	// Project a single (direction, color) radiance sample into RGB SH.
	// Accumulate these in your integration loop, then Scale by 4*PI/sampleCount.
	sh2RGB Project(float3 dir, float3 color)
	{
		sh2 basis = Evaluate(dir);
		sh2RGB result;
		result.r = Scale(basis, color.r);
		result.g = Scale(basis, color.g);
		result.b = Scale(basis, color.b);
		return result;
	}

	sh2RGB Add(sh2RGB a, sh2RGB b)
	{
		sh2RGB result;
		result.r = a.r + b.r;
		result.g = a.g + b.g;
		result.b = a.b + b.b;
		return result;
	}

	sh2RGB Scale(sh2RGB sh, float v)
	{
		sh2RGB result;
		result.r = sh.r * v;
		result.g = sh.g * v;
		result.b = sh.b * v;
		return result;
	}

	// Per-channel scale (e.g. a color tint).
	sh2RGB Scale(sh2RGB sh, float3 v)
	{
		sh2RGB result;
		result.r = sh.r * v.r;
		result.g = sh.g * v.g;
		result.b = sh.b * v.b;
		return result;
	}

	sh2RGB Scale(sh2 sh, float3 v)
	{
		sh2RGB result;
		result.r = sh * v.r;
		result.g = sh * v.g;
		result.b = sh * v.b;
		return result;
	}

	// Band-limited radiance in direction dir ("what color is the sky there").
	float3 Unproject(sh2RGB sh, float3 dir)
	{
		sh2 basis = Evaluate(dir);
		return float3(dot(sh.r, basis), dot(sh.g, basis), dot(sh.b, basis));
	}

	// Channel-wise product integral against a single monochrome kernel.
	// Pass Evaluate(dir) for radiance, EvaluateCosineLobe(N) for irradiance.
	float3 FuncProductIntegral(sh2RGB sh, sh2 kernel)
	{
		return float3(dot(sh.r, kernel), dot(sh.g, kernel), dot(sh.b, kernel));
	}

	// Full RGB-vs-RGB product integral (each channel dotted independently).
	float3 FuncProductIntegral(sh2RGB a, sh2RGB b)
	{
		return float3(dot(a.r, b.r), dot(a.g, b.g), dot(a.b, b.b));
	}

	sh2RGB DiffuseConvolution(sh2RGB sh)
	{
		sh2RGB result;
		result.r = DiffuseConvolution(sh.r);
		result.g = DiffuseConvolution(sh.g);
		result.b = DiffuseConvolution(sh.b);
		return result;
	}

	sh2RGB HanningConvolution(sh2RGB sh, float w)
	{
		sh2RGB result;
		result.r = HanningConvolution(sh.r, w);
		result.g = HanningConvolution(sh.g, w);
		result.b = HanningConvolution(sh.b, w);
		return result;
	}

	// Irradiance E arriving at a surface with normal N (cosine-weighted integral).
	float3 Irradiance(sh2RGB sh, float3 N)
	{
		return FuncProductIntegral(sh, EvaluateCosineLobe(N));
	}

	// Outgoing radiance of a white Lambertian surface (albedo 1) = E / PI.
	float3 DiffuseRadiance(sh2RGB sh, float3 N)
	{
		return max(0, Irradiance(sh, N) / Math::PI);
	}

	// ------------------------------------------------------------------ //
	// Order 3 (sh3RGB)                                                    //
	// ------------------------------------------------------------------ //

	sh3RGB Zero3RGB()
	{
		sh3RGB result;
		[unroll] for (int i = 0; i < 9; i++)
		{
			result.r.coeff[i] = 0.0;
			result.g.coeff[i] = 0.0;
			result.b.coeff[i] = 0.0;
		}
		return result;
	}

	sh3RGB ProjectSH3(float3 dir, float3 color)
	{
		sh3 basis = EvaluateSH3(dir);
		sh3RGB result;
		result.r = ScaleSH3(basis, color.r);
		result.g = ScaleSH3(basis, color.g);
		result.b = ScaleSH3(basis, color.b);
		return result;
	}

	sh3RGB AddSH3(sh3RGB a, sh3RGB b)
	{
		sh3RGB result;
		result.r = AddSH3(a.r, b.r);
		result.g = AddSH3(a.g, b.g);
		result.b = AddSH3(a.b, b.b);
		return result;
	}

	sh3RGB ScaleSH3(sh3RGB sh, float v)
	{
		sh3RGB result;
		result.r = ScaleSH3(sh.r, v);
		result.g = ScaleSH3(sh.g, v);
		result.b = ScaleSH3(sh.b, v);
		return result;
	}

	sh3RGB ScaleSH3(sh3RGB sh, float3 v)
	{
		sh3RGB result;
		result.r = ScaleSH3(sh.r, v.r);
		result.g = ScaleSH3(sh.g, v.g);
		result.b = ScaleSH3(sh.b, v.b);
		return result;
	}

	float3 UnprojectSH3(sh3RGB sh, float3 dir)
	{
		sh3 basis = EvaluateSH3(dir);
		float3 result = 0.0;
		[unroll] for (int i = 0; i < 9; i++)
			result += float3(sh.r.coeff[i], sh.g.coeff[i], sh.b.coeff[i]) * basis.coeff[i];
		return result;
	}

	// Channel-wise product integral against a single monochrome kernel.
	float3 ProductIntegralSH3(sh3RGB sh, sh3 kernel)
	{
		float3 result = 0.0;
		[unroll] for (int i = 0; i < 9; i++)
			result += float3(sh.r.coeff[i], sh.g.coeff[i], sh.b.coeff[i]) * kernel.coeff[i];
		return result;
	}

	float3 ProductIntegralSH3(sh3RGB a, sh3RGB b)
	{
		float3 result = 0.0;
		[unroll] for (int i = 0; i < 9; i++)
			result += float3(a.r.coeff[i] * b.r.coeff[i],
				a.g.coeff[i] * b.g.coeff[i],
				a.b.coeff[i] * b.b.coeff[i]);
		return result;
	}

	sh3RGB LerpSH3(sh3RGB a, sh3RGB b, float t)
	{
		sh3RGB result;
		result.r = LerpSH3(a.r, b.r, t);
		result.g = LerpSH3(a.g, b.g, t);
		result.b = LerpSH3(a.b, b.b, t);
		return result;
	}

	float3 IrradianceSH3(sh3RGB sh, float3 N)
	{
		return ProductIntegralSH3(sh, EvaluateCosineLobeSH3(N));
	}

	float3 DiffuseRadianceSH3(sh3RGB sh, float3 N)
	{
		return max(float3(0, 0, 0), IrradianceSH3(sh, N) / Math::PI);
	}

	// ------------------------------------------------------------------ //
	// Packing                                                            //
	// ------------------------------------------------------------------ //

	// sh2RGB -> 3 consecutive RGBA texels (one per channel). Matches the common
	// 3-pixel IBL layout: texel base+0 = R coeffs, base+1 = G, base+2 = B.
	void PackSH2RGB(sh2RGB sh, uint2 probePos, RWTexture2DArray<float4> arr)
	{
		arr[uint3(probePos, 0)] = sh.r;
		arr[uint3(probePos, 1)] = sh.g;
		arr[uint3(probePos, 2)] = sh.b;
	}

	sh2RGB UnpackSH2RGB(uint2 probePos, Texture2DArray<float4> arr)
	{
		sh2RGB sh;
		sh.r = arr[uint3(probePos, 0)];
		sh.g = arr[uint3(probePos, 1)];
		sh.b = arr[uint3(probePos, 2)];
		return sh;
	}

	// sh3RGB -> Texture2DArray with 9 slices, each float4 = (r_i, g_i, b_i, 0).
	// (Differs from the base PackSH3, which packs ONE channel's 9 coeffs into 3
	// slices; RGB needs the per-coefficient layout below.)
	void PackSH3RGB(sh3RGB sh, uint2 probePos, RWTexture2DArray<float4> arr)
	{
		[unroll] for (int i = 0; i < 9; i++)
			arr[uint3(probePos, i)] = float4(sh.r.coeff[i], sh.g.coeff[i], sh.b.coeff[i], 0);
	}

	sh3RGB UnpackSH3RGB(uint2 probePos, Texture2DArray<float4> arr)
	{
		sh3RGB sh;
		[unroll] for (int i = 0; i < 9; i++)
		{
			float4 c = arr[uint3(probePos, i)];
			sh.r.coeff[i] = c.x;
			sh.g.coeff[i] = c.y;
			sh.b.coeff[i] = c.z;
		}
		return sh;
	}
}

#endif  // __SPHERICAL_HARMONICS_RGB_HLSL__
