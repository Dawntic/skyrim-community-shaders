// SphericalHarmonics.hlsl from https://github.com/sebh/HLSL-Spherical-Harmonics

// Great documents about spherical harmonics:
// [1]  http://www.cse.chalmers.se/~uffe/xjobb/Readings/GlobalIllumination/Spherical%20Harmonic%20Lighting%20-%20the%20gritty%20details.pdf
// [2]  https://www.ppsloan.org/publications/StupidSH36.pdf
// [3]  https://cseweb.ucsd.edu/~ravir/papers/envmap/envmap.pdf
// [4]  https://d3cw3dd2w32x2b.cloudfront.net/wp-content/uploads/2011/06/10-14.pdf
// [5]  https://github.com/kayru/Probulator
// [6]  https://www.ppsloan.org/publications/SHJCGT.pdf
// [7]  http://www.patapom.com/blog/SHPortal/
// [8]  https://grahamhazel.com/blog/2017/12/22/converting-sh-radiance-to-irradiance/
// [9]  http://www.ppsloan.org/publications/shdering.pdf
// [10] http://limbicsoft.com/volker/prosem_paper.pdf
// [11] https://bartwronski.files.wordpress.com/2014/08/bwronski_volumetric_fog_siggraph2014.pdf
//

//
// Provided functions are commented. A "SH function" means a "spherical function represented as spherical harmonics".
// You can also find a FAQ below.
//
//**** HOW TO PROJECT RADIANCE FROM A SPHERE INTO SH?
//
//		// Initialise sh to 0
//		sh2 shR = shZero();
//		sh2 shG = shZero();
//		sh2 shB = shZero();
//
//		// Accumulate coefficients according to surounding direction/color tuples.
//		for (float az = 0.5f; az < axisSampleCount; az += 1.0f)
//			for (float ze = 0.5f; ze < axisSampleCount; ze += 1.0f)
//			{
//				float3 rayDir = shGetUniformSphereSample(az / axisSampleCount, ze / axisSampleCount);
//				float3 color = [...];
//
//				sh2 sh = shEvaluate(rayDir);
//				shR = shAdd(shR, shScale(sh, color.r));
//				shG = shAdd(shG, shScale(sh, color.g));
//				shB = shAdd(shB, shScale(sh, color.b));
//			}
//
//		// integrating over a sphere so each sample has a weight of 4*PI/samplecount (uniform solid angle, for each sample)
//		float shFactor = 4.0 * Math::PI / (axisSampleCount * axisSampleCount);
//		shR = shScale(shR, shFactor );
//		shG = shScale(shG, shFactor );
//		shB = shScale(shB, shFactor );
//
//
//**** HOW TO VIZUALISE A SPHERICAL FUNCTION REPRESENTED AS SH?
//
//		sh2 shR = fromSomewhere.Load(...);
//		sh2 shG = fromSomewhere.Load(...);
//		sh2 shB = fromSomewhere.Load(...);
//		float3 rayDir = compute(...);										// the direction for which you want to know the color
//		float3 rgbColor = max(0.0f, shUnproject(shR, shG, shB, rayDir));	// A "max" is usually recomended to avoid negative values (can happen with SH)
//

#ifndef __SPHERICAL_HARMONICS_DEPENDENCY_HLSL__
#define __SPHERICAL_HARMONICS_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"

#define sh2 float4
struct sh3
{
	float coeff[9];
};

namespace SH
{
	// Generates a uniform distribution of directions over a unit sphere.
	// Adapted from http://www.pbr-book.org/3ed-2018/Monte_Carlo_Integration/2D_Sampling_with_Multidimensional_Transformations.html#fragment-SamplingFunctionDefinitions-6
	// azimuthX and zenithY are both in [0, 1]. You can use random value, stratified, etc.
	// Top and bottom sphere pole (+-zenith) are along the Y axis.
	float3 GetUniformSphereSample(float azimuthX, float zenithY)
	{
		float phi = 2.0f * Math::PI * azimuthX;
		float z = 1.0f - 2.0f * zenithY;
		float r = sqrt(max(0.0f, 1.0f - z * z));
		return float3(r * cos(phi), z, r * sin(phi));
	}

	sh2 ZeroSH2()
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	sh2 UnitSH2()
	{
		return float4(sqrt(4.0 * Math::PI), 0, 0, 0);
	}

	sh2 HemisphereSH2()
	{
		sh2 HemiUp = SH::ZeroSH2();
		HemiUp.x = 1.77245385f;  // 0.28209479 * 2PI   (same DC as any hemisphere: half the sphere)
		HemiUp.z = 1.53499006f;  // -(0.48860251 * PI)  negative z -> lobe points down
		return HemiUp;
	}

	// Evaluates spherical harmonics basis for a direction dir.
	// This follows [2] Appendix A2 order when storing in x, y, z and w.
	// (evaluating the associated Legendre polynomials using the polynomial forms)
	sh2 Evaluate(float3 dir)
	{
		sh2 result;
		result.x = 0.28209479177387814347403972578039f;           // L=0 , M= 0
		result.y = -0.48860251190291992158638462283836f * dir.y;  // L=1 , M=-1
		result.z = 0.48860251190291992158638462283836f * dir.z;   // L=1 , M= 0
		result.w = -0.48860251190291992158638462283836f * dir.x;  // L=1 , M= 1
		return result;
	}

	// Recovers the value of a SH function in the direction dir.
	float Unproject(sh2 functionSh, float3 dir)
	{
		sh2 sh = Evaluate(dir);
		return dot(functionSh, sh);
	}

	float3 Unproject(sh2 functionShX, sh2 functionShY, sh2 functionShZ, float3 dir)
	{
		sh2 sh = Evaluate(dir);
		return float3(dot(functionShX, sh), dot(functionShY, sh), dot(functionShZ, sh));
	}

	// Projects a cosine lobe function, with peak value in direction dir, into SH. (from [4])
	// The integral over the unit sphere of the SH representation is PI.
	sh2 EvaluateCosineLobe(float3 dir)
	{
		sh2 result;
		result.x = 0.8862269254527580137f;           // L=0 , M= 0
		result.y = -1.0233267079464884885f * dir.y;  // L=1 , M=-1
		result.z = 1.0233267079464884885f * dir.z;   // L=1 , M= 0
		result.w = -1.0233267079464884885f * dir.x;  // L=1 , M= 1
		return result;
	}

	// Projects a Henyey-Greenstein phase function, with peak value in direction dir, into SH. (from [11])
	// The integral over the unit sphere of the SH representation is 1.
	sh2 EvaluatePhaseHG(float3 dir, float g)
	{
		sh2 result;
		const float factor = 0.48860251190291992158638462283836 * g;
		result.x = 0.28209479177387814347403972578039;  // L=0 , M= 0
		result.y = -factor * dir.y;                     // L=1 , M=-1
		result.z = factor * dir.z;                      // L=1 , M= 0
		result.w = -factor * dir.x;                     // L=1 , M= 1
		return result;
	}

	// Adds two SH functions together.
	sh2 Add(sh2 shL, sh2 shR)
	{
		return shL + shR;
	}

	// Scales a SH function uniformly by v.
	sh2 Scale(sh2 sh, float v)
	{
		return sh * v;
	}

	// Integrates the product of two SH functions over the unit sphere.
	float FuncProductIntegral(sh2 shL, sh2 shR)
	{
		return dot(shL, shR);
	}

	// Operates a rotation of a SH function.
	sh2 Rotate(sh2 sh, float3x3 rotation)
	{
		// TODO verify and optimize
		sh2 result;
		result.x = sh.x;
		float3 tmp = float3(sh.w, sh.y, sh.z);  // undo direction component shuffle to match source/function space
		result.yzw = mul(tmp, rotation).yzx;    // apply rotation and re-shuffle
		return result;
	}

	sh2 Product(sh2 a, sh2 b)
	{
		sh2 result = float4(dot(a, b), a.x * b.yzw + b.x * a.yzw);
		result *= 1.0 / (2.0 * sqrt(Math::PI));
		return result;
	}

	// Convolves a SH function using a Hanning filtering. This helps reducing ringing and negative values. (from [2], Windowing p.16)
	// A lower value of w will reduce ringing (like the frequency of a filter)
	sh2 HanningConvolution(sh2 sh, float w)
	{
		sh2 result = sh;
		float invW = 1.0 / w;
		float factorBand1 = (1.0 + cos(Math::PI * invW)) / 2.0f;
		result.y *= factorBand1;
		result.z *= factorBand1;
		result.w *= factorBand1;
		return result;
	}

	// Convolves a SH function using a cosine lob. This is tipically used to transform radiance to irradiance. (from [3], eq.7 & eq.8)
	sh2 DiffuseConvolution(sh2 sh)
	{
		sh2 result = sh;
		// L0
		result.x *= Math::PI;
		// L1
		result.yzw *= 2.0943951023931954923f;
		return result;
	}

	// Author: ProfJack
	// Constructs the SH of an approximate specular lobe
	sh2 FauxSpecularLobe(float3 N, float3 V, float roughness)
	{
		// https://www.gdcvault.com/play/1026701/Fast-Denoising-With-Self-Stabilizing
		// get dominant ggx reflection direction
		float f = (1 - roughness) * (sqrt(1 - roughness) + roughness);
		float3 R = reflect(-V, N);
		float3 D = lerp(N, R, f);
		float3 dominantDir = normalize(D);

		// lobe half angle
		// credit: Olivier Therrien
		float roughness2 = roughness * roughness;
		float halfAngle = clamp(4.1679 * roughness2 * roughness2 - 9.0127 * roughness2 * roughness + 4.6161 * roughness2 + 1.7048 * roughness + 0.1, 0, Math::HALF_PI);
		float lerpFactor = halfAngle / Math::HALF_PI;
		sh2 directional = SH::Evaluate(dominantDir);
		sh2 cosineLobe = SH::EvaluateCosineLobe(dominantDir) / Math::PI;
		sh2 result = SH::Add(SH::Scale(directional, lerpFactor), SH::Scale(cosineLobe, 1 - lerpFactor));

		return result;
	}

	// Hallucinate zonal harmonics for diffuse lighting with more contrast
	// http://torust.me/ZH3.pdf
	float SHHallucinateZH3Irradiance(sh2 inSH, float3 direction)
	{
		float3 zonalAxis = normalize(float3(inSH.w, inSH.y, inSH.z));
		float ratio = 0.0;
		ratio = abs(dot(float3(-inSH.w, -inSH.y, inSH.z), zonalAxis));
		ratio /= inSH.x;
		float zonalL2Coeff = inSH.x * (0.08f * ratio + 0.6f * ratio * ratio);  // Curve-fit; Section 3.4.3
		float fZ = dot(zonalAxis, direction);
		float zhDir = sqrt(5.0f / (16.0f * Math::PI)) * (3.0f * fZ * fZ - 1.0f);
		// Convolve inSH with the normalized cosine kernel (multiply the L1 band by the zonal scale 2/3), then dot with
		// inSH(direction) for linear inSH (Equation 5).
		float result = SH::FuncProductIntegral(inSH, SH::EvaluateCosineLobe(direction));
		// Add irradiance from the ZH3 term. zonalL2Coeff is the ZH3 coefficient for a radiance signal, so we need to
		// multiply by 1/4 (the L2 zonal scale for a normalized clamped cosine kernel) to evaluate irradiance.
		result += 0.25f * zonalL2Coeff * zhDir;
		return max(0, result);
	}

	// 3rd Order Functions //
	sh3 AddSH3(sh3 shL, sh3 shR)
	{
		sh3 result;
		[unroll] for (int i = 0; i < 9; i++)
			result.coeff[i] = shL.coeff[i] + shR.coeff[i];
		return result;
	}

	sh3 ScaleSH3(sh3 sh, float v)
	{
		sh3 result;
		[unroll] for (int i = 0; i < 9; i++)
			result.coeff[i] = sh.coeff[i] * v;
		return result;
	}

	float ProductIntegralSH3(sh3 shL, sh3 shR)
	{
		float result = 0.0;
		[unroll] for (int i = 0; i < 9; i++)
			result += shL.coeff[i] * shR.coeff[i];
		return result;
	}

	sh3 EvaluateSH3(float3 dir)
	{
		sh3 result;

		// L=0, M=0
		result.coeff[0] = 0.28209479177387814f;

		// L=1, M=-1,0,1
		result.coeff[1] = -0.48860251190291992f * dir.y;
		result.coeff[2] = 0.48860251190291992f * dir.z;
		result.coeff[3] = -0.48860251190291992f * dir.x;

		// L=2, M=-2,-1,0,1,2
		result.coeff[4] = 1.09254843059207908f * dir.x * dir.y;
		result.coeff[5] = -1.09254843059207908f * dir.y * dir.z;
		result.coeff[6] = 0.31539156525252001f * (3.0f * dir.z * dir.z - 1.0f);
		result.coeff[7] = -1.09254843059207908f * dir.x * dir.z;
		result.coeff[8] = 0.54627421529603954f * (dir.x * dir.x - dir.y * dir.y);

		return result;
	}

	sh3 EvaluateCosineLobeSH3(float3 dir)
	{
		sh3 result;
		// L=0, M= 0
		result.coeff[0] = 0.8862269254527580137f;
		// L=1, M=-1, 0, 1
		result.coeff[1] = -1.0233267079464884885f * dir.y;
		result.coeff[2] = 1.0233267079464884885f * dir.z;
		result.coeff[3] = -1.0233267079464884885f * dir.x;
		// L=2, M=-2, -1, 0, 1, 2
		result.coeff[4] = 0.8580855308097834790f * dir.x * dir.y;
		result.coeff[5] = -0.8580855308097834790f * dir.y * dir.z;
		result.coeff[6] = 0.2477079561003757808f * (3.0f * dir.z * dir.z - 1.0f);
		result.coeff[7] = -0.8580855308097834790f * dir.x * dir.z;
		result.coeff[8] = 0.4290427654048917395f * (dir.x * dir.x - dir.y * dir.y);
		return result;
	}

	float UnprojectSH3(sh3 functionSh, float3 dir)
	{
		sh3 sh = EvaluateSH3(dir);
		float result = 0.0;
		[unroll] for (int i = 0; i < 9; i++)
			result += functionSh.coeff[i] * sh.coeff[i];
		return result;
	}

	float3 UnprojectSH3(sh3 functionShR, sh3 functionShG, sh3 functionShB, float3 dir)
	{
		sh3 sh = EvaluateSH3(dir);
		float3 result = 0.0;
		[unroll] for (int i = 0; i < 9; i++)
			result += float3(functionShR.coeff[i], functionShG.coeff[i], functionShB.coeff[i]) * sh.coeff[i];
		return result;
	}

	sh3 UnitSH3()
	{
		sh3 result = (sh3)0;
		result.coeff[0] = sqrt(4 * Math::PI);
		return result;
	}

	sh3 LerpSH3(sh3 a, sh3 b, float t)
	{
		sh3 result;
		[unroll] for (int i = 0; i < 9; i++)
			result.coeff[i] = lerp(a.coeff[i], b.coeff[i], t);
		return result;
	}

	sh3 UnpackSH3(uint2 ProbePos, Texture2DArray Array)
	{
		sh3 OutputSH;

		float3 band0 = Array[uint3(ProbePos.xy, 0)].xyz;
		float3 band1 = Array[uint3(ProbePos.xy, 1)].xyz;
		float3 band2 = Array[uint3(ProbePos.xy, 2)].xyz;

		OutputSH.coeff[0] = band0.x;
		OutputSH.coeff[1] = band0.y;
		OutputSH.coeff[2] = band0.z;

		OutputSH.coeff[3] = band1.x;
		OutputSH.coeff[4] = band1.y;
		OutputSH.coeff[5] = band1.z;

		OutputSH.coeff[6] = band2.x;
		OutputSH.coeff[7] = band2.y;
		OutputSH.coeff[8] = band2.z;

		return OutputSH;
	}

	// ScalarPackSH3
	void PackSH3(sh3 SHdata, uint2 ProbePos, RWTexture2DArray<float4> Array)
	{
		Array[uint3(ProbePos.xy, 0)] = float4(SHdata.coeff[0], SHdata.coeff[1], SHdata.coeff[2], 0);
		Array[uint3(ProbePos.xy, 1)] = float4(SHdata.coeff[3], SHdata.coeff[4], SHdata.coeff[5], 0);
		Array[uint3(ProbePos.xy, 2)] = float4(SHdata.coeff[6], SHdata.coeff[7], SHdata.coeff[8], 0);
	}

	sh3 FauxSpecularLobeSH3(float3 N, float3 V, float roughness)
	{
		// https://www.gdcvault.com/play/1026701/Fast-Denoising-With-Self-Stabilizing
		// get dominant ggx reflection direction
		float f = (1 - roughness) * (sqrt(1 - roughness) + roughness);
		float3 R = reflect(-V, N);
		float3 D = lerp(N, R, f);
		float3 dominantDir = normalize(D);

		// lobe half angle
		// credit: Olivier Therrien
		float roughness2 = roughness * roughness;
		float halfAngle = clamp(4.1679 * roughness2 * roughness2 - 9.0127 * roughness2 * roughness + 4.6161 * roughness2 + 1.7048 * roughness + 0.1, 0, Math::HALF_PI);
		float lerpFactor = halfAngle / Math::HALF_PI;

		sh3 directional = SH::EvaluateSH3(dominantDir);
		sh3 cosineLobe = SH::ScaleSH3(SH::EvaluateCosineLobeSH3(dominantDir), rcp(Math::PI));
		sh3 result = SH::AddSH3(SH::ScaleSH3(directional, lerpFactor), SH::ScaleSH3(cosineLobe, 1 - lerpFactor));

		return result;
	}

}

#endif  // __SPHERICAL_HARMONICS_DEPENDENCY_HLSL__
