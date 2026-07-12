#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif
#ifndef LUTGEN
#	define LUTGEN 0
#endif

#define PS_PREPASS_SAMPLERS
#define PS_PREPASS_RSRCS
#define OMIT_PS_NAMESPACE
#include "PhysicalSky/Common.hlsli"

#if LUTGEN == 3
RWTexture3D<float4> RWTexOutput : register(u0);
#else
RWTexture2D<float4> RWTexOutput : register(u0);
#endif

void rayMarch(
	float3 pos, float3 rayDir, 
#if LUTGEN == 0
	float3 sunDir,
	inout float3 tr
#elif LUTGEN == 1
	float3 sunDir,
	inout float3 tr,
	inout float3 lum, inout float3 lumFactor
#elif LUTGEN == 2 || LUTGEN == 5
	inout float3 tr,
	inout float3 lum
#elif LUTGEN == 3
	uint2 tid, uint depth,
	inout float3 tr,
	inout float3 lum
#endif
)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;

#if LUTGEN == 0
	const uint nsteps = 40;
#elif LUTGEN == 1
	const uint nsteps = 20;
#elif LUTGEN == 2 || LUTGEN == 5
	const uint nsteps = 30;
#else
	const uint nsteps = depth - 1;
#endif

#if LUTGEN > 1
	const float3 sunDir = data.sunDir;
#endif

	float tGround = RayIntersectSphere(pos, rayDir, 0, data.rPlanet);
#if LUTGEN == 0
	if (tGround > 0.0) {
		tr = 0;
		return;
	}
#endif

	float tAtmos = RayIntersectSphere(pos, rayDir, 0, data.rAtmosphere);
#if LUTGEN == 3
	float tMax = AP_MAX_DIST;
#else
	float tMax = tGround > 0 ? tGround : tAtmos;
#endif
	float dt = tMax / float(nsteps);
	float3 stride = dt * rayDir;

#if LUTGEN != 0
	float uSun = dot(rayDir, sunDir);
	float phaseAerosolSun = Phase::CornetteShanks(uSun, data.aerosolPhaseG);
	float phaseRayleighSun = Phase::Rayleigh(uSun);

#	if LUTGEN != 1
	float uMasser = dot(rayDir, data.masserDir);
	float phaseAerosolMasser = Phase::CornetteShanks(uMasser, data.aerosolPhaseG);
	float phaseRayleighMasser = Phase::Rayleigh(uMasser);

	float uSecunda = dot(rayDir, data.secundaDir);
	float phaseAerosolSecunda = Phase::CornetteShanks(uSecunda, data.aerosolPhaseG);
	float phaseRayleighSecunda = Phase::Rayleigh(uSecunda);
#	endif
#endif

	float3 curr_pos = pos;
	[loop] for (uint i = 0; i < nsteps; ++i)
	{
		curr_pos += stride;

		float rouRayleigh, rouAerosol, rouOzone;
		SampleAtmosphere(
			max(0.f, (length(curr_pos) - data.rPlanet)),
			rouRayleigh, rouAerosol, rouOzone);
		float3 muSRayleigh = rouRayleigh * data.rayleighScatter;
		float3 muSAerosol = rouAerosol * data.aerosolScatter;
		float3 extinction = muSRayleigh + muSAerosol +
		                    rouAerosol * data.aerosolAbsorption +
		                    rouOzone * data.ozoneAbsorption;

		float3 trSample = exp(-dt * extinction);

#if LUTGEN != 0
		float3 scatterFactor = (1 - trSample) / extinction;

		float3 scatterNoPhase = muSRayleigh + muSAerosol;
#	if LUTGEN == 1  // multiscatter
		float3 fScatter = scatterNoPhase * scatterFactor;
		lumFactor += tr * fScatter;
#	endif

		float2 lutUvSun = TrLutUvPlanet(curr_pos, sunDir);
		float3 trSun = TexTrLut.SampleLevel(SampTr, lutUvSun, 0).rgb;
#	if LUTGEN != 1
		float2 lutUvMasser = TrLutUvPlanet(curr_pos, data.masserDir);
		float3 trMasser = TexTrLut.SampleLevel(SampTr, lutUvMasser, 0).rgb;

		float2 lutUvSecunda = TrLutUvPlanet(curr_pos, data.secundaDir);
		float3 trSecunda = TexTrLut.SampleLevel(SampTr, lutUvSecunda, 0).rgb;
		
		float3 psiMs = TexMsLut.SampleLevel(SampTr, lutUvSun, 0).rgb * data.sunlightColor;
		psiMs += TexMsLut.SampleLevel(SampTr, lutUvMasser, 0).rgb * data.masserColor;
		psiMs += TexMsLut.SampleLevel(SampTr, lutUvSecunda, 0).rgb * data.secundaColor;
#	endif

		float3 inscatter = (muSRayleigh * phaseRayleighSun + muSAerosol * phaseAerosolSun) * trSun;
#	if LUTGEN != 1
		inscatter *= data.sunlightColor;
		inscatter += (muSRayleigh * phaseRayleighMasser + muSAerosol * phaseAerosolMasser) * trMasser * data.masserColor;
		inscatter += (muSRayleigh * phaseRayleighSecunda + muSAerosol * phaseAerosolSecunda) * trSecunda * data.secundaColor;
		inscatter += scatterNoPhase * psiMs;
#	endif

		float3 scatterIntegeral = inscatter * scatterFactor;

		lum += scatterIntegeral * tr;
#endif
		tr *= trSample;

#if LUTGEN == 3
		RWTexOutput[uint3(tid.xy, i + 1)] = float4(lum, dot(tr, float3(0.2126, 0.7152, 0.0722)));
#endif
	}

#if LUTGEN == 1  // multiscatter
	if (tGround > 0) {
		// Lambert ground bounce: albedo/pi BRDF times N.L. The old
		// dot(pos, sunDir) > 0 guard keyed on the ray ORIGIN and is subsumed
		// by the N.L term at the actual hit point.
		float3 hit_pos = pos + tGround * rayDir;
		float3 normal = normalize(hit_pos);
		hit_pos = normal * data.rPlanet;
		float ndl = saturate(dot(normal, sunDir));
		float2 lutUv = TrLutUvPlanet(hit_pos, sunDir);
		lum += tr * (data.groundAlbedo / Math::PI) * ndl * TexTrLut.SampleLevel(SampTr, lutUv, 0).rgb;
	}
#endif
}

[numthreads(8, 8, 1)] void main(uint3 tid
								: SV_DispatchThreadID) {
	const SharedData::PhysSkyData data = SharedData::physSkyData;

#if LUTGEN == 3
	RWTexOutput[uint3(tid.xy, 0)] = float4(0, 0, 0, 1);
#endif

	uint3 outDims;
#if LUTGEN == 3
	RWTexOutput.GetDimensions(outDims.x, outDims.y, outDims.z);
#else
	RWTexOutput.GetDimensions(outDims.x, outDims.y);
#endif
	float2 uv = (tid.xy + 0.5) / outDims.xy;

#if LUTGEN < 2
	float altitude = lerp(data.rPlanet, data.rAtmosphere, uv.y);
	float3 pos = float3(0, 0, altitude);

	// float horZenithCos = HorizonZenithCos(altitude);
	float horZenithCos = -0.414;
	float zenithCos = lerp(horZenithCos, 1, uv.x);
	float3 sunDir = float3(0, sqrt(1 - zenithCos * zenithCos), zenithCos);
#elif LUTGEN == 2 || LUTGEN == 3
	float3 rayDir = InvSkyViewLutUv(uv);
	float3 sunDir = data.sunDir;
	float3 pos = float3(0, 0, data.zCameraPlanet);
#endif

	float3 tr = 1.0;
#if LUTGEN == 0
	rayMarch(pos, sunDir, sunDir, tr);
	RWTexOutput[tid.xy] = float4(tr, 1.0);

#elif LUTGEN == 1
	const uint sqrtSamples = 4;
	const float rcpSqrtSamples = rcp(sqrtSamples);
	const float rcpSamples = rcpSqrtSamples * rcpSqrtSamples;

	float3 lumTotal = 0;
	float3 fMs = 0;
	for (uint i = 0; i < sqrtSamples; ++i)
		for (uint j = 0; j < sqrtSamples; ++j) {
			const float theta = (i + 0.5) * Math::PI * rcpSqrtSamples;
			const float phi = acos(1.0 - 2.0 * (j + 0.5) * rcpSqrtSamples);
			const float3 rayDir = SphericalDir(theta, phi);

			tr = 1;
			float3 lum = 0;
			float3 lumFactor = 0;
			rayMarch(pos, rayDir, sunDir, tr, lum, lumFactor);

			fMs += lumFactor;
			lumTotal += lum;
		}
	RWTexOutput[tid.xy] = float4(lumTotal * rcpSamples / (1 - fMs * rcpSamples), 1.0);

#elif LUTGEN == 2
	float3 lum = 0;
	rayMarch(pos, rayDir, tr, lum);
	RWTexOutput[tid.xy] = float4(lum, 1.0);

#elif LUTGEN == 3
	float3 lum = 0;
	rayMarch(pos, rayDir, tid.xy, outDims.z, tr, lum);

#elif LUTGEN == 4
	// Windowed cloud sun-transmittance LUT: x = sun zenith cosine mu over
	// [cloudTrMuMin, cloudTrMuMax] (window tracks the sun per frame, mu < 0 is
	// the afterglow/underlighting range), y = radius over [cloudTrRBot,
	// cloudTrRTop]. Self-contained integrand: deliberately does NOT reuse
	// rayMarch, so the dense preprocessor lattice above stays untouched.
	float r = lerp(data.cloudTrRBot, data.cloudTrRTop, uv.y);
	// The window may overshoot the physical domain; clamp so dir stays unit.
	float mu = clamp(lerp(data.cloudTrMuMin, data.cloudTrMuMax, uv.x), -1.0, 1.0);
	float3 pos = float3(0, 0, r);
	float3 dir = float3(0, sqrt(saturate(1.0 - mu * mu)), mu);

	tr = 0;  // ground-occluded default -- this encodes the rising terminator
	if (RayIntersectSphere(pos, dir, 0, data.rPlanet) < 0.0) {
		float tMax = RayIntersectSphere(pos, dir, 0, data.rAtmosphere);
		const uint nsteps = 64;
		float dt = tMax / nsteps;
		float3 odSum = 0;
		float3 p = pos + 0.5 * dt * dir;  // midpoint rule
		[loop] for (uint i = 0; i < nsteps; ++i, p += dt * dir)
		{
			float rouRayleigh, rouAerosol, rouOzone;
			SampleAtmosphere(
				max(0.f, length(p) - data.rPlanet),
				rouRayleigh, rouAerosol, rouOzone);
			odSum += rouRayleigh * data.rayleighScatter +
			         rouAerosol * (data.aerosolScatter + data.aerosolAbsorption) +
			         rouOzone * data.ozoneAbsorption;
		}
		tr = exp(-dt * odSum);
	}
	RWTexOutput[tid.xy] = float4(tr, 1.0);

#elif LUTGEN == 5
	// Cloud ambient endpoint LUT. Texel 0 = cosine-weighted mean radiance
	// (E/pi) over the LOWER hemisphere at the cloud layer bottom (upwelling:
	// ground bounce + low-atmosphere in-scatter); texel 1 = same over the
	// UPPER hemisphere at the layer top (downwelling sky). Reuses rayMarch in
	// its LUTGEN 2 configuration (sun + both moons + psi_ms, march stops at
	// the ground).
	if (tid.x >= 2 || tid.y >= 1)
		return;  // Dispatch(1, 1, 1)

	const bool top = (tid.x == 1);
	const float r = top ? data.cloudTrRTop : data.cloudTrRBot;
	const float zSign = top ? 1.0 : -1.0;
	const float3 pos = float3(0, 0, r);

	const uint K = 64;
	float3 sum = 0;
	[loop] for (uint i = 0; i < K; ++i)
	{
		float z = (i + 0.5) / K;  // Fibonacci hemisphere
		float rad = sqrt(saturate(1.0 - z * z));
		float phi = i * 2.39996323;  // golden angle
		float3 dir = float3(rad * cos(phi), rad * sin(phi), z * zSign);

		float3 trRay = 1.0;
		float3 lumRay = 0;
		rayMarch(pos, dir, trRay, lumRay);

		// ground bounce for rays that hit the planet (rayMarch clamps tMax to
		// the ground but adds no bounce for this configuration)
		float tGround = RayIntersectSphere(pos, dir, 0, data.rPlanet);
		if (tGround > 0.0) {
			float3 n = normalize(pos + tGround * dir);
			float ndl = saturate(dot(n, data.sunDir));
			float2 uvGround = TrLutUvPlanet(n * data.rPlanet, data.sunDir);
			lumRay += trRay * data.groundAlbedo * (1.0 / Math::PI) * ndl *
			          TexTrLut.SampleLevel(SampTr, uvGround, 0).rgb * data.sunlightColor;
		}
		sum += lumRay * z;  // cosine weight; |dir.z| == z
	}
	RWTexOutput[tid.xy] = float4(sum * (2.0 / K), 1.0);  // E/pi = (2/K) * sum(L*cos)
#endif
}