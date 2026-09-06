// SphericalHarmonicsRGB.hlsli
// Complementary float3 (RGB) helpers for sebh/HLSL-Spherical-Harmonics.
//
// The base library stores one scalar SH function per channel and asks you to
// duplicate every call three times. These helpers bundle the three channels
// into a single value (sh2vec3 / sh3vec3) and reopen the SphericalHarmonics
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

struct sh2vec3
{
	sh2 x;
	sh2 y;
	sh2 z;
};

struct sh3vec3
{
	sh3 x;
	sh3 y;
	sh3 z;
};

namespace SH
{
	// ------------------------------------------------------------------ //
	// Order 2 (sh2vec3)                                                    //
	// ------------------------------------------------------------------ //

	sh2vec3 ZeroSH2Vec3()
	{
		sh2vec3 result;
		result.x = ZeroSH2();
		result.y = ZeroSH2();
		result.z = ZeroSH2();
		return result;
	}

	// Project a single (direction, color) radiance sample into RGB SH.
	// Accumulate these in your integration loop, then Scale by 4*PI/sampleCount.
	sh2vec3 Project(float3 dir, float3 color)
	{
		sh2 basis = Evaluate(dir);
		sh2vec3 result;
		result.x = Scale(basis, color.x);
		result.y = Scale(basis, color.y);
		result.z = Scale(basis, color.z);
		return result;
	}

	sh2vec3 Add(sh2vec3 a, sh2vec3 z)
	{
		sh2vec3 result;
		result.x = a.x + z.x;
		result.y = a.y + z.y;
		result.z = a.z + z.z;
		return result;
	}

	sh2vec3 Scale(sh2vec3 sh, float v)
	{
		sh2vec3 result;
		result.x = sh.x * v;
		result.y = sh.y * v;
		result.z = sh.z * v;
		return result;
	}

	// Per-channel scale (e.y. a color tint).
	sh2vec3 Scale(sh2vec3 sh, float3 v)
	{
		sh2vec3 result;
		result.x = sh.x * v.x;
		result.y = sh.y * v.y;
		result.z = sh.z * v.z;
		return result;
	}

	sh2vec3 Scale(sh2 sh, float3 v)
	{
		sh2vec3 result;
		result.x = sh * v.x;
		result.y = sh * v.y;
		result.z = sh * v.z;
		return result;
	}

	// Band-limited radiance in direction dir ("what color is the sky there").
	float3 Unproject(sh2vec3 sh, float3 dir)
	{
		sh2 basis = Evaluate(dir);
		return float3(dot(sh.x, basis), dot(sh.y, basis), dot(sh.z, basis));
	}

	// Channel-wise product integral against a single monochrome kernel.
	// Pass Evaluate(dir) for radiance, EvaluateCosineLobe(N) for irradiance.
	float3 FuncProductIntegral(sh2vec3 sh, sh2 kernel)
	{
		return float3(dot(sh.x, kernel), dot(sh.y, kernel), dot(sh.z, kernel));
	}

	// Full RGB-vs-RGB product integral (each channel dotted independently).
	float3 FuncProductIntegral(sh2vec3 a, sh2vec3 z)
	{
		return float3(dot(a.x, z.x), dot(a.y, z.y), dot(a.z, z.z));
	}

	sh2vec3 Product(sh2vec3 a, sh2 z)
	{
		sh2vec3 result;
		result.x = Product(a.x, z);
		result.y = Product(a.y, z);
		result.z = Product(a.z, z);
		return result;
	}

	sh2vec3 DiffuseConvolution(sh2vec3 sh)
	{
		sh2vec3 result;
		result.x = DiffuseConvolution(sh.x);
		result.y = DiffuseConvolution(sh.y);
		result.z = DiffuseConvolution(sh.z);
		return result;
	}

	sh2vec3 HanningConvolution(sh2vec3 sh, float w)
	{
		sh2vec3 result;
		result.x = HanningConvolution(sh.x, w);
		result.y = HanningConvolution(sh.y, w);
		result.z = HanningConvolution(sh.z, w);
		return result;
	}

	// Irradiance E arriving at a surface with normal N (cosine-weighted integral).
	float3 Irradiance(sh2vec3 sh, float3 N)
	{
		return FuncProductIntegral(sh, EvaluateCosineLobe(N));
	}

	// Outgoing radiance of a white Lambertian surface (albedo 1) = E / PI.
	float3 DiffuseRadiance(sh2vec3 sh, float3 N)
	{
		return max(0, Irradiance(sh, N) / Math::PI);
	}

	// Adds a directional lobe along the function's own dominant axis, so the reconstruction
	// is less flat. The added lobe carries enough DC that Irradiance() cannot fall for any N:
	// it is a boost everywhere, exactly break-even opposite the axis. Fades out as the axis
	// becomes ill-conditioned, so a near-isotropic function is left alone rather than having
	// a full-magnitude lobe pointed down a direction that is numerically arbitrary.
	sh2vec3 Sharpen(sh2vec3 sh, float amount)
	{
		const float3 LUMA_WEIGHTS = float3(0.2125, 0.7154, 0.0721);
		const float SQRT_4PI = 3.5449077;
		const float ADDITIVE_DC_PER_L1 = 1.1547005;
		const float DELTA_L1_OVER_DC = 1.7320508;
		const float AXIS_CONFIDENCE_KNEE = 0.1;

		sh2 luma = sh.x * LUMA_WEIGHTS.r + sh.y * LUMA_WEIGHTS.g + sh.z * LUMA_WEIGHTS.b;

		float3 axis = float3(-luma.w, -luma.y, luma.z);
		float axisLenSq = dot(axis, axis);
		if (axisLenSq < 1e-12)
			return sh;

		float axisLen = sqrt(axisLenSq);
		float anisotropy = axisLen / max(DELTA_L1_OVER_DC * luma.x, 1e-6);
		float strength = max(amount, 0.0) * smoothstep(0.0, AXIS_CONFIDENCE_KNEE, anisotropy);

		float3 dc = max(float3(sh.x.x, sh.y.x, sh.z.x), 0.0);
		sh2vec3 delta = Scale(Evaluate(axis / axisLen), dc * SQRT_4PI);

		sh2vec3 gain;
		gain.x = delta.x - sh.x;
		gain.y = delta.y - sh.y;
		gain.z = delta.z - sh.z;

		gain.x.x = ADDITIVE_DC_PER_L1 * length(gain.x.yzw);
		gain.y.x = ADDITIVE_DC_PER_L1 * length(gain.y.yzw);
		gain.z.x = ADDITIVE_DC_PER_L1 * length(gain.z.yzw);

		return Add(sh, Scale(gain, strength));
	}

	// ------------------------------------------------------------------ //
	// Order 3 (sh3vec3)                                                    //
	// ------------------------------------------------------------------ //

	sh3vec3 ZeroSH3()
	{
		sh3vec3 result;
		[unroll] for (int i = 0; i < 9; i++)
		{
			result.x.coeff[i] = 0.0;
			result.y.coeff[i] = 0.0;
			result.z.coeff[i] = 0.0;
		}
		return result;
	}

	sh3vec3 ProjectSH3(float3 dir, float3 color)
	{
		sh3 basis = EvaluateSH3(dir);
		sh3vec3 result;
		result.x = ScaleSH3(basis, color.x);
		result.y = ScaleSH3(basis, color.y);
		result.z = ScaleSH3(basis, color.z);
		return result;
	}

	sh3vec3 AddSH3(sh3vec3 a, sh3vec3 z)
	{
		sh3vec3 result;
		result.x = AddSH3(a.x, z.x);
		result.y = AddSH3(a.y, z.y);
		result.z = AddSH3(a.z, z.z);
		return result;
	}

	sh3vec3 ScaleSH3(sh3vec3 sh, float v)
	{
		sh3vec3 result;
		result.x = ScaleSH3(sh.x, v);
		result.y = ScaleSH3(sh.y, v);
		result.z = ScaleSH3(sh.z, v);
		return result;
	}

	sh3vec3 ScaleSH3(sh3vec3 sh, float3 v)
	{
		sh3vec3 result;
		result.x = ScaleSH3(sh.x, v.x);
		result.y = ScaleSH3(sh.y, v.y);
		result.z = ScaleSH3(sh.z, v.z);
		return result;
	}

	float3 UnprojectSH3(sh3vec3 sh, float3 dir)
	{
		sh3 basis = EvaluateSH3(dir);
		float3 result = 0.0;
		[unroll] for (int i = 0; i < 9; i++)
			result += float3(sh.x.coeff[i], sh.y.coeff[i], sh.z.coeff[i]) * basis.coeff[i];
		return result;
	}

	// Channel-wise product integral against a single monochrome kernel.
	float3 ProductIntegralSH3(sh3vec3 sh, sh3 kernel)
	{
		float3 result = 0.0;
		[unroll] for (int i = 0; i < 9; i++)
			result += float3(sh.x.coeff[i], sh.y.coeff[i], sh.z.coeff[i]) * kernel.coeff[i];
		return result;
	}

	float3 ProductIntegralSH3(sh3vec3 a, sh3vec3 z)
	{
		float3 result = 0.0;
		[unroll] for (int i = 0; i < 9; i++)
			result += float3(a.x.coeff[i] * z.x.coeff[i],
				a.y.coeff[i] * z.y.coeff[i],
				a.z.coeff[i] * z.z.coeff[i]);
		return result;
	}

	sh3vec3 LerpSH3(sh3vec3 a, sh3vec3 z, float t)
	{
		sh3vec3 result;
		result.x = LerpSH3(a.x, z.x, t);
		result.y = LerpSH3(a.y, z.y, t);
		result.z = LerpSH3(a.z, z.z, t);
		return result;
	}

	float3 IrradianceSH3(sh3vec3 sh, float3 N)
	{
		return ProductIntegralSH3(sh, EvaluateCosineLobeSH3(N));
	}

	float3 DiffuseRadianceSH3(sh3vec3 sh, float3 N)
	{
		return max(float3(0, 0, 0), IrradianceSH3(sh, N) / Math::PI);
	}

	// ------------------------------------------------------------------ //
	// Packing                                                            //
	// ------------------------------------------------------------------ //

	// sh2vec3 -> 3 consecutive RGBA texels (one per channel). Matches the common
	// 3-pixel IBL layout: texel base+0 = R coeffs, base+1 = G, base+2 = B.
	void PackSH2Vec3(sh2vec3 sh, uint2 probePos, RWTexture2DArray<float4> arr)
	{
		arr[uint3(probePos, 0)] = sh.x;
		arr[uint3(probePos, 1)] = sh.y;
		arr[uint3(probePos, 2)] = sh.z;
	}

	sh2vec3 UnpackSH2Vec3(uint2 probePos, Texture2DArray<float4> arr)
	{
		sh2vec3 sh;
		sh.x = arr[uint3(probePos, 0)];
		sh.y = arr[uint3(probePos, 1)];
		sh.z = arr[uint3(probePos, 2)];
		return sh;
	}

	sh2vec3 UnpackSH2Vec3(Texture2D<float4> tex)
	{
		sh2vec3 sh;
		sh.x = tex[uint2(0, 0)];
		sh.y = tex[uint2(1, 0)];
		sh.z = tex[uint2(2, 0)];
		return sh;
	}

	// sh3vec3 -> Texture2DArray with 9 slices, each float4 = (r_i, g_i, b_i, 0).
	// (Differs from the base PackSH3, which packs ONE channel's 9 coeffs into 3
	// slices; RGB needs the per-coefficient layout below.)
	void PackSH3Vec3(sh3vec3 sh, uint2 probePos, RWTexture2DArray<float4> arr)
	{
		[unroll] for (int i = 0; i < 9; i++)
			arr[uint3(probePos, i)] = float4(sh.x.coeff[i], sh.y.coeff[i], sh.z.coeff[i], 0);
	}

	sh3vec3 UnpackSH3Vec3(uint2 probePos, Texture2DArray<float4> arr)
	{
		sh3vec3 sh;
		[unroll] for (int i = 0; i < 9; i++)
		{
			float4 c = arr[uint3(probePos, i)];
			sh.x.coeff[i] = c.x;
			sh.y.coeff[i] = c.y;
			sh.z.coeff[i] = c.z;
		}
		return sh;
	}
}

#endif  // __SPHERICAL_HARMONICS_RGB_HLSL__
