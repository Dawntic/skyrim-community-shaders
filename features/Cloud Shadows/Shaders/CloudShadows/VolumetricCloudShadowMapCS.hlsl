// Beer-Lambert cloud shadow map for the Physical Sky volumetric cloud system.
//
// The vanilla cloud shadow path rasterises the sky cloud planes into a cubemap
// and reads back an opacity. Physical Sky replaces those planes with a raymarched
// density field, so there is nothing to rasterise -- this pass integrates the
// field directly instead.
//
// Layout: a camera-centred, world-axis-aligned square in the horizontal plane at
// the cloud layer base. Each texel stores the Beer-Lambert transmittance
// exp(-tau) of the directional light along its path up through the layer, where
// tau is the optical depth (density x extinction, integrated over path length).
// A receiver only has to intersect its light ray with that plane and read one
// texel, which is what CloudShadows.hlsli does.
//
// Transmittance -- not tau -- is what gets stored, because the mip chain and the
// blur must average the quantity that is finally applied to the lighting.
// Averaging tau and exponentiating afterwards is a different (darker) number by
// Jensen's inequality, and the whole point of the mip chain is to make a filtered
// read equal the mean shadow over the footprint.
//
// Three permutations share this file:
//   CLOUD_SHADOW_TRACE      -- raymarch the cloud volume, write mip 0
//   CLOUD_SHADOW_BLUR       -- separable Gaussian over mip 0 (+ BLUR_VERTICAL)
//   CLOUD_SHADOW_DOWNSAMPLE -- binomial 4x4 tent, one mip level per dispatch

#ifndef COMPUTESHADER
#define COMPUTESHADER
#endif

#include "Common/Math.hlsli"
#include "PhysicalSky/CloudCommon.hlsli"

cbuffer CloudShadowMapCB : register(b2)
{
	float3 LightDir;     // normalized, points TOWARD the directional light
	float RcpStepCount;  //

	float2 MapCenterKm;  // world XY of the map centre, km
	float MapExtentKm;   // half-size of the covered square, km
	uint StepCount;      //

	uint2 DstDim;       // destination dimensions (mip 0 for the trace pass)
	float MaxPathKm;    // path-length clamp; keeps a grazing light finite
	float cloudMapPad;  //
};

RWTexture2D<float> RWShadowMap : register(u0);
// t0-t19 are claimed by CloudCommon.hlsli's cloud volume textures.
Texture2D<float> ShadowSrc : register(t20);

#ifdef CLOUD_SHADOW_TRACE

// A light this close to the horizon would need a path many times the layer
// thickness to escape, and its own illumination has already faded out. Keep in
// sync with CloudShadows::kMinLightCos on the C++ side.
static const float kMinLightCos = 0.02;

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= DstDim))
		return;

	float2 uv = (tid + 0.5) / (float2)DstDim;
	float3 pos = float3(MapCenterKm + (uv * 2.0 - 1.0) * MapExtentKm, bottomRadius);

	float tau = 0.0;
	if (LightDir.z > kMinLightCos) {
		// Vertical extent / cos(zenith) is the geometric path through a flat
		// layer; the clamp bounds the sample spacing as the light gets low.
		float dt = min((topRadius - bottomRadius) / LightDir.z, MaxPathKm) * RcpStepCount;

		// Midpoint rule, no jitter: a temporally varying offset would make the
		// shadows crawl, and this map is blurred rather than accumulated.
		pos += LightDir * (0.5 * dt);

		for (uint i = 0; i < StepCount; ++i) {
			// Unsaturated, so leaving through the layer top ends the march.
			float height = LinearStep(bottomRadius, topRadius, pos.z);
			if (height > 1.0)
				break;

			// Base profile only. The detail octaves are far smaller than one
			// texel of this map, so marching them would alias rather than add
			// detail -- the in-cloud sun march in Clouds.hlsl skips them for the
			// same reason.
			tau += GetCloudProfile(pos, saturate(height));
			pos += LightDir * dt;
		}

		tau *= dt * CloudExtinction;
	}

	RWShadowMap[tid] = exp(-tau);
}
#endif

#ifdef CLOUD_SHADOW_BLUR

// 9-tap Gaussian, sigma ~2 texels. The trace resolves cloud edges down to a
// single texel; without this the mip chain inherits that step and distant
// receivers shimmer as the footprint slides across it.
static const float kBlurWeights[5] = { 0.227027,
	0.1945946,
	0.1216216,
	0.054054,
	0.016216 };

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= DstDim))
		return;

#ifdef BLUR_VERTICAL
	const int2 tapStep = int2(0, 1);
#else
	const int2 tapStep = int2(1, 0);
#endif

	const int2 maxCoord = (int2)DstDim - 1;

	float sum = ShadowSrc.Load(int3((int2)tid, 0)) * kBlurWeights[0];
	[unroll] for (int i = 1; i < 5; ++i)
	{
		sum += ShadowSrc.Load(int3(clamp((int2)tid + tapStep * i, 0, maxCoord), 0)) * kBlurWeights[i];
		sum += ShadowSrc.Load(int3(clamp((int2)tid - tapStep * i, 0, maxCoord), 0)) * kBlurWeights[i];
	}

	RWShadowMap[tid] = sum;
}
#endif

#ifdef CLOUD_SHADOW_DOWNSAMPLE

	[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID)
{
	if (any(tid >= DstDim))
		return;

	// Binomial 4x4 tent rather than a 2x2 box: the box leaves enough energy above
	// the new Nyquist limit that trilinear blending between two levels pops as the
	// camera moves.
	const float w[4] = { 0.125, 0.375, 0.375, 0.125 };
	const int2 srcMaxCoord = (int2)DstDim * 2 - 1;
	const int2 base = (int2)tid * 2 - 1;

	float sum = 0.0;
	[unroll] for (int y = 0; y < 4; ++y)
	{
		[unroll] for (int x = 0; x < 4; ++x)
		{
			sum += w[x] * w[y] * ShadowSrc.Load(int3(clamp(base + int2(x, y), 0, srcMaxCoord), 0));
		}
	}

	RWShadowMap[tid] = sum;
}
#endif
