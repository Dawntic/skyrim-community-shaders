# Physical Sky: Transmittance LUT μ-Mapping Round-Trip Audit (Step 0)

Read-only audit of the forward (generation) and inverse (sampling) mappings of
the atmospheric transmittance LUT, per the cloud lighting upgrade plan
(`features/Physical Sky/Shaders/PhysicalSky/cloud-lighting-upgrade-plan.md`,
Step 0). No code is changed by this audit.

## The two mappings

**Forward** — `LutGen.cs.hlsl`, `main()`, `LUTGEN < 2` block (generates texel
`(i, j)` of the 256×64 LUT at `uv = (tid.xy + 0.5) / outDims.xy`):

```hlsl
float altitude = lerp(data.rPlanet, data.rAtmosphere, uv.y);
// float horZenithCos = HorizonZenithCos(altitude);
float horZenithCos = -0.414;
float zenithCos = lerp(horZenithCos, 1, uv.x);
```

**Inverse** — `Common.hlsli`, `TrLutUv(r, cosSunZenith)` (consumed by
`TrLutUvPlanet`, which is what LUTGEN 1/2/3 and every other Tr LUT consumer
goes through):

```hlsl
// float cosHorZenith = HorizonZenithCos(r);
const float cosHorZenith = -0.414;
float2 uv = float2(
    saturate((cosSunZenith - cosHorZenith) / (1 - cosHorZenith)),
    saturate((r - data.rPlanet) / (data.rAtmosphere - data.rPlanet)));
uv = clamp(uv, float2(0.5 / 256.0, 0.5 / 64.0), float2(1.0 - 0.5 / 256.0, 1.0 - 0.5 / 64.0));
```

## Verdict: the mappings agree

`TrLutUv` implements the **exact algebraic inverse of the hardcoded linear
mapping**, not the original per-altitude horizon mapping:

-   Both sides hardcode `-0.414` and both have the per-altitude
    `HorizonZenithCos(r)` call commented out symmetrically. Neither side was
    half-migrated.
-   x-axis: forward `μ = lerp(-0.414, 1, u)` vs inverse
    `u = (μ + 0.414) / 1.414` — exact inverses.
-   y-axis: forward `r = lerp(rPlanet, rAtmosphere, v)` vs inverse
    `v = (r - rPlanet) / (rAtmosphere - rPlanet)` — exact inverses.
-   Texel alignment: for μ generated at texel center `i`
    (`μ_i = lerp(-0.414, 1, (i + 0.5) / 256)`), the inverse produces sampling
    coordinate `u·256 = i + 0.5` — bilinear weight lands **exactly** on texel
    `i`. Round-trip error of the parameterization itself: **0**.

The Step-0 suspicion that forward/inverse disagree is therefore **cleared**.
LUTGEN 1/2/3's consumption of the Tr LUT via `TrLutUvPlanet` is
self-consistent, which also clears LUTGEN 5 (ambient endpoint LUT) to reuse
`rayMarch` and `TrLutUvPlanet` as-is.

## Quantified residuals (correct but lossy behavior)

These are properties of the parameterization, not bugs; they motivate the
windowed cloud LUT (LUTGEN 4) rather than any fix to the global LUT.

1. **Half-texel edge clamp.** `TrLutUv` clamps uv into
    `[0.5/256, 1 − 0.5/256] × [0.5/64, 1 − 0.5/64]`. μ queries within half a
    texel of the domain edges read the edge texel: max |Δμ| = 1.414/512 ≈
    **2.76×10⁻³**. Standard bilinear-safety practice. Note the clamp constants
    hardcode the 256×64 dimensions, which match `kTrLutW`/`kTrLutH`
    (`PhysicalSky.h`) today but silently desynchronize if the LUT is resized.

2. **Over-deep domain floor.** `-0.414` is the geometric horizon cosine seen
    from r ≈ 6987 km (altitude ≈ 627 km) — far above the atmosphere top
    (6420 km, horizon μ = −0.1363). For every altitude in the atmosphere, all
    texels with μ below the local horizon are ground-occluded black
    (LUTGEN 0 writes `tr = 0`): **29.3 %** of the x-axis at sea level
    (horizon μ = 0), shrinking to **19.6 %** at the atmosphere top. Correct
    values, dead resolution.

3. **μ resolution in the sunset window.** Full-axis resolution is
    1.414/256 ≈ **5.5×10⁻³ μ per texel**. The orange→black transmittance
    transition (~0.01–0.02 wide in μ) therefore spans **~2–4 texels** under
    bilinear — the marginality that motivates the per-frame windowed LUT.
    Framed as the plan does: a ±0.04 twilight band around the horizon covers
    ~6 % of the axis, i.e. **~94 % of texels lie outside it**; even the full
    cloud-field window (±(600 km / 6360 km + 0.02) ≈ ±0.114) leaves ~84 %
    outside.

4. **Consumer-side hard cut.** `SampleTr` (`Common.hlsli`) zeroes
    transmittance for `sunDir.z ≤ -0.414`, consistent with the domain floor —
    but note it keys on the **camera-local** sun z, another reason cloud-layer
    queries (which need μ at the *sample's* altitude, including μ < 0 with the
    sun below the camera horizon) cannot go through this path.

## Unit-scale note (feeds the Step 1 assert)

`physSkyData` radii are in **game units** (`rPlanet =
settings.planetRadius / Util::Units::GAME_UNIT_TO_KM ≈ 4.454×10⁸`), while the
cloud raymarcher's cbuffer radii (`groundRadius`, `bottomRadius`, `topRadius`)
are in **kilometers**. The conversion constants agree exactly today
(`Util::Units::GAME_UNIT_TO_KM = 1.428/100/1000 = 1.428e-5` vs the shader-side
`GAME_UNIT_TO_KM = 1.428e-5` in `Clouds.hlsl` and the `1.428e-5f` literals in
`Common.hlsli`), but nothing enforces it — this is the silent
"slightly wrong colors" hazard the Step 1 CPU-side scale assert guards.
The windowed LUT's axes are normalized, so LUTGEN 4/5 integrate in game units
(consistent with `SampleAtmosphere` and the per-game-unit scatter
coefficients) while the cloud shader samples the same axes with km inputs.
