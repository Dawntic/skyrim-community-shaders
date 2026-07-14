# Request: Physically-Based Sunset Lighting for Volumetric Clouds

## Status

**Implemented and merged (PR #1)** — all plan steps 0–6, one commit per step:

-   **V0 report**: `docs/development/physical-sky-trlut-mapping-audit.md`. Verdict: the global Tr LUT forward/inverse mappings **agree** (both hardcode -0.414); round-trip error is zero. The windowed LUT is justified by μ resolution (~2–4 texels across the twilight transition), not by a mapping bug.
-   LUTGEN 4 (windowed sun-transmittance LUT) + LUTGEN 5 (ambient endpoint LUT), cloud shader consumption via `ComputeLightingV3` (white TOA `sunlightColor` × `sunTr`, never `DirLightColor`), in-cloud sun light march + Wrenninge octaves with optional per-octave phase, debug seams on cbuffer uniforms, corner overlay + ImGui panel, and the five Step 6 fixes (albedo 0.996, TrDepthSum, LUTGEN 1 Lambert bounce, 8×8 MS sampling, gamma TODO).
-   LUTGEN 0–3 verified token-identical except the two sanctioned Step 6 output changes (LUTGEN 1 only).

**Post-merge fixes** (on `cloud-lighting`): LUTGEN 4 compile fix (the unused `rayMarch` definition had a malformed signature in that permutation — now guarded out), retuned lighting/layer defaults, the coarse light-march density removed (the light march now marches the full base profile), and vanilla game clouds discarded in `Sky.hlsl`.

**Follow-up jobs (second PR)**:

1. ~~Scroll setting~~ — done: `cloudSettings.scroll` → `CloudDataCB.Scroll` (the old unused `WeatherScale` slot) drives the base-noise z slice and the detail lookup; drag control in the Clouds tab.
2. ~~Persist cloud settings~~ — done: `CloudSettings` (incl. scroll + detail sculpting params) serializes under `cloudSettings`; the lighting tuning subset (gains, octave extinction attenuation, medium coefficients) under `cloudLighting`. Debug seams stay runtime-only.
3. ~~Fold redundant gains~~ — done: `SunMsGain` removed (it multiplied the same term as `SunGain`; effective product folded into the 3.6 default) and the Wrenninge octave *energy* attenuation fixed in-shader at 0.6 (it acted as another flat sun gain). The *extinction* attenuation stays tunable — it shapes glow depth, not brightness.
4. ~~Detail density function~~ — done: `ApplyCloudDetail` runs after `GetCloudProfile` in the view march (Nubis/Schneider technique): curl-distorted 32³ Worley FBM, wispy-base → billowy-top height transition, applied through the edge-biased remap so interiors can't be carved and the base silhouette survives (`DetailStrength`, reference 0.2, hard clamp 0.9); distance-faded since the detail texture has no mips. The sun light march keeps the un-detailed base density.

**Weather system (third PR, stacked on the second)**:

1. ~~Weather config~~ — done: PhysicalSky registers its sky scattering (Rayleigh/aerosol/ozone), cloud layer + shape + detail, and cloud lighting tuning with the existing `WeatherVariables` registry. Weathers are authored per TESWeather form in the CS Editor's weather widget (persisted by `WeatherManager`); overridden values lerp through game weather transitions automatically.
2. ~~Incoming clouds over the horizon~~ — done: during transitions, Coverage / Cloud Type / Coverage 2 blend **spatially** across a wind-aligned front instead of time-fading in place — the incoming weather's clouds appear at the upwind horizon and sweep across the sky with the transition. Soft, noise-ragged front edge (`WeatherFrontWidth`); departure/arrival endpoints reconstructed from `WeatherManager` overrides (interrupted transitions fold their progress in); the sun light march reuses the view sample's blended shape. Wind direction is a fixed placeholder heading (`kWeatherWindDir`) until a real wind system exists. A Manual Front Test panel drives the front without waiting on game weather.

**Still open**: the V1–V4 in-game verification ladder below (needs a Windows build + sunset sweeps); the gamma-placement TODO at the cloud encode; the global Tr LUT reparameterization remains a documented follow-up option, not scheduled; real wind direction for the weather front.

## Goal

Make the volumetric cloud system correctly display twilight/sunset colors (orange/red underlit cloud bases, blue-lit tops, purple mixing zones) by:

1. Feeding the cloud **direct sun term** from a new per-frame **windowed atmospheric transmittance LUT** with below-horizon (μ < 0) support, sampled per raymarch step at each sample's actual altitude and sun zenith angle.
2. Feeding the cloud **ambient term** from a new per-frame **2-texel ambient endpoint LUT** (upwelling irradiance at cloud-layer bottom, downwelling at cloud-layer top), lerped by in-layer height.
3. Reinstating a cheap **in-cloud sun light march** (5–6 exponential steps, density-only) with **Wrenninge multi-scatter octaves**, with no assumption that the sun is above the layer — the march must be able to exit through the cloud **base** as unoccluded (that is the sunset-underlighting case).
4. Adding **debug override modes at every lighting seam** so each subsystem can be verified in isolation before the next is trusted.

Physics rationale (drives several decisions below): sunset cloud color is carried by the **direct** solar beam grazing tangentially through the atmosphere — a cloud base at 1–2 km altitude still geometrically sees a sun 2–5° below the _camera's_ horizon. All time-of-day color must therefore enter through a positionally-parameterized transmittance query, not through an artist-tinted light color. Ambient supplies only the blue side of the purple mix. Cloud droplets are spectrally neutral (albedo ≈ 0.996), so no color may be injected in the cloud medium itself.

## Codebase orientation

This is the Community Shaders repo (Skyrim mod, DX11/HLSL shaders + C++ SKSE-side feature code). Locate the relevant files by searching for:

-   **Physical sky LUT generation compute shader** — contains `#if LUTGEN == 0/1/2/3` branches and a `rayMarch(...)` function (transmittance / multiscatter / sky-view / aerial-perspective LUTs).
-   **PhysicalSky common include** — contains `TrLutUvPlanet`, `InvSkyViewLutUv`, `SampleAtmosphere`, `RayIntersectSphere`, `SharedData::physSkyData`.
-   **Volumetric cloud pixel shader** — contains `GetCloudProfile`, `ComputeLightingV1`, a 512-step view raymarch, `CloudParticpatingMedium`.
-   **C++ feature code** for PhysicalSky / clouds — dispatches the LUTGEN passes, owns the cbuffers, and has an ImGui settings panel.

Do not assume file paths; find them. Read all four before editing anything.

## Constraints

-   Existing LUTGEN 0–3 output must be **bit-identical** after this work, with two deliberate exceptions listed in Step 6 (each in its own commit, guarded/flagged).
-   All coordinates in the new code are **planet-center-relative kilometers**, matching the existing sky code. Add a CPU-side assert that the cloud shader's `groundRadius`/`bottomRadius`/`topRadius` and `physSkyData.rPlanet` are in the same scale (a mismatch fails silently as "slightly wrong colors").
-   New LUTs are **RGBA16F** (not R11G11B10 — twilight transmittance channel ratios span ~4 decades).
-   Debug branches switch on **cbuffer uniforms** (uniform flow control), never compile-time defines, so a single shader build supports the whole verification ladder.
-   One commit per numbered step below.

---

## Step 0 — Mapping round-trip audit (read-only, report first)

The LUTGEN 0 generator uses a **hardcoded linear μ mapping**: `horZenithCos = -0.414; zenithCos = lerp(horZenithCos, 1, uv.x)` with a commented-out `HorizonZenithCos(altitude)` call. Verify whether `TrLutUvPlanet` in the common include implements the **inverse of this exact mapping** or the original per-altitude horizon mapping.

## Step 1 — LUTGEN 4: windowed cloud sun-transmittance LUT

**Decision: new per-frame windowed LUT, not a modification of the global Tr LUT.** Two alternatives were considered and rejected:

-   _Resolution bump of the global LUT:_ its μ axis is linear over `[-0.414, 1]`, so ~94% of texels lie outside the sunset window regardless of resolution. Even at 1024 wide, the orange→black transition (~0.01–0.02 in μ) lands on ~7–14 texels under bilinear — marginal at best.
-   _Re-parameterizing the global mapping_ (horizon-concentrated remap): the correct long-term fix, but it changes `TrLutUvPlanet`, which is consumed by LUTGEN 1/2/3 and any other shader sampling the Tr LUT — and Step 0 already suspects the forward/inverse mappings disagree today. Large regression surface; the opposite of the verify-in-isolation goal.

The windowed LUT is purely additive (zero regression risk to LUTGEN 0–3), gives ~50× effective μ resolution centered on the current sun automatically (the window tracks `mu0` per frame), covers μ < 0 natively, and costs 64×32 texels × 64 integrand steps per frame — trivial. Verification stage V3 A/Bs it against the global LUT and quantifies the error; if the delta turns out small, consolidating back onto a re-parameterized global LUT is a documented follow-up option, not part of this task. Do not relitigate this choice during implementation.

**New texture:** 64×32 RGBA16F, regenerated every frame.
**Axes:** x = sun zenith cosine μ in `[cloudTrMuMin, cloudTrMuMax]`, y = altitude r in `[bottomRadius, topRadius]`.

**CPU per frame** (new cbuffer fields):

```cpp
float mu0    = dot(normalize(cameraPosPlanetRel), sunDir);
float halfW  = maxCloudDistanceKm / rPlanetKm + 0.02f;  // margin for bilinear filtering
cloudTrMuMin = mu0 - halfW;
cloudTrMuMax = mu0 + halfW;
cloudTrRBot  = bottomRadius;   // km, planet-center-relative
cloudTrRTop  = topRadius;
```

**Shader** — add to the LUT gen compute shader as a **self-contained branch**; deliberately do NOT reuse `rayMarch` here (its preprocessor lattice is dense; a standalone integrand carries zero risk of a mis-evaluated `#if`):

```hlsl
#elif LUTGEN == 4
{
	float r  = lerp(data.cloudTrRBot, data.cloudTrRTop, uv.y);
	float mu = lerp(data.cloudTrMuMin, data.cloudTrMuMax, uv.x);
	float3 pos = float3(0, 0, r);
	float3 dir = float3(0, sqrt(saturate(1.0 - mu * mu)), mu);

	float3 tr = 0;  // ground-occluded default — this encodes the rising terminator
	if (RayIntersectSphere(pos, dir, 0, data.rPlanet) < 0.0) {
		float tMax = RayIntersectSphere(pos, dir, 0, data.rAtmosphere);
		const uint nsteps = 64;
		float dt = tMax / nsteps;
		float3 odSum = 0;
		float3 p = pos + 0.5 * dt * dir;  // midpoint rule
		[loop] for (uint i = 0; i < nsteps; ++i, p += dt * dir) {
			float rouR, rouA, rouO;
			SampleAtmosphere(max(0.f, length(p) - data.rPlanet), rouR, rouA, rouO);
			odSum += rouR * data.rayleighScatter
			       + rouA * (data.aerosolScatter + data.aerosolAbsorption)
			       + rouO * data.ozoneAbsorption;
		}
		tr = exp(-dt * odSum);
	}
	RWTexOutput[tid.xy] = float4(tr, 1.0);
}
```

Note `mu` must be allowed **negative** — that is the afterglow/underlighting window, and the ground-intersection test (not any clamp) decides occlusion. Because occlusion depends on altitude `r`, low-altitude texels black out before high-altitude ones at the same μ — the Earth-shadow terminator rising through the layer falls out for free.

**CPU:** create the texture + UAV/SRV, dispatch `(64/8, 32/8, 1)` per frame after LUTGEN 0.

## Step 2 — LUTGEN 5: ambient endpoint LUT

**New texture:** 2×1 RGBA16F, per frame. Texel 0 = **AmbientBottom** = cosine-weighted mean radiance (E/π) over the **lower** hemisphere at `r = bottomRadius` (upwelling: ground bounce + low-atmosphere in-scatter). Texel 1 = **AmbientTop** = same over the **upper** hemisphere at `r = topRadius` (downwelling sky).

This branch **does** reuse `rayMarch` in its LUTGEN 2 configuration (sun + both moons + ψ_ms in-scatter, march stops at ground). Required preprocessor edits — extend two conditions:

```hlsl
#elif LUTGEN == 2 || LUTGEN == 5    // signature block: (pos, rayDir, inout tr, inout lum)
...
#if LUTGEN == 2 || LUTGEN == 5
	const uint nsteps = 30;
```

Then verify every other `#if` in `rayMarch` evaluates correctly for `LUTGEN == 5` (`> 1` → uses `data.sunDir` ✓; `!= 0` → in-scatter on ✓; `!= 1` → moons + ψ_ms on ✓; `== 3` → AP writes off ✓). State the verification result in the commit message.

**Main branch:**

```hlsl
#elif LUTGEN == 5
	if (tid.x >= 2 || tid.y >= 1) return;   // Dispatch(1,1,1)

	const bool  top   = (tid.x == 1);
	const float r     = top ? data.cloudTrRTop : data.cloudTrRBot;
	const float zSign = top ? 1.0 : -1.0;
	const float3 pos  = float3(0, 0, r);

	const uint K = 64;
	float3 sum = 0;
	[loop] for (uint i = 0; i < K; ++i) {
		float z   = (i + 0.5) / K;               // Fibonacci hemisphere
		float rad = sqrt(saturate(1.0 - z * z));
		float phi = i * 2.39996323;              // golden angle
		float3 dir = float3(rad * cos(phi), rad * sin(phi), z * zSign);

		float3 tr = 1.0;
		float3 lum = 0;
		rayMarch(pos, dir, tr, lum);

		// ground bounce for rays that hit the planet (rayMarch clamps tMax but adds no bounce)
		float tG = RayIntersectSphere(pos, dir, 0, data.rPlanet);
		if (tG > 0.0) {
			float3 n = normalize(pos + tG * dir);
			float ndl = saturate(dot(n, data.sunDir));
			float2 uvG = TrLutUvPlanet(n * data.rPlanet, data.sunDir);
			lum += tr * data.groundAlbedo * (1.0 / Math::PI) * ndl
			     * TexTrLut.SampleLevel(SampTr, uvG, 0).rgb * data.sunlightColor;
		}
		sum += lum * z;                          // cosine weight; |dir.z| == z
	}
	RWTexOutput[tid.xy] = float4(sum * (2.0 / K), 1.0);   // E/pi = (2/K) * sum(L*cos)
```

**CPU:** dispatch after LUTGEN 0 and 1 (it samples the Tr and MS LUTs).

## Step 3 — Cloud shader: consume the LUTs, add debug seams

Bind `TexCloudSunTr` and `TexCloudAmbient` to the cloud pixel shader. Add a debug cbuffer:

```hlsl
cbuffer CloudDebugCB {
	uint  DebugSunTrMode;    // 0 live | 1 force white(=1) | 2 force orange | 3 A/B: sample GLOBAL Tr LUT instead
	uint  DebugAmbientMode;  // 0 live | 1 DebugColor | 2 literal red | 3 red->blue height gradient
	float3 DebugColor;
	float SunGain;           // 0 while validating ambient, 1 normally
	float AmbientGain;
	float SunMsGain;         // replaces CLOUD_MS_GAIN on the sun path only
	float cloudTrMuMin, cloudTrMuMax, cloudTrRBot, cloudTrRTop;
}
```

Fetch + eval functions (overrides sit **after** any UV remap, so remap bugs are excluded from application-side tests):

```hlsl
float3 SampleCloudSunTr(float3 posPlanetRel)
{
	if (DebugSunTrMode == 1) return 1.0;
	if (DebugSunTrMode == 2) return float3(1.0, 0.35, 0.08);
	if (DebugSunTrMode == 3) {   // A/B against the global LUT via its real remap
		float2 g = TrLutUvPlanet(posPlanetRel, SharedData::physSkyData.sunDir);
		return TexTrLut.SampleLevel(SampTr, g, 0).rgb;
	}
	float r  = length(posPlanetRel);
	float mu = dot(posPlanetRel / r, SharedData::physSkyData.sunDir);
	float2 uv = float2((mu - cloudTrMuMin) / (cloudTrMuMax - cloudTrMuMin),
	                   (r  - cloudTrRBot)  / (cloudTrRTop  - cloudTrRBot));
	return TexCloudSunTr.SampleLevel(LinearClampSampler, saturate(uv), 0).rgb;
}

float3 EvalCloudAmbient(float heightFrac, float3 ambBottom, float3 ambTop)
{
	switch (DebugAmbientMode) {
		case 1: return DebugColor;
		case 2: return float3(1, 0, 0);
		case 3: return lerp(float3(1,0,0), float3(0,0,1), heightFrac);
	}
	return lerp(ambBottom, ambTop, heightFrac);
}
```

New lighting function (replace `ComputeLightingV1` usage; keep V1 in the file for reference):

```hlsl
void ComputeLightingV3(float density, float stepLength, float sunVisibility, float heightFrac,
                       float3 sunTr, float3 ambBottom, float3 ambTop,
                       CloudParticpatingMedium medium,
                       inout float3 Inscattering, inout float Transmittance)
{
	float albedo = medium.scattering / medium.extinction;
	float extinction = medium.extinction * density;
	float tr = exp(-extinction * stepLength);

	float3 sunRad = SharedData::physSkyData.sunlightColor  // TOA illuminance, WHITE — see note
	              * sunTr * medium.phase * sunVisibility * SunGain;
	float3 ambRad = EvalCloudAmbient(heightFrac, ambBottom, ambTop) * AmbientGain; // no phase: pre-integrated

	float3 inscatter = (sunRad + ambRad) * albedo * (1.0 - tr);
	Inscattering += inscatter * Transmittance;
	Transmittance *= tr;
}
```

In `main`, before the march loop:

```hlsl
float3 ambBottom = TexCloudAmbient.Load(int3(0, 0, 0)).rgb;
float3 ambTop    = TexCloudAmbient.Load(int3(1, 0, 0)).rgb;
```

Per march step (inside the density > 0 branch): `float3 sunTr = SampleCloudSunTr(SamplePos);` — `SamplePos` is already planet-center-relative km. `heightFrac` = the existing `EnvelopeZ`.

**Important convention change:** the sun term uses `physSkyData.sunlightColor` (constant TOA illuminance) × `sunTr`, **not** `SharedData::DirLightColor` — the game's directional light is artist-tinted at sunset and would double-tint. All time-of-day color must come from `sunTr` alone.

**Do not clamp the sun anywhere.** Grep the cloud shader (and any code feeding it sun direction/intensity) for `saturate`/`max(0, ...)` applied to sun elevation or NdotL-style terms and remove/flag them for the cloud path — the sun must stay live to roughly −5° elevation, with the Tr LUT deciding when light stops arriving.

## Step 4 — In-cloud sun light march + multi-scatter octaves

Replace the `sunVis = EnvelopeZ` placeholder:

```hlsl
float SunOpticalDepth(float3 samplePos, float3 sunDir, float extinction)
{
	// 5 exponential steps, ~1.5 km total; coarse density (erosion skipped)
	float od = 0.0;
	float t = 0.0, dt = 0.05;                    // km
	[unroll] for (int s = 0; s < 5; ++s) {
		t += dt;
		float3 p = samplePos + sunDir * t;       // sunDir points TOWARD the sun; may go DOWNWARD
		float h = GetEnvelopeRelativeZ(p, float2(bottomRadius, topRadius));
		if (h < 0.0 || h > 1.0) break;           // exited layer (through base OR top) -> unoccluded beyond
		od += GetCloudProfileCoarse(p, h) * dt * extinction;
		dt *= 2.0;
	}
	return od;
}

float SunVisibilityMS(float od)
{
	// Wrenninge octaves: fakes deep multiple scattering of the (gray) droplet medium
	float vis = 0.0, a = 1.0, b = 1.0;
	[unroll] for (int o = 0; o < 3; ++o) {
		vis += b * exp(-a * od);
		a *= 0.5;
		b *= 0.6;
	}
	return vis;
}
```

Requirements:

-   The march direction is **toward the sun**, which at sunset points _downward_ through the layer. Exiting through the layer **base** is the unoccluded case — do not treat descending out of the layer as shadowed. (The atmosphere beyond the exit is already accounted for by `sunTr`; the two are independent path segments composed by multiplication.)
-   `GetCloudProfileCoarse` = `GetCloudProfile` with the erosion path skipped: base Perlin-Worley remap + height gradient + coverage only, no Worley FBM / `Erosion` chain. Implement as a bool parameter or overload. **Note: the cloud noise textures have no mip chains**, so mip-based LOD is not an option here — do not add mip generation to the texture pipeline (out of scope); skipping erosion is cheaper than a mip fetch of the full profile anyway and is standard practice for light marches (light-march detail is barely visible in the result).
-   Skip the march when view `Transmittance < 0.01` (early-out) and when `CloudDensity <= 0`.
-   Optional (behind a define, default on): per-octave phase eccentricity attenuation — evaluate `CloudPhase` with `g *= 0.5^o` per octave so deeply-scattered light goes isotropic. If awkward with the current single-phase structure, note it and leave for follow-up.

## Step 5 — Debug overlay + ImGui

-   Blit `TexCloudSunTr` (scaled up ~4×) plus the two `TexCloudAmbient` texels as swatches into a screen-corner overlay, toggleable.
-   Expose in the feature's ImGui panel: `DebugSunTrMode`, `DebugAmbientMode`, `DebugColor`, `SunGain`, `AmbientGain`, `SunMsGain`, overlay toggle, and the octave `a`/`b` params.

## Step 6 — Correctness fixes (each its own commit)

1. **Cloud medium albedo:** `medium.scattering = 10 / extinction = 25` → albedo 0.4, which makes interiors charcoal and kills any glow penetration. Change to `scattering = 24.9, extinction = 25` (albedo 0.996). Expose both in ImGui.
2. **`TrDepthSum` bug:** currently `TrDepthSum += Transmittance * RayT.x` (constant entry distance) — should be `Transmittance * (RayT.x + i * StepLength)`; otherwise the mean-depth output is always the entry distance.
3. **LUTGEN 1 ground bounce** is missing the Lambert terms: `lum += tr * data.groundAlbedo * TrLUT` → `lum += tr * (data.groundAlbedo / Math::PI) * saturate(dot(normalize(hit_pos), sunDir)) * TrLUT` (the existing `dot(pos, sunDir) > 0` guard becomes redundant). This changes ψ_ms slightly — flag it clearly; this is one of the two allowed LUTGEN 0–3 output changes.
4. **MS LUT sample count:** the 4×4 = 16-ray hemisphere integration aliases at twilight (maximally anisotropic illumination). Put `sqrtSamples` behind a define, default 8 (=64 rays, Hillaire's reference). Second allowed output change.
5. **Gamma placement (note only, no change):** `Color::LLLinearToGamma(Inscattering)` inside the cloud pass is only valid if the composite consumes gamma; if clouds blend with linear-HDR sky pre-tonemap, hues shift at twilight. Add a `// TODO` with this note where the encode happens.

---

## Verification ladder (acceptance criteria)

Run in order; each stage gates the next.

**V0 — mapping audit:** Step 0 report produced; round-trip error quantified.

**V1 — application isolation.** `SunGain = 0`, `AmbientGain = 1`:

-   `DebugAmbientMode = 1/2`: optically thick cloud interiors converge to `albedo × C`, **direction-independent** — orbit the camera; any view-angle variation is an application bug. (Edge darkening from `1 − exp` at low density is correct Beer–Lambert, not a failure.)
-   `DebugAmbientMode = 3`: cloud bases visibly red, tops blue, smooth vertical blend. Uniform murky purple = height/lerp collapsed; inverted = `heightFrac` flipped.

**V2 — input inspection.** Overlay on, scripted sun elevation sweep +10° → −6°:

-   `TexCloudSunTr` sweeps white → gold → deep red → black, with the black (ground-occluded) region crossing the μ axis and reaching **low-altitude rows first** (terminator rises bottom-up).
-   `AmbientTop` swatch stays **blue** into civil twilight; `AmbientBottom` goes orange → dusky. If AmbientTop drifts gray-green, ozone is wrong — sanity: `ozoneAbsorption ≈ (0.650, 1.881, 0.085) × 10⁻³ km⁻¹` peak, tent profile centered ~25 km.

**V3 — A/B the parameterization.** `DebugSunTrMode` 3 vs 0 screenshot diff at sun elevation −2°…+4°. Delta = global LUT μ-resolution error; record magnitude (expected: large; justifies keeping the windowed LUT permanently).

**V4 — full enable.** All modes 0, `SunGain = 1`, light march live. Expected at sun −1°…−4°: bright orange/red confined to cloud **undersides** and thin edges (gold rims), tops neutral-to-blue, partial-depth regions mixing toward purple; azimuthal asymmetry (sunset-facing clouds brighter than antisolar). High clouds stay pink after low clouds gray out. The orange must penetrate hundreds of meters into the cloud (octaves working), not form a thin rim over a flat ambient core.

## Out of scope

-   Fixing the global Tr LUT μ parameterization (Step 0 only reports it).
-   Refraction bias (+0.009 to μ when sun below horizon) — add as a commented-out constant with a note.
-   Temporal reprojection / disocclusion changes; the inner light march interaction with reprojection is unchanged.
-   The sky-view LUT and aerial perspective passes.
