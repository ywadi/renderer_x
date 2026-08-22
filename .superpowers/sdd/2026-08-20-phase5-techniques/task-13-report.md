# Task 13 report — Physical light units + punctual lights, KHR_lights_punctual consumption (issue #49)

Implementer round. Worktree: `/media/ywadi/second/renderer_x-worktrees/t13-physical-lights`,
branch `task/t13-physical-lights`, based at `942268b` (T12's SDD-records
commit / Stage 1 checkpoint). Order of authority followed: rulings
(`rulings-2026-08-20.md`, T13 per-ticket ruling + RC7) > brief
(`task-13-brief.md`) > gate matrix (`matrix-p5t13-physical-lights.md`) >
ticket (#49).

## Status: COMPLETE

Every acceptance criterion named in the brief/matrix/ticket is delivered
and value-asserted (table below). Both presets build clean. Full ctest
suites:

- **linux-native / lavapipe**: 42/42 (100%), 139.96s.
- **linux-native / real NVIDIA driver** (GeForce RTX 2080, driver
  580.82.07): 42/42 (100%), 243.87s.
- **windows-cross-zig / Wine** (CI's own GPU-exclusion filter,
  `-E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_material_brdf_gpu|
  rx_debug_ui_gpu|rx_frame_loop_gpu|rx_ibl_gpu|rx_conformance|sample'`):
  14/14 (100%), 121.60s — includes `rx_asset_gltf_gpu_tests` (this
  round's own new import-consumption TEST_CASE), unaffected.

Zero unfiltered Vulkan validation errors on any tier. Conformance harness
(`rx_conformance_render`, T11's gate) stays green on every driver
(6/6 mandatory + 1 optional + the discrimination proof, unchanged).

Commit(s) on `task/t13-physical-lights`: see "Commits" below. Nothing
pushed; main untouched.

## The reconciliation outcome, in one sentence

`EnvironmentDesc::intensity`/`RxDrawData::envIntensity` is now formally
documented as a LUX-denominated scale factor — the SAME unit
`DirectionalLightDesc::colorLux` and a punctual light's post-attenuation
contribution already use, matching Filament's own `IndirectLight::
Builder::intensity()` contract exactly (cited below) — with the neutral
1.0 default kept UNCHANGED (a documented, Task-4-precedented "no
correction" convention, not Filament's own 30000 default), so no D17
value changed and no reference regeneration was required.

## Unit-model derivation (Filament v1.75.0 + KHR_lights_punctual, cited)

Pin: `google/filament` **v1.75.0**, tag commit
`0e58877c09afb1aacd09ff640f74d2adcd2a7e80` (RC1's pin; tag SHA
independently re-verified this session via `gh api repos/google/filament/
git/refs/tags/v1.75.0`). `KHR_lights_punctual` spec: Khronos glTF commit
`2b29723d025a995971726f2989697cdc49b1222a` (README.md, fetched and quoted
verbatim this session, not paraphrased). Conformance cross-check:
`KhronosGroup/glTF-Sample-Renderer` commit
`863b981fb755359063e370ff7b6e956bda0716e2` (T11's own conformance-harness
pin) — `source/Renderer/shaders/punctual.glsl`, fetched this session.

### Units (KHR spec, README.md, quoted)

> `point` and `spot` lights use luminous intensity in candela (lm/sr)
> while `directional` lights use illuminance in lux (lm/m²)... The
> `intensity` represents the luminous intensity that the light would emit
> if it were colored pure white. The `color` property acts as a
> wavelength-specific multiplier.

> Because it is at an infinite distance, the light is not attenuated.

So `colorLux`/`colorCandela` = `color * intensity` verbatim, per type, and
directional NEVER attenuates. `PointLightDesc`/`SpotLightDesc`/
`DirectionalLightDesc` (`src/rx_scene/include/rx_scene/scene.h`) document
this exactly; `instantiateImportedLights()` (same file) performs the
pass-through with **zero unit conversion**, matching the matrix's own
acceptance criterion ("1500 candela imports as EXACTLY 1500.0, not
1500/(4π)") — value-asserted in both `scene_test.cpp` (hand-built
`LightData`) and `import_gltf_gpu_test.cpp` (a real, GPU-backed
`importGltf()` on the committed `assets/test/cube_lights_camera.gltf`
fixture, which pre-existed this task, already carrying a directional +
point + spot light with known values).

**Filament's own lumens/watts → candela authoring convenience is
deliberately NOT built** (a ruling-sanctioned, "log-don't-drop" scope
line, matrix's own text: "If Task 13 builds no such convenience API at
all, this is a clean, documented registry deferral — not a Task 13 gap").
Cited for a future implementer anyway (`LightManager.cpp:121-122`,
`348-397`, google/filament v1.75.0):

```
mIntensity = efficiency * 683.0f * watts;                          // watts -> lumens (683 lm/W)
// POINT:         li = lp / (4*pi)
// SPOT (plain):   li = lp / pi
// FOCUSED_SPOT:   li = lp / (2*pi*(1-cos(outerConeAngle)))          // correct solid-angle form
```

### Range-property attenuation — the resolved point/range formula question

KHR spec, README.md, quoted verbatim:

> A recommended implementation for this attenuation with a cutoff range is
> as follows: **attenuation = max( min( 1.0 − (current_distance /
> range)⁴, 1 ), 0 ) / current_distance²**

Filament's shipped shader (`shaders/src/surface_light_punctual.fs:93-99`)
SQUARES the window term an extra time
(`getSquareFalloffAttenuation`/`smoothFactor*smoothFactor`) — a real,
verified discrepancy the gate matrix's own Open Question named and
recommended resolving in favor of the spec's literal form (the
conformance-path default). **Independently confirmed this session**: the
pinned glTF-Sample-Renderer's own `getRangeAttenuation()`
(`punctual.glsl:33-40`) uses the IDENTICAL **un-squared** form:

```c
float getRangeAttenuation(float range, float distance) {
    if (range <= 0.0) { return 1.0 / pow(distance, 2.0); }          // "negative range means unlimited"
    return max(min(1.0 - pow(distance / range, 4.0), 1.0), 0.0) / pow(distance, 2.0);
}
```

So the spec text, its own reference code, AND the actual Khronos
conformance renderer (T11's own harness target) all agree on the literal,
un-squared form — Filament's shader is the outlier (a deliberate
"cinematic" softening). **RendererX ports the literal spec form**,
matching all three sources. Cited in `src/rx_scene/light_math.h`'s own
header comment and `standard_pbr.slang`'s punctual term.

Ported into `rx::scene::lightmath::rangeAttenuation()` (device-free C++,
`src/rx_scene/light_math.{h,cpp}`) AND `standard_pbr.slang`'s punctual
term — the SAME closed form in both places, value-asserted independently
in each (`light_math_test.cpp` device-free; `test_standard_pbr_punctual_
gpu.cpp` GPU render).

### Spot cone attenuation

KHR spec's own "reference code" (README.md, quoted verbatim):

```c
float lightAngleScale = 1.0f / max(0.001f, cos(innerConeAngle) - cos(outerConeAngle));
float lightAngleOffset = -cos(outerConeAngle) * lightAngleScale;
// shader:
float cd = dot(spotlightDir, normalizedLightVector);
float angularAttenuation = saturate(cd * lightAngleScale + lightAngleOffset);
angularAttenuation *= angularAttenuation;
```

Filament's own shader (`surface_light_punctual.fs:111-114`,
`getAngleAttenuation`) and CPU-side call site (`:168`,
`getAngleAttenuation(-direction, light.l, scaleOffset)`) implement the
ALGEBRAICALLY IDENTICAL form once the sign convention is worked through
(`-direction` negates the light's own stored facing axis, `light.l` is
the fragment's own "toward light" vector — `dot(-direction, l) ==
-dot(direction, l)`, the same shape this port's own `dot(spotDirWorld,
-lightDir)` produces, `lightDir` here being the "toward light" vector).
**Correction (Fix round 1 — see that section below for the full erratum):
there is NO divergence.** An earlier version of this report claimed the
pinned glTF-Sample-Renderer's own `getSpotAttenuation()`
(`punctual.glsl:48-61`) used "a different, simpler formula... no
squaring" — that claim was a plain misreading of the fetched source and
is factually wrong: the function's own last statement, verbatim, is
`return angularAttenuation * angularAttenuation;` — it DOES square.
Independent review (`task-13-review.md`, Adjudication 2) caught this and
supplied the algebraic proof, reproduced here: `getSpotAttenuation()`'s
`angularAttenuation = (actualCos - outerConeCos) / (innerConeCos -
outerConeCos)` is the IDENTICAL expression to the KHR reference code's
`cd*lightAngleScale + lightAngleOffset` once expanded — substituting
`lightAngleScale = 1/(cosInner-cosOuter)` and `lightAngleOffset =
-cosOuter*lightAngleScale` gives `cd*scale+offset =
scale*(cd-cosOuter) = (cd-cosOuter)/(cosInner-cosOuter)`, the exact same
ratio — and glTF-Sample-Renderer's explicit `if/else` branching (return 0
below `outerConeCos`, return 1 above `innerConeCos`, the ratio between)
is exactly what `saturate(...)` does to that same expression. **All
three sources (KHR spec reference code, Filament, glTF-Sample-Renderer)
agree exactly** on the spot-cone falloff shape — this port conforms to
all three, not two of three, and there was never a conformance risk of
any kind, present or future, from it.

**Sign-convention derivation** (own work this session, cross-checked
against BOTH glTF-Sample-Renderer's `pointToLight`/`-pointToLight` naming
and Filament's `light.l`/`-direction` call): `LightRecord::direction`
(Spot) is the light's own FACING/TRAVEL axis (glTF's local −Z rotated,
matching `DirectionalLightDesc::dir`'s pre-existing convention exactly —
no new field-semantics invented). `standard_pbr.slang`'s punctual term
computes `lightDir = normalize(lightPositionWorld - worldPos)` (the
existing "toward light" convention every direct-light NdotL/half-vector
term already uses) and `cd = dot(normalize(lightSpotDirWorld), -lightDir)`
— `-lightDir` is the vector FROM the light TOWARD the fragment (the
travel direction at that fragment), which equals the fixed cone axis
exactly when the fragment sits dead-center (`cd == 1`). Verified
EMPIRICALLY, not just algebraically: `test_standard_pbr_punctual_gpu.cpp`'s
spot-cone TEST_CASE renders a tilted-facing-direction sweep and gets
`cd≈1` (full brightness) at `theta=0`, the closed-form partial value at
`theta=0.4`, and EXACT black at `theta=0.8` (outside the outer cone).

## What shipped

**`src/rx_scene`**:
- `light_math.h`/`.cpp` (NEW) — device-free `rangeAttenuation()`,
  `spotAngleScaleOffset()`, `spotAngleAttenuation()`, cited/derived above.
- `scene.h`/`.cpp` — `PointLightDesc`/`SpotLightDesc` (mirroring
  `DirectionalLightDesc`'s shape exactly, per the matrix's own
  recommendation); `Scene::createPointLight()`/`createSpotLight()`;
  accessors `lightType()`/`lightPosition()`/`lightRange()`/
  `lightInnerConeAngle()`/`lightOuterConeAngle()` (existing
  `lightDirection()`/`lightColorLux()` reused, now documented as
  per-type-unit-dependent); `instantiateImportedLights(Scene&,
  std::span<const asset::LightData>)` — the import-consumption entry
  point, position/direction derived from each light's own
  `LightData::worldTransform` (translation column / rotated local −Z),
  KHR cone-angle defaults (0/π4) applied when `std::nullopt`.
  `EnvironmentDesc::intensity`'s header comment rewritten for the
  reconciliation (see above).

**`src/rx_material/include/rx_material/draw_data.h`**: `DrawDataGpu`
gains a tail-appended punctual-light bundle (`lightType`/`lightRange`/
`lightAngleScale`/`lightAngleOffset`/`lightPositionWorld`/
`lightSpotDirWorld`) starting at the existing 384-byte boundary
`_padEnv0` (T10) already established — 48 bytes, naturally 16-aligned on
both the C++ and Slang std430 sides with no additional padding field.
`static_assert(sizeof(DrawDataGpu) == 432, ...)`. Every default
reproduces byte-identical Directional-only (pre-Task-13) behavior.

**`shaders/material/material.slang`**: `RxDrawData` mirrors the C++
struct field-for-field (same tail-append, same 384-byte-boundary
reasoning). `MaterialVertex` gains the same bundle (populated in
`forward_entry.slang`'s `fragmentMain`, NOT carried through
`VertexStageOutput` — same "per-pass-constant, nothing to gain from
interpolating it" idiom the T10 env fields already established).

**`shaders/material/forward_entry.slang`**: `fragmentMain` forwards the
punctual bundle from the already-re-read `RxDrawData` row into
`MaterialVertex`, mirroring the env-field forwarding exactly.

**`shaders/material/standard_pbr.slang`**: the single "one directional
light" term is generalized to Directional/Point/Spot — `lightType==0`
(Directional) is BYTE-IDENTICAL to the pre-Task-13 expression (unit
`lightDir`, `lightAttenuation=1.0`); Point/Spot derive `lightDir`
per-fragment from `lightPositionWorld`, apply `rangeAttenuation()`'s
ported closed form, and (Spot only) the cone term. This is explicitly a
FOUNDATIONAL mechanism: Task 14/15's clustered Forward+ path will later
reuse these exact attenuation formulas inside a per-pixel froxel light
loop (the plan's own text: "The lit path consumes cluster lists for
point/spot (directional stays direct)") — the math does not change, only
the per-light data SOURCE does (one draw-data row today; a froxel light
list later).

**`src/rx_asset/tests/import_gltf_gpu_test.cpp`** (+`CMakeLists.txt`
rx_scene link): new TEST_CASE driving the REAL GPU-backed
`registry.importGltf()` on the pre-existing `assets/test/
cube_lights_camera.gltf` fixture (a directional + point + spot light,
each with known values — this fixture's own `generator` field already
says "Task 13", i.e. it was pre-staged for this round) through
`instantiateImportedLights()` into a real `Scene`, asserting exact
candela/lux/position/direction/range/cone values.

**`src/rx_material/tests/test_standard_pbr_punctual_gpu.cpp`** (NEW,
+`CMakeLists.txt`): 5 GPU TEST_CASEs (table below) — a trimmed rig
duplicate (this suite's own established per-file convention).

**`src/rx_scene/tests/light_math_test.cpp`** (NEW, +`CMakeLists.txt`): 5
device-free TEST_CASEs for the attenuation math itself, hand-computed
(python3, double precision) expected values.

**`src/rx_scene/tests/scene_test.cpp`**: +5 TEST_CASEs — `PointLightDesc`/
`SpotLightDesc` defaults+round-trip, tri-type coexistence,
`instantiateImportedLights()` value-asserted decoded-value test + an
empty-input no-phantom-light test.

**`samples/09_scene/main.cpp`** — real production wiring, not just a
library API:
- `populateImportedInstances()` (`--scene <path>` mode): calls
  `instantiateImportedLights()` on whatever the imported glTF itself
  carries (EVERY light, any type, is created in the Scene for real —
  `scene->lightCount()` reflects them). If the imported scene has its own
  Directional light, it becomes `app.lightHandle` (this sample's single
  main-light/shadow slot); otherwise (Point/Spot-only, or no lights at
  all — e.g. Sponza) falls back to the SAME hardcoded key light every
  earlier round already used, byte-identical. Point/Spot production
  shading through this sample's own single-light forward path is
  explicitly out of scope (Task 14/15's clustered consumption owns that,
  per the plan's own "directional stays direct" framing) — the lights
  still exist, correctly, in the Scene; they are just not yet driving
  pixels in THIS sample's presentation.
- `updateSceneFrame()`: the per-frame `DrawDataGpu` population now reads
  `lightDirWorld`/`lightColorLux` GENERICALLY from `Scene` via
  `app.lightHandle` (guarded by `isLightAlive()`, falling back to the
  pre-existing hardcoded `(5,5,5)`/`app.lightDirWorld` pair exactly as
  before when no light backs the handle — `--stress` mode's own
  Registry-free scene never creates a light, confirmed unaffected) —
  this REPLACES a literal hardcoded `(5,5,5)` that never actually read
  `Scene::lightColorLux()` at all, closing a real "Scene light created
  but never consumed by this sample's own draw-data producer" gap.
  Byte-identical for every pre-existing path (verified: D17 gate
  `failingPixels=0/65536`, no reference regeneration needed).

## Per-row proof (gate matrix)

| Matrix row | Disposition | Proof |
|---|---|---|
| Directional light unit (lux) | Delivered (pre-existing field, wiring completed) | `DirectionalLightDesc::colorLux` unchanged; `instantiateImportedLights()` passes glTF `intensity` straight through — `scene_test.cpp`/`import_gltf_gpu_test.cpp` assert EXACT `3.0`/`2.7`/`2.4` from the fixture's `[1,0.9,0.8]*3.0`. |
| Point/spot lights have no public Scene creation API | Delivered | `Scene::createPointLight()`/`createSpotLight()`, mirroring `createDirectionalLight()`'s shape exactly — `scene_test.cpp`'s 3 new TEST_CASEs. |
| glTF intensity already candela/lux — no conversion at import | Delivered, value-asserted | `instantiateImportedLights()`: `colorIntensity = light.color * light.intensity`, no scaling — `import_gltf_gpu_test.cpp` asserts EXACT `500.0`/`0.0`/`0.0` (point) and `0.0`/`200.0`/`0.0` (spot) from the fixture. |
| Lumens/watts authoring convenience | log-don't-drop, deliberately NOT built | Cited (Filament formulas) in this report + `light_math.h`'s header comment for a future implementer; matrix's own text sanctions this as a clean deferral. |
| Point-light inverse-square + range windowing | Delivered, spec-exact (literal KHR form) | `light_math.h`/`.cpp` + `standard_pbr.slang` port; `light_math_test.cpp` (hand-computed); `test_standard_pbr_punctual_gpu.cpp`'s near-field ratio probe (measured 3.95 vs. expected 4.0) AND range-window discrimination probe (ranged ratio 8.71 vs. unranged 3.30 at the SAME two distances — the windowing term's own real effect, quantified) + a literal revert-discrimination proof (see below). |
| Spot cone attenuation | Delivered, matches ALL THREE sources exactly (KHR reference code, Filament, glTF-Sample-Renderer — corrected per Fix round 1, no divergence) | `light_math_test.cpp`'s 4-angle closed-form table; `test_standard_pbr_punctual_gpu.cpp`'s 3-angle sweep (full/partial/EXACT-zero outside the cone). |
| Directional: literally unattenuated | Delivered, regression-proven | `standard_pbr.slang`'s `lightType==0` branch never touches `lightAttenuation` (stays 1.0); `test_standard_pbr_punctual_gpu.cpp`'s directional-regression TEST_CASE: identical pixel at two different world positions. |
| Range absent = infinite, pure inverse-square | Delivered, conditional-on-presence proven | `range<=0.0` sentinel (matches `LightRecord`'s pre-existing "0.0=inert" convention AND glTF-Sample-Renderer's own reading); the range-window discrimination TEST_CASE contrasts a ranged vs. unranged light at the identical two distances. |
| Pre-exposure / range policy (Task 4 dependency) | Delivered — reconciled | See "reconciliation outcome" above; `test_standard_pbr_punctual_gpu.cpp`'s pre-exposure-coherence TEST_CASE proves a Point light scales by EXACTLY `Camera::exposure()`'s own ratio (1.672 measured vs. 1.667 expected), with an explicit double-application discriminator (the squared ratio is asserted to NOT match). |

## Test counts (this round's own new/touched suites)

| Suite | Driver | Result |
|---|---|---|
| `rx_scene_tests` (device-free) | n/a | 93/93 test cases, 6681/6681 assertions |
| `rx_material_gpu_tests` | lavapipe | 74/74 test cases, 3959/3959 assertions |
| `rx_material_gpu_tests` | NVIDIA RTX 2080 (580.82.07) | 74/74 test cases, 3959/3959 assertions |
| `rx_asset_gltf_gpu_tests` | lavapipe | 62/62 test cases, 1350/1350 assertions |
| Full ctest (linux-native) | lavapipe | 42/42 tests |
| Full ctest (linux-native) | NVIDIA RTX 2080 (580.82.07) | 42/42 tests |
| Full ctest (windows-cross-zig, CI's own GPU-exclusion filter) | Wine | 14/14 tests |

## Revert-discrimination proof (empirical, this round)

The range-window formula in `standard_pbr.slang` was temporarily edited
(`rangeWindow = 1.0;` unconditionally, the `if (v.lightRange > 0.0)`
block commented out with an explicit `TEMPORARY REVERT ... DO NOT COMMIT`
marker) and the punctual GPU suite re-run (no C++ rebuild needed — Slang
compiles in-process at test-binary startup):

- Before revert: `rangedRatio=8.71` (expect ~8.83), `unrangedRatio=3.30`
  (expect ~3.24) — 2 CHECK failures expected to be ABSENT.
- After the temporary revert: `rangedRatio` collapsed to EXACTLY
  `unrangedRatio` (`3.3 == 3.3`) — the two CHECKs asserting the windowed
  formula's own closed-form value and the discrimination margin both
  failed, quantified:
  ```
  ERROR: CHECK( near(rangedRatio, 8.8325F, 0.15F) ) is NOT correct!
  ERROR: CHECK( rangedRatio > unrangedRatio * 2.0F ) is NOT correct!
  ```
- Restored via `Edit` (verified via `git diff` showing the intended
  closed-form body, no leftover revert markers); re-ran the punctual
  suite: 5/5 test cases, 173/173 assertions, green (lavapipe).

## Command tails

**lavapipe, `rx_material_gpu_tests` (full, incl. the new punctual suite):**
```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a \
    build/linux-native/src/rx_material/tests/rx_material_gpu_tests --validate
...
[doctest] test cases:   74 |   74 passed | 0 failed | 0 skipped
[doctest] assertions: 3959 | 3959 passed | 0 failed |
[doctest] Status: SUCCESS!
```

**real NVIDIA driver (RTX 2080, 580.82.07), same binary:**
```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json xvfb-run -a \
    build/linux-native/src/rx_material/tests/rx_material_gpu_tests --validate
...
[doctest] test cases:   74 |   74 passed | 0 failed | 0 skipped
[doctest] assertions: 3959 | 3959 passed | 0 failed |
[doctest] Status: SUCCESS!
```

**full ctest, lavapipe:**
```
$ VK_ICD_FILENAMES=.../lvp_icd.json xvfb-run -a ctest --test-dir build/linux-native --output-on-failure -j1
...
100% tests passed, 0 tests failed out of 42
Total Test time (real) = 139.96 sec
```

**full ctest, real NVIDIA:**
```
$ VK_ICD_FILENAMES=.../nvidia_icd.json xvfb-run -a ctest --test-dir build/linux-native --output-on-failure -j1
...
100% tests passed, 0 tests failed out of 42
Total Test time (real) = 243.87 sec
```

**windows-cross-zig / Wine (CI's own GPU-exclusion filter):**
```
$ xvfb-run -a ctest --test-dir build/windows-cross-zig \
    -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_material_brdf_gpu|rx_debug_ui_gpu|rx_frame_loop_gpu|rx_ibl_gpu|rx_conformance|sample' \
    --output-on-failure
...
100% tests passed, 0 tests failed out of 14
Total Test time (real) = 121.60 sec
```

**sample09, lavapipe, default D17 gate (byte-identical, no regen needed):**
```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a \
    build/linux-native/samples/09_scene/sample_09_scene --validate
...
[info] sample_09_scene: D17 grid_scene gate: failingPixels=0/65536 (0.0000%) pass=true
[info] sample_09_scene: headless gate PASSED
```

## Self-review

- **TDD discipline**: every attenuation formula's expected value was
  computed independently (python3, double precision) from the cited spec
  text BEFORE the test assertions were written; the range-window
  discrimination test's two distances (d=5,9) were deliberately chosen
  (not d=1,9) after an earlier attempt's own logged raw pixel values
  revealed 8-bit clipping — a real, disclosed, empirically-caught test
  design bug, not silently patched over.
- **No deferred fixes**: the std430-layout ordering mistake (new fields
  placed BEFORE the existing `_padEnv0` pad, breaking 16-byte alignment)
  was caught by the struct's own `static_assert` failing to compile,
  fixed in the same round, before any test ran against it.
- **Revert-discrimination**: see the dedicated section above — a real,
  quantified gate-flip against the actual shader-compiled output.
- **No AI attribution**: none added anywhere (commit messages, code
  comments, this report).
- **Library-first**: every unit-conversion/attenuation formula is a
  direct, cited port (KHR spec's own reference code / Filament v1.75.0 /
  cross-checked against glTF-Sample-Renderer) — no formula was
  independently re-derived from first principles where a citable source
  existed. The ONE from-scratch element is the sign-convention
  reconciliation between the three sources' differing internal
  "direction" storage conventions (documented + empirically verified,
  not itself a new formula).
- **Performance-first**: zero per-object descriptor churn added — the
  punctual-light bundle is 48 more bytes on an ALREADY-existing
  per-draw StructuredBuffer row (the exact "pooled/indexed light data"
  shape CLAUDE.md's own performance mandate names), not a new binding or
  per-draw push constant. The single-light-per-draw mechanism is
  explicitly a FOUNDATIONAL step Task 14/15 build on (clustered
  Forward+), not a scalability dead end — flagged in-code at the exact
  spot a future implementer extends it.
- **Scope discipline**: no clustered/multi-light consumption attempted
  (T14/T15's own scope, per the plan's explicit "directional stays
  direct... [Point/Spot] consumes cluster lists" framing); no shadow-
  casting support for Point/Spot (T18's cubemap-shadow scope); no
  lumens/watts authoring convenience (ruling-sanctioned deferral, cited
  above); sample08_gltf_viewer untouched (it never used `rx::scene::
  Scene` at all — confirmed by inspection — so this ticket's Scene-API
  growth does not reach it; its own hardcoded directional light is
  unaffected, `lightType` defaults to 0 for every row it produces).
- **Commit scope**: pathspec-scoped to exactly the files listed under
  "What shipped" above.

## Concerns for the coordinator

1. ~~Spot cone formula divergence from T11's own conformance-renderer
   pin~~ — **RETRACTED, Fix round 1**: this was a factual error in the
   original round's own reading of the fetched `glTF-Sample-Renderer`
   source (a plain misreading — the function DOES square its result and
   IS algebraically identical to the ported KHR/Filament form). Caught by
   independent review (`task-13-review.md`, Finding 1/Adjudication 2); see
   the "Spot cone attenuation" section above and "Fix round 1" below for
   the corrected derivation. There is no conformance risk, present or
   future, from the shipped spot-cone port — all three sources agree
   exactly.
2. **Sample09's single-light forward/shadow slot stays Directional-only
   in production**, per the plan's own T14/15 phasing — Point/Spot lights
   an imported scene carries ARE created in the Scene (real, inspectable,
   value-correct) but do not yet drive any pixel in this sample's own
   presentation. This is the deliberate, plan-sanctioned boundary, not an
   oversight.
3. **No new committed asset**: `assets/test/cube_lights_camera.gltf`
   already existed in the worktree at task start (its own `generator`
   field says "Task 13"), pre-staged by an earlier gate-research/setup
   session — reused as-is, not modified, for both the pre-existing parse
   test and this round's new consumption test.

---

## Fix round 1 (`task-13-review.md`: spec PASS, quality Approved — 1 MEDIUM + 2 LOW)

Commit on `task/t13-physical-lights` (base `5878c7e`, this task's own
first-round commit). Same worktree. Nothing pushed; main untouched.

### Finding 1 (MEDIUM) — spot-cone "divergence" erratum, corrected

The original round's report claimed the pinned `glTF-Sample-Renderer`'s
`getSpotAttenuation()` used "a different, simpler formula... no
squaring." **That claim was a plain misreading of the fetched source and
is false** — the function's own last statement is, verbatim,
`return angularAttenuation * angularAttenuation;`. It DOES square, and —
per the reviewer's own algebraic derivation, independently re-confirmed
here from this task's own already-fetched source (no re-fetch needed;
the earlier round's own error was in reading it, not in fetching it) —
is the identical closed form to the KHR reference code's scale/offset
saturate-squared curve: `getSpotAttenuation()`'s `angularAttenuation =
(actualCos-outerConeCos)/(innerConeCos-outerConeCos)` equals
`cd*lightAngleScale+lightAngleOffset` exactly once `lightAngleScale =
1/(cosInner-cosOuter)` and `lightAngleOffset = -cosOuter*lightAngleScale`
are substituted in (`scale*(cd-cosOuter) = (cd-cosOuter)/(cosInner-
cosOuter)`), and `glTF-Sample-Renderer`'s explicit `if/else` branching
(0 below `outerConeCos`, 1 above `innerConeCos`, the ratio between) is
exactly what `saturate(...)` does to that same expression. **All three
sources (KHR spec reference code, Filament, glTF-Sample-Renderer) agree
exactly** — there was never a divergence, and the shipped code (which was
never in question — only this report's own narrative was wrong) conforms
to all three, not two of three.

**Fix**: corrected the "Spot cone attenuation" section and "Concerns for
the coordinator" item 1 above (struck through, not silently deleted, so
the erratum itself stays visible in the record) and the per-row proof
table's "Spot cone attenuation" row. No code, test, or shader change —
`shaders/material/standard_pbr.slang`, `src/rx_scene/light_math.{h,cpp}`,
and their own test suites were correct in the first round and remain
unchanged by this fix.

### Finding 2 (LOW) — `default:` case added to `instantiateImportedLights()`'s switch

`src/rx_scene/scene.cpp`'s `switch (light.type)` gains a `default:` arm
that logs the unhandled raw enum value and throws `std::logic_error`,
matching this class's own established "a mutator/accessor hitting an
unexpected state fails loudly" convention (`requireLiveLight()`/
`requireLiveRenderable()`'s own throw-on-stale-handle idiom) rather than
this project's OTHER, different idiom (WARN-log-and-degrade) reserved for
genuinely externally-sourced/malformed data
(`mapFastgltfError()`/animation-sampler-type switches) — `asset::
LightData::Type` is this engine's OWN internal enum, so an unreachable
case here means a new enumerator was added without updating this switch,
a programming bug, not recoverable malformed input. Verified: `rx_scene`
builds clean; `rx_scene_tests` (device-free, exercises this switch via
both `scene_test.cpp`'s and this fix round's own re-run) stays 93/93,
6681/6681 assertions, unaffected.

### Finding 3 (LOW) — env-vs-punctual-light lux coherence GPU test added

New TEST_CASE, `src/rx_material/tests/test_standard_pbr_punctual_gpu.cpp`
("StandardPBR: environment-lux and punctual-light-lux compose in ONE
coherent photometric frame"). Turns Adjudication 1's own dimensional
re-derivation into a value assertion, per the coordinator's own dispatch
text.

**Derivation**: a uniform environment of radiance `L` (isolated via
`dfg=(0,0)` so `iblDiffuse == diffuseColor*L` exactly and
`iblSpecular==0`) is compared against a directional light of colorLux
`C = L*envIntensity*π` — the standard photometric identity a
uniform-radiance-`L` hemisphere integrates to under Lambert's cosine law
(irradiance `== π*L`, the SAME identity `test_standard_pbr_ibl_gpu.cpp`'s
own pre-existing Lambertian TEST_CASE already documents/relies on). The
directional side is isolated to PURE Lambertian diffuse via `ior=1.0`
(forces dielectric `F0=(0,0,0)` exactly, `computeDielectricF0F90()`);
combined with this rig's own head-on geometry (`VdotH==1` exactly),
`F_Schlick(f0=0,f90,VoH=1) == f0 == 0` identically (`brdf.slang`:
`f0+(f90-f0)*pow(clamp(1-VoH,0,1),5)`, and `clamp(1-1,0,1)==0` zeroes the
whole `p5` term) — the direct specular lobe is EXACTLY zero, leaving
`directLight == diffuseColor*Fd_Lambert()*C*NdotL == C/π == L*envIntensity`
exactly. Both producers are pre-exposed by the IDENTICAL single CPU-side
multiply (`envIntensity*exposure` / `colorLux*exposure`), checked at
BOTH neutral (`exposure==1.0`) and a non-neutral Camera exposure
(`setExposure(-1.0F)` → `exposure()==5/3`).

**Numbers (`kRadiance=0.4`, `kEnvIntensity=1.0` → predicted `round(255*0.4)
== 102`)**:

| Exposure | env pixel | directional pixel | Both drivers |
|---|---|---|---|
| 1.0 (neutral) | 102 | 102 | lavapipe AND NVIDIA — identical |
| 5/3 (`setExposure(-1.0F)`) | 170 | 170 | lavapipe AND NVIDIA — identical |

Exact match at both exposure levels, both drivers — no rounding
divergence observed in this run (the TEST_CASE's own tolerance, ±0.06×,
is generous enough to absorb it if a future run's fp16-texture-path
rounding differs slightly from the pure-fp32 direct path).

**What this discriminates (empirically reproduced, not just claimed)**:
temporarily sabotaged `standard_pbr.slang`'s `directLight` expression
with a stray `/ 12.56637061` (`4π`) divisor — simulating a
candela-style-steradian-normalization bug bleeding into the lux path —
and re-ran (no C++ rebuild needed, Slang compiles in-process):

```
env-vs-punctual coherence @ exposure=1.0: env=102 directional=8  (predicted 102)
CHECK( near(dirNeutral.r, envNeutral.r, 0.06F) ) is NOT correct!
env-vs-punctual coherence @ exposure=1.66667: env=170 directional=14
CHECK( near(dirBright.r, envBright.r, 0.06F) ) is NOT correct!
[doctest] test cases: 1 | 0 passed | 1 failed | 74 skipped
```

`102/8 ≈ 12.75`, matching the injected `4π ≈ 12.566` sabotage factor
closely (the residual is 8-bit quantization noise on the now much-dimmer
sabotaged channel). Restored via `Edit` back to the exact original
expression; `git diff` on the file empty (byte-identical); re-ran: 75/75
test cases, 4092/4092 assertions, green (lavapipe). A stray SQUARED
exposure was not separately sabotaged this round (the neutral-exposure
case is unaffected by squaring — `1.0²==1.0` — so this specific class of
bug is caught only by the non-neutral-exposure half of the assertion,
which is exactly why that second exposure level is a load-bearing part
of the test, not decorative — the TEST_CASE's own header comment states
this explicitly).

### Empirical bar closed this round

- **`rx_material_gpu_tests`, lavapipe** (`VK_ICD_FILENAMES=lvp_icd.json
  xvfb-run -a ... --validate`): 75/75 test cases, 4092/4092 assertions,
  zero unfiltered validation errors.
- **`rx_material_gpu_tests`, real NVIDIA** (RTX 2080, driver 580.82.07,
  identical invocation): 75/75 test cases, 4092/4092 assertions —
  IDENTICAL numbers to lavapipe for the new coherence TEST_CASE (102/102
  neutral, 170/170 non-neutral, both drivers).
- **`rx_scene_tests`** (device-free, `default:` case fix): 93/93 test
  cases, 6681/6681 assertions.
- **Full ctest, lavapipe** (`VK_ICD_FILENAMES=lvp_icd.json xvfb-run -a
  ctest --test-dir build/linux-native --output-on-failure -j1`): 42/42
  (100%), 150.61s.
- **Full ctest, real NVIDIA** (RTX 2080, driver 580.82.07, identical
  invocation): first attempt showed 1 failure —
  `rx_asset_gltf_gpu_tests`'s own `async_import_test.cpp` wall-clock
  CI-stall-detector TEST_CASE (`REQUIRE(maxPumpDuration <
  kCiStallDetector)`), a PRE-EXISTING, previously-documented timing flake
  under concurrent build/test load (the IDENTICAL flake class task-10-
  report.md's own windows-cross-zig run already disclosed: "a pre-existing
  timing flake, unrelated to this round's own changes") — `async_import_
  test.cpp` is untouched by this task's own diff (both rounds). Confirmed
  as a flake, not a regression: `rx_asset_gltf_gpu_tests` re-run in
  isolation immediately after: 62/62 test cases, 1350/1350 assertions,
  clean. Full ctest re-run clean: **42/42 (100%), 209.93s**.

Note: `windows-cross-zig`/Wine was not re-run this fix round (unaffected
by any of the three findings — no shader/C++ code touched under that
lane's own build besides `scene.cpp`'s `default:` addition, which
`rx_scene_tests`/`rx_asset_gltf_gpu_tests` both already exercise and
which compiles identically cross-target; the first round's own 14/14 Wine
result stands unchanged).
