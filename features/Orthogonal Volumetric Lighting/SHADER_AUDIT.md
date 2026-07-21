# Orthogonal Volumetric Lighting — Shader Audit

Audit of `features/Orthogonal Volumetric Lighting/Shaders/OrthogonalVolumetricLighting/Volumetric Lighting.hlsl`
and its CPU counterpart `src/Features/OrthogonalVolumetricLighting.cpp/.h`.

Scope:

1. Issues/mistakes in the HLSL passes (`SHADOW_COMPUTE`, `EVSM_COMPUTE`, `EVSMBLUR_COMPUTE`, `MEDIA_COMPUTE`,
   `SCATTER_COMPUTE`, `MARCH_COMPUTE`, `APPLY_PIXEL`, `DRAW_FOGMAP`, `PERLIN_COMPUTE`).
2. CPU ↔ GPU parameterization/unit cross-check (constant buffer layouts, unit conversions, dispatch sizes,
   default values).
3. A concrete plan to fix ghosting in the temporal reprojection.

Line numbers refer to the current state of each file on this branch.

---

## 1. Confirmed bugs in `Volumetric Lighting.hlsl`

### 1.1 Debug leftovers neutralize the shadow volume (SHADOW_COMPUTE)

`Volumetric Lighting.hlsl:369-372`:

```hlsl
ReprojectionValue = 0.8;                    // overrides the line above it
if(UIUseHistory)
    Shadow = lerp(Shadow, ShadowHistory, ReprojectionValue);
Shadow = min(Shadow, 0.15);                 // caps sun visibility at 15% ALWAYS
```

- `ReprojectionValue = 0.8;` discards the confidence/disocclusion-weighted value computed on lines 366–367,
  so history is blended at a fixed 80% even when the reprojected sample is off-screen or disoccluded.
  This is the second-largest contributor to ghosting (see §3).
- `Shadow = min(Shadow, 0.15);` unconditionally clamps directional-light visibility to 0.15. This runs even
  with history disabled, and means the sun contribution in the scatter pass can never exceed 15% of intended
  brightness. Both lines are clearly stale debug code and should be deleted.

### 1.2 Cascade fallback returns "fully shadowed" (GetCascadeShadow)

`Volumetric Lighting.hlsl:314-316`:

```hlsl
uint CascadeIndex = (ViewZ < ShadowCascadeEndSplit.x) ? 0 : 1;
if(ViewZ > ShadowCascadeEndSplit.y)
    return 0.0; //////////////////////////
```

- Beyond the second cascade's end split the function returns `0.0` (shadowed) instead of `1.0` (lit). All
  froxels past ~cascade 1 receive zero sun light. Combined with §1.1 the sun shafts effectively exist only
  near the camera. The `//////` marker suggests the author already suspected this.
- Only cascades 0–1 are ever addressed even though the CPU uploads 4 cascade matrices
  (`UpdateShadowBuffer`, `OrthogonalVolumetricLighting.cpp:405-407`) and creates `nCascades` (INI-driven,
  typically 3) EVSM array slices. `ShadowCascadeEndSplit.z` is uploaded but unused.
- The early `return` sits inside the 4-sample jitter loop, so a ray that starts inside range but jitters past
  the split aborts the whole average instead of contributing partial samples.

### 1.3 Ambient lighting multiplier applied twice (SCATTER_COMPUTE)

- `GetAmbientLighting()` already returns `AmbientLight * UIAmibentLightingMultiplier * SkyDiffuse`
  (`Volumetric Lighting.hlsl:709`).
- The caller multiplies by it again: `GetAmbientLighting(...) * UIAmibentLightingMultiplier`
  (`Volumetric Lighting.hlsl:741`).
- With the default value 3.5, the effective ambient gain is 12.25×. Remove one of the two multiplications
  (keep the one in `main` and delete it from the helper) and expect ambient defaults to need retuning.

### 1.4 Paraboloid shadow uses raw depth as light attenuation (GetLocalLighting)

`Volumetric Lighting.hlsl:659-665`:

```hlsl
float Shadow = ParabolicShadowTex.SampleLevel(...).x;      // stored shadow-map depth
float shadowMapCompareValue = saturate(length(CoordsLS.xyz) / light.radius);
Shadow = (Shadow <= shadowMapCompareValue) ? 0.0 : Shadow; // <-- should be : 1.0
Radiance *= Shadow;
```

When the light is *not* occluded, `Shadow` keeps the sampled depth value (a distance ratio in [0,1]), so
unshadowed local lights are dimmed by an arbitrary factor equal to the nearest occluder's normalized depth.
The un-occluded branch should yield `1.0` (or a PCF-style weight), not the depth itself.

### 1.5 Temporal reprojection ignores camera translation (GetHistoryUV)

`Volumetric Lighting.hlsl:179-188`:

```hlsl
float4 PrevClip = mul(PrevCameraViewProj, float4(CoordsWS, 1.0));
```

`CoordsWS` is `RayDirection * ViewZ`, i.e. a position **relative to the current eye** (the froxel grid is
camera-relative; the view matrices carry no translation). Skyrim/Community Shaders convention (see
`package/Shaders/Common/MotionBlur.hlsli` and `CameraPosAdjust` / `CameraPreviousPosAdjust` in
`Common/FrameBuffer.hlsli`) is that `CameraPreviousViewProjUnjittered` expects positions relative to the
**previous** eye. Because the eye delta is never applied, reprojection is only correct for pure rotation —
every camera translation (walking, riding, flying) reprojects to the wrong froxel and smears history.
This is the primary ghosting root cause; the fix is detailed in §3.

Additional related problems:

- The history fetch clamps out-of-range UVs (`saturate(PrevCoordsUV)`, line 363) instead of rejecting them;
  with §1.1's hardcoded blend weight, screen-edge froxels endlessly re-ingest stretched border values.
- The `PrevUV.z >= 0.0` half of the confidence test (line 185) is ineffective: `ViewDepthToUV` wraps its
  `log` in `abs()`, so depths in front of the froxel near plane still map into [0,1].
- `CameraViewProjInverse` used to build ray directions is the **jittered** inverse (and a cached one at
  that, see §2.6) while `PrevCameraViewProj` is **unjittered** — the two transforms disagree by the TAA
  jitter every frame, adding subpixel-scale reprojection error.

### 1.6 `LumTempSat` divides by zero / hardcoded color target

`Volumetric Lighting.hlsl:613-623`:

- `Color *= Luminance / CurrentLuminance;` produces NaN when a light's color is black
  (`CurrentLuminance == 0`). Guard with `max(CurrentLuminance, EPSILON_DIVISION)`.
- `float3 TempColorTarget = float3(9.6, 2.8, 0.4) * Luminance; // Eh?` — undocumented magic warm-color
  target; at minimum document the intent (approximate blackbody tint for the "min temperature" clamp).

### 1.7 EVSM exponent range overflows fp32

- The shader computes `exp(UIEVSMExponent * z)` (lines 281, 297) and the EVSM generation pass squares it
  (`ExpValue * ExpValue`, line 405) into `R32G32_FLOAT` moments.
- The UI slider allows `esmExponent` up to **100** (`DrawSettings`, `OrthogonalVolumetricLighting.cpp:905`).
  `exp(89)` already exceeds fp32 max (~3.4e38); the squared moment overflows beyond exponent ≈ 44.
  Clamp the slider to ~40 (typical fp32 EVSM ceiling is 42).
- CPU-side `EVSMData.z/w = exp(e), exp(2e)` (`UpdateShadowBuffer`, cpp:433) are **never read** by the shader
  (it recomputes from `UIEVSMExponent`) — dead parameters that also overflow on the CPU for e > 88 / 44.

### 1.8 Inconsistent froxel sampling positions between passes

- `FroxelWorldDirection()` (lines 171-177) is always called with the raw integer `Froxel`, so ray directions
  point at the **corner** of each froxel, not its center — a systematic half-texel XY misalignment relative
  to the final `APPLY_PIXEL` sampling. SHADOW_COMPUTE even computes a centered `CoordsUV` (line 337) that is
  then never used.
- Depth anchor differs per pass:
  - SHADOW shades at `z − 2` with a 1-slice thickness (lines 339–340, "bias to avoid leaks") but its history
    reprojection anchor is the *unbiased* center `z + 0.5` (line 361), and SCATTER consumes it at index `z`
    — a systematic ~2.5-slice depth mismatch between where shadow is evaluated and where it is applied.
  - MEDIA integrates a step from `z − 1` to `z + 0.5` = **1.5 slices** (lines 468–480), while MARCH steps
    exactly 1 slice (`Slice + 1.0`, lines 808–811). Per-froxel optical depth is therefore over-integrated by
    ~50% relative to the march distance; the `OpticalDepth * rcp(StepLength)` extinction reconstruction in
    both `MEDIA` (line 513) and `AccumulateScattering` (line 790) uses two different step lengths for the
    same quantity.
  - MEDIA's reprojection anchor is the slice **edge** `FroxelDepthToView(Froxel.z)` (line 543), while
    SHADOW's is the center `z + 0.5`.
  - SCATTER jitters `ViewZ` by `+ ThicknessZ * RayJitter` with jitter in [0,1) starting from the center
    `z + 0.5` (lines 722–725), covering `[z+0.5, z+1.5]` — biased half a slice deep instead of `[z, z+1]`.
- Recommendation: standardize on froxel centers (`xy + 0.5`, `z + 0.5`), one shared step-length definition,
  and symmetric jitter (`jitter − 0.5`).

### 1.9 `GetAnalyticOpticalDepth` falloff guard is wrong

`Volumetric Lighting.hlsl:223`:

```hlsl
float InverseFalloff = max(rcp(InFogFalloff), 1e-8); //CPU
```

- `rcp(0)` = +inf, and `max(inf, 1e-8)` is still inf; a zero falloff produces inf/NaN optical depths.
- The UI allows **negative** falloff (`Fog Falloff Height` slider min −1000, cpp:897), for which
  `max(rcp(x), 1e-8)` silently flips the sign to +1e-8, changing the curve's meaning entirely; other call
  sites feed the unvalidated fog-map falloff channel (§2.5).
- The `//CPU` note is right: sanitize falloff (e.g. `max(falloff, 1)`) and precompute the reciprocal on the
  CPU; in-shader, guard the fog-map path the same way.

### 1.10 Weather fog toggle is a no-op

- The only consumer of `GetWeatherBasedFog`/`UIUseWeatherFog` is commented out
  (`//OpticalDepth = lerp(OpticalDepth, HomogeneousOpticalDepth, FogParam.w * UIUseWeatherFog);`, line 511),
  so the "Enable Weather Fog" checkbox and all the CPU `fogParams` plumbing do nothing.
- The magic `* 0.002` on line 509 (weather fog factor → extinction) is undocumented, and `GetWeatherBasedFog`
  is fed a raw view depth where the game's fog formula expects its own normalized distance parameterization.
  Either finish and document the feature or remove the toggle from the UI until it works.

### 1.11 Correct-but-fragile: missing perspective divide in `FroxelWorldDirection`

`mul(CameraViewProjInverse, float4(ndc.xy, 0.0, 1.0)).xyz` (line 174) skips the homogeneous divide. This is
*actually correct* for this specific construction: for a standard perspective projection,
`P⁻¹ · (x, y, 0, 1)` yields the near-plane point scaled by `1/near`, i.e. a vector with **view depth exactly
1**, which is why `RayDirection * ViewZ` works. But nothing in the code says so — it silently breaks if the
NDC z reference, projection form (reversed-Z), or matrix convention changes. Add a comment, or divide by `w`
and normalize by view depth explicitly.

### 1.12 Miscellaneous shader issues

| Issue | Location |
| --- | --- |
| `EVSMBLUR_COMPUTE` is a **min-filter erosion**, not a blur; misnamed pass/UI label ("Erosion Kernal Size" vs `RenderEVSMBlur`). Cost is `(2r+1)²` taps — up to 289 loads/texel at shadow-map resolution × nCascades (default r=4 → 81). Consider separable min or downsampled EVSM (`EVSM_Size = CSM_Size` currently, the `/ 4` is commented out in `DataLoaded`). | hlsl:415-439, h:41 |
| `CSPhase` omits the `1/(4π)` normalization (`Normalization = 3/(2(2+g²))`), while the ambient path uses SH `EvaluatePhaseHG` which **is** normalized (L0 = 1/(2√π)). Direct light and ambient are therefore inconsistent by ~4π ≈ 12.6×, hidden inside the magic default multipliers. Normalize `CSPhase` and retune. | hlsl:592-602 vs `SphericalHarmonics.hlsli:124` |
| Sky ambient zonal phase is evaluated with "up" = `float3(0, 0, -1)` — that is **down** in Skyrim's Z-up world. | hlsl:689 |
| Blue-noise indexing hardcodes `& 63` / `& 31` in four places while the `NoiseSize` cbuffer field (64, 64, 32) goes unused. | hlsl:342, 471, 719, 850-851 |
| Unused/dead functions and blocks: `R2Sequence`, `GameUnitToMeter`, `VSM_Visibility`, `HGPhase`, `Uncharted2TonemapA`, `Exact_CubicBasisSpline3`, `LinearStep(float4)`, the wind-vectoring block (527-539), the box-SDF debug block (379-382), commented `GetAnalyticOpticalDepth` v1 (206-219), plus ~15 other commented experiments. Strip before release. | throughout |
| `APPLY_PIXEL` discards the marched transmittance (`NormalizedRadiance.w`) and recomputes it analytically (line 974) — intentional for DAA, but it means in-volume-only media (e.g. future noise/wind density) will brighten radiance without attenuating the scene. Document the constraint. | hlsl:955-975 |
| `GetLocalFogData` indexes the fog map with an unclamped `int2` (negative / out-of-range UVs for world positions behind the ortho projection); relies on D3D OOB-load-returns-zero. Clamp or early-out. | hlsl:455-460, 936-941 |
| `DRAW_FOGMAP` ignores `BrushFeather` (uploaded + exposed in UI), and line 1023 `OutputExtinction = CurrentExtinction + InputExtinction;` overwrites the `saturate`/`BrushAdditive` selection on the previous line, making the brush always additive and unclamped (negative extinction possible with Erase). | hlsl:1020-1027 |
| Typos baked into API-visible names: `UIAmibentLightingMultiplier`, `UIDisocclutionThreshold`, `UIDirLightMultipler`, `IntergrationVolume`, `EVSMSeachSize`, "Temprature"/"Kernal" in UI labels. Worth fixing while the feature is pre-release (settings JSON keys included). | hlsl/cpp/h |

---

## 2. CPU ↔ GPU parameterization and unit check

Constant-buffer layouts were verified field-by-field. **All four cbuffers (b0–b3) match** their CPU structs:

- `ShadowBuffer` (b0) ↔ `ShadowDataCB` — the 80-byte `LocalShadowLightTransform` stride (64B matrix + uint +
  3×uint pad) matches HLSL struct-array element alignment. ✔
- `FroxelBuffer` (b1) ↔ `FroxelGridCB` — order and packing match. ✔ (Shadow matrices are pre-transposed by
  `GetCascadeMatrix` to suit the `mul(M, v)` row-major convention; frame-buffer matrices are used in the same
  `mul(M, v)` form as the rest of the codebase. ✔)
- `FogMapperBuffer` (b2) ↔ `FogMapper` — matches, including the scalar tail packing. ✔
- `SettingsBuffer` (b3) ↔ `Settings` + `gFogParams` — the manual `float _pad[3]` before `scatteringRatio`
  correctly mirrors HLSL float4 alignment; every subsequent field lines up (`UIGlobalFogFalloff` ↔
  `globalFogFalloffHeight` at offset 100, `UIGlobalFogBaseHeight` ↔ `globalFogStartHeight` at 104, etc.). ✔

The real problems are in units, dispatch sizes, and resource state:

### 2.1 Unit mismatch: fog-map extinction is ~70× the global extinction

- Global: `UpdateSettingBuffer` converts the slider from per-meter to per-game-unit:
  `settingsData.cbsettings.extinction *= Util::Units::GAME_UNIT_TO_M;` (cpp:448, ×≈0.01428).
- Local fog map: the painted value goes through `fogMapper.FogData.w` → `FogMapInput.w` → stored raw in
  `FogMap.w` (`DRAW_FOGMAP`, hlsl:1020-1027) → consumed directly as a per-game-unit rate in `MEDIA_COMPUTE`
  (hlsl:506) and `APPLY_PIXEL` (hlsl:971).
- Both sliders share the same 0–0.6 range and the label "Extinction", but the painted fog is ~70× denser for
  the same number. Convert `localFogExtinction` with `GAME_UNIT_TO_M` at paint time (or in
  `UpdateFogMappingBuffer`) so the two are commensurable.

### 2.2 Magic constant on distant haze

`settingsData.cbsettings.distantHazeExtinction *= 0.000002;` (cpp:449) — the 0–1 slider becomes a
per-game-unit rate (max ≈ 1.4e-4 /m) with no comment. Express it as
`slider * <chosen max per-meter rate> * GAME_UNIT_TO_M` with the rationale documented.

### 2.3 Shadow-volume dispatch misses the last 4 depth slices

- Volume is 240×136×**68** (`volumeDimensions`, h:159). All volume passes use `numthreads(4,4,4)`.
- `GenerateMediaVolume` / `GenerateScatteringVolume` dispatch `(60, 34, 17)` → 68 slices ✔ (cpp:581, 629).
- `GenerateShadowVolume` dispatches `(60, 34, 16)` → **64 slices** (cpp:553). Slices 64–67 of the shadow
  volume are never written; SCATTER reads them (stale or undefined data at the far end of the volume).
  Change to 17 — or better, derive all three dispatches from `volumeDimensions` instead of hardcoding.

### 2.4 Pixel-shader sampler slots don't match the shader declarations

- HLSL declares `s10` linear, `s11` point, `s13` aniso-clamp, `s14` aniso-wrap (hlsl:121-124).
- CS path binds 10, 11, 13, 14 ✔ (`UpdateAndSetupResources`, cpp:351-354).
- PS path binds 10, 11, **12, 13** (`SetupApplyPassResources`, cpp:666-669) → in `APPLY_PIXEL`, s13 receives
  the *wrap* sampler and s14 is unbound. Benign today (the apply pass only uses s10/s11) but a latent
  landmine; fix the PS bindings to 13/14.

### 2.5 `fogMapTexture` is never initialized

Created without initial data and only ever written inside the brush radius (`DrawFogMap`), yet
`GetLocalFogData` samples it unconditionally every frame in MEDIA and APPLY. Until the user paints, the fog
contribution comes from an undefined texture (zeros in practice on most drivers, but undefined per spec —
and zero falloff then hits §1.9's `rcp(0)`). Clear the texture to a known "no fog" value
(e.g. `(0, 0, 1, 0)`) at creation, and clear `UIFogMapTexture` too.

### 2.6 Fragile matrix caching in `UpdateFroxelBuffer`

cpp:369-374 caches `CameraViewProjInverse` and refreshes it only when row 1 differs from the exact float
pattern `(-0, 0.5, 0, -0)` — an exact-equality fingerprint of some "bad" camera state (map menu / shadow
render). This will silently break with different FOV/aspect/dynamic-resolution values. Replace with an
explicit game-state check (e.g. skip the update while a full-screen menu camera is active), and note that
the cached matrix is **jittered** while `prevCameraViewProj` is unjittered (§1.5).

### 2.7 Dead or contradictory CPU-side data

| Item | Detail |
| --- | --- |
| `CameraView`, `CameraProj`, `CameraViewInverse`, `CameraProjInverse` | Uploaded every frame (cpp:377-380), never referenced by any pass. |
| `NoiseSize` | Uploaded, unused (shader hardcodes `& 63` / `& 31`). |
| `preExposure` / `UIExposure` | Serialized + uploaded, never used ("Obsolete Exposure" slider is commented out). |
| `EVSMData.z/w` | `exp(e)`, `exp(2e)` uploaded, shader recomputes from `UIEVSMExponent` instead (§1.7). |
| `frameCounter` | Incremented in `VLightingRenderChain`, never used (shader uses `SharedData::FrameCountAlwaysActive`). |
| `fogParams.w` | Set to `sky->fogClamp` on cpp:442 and immediately overwritten on cpp:443. |
| `Settings::_pad[3]` | Uninitialized memory uploaded to the GPU (harmless padding, but trivially fixable with `= {}`). |
| "Reload Shaders" button | Nulls raw COM pointers without `Release()` (leaks each reload) and omits `EVSMComputeShader`, `generateMediaVolumeCS`, `generatePerlinCS`, `bypassVS` from the reset list (they leak via reassignment in `CompileShaders`). |
| `DrawFogMap` | Unbinds only UAV slot 0; `fogMapUAV` stays bound on slot 1 while later frames bind the same texture as an SRV (D3D will force-null it with a warning). `numthreads(1,1,1)` with a per-texel dispatch is also the worst possible occupancy — use 8×8 threads and `(w+7)/8` groups. |
| `std::_Pi_val` (cpp:949) | MSVC-internal identifier; use `std::numbers::pi_v<float>`. |
| `#include "DynamicCubemaps.h"` (cpp:817) | Stray include above `DrawSettings`, unused. |
| History resources on toggle | `useHistory` swaps ping-pong indices only while enabled; volumes are never cleared, so re-enabling blends arbitrarily stale history for a frame (§3.6). |

### 2.8 Default values review

| Setting | Default | Assessment |
| --- | --- | --- |
| `nearPlane` / `farPlane` (froxel grid) | 100 / 353,840 GU (≈1.4 m / ≈5.1 km) | Reasonable with 68 exponential slices, λ = 1.6. Not serialized (resets each launch) — intentional? |
| `extinction` | 0.1 /m | Only sensible because of the height falloff below; at z = 0 the effective σ ≈ 6.7e-4 /m (≈4.5 km visibility). Fine, but worth a tooltip explaining the interplay. |
| `globalFogStartHeight` / `globalFogFalloffHeight` | −10,000 / 2,000 GU | Consistent game-unit heights vs `CameraPosition.z`. ✔ |
| `dirLightRadianceMultiplier` | 5.0 (slider 1–100) | Slider min of 1.0 prevents ever *reducing* the sun below 1× — lower the floor to ~0.1. The 5.0 partially compensates the missing 1/(4π) in `CSPhase` (§1.12). |
| `amibentLightingMultiplier` | 3.5 | Currently compensating the double-multiply bug (§1.3) — retune to ~1 after fixing. |
| `anisotropy` 0.3 / `localLightsAnisotropy` 0.6 | OK | Typical values. |
| `scatteringRatio` | (1,1,1) | Pure-scattering albedo; fine as default. |
| `esmExponent` | 8 (slider 1–100) | 8 is low for EVSM (light bleeding); slider max is unsafe (§1.7). Recommend default ≈ 20–40, max 40. |
| `disocclutionThreshold` | 0.05 | Reasonable once the hardcoded 0.8 blend (§1.1) is removed. |
| `EVSMSeachSize` | 4.0 (float, used as int) | 81 taps/texel at full shadow-map res ×3 cascades — expensive; see §1.12. |
| `useHistory` | **false** | The temporal filter is off by default while SHADOW relies on it to hide 4-sample shadow noise; after §3 fixes, default it to true. |
| `useWeatherFog` | false | Correct, since the feature is dead code (§1.10). |
| `localLightsMaxLum` 10 / `localLightsMinTemp` 0.8 | OK | Ad-hoc but functional; document the LumTempSat heuristic. |
| `distanceFadeIn` | 500 GU | Fine (≈7 m fade of in-scatter near camera). |

---

## 3. Plan: fixing ghosting in the temporal reprojection

Root causes in order of impact, each with its fix. Items 1–3 are the essential ones.

### 3.1 Compensate camera translation (the actual bug)

**Problem** (§1.5): `GetHistoryUV` reprojects current-eye-relative positions with `PrevCameraViewProj`,
which expects previous-eye-relative positions. History smears whenever the camera translates.

**Fix**:
1. Add the previous eye position (or the delta) to `FroxelGridCB`: on the CPU, keep last frame's
   `eyePositionWS` (`prevEyePositionWS`) and upload `eyeDelta = eyePositionWS − prevEyePositionWS`
   (update it in `UpdateFroxelBuffer` *before* overwriting the cached value).
2. In `GetHistoryUV`:
   ```hlsl
   float3 PrevRelPos = CoordsWS + EyeDelta.xyz;   // current-relative -> previous-relative
   float4 PrevClip   = mul(PrevCameraViewProj, float4(PrevRelPos, 1.0));
   ```
   (Equivalent to `absoluteWorldPos − prevEyePos`, matching `MotionBlur.hlsli` semantics.)
3. Use an **unjittered** current inverse view-projection for `FroxelWorldDirection` so the current and
   previous transforms differ only by real camera motion (replace the cached jittered matrix from §2.6).

### 3.2 Delete the hardcoded blend weight and honor confidence

Remove `ReprojectionValue = 0.8;` (hlsl:369) so the existing logic runs:

```hlsl
float ReprojectionValue = 1.0 - LinearStep(DeltaLimit, 1.0, abs(Shadow - ShadowHistory));
ReprojectionValue = Confidence * min(ReprojectionValue, MaxHistory) * UIUseHistory;
```

Also remove `Shadow = min(Shadow, 0.15);` (§1.1) — not a ghosting issue but it sits in the same block and
masks the result of any tuning.

### 3.3 Reject, don't clamp, out-of-volume history

- Keep `saturate(PrevCoordsUV)` on the *sample* (avoids wrap artifacts) but ensure the blend weight is zero
  when confidence fails — after §3.2 this is automatic (`Confidence` is 0/1).
- Improve on the binary cut with a border fade so newly revealed screen edges converge smoothly instead of
  popping:
  ```hlsl
  float2 border = min(PrevUV.xy, 1.0 - PrevUV.xy);
  Confidence *= smoothstep(0.0, 2.0 * InverseVolumeSize.x, border.x)
              * smoothstep(0.0, 2.0 * InverseVolumeSize.y, border.y);
  ```

### 3.4 Reproject the point that was actually shaded, consistently

- SHADOW: reprojection anchors at `z + 0.5` while shading anchors at `z − 2` (§1.8). Reduce the leak bias to
  ≤ 0.5 slice (the 2-slice bias also *creates* temporal mismatch because the history delta test compares
  values shaded at different depths) and use the same anchor for shading and reprojection.
- MEDIA: change the history anchor from `FroxelDepthToView(Froxel.z)` (edge) to `z + 0.5` (center) to match.
- Use centered XY (`Froxel.xy + 0.5`) in `FroxelWorldDirection` for all passes (§1.8) so reprojection,
  shading, and the apply-pass sampling agree spatially.

### 3.5 Replace the scalar delta test with neighborhood clamping

The current disocclusion metric (`abs(Shadow − ShadowHistory)` vs `UIDisocclutionThreshold`) can't
distinguish "history is stale" from "history is converged but the current sample is noisy", so it either
ghosts or flickers. Standard TAA-style clamp fixes both:

1. Compute min/max (or mean ± γ·σ) of the current-frame `Shadow` over the 3×3 XY neighborhood at the same
   slice (cheap in a compute pass via a few extra evaluations, or via LDS with a 6×6×4 group tile).
2. `ShadowHistory = clamp(ShadowHistory, NeighborhoodMin - slack, NeighborhoodMax + slack);` with
   `slack = UIDisocclutionThreshold` (keeps the existing setting meaningful).
3. Then blend with a constant `MaxHistory` (raise to 0.9–0.95 — with correct rejection + clamping, higher
   history no longer ghosts and shafts get visibly smoother).

The same pattern applies per-channel to the media volume (`min4`/`max4` over float4), which is what the
`//need min4` note (hlsl:554) was reaching for.

### 3.6 Re-enable the MEDIA temporal path deliberately

Currently MEDIA's history blend and its Z jitter are both commented out (hlsl:474, 545, 551-558), yet the
CPU still ping-pongs media volumes and binds the history SRV each frame. Either:

- **(preferred)** re-enable jitter + history with the §3.5 per-channel clamp — this is what makes painted
  fog-map edges and future noise-based density converge smoothly; or
- drop the media ping-pong (single volume, no history SRV) and save the bandwidth/memory until the temporal
  path is ready.

Additionally, clear both shadow-volume and media-volume history (or force `ReprojectionValue = 0` for one
frame) when `useHistory` is toggled on, when the volumes are (re)created, and on scene/cell transitions —
today re-enabling the toggle blends whatever stale data the history volume last held (§2.7).

### 3.7 Keep jitter and accumulation in balance

- The 16-frame `kPhi` (R2) sequence with `MaxHistory = 0.85` gives an effective accumulation window of
  ~6–7 frames — shorter than the jitter period, so the integral never fully converges and shimmers.
  After §3.1–3.5, either raise `MaxHistory` toward 0.94 (window ≈ 16 frames) or shorten the jitter cycle
  (e.g. `% 8`) so window ≥ cycle.
- Verification checklist for the whole section:
  1. Strafe sideways at walking speed past a shadow edge — no directional smear (validates §3.1).
  2. Snap the camera 180° — one-frame convergence at screen edges, no border streaks (validates §3.3).
  3. Toggle a nearby light source — in-scatter responds within ~2–3 frames without a ghost trail
     (validates §3.5).
  4. Stand still — shafts stable, no visible 16-frame pulsing (validates §3.7).
  5. RenderDoc: confirm shadow-volume slices 64–67 are written after the §2.3 dispatch fix, since stale far
     slices read as "temporal" artifacts in motion.
