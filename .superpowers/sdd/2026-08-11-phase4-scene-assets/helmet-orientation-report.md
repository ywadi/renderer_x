# Issue #35: sample 09 grid mode drops DamagedHelmet's node rotation

Repo `main` at `ab4f1a7` at task start. Real path (`cd -P`) used throughout.

## Root cause

DamagedHelmet.gltf's single root node (`assets/fetched/DamagedHelmet/glTF/DamagedHelmet.gltf`,
`nodes[0]`) carries a rotation, verbatim:

```json
{"mesh": 0, "name": "node_damagedHelmet_-6514",
 "rotation": [0.7071068286895752, 0.0, -0.0, 0.7071068286895752]}
```

glTF quaternion order is `[x, y, z, w]`. `angle = 2*acos(w) = 2*acos(0.70710683) = 90°`;
axis `= (x,y,z)/sin(45°) = (1,0,0)` — a **+90° rotation about world X**. This is what stands
the mesh upright; without it the helmet renders lying on its back, visor toward the sky —
exactly the reported "looking up" symptom.

`src/rx_asset/import_gltf.cpp` composes this into `InstanceRecord::worldTransform`
(`src/rx_asset/include/rx_asset/mesh_asset.h:206`) for every node the importer flattens.
Two consumers apply it correctly:

- `populateImportedInstances()` (`samples/09_scene/main.cpp:1546`, the `--scene sponza`/
  `--scene workshop` present-mode path): `desc.transform = instance.worldTransform;` — uses it
  directly.
- Sample 08 (`samples/08_gltf_viewer/main.cpp:1306-1318`): also reads
  `instance.worldTransform` directly for every renderable.

The grid path did not go through either of those. `runHeadless()`/`runPresent()`'s default
(no `--scene`, no `--stress`) branch calls `app->registry->importGltf(...)` directly, keeps
only `result.meshes[0]` (the geometry handle), and **discards `result.scene` entirely** — the
field that carries `InstanceRecord::worldTransform`. `populateHelmetGrid()`
(`samples/09_scene/main.cpp`, pre-fix) then hand-built each instance's transform as:

```cpp
glm::mat4 gridTransform(uint32_t row, uint32_t col, float spacing) {
    const float x = (static_cast<float>(col) - 0.5F * static_cast<float>(kGridCols - 1)) * spacing;
    const float z = -(static_cast<float>(row) * spacing);
    return glm::translate(glm::mat4(1.0F), glm::vec3(x, 0.0F, z));
}
...
desc.transform = gridTransform(row, col, spacing);
```

Pure translation — no rotation composed in at all. This confirms the hypothesis exactly:
the grid mode drops the asset's own node transform; the import path and sample 08 do not.

`setupShadow()` was investigated too (requirement 4): it does **not** build any instance
transforms itself — it only computes the light frustum fit and allocates the shadow draw-data
buffers. Both the forward pass (`updateSceneFrame()`, `row.model = transforms[...]`) and the
shadow pass (`row.model = glm::transpose(transforms[payload.instanceDataIndex]);`,
`main.cpp:2006`) read from the **same** `app.scene->transformsSpan()`, populated once by
`RenderableDesc::transform` in `populateHelmetGrid()`. So the shadow of a lying-down helmet was
indeed wrong, and fixing `populateHelmetGrid()`'s composition fixes both forward rendering and
shadows with one change — confirmed empirically below (C1 discrimination re-proof stayed
qualitatively unchanged across the fix).

`--stress` mode was also checked: it builds a procedural cube (`generateStressCube()`,
Registry-free, no glTF import, no node concept at all) and explicitly sets
`app.shadowEnabled = false`. There is no asset node transform to drop here — confirmed by a
clean headless `--stress` run (all counters correct, gate PASSED) with no code change needed.

## Fix

1. New device-free header `samples/09_scene/grid_layout.h` (same "no VkDevice/Window seam"
   precedent as `fly_camera.h`/`mouse_capture.h`/`draw_recording.h`):
   - `gridTransform(row, col, spacing, gridCols)` — the original pure-translation placement,
     parameterized instead of closing over `kGridCols`.
   - `gridInstanceTransform(row, col, spacing, gridCols, assetNodeTransform)` =
     `gridTransform(...) * assetNodeTransform` — composes grid placement with the asset's own
     node rotation.
2. `samples/09_scene/main.cpp`:
   - New `helmetAssetNodeTransform(const ImportResult&)`: returns
     `result.scene.instances[0].worldTransform` (defensive identity + `RX_LOG_WARN` fallback if
     the import ever produces zero instances — never observed against the real asset, which has
     exactly one root node).
   - `populateHelmetGrid()` signature extended to take `const glm::mat4& assetNodeTransform`;
     the grid loop now calls `rx::samples9::gridInstanceTransform(row, col, spacing, kGridCols,
     assetNodeTransform)`.
   - Both call sites (`runHeadless()` line ~2649, `runPresent()` line ~3167) updated to pass
     `helmetAssetNodeTransform(result)`.
3. D17 reference regenerated: `bash tools/regen_references.sh linux-native 09_scene` (forces
   `VK_ICD_FILENAMES` to the system lavapipe ICD per that script's own provenance guarantee —
   never any other driver). `samples/09_scene/references/grid_scene.png` updated.

## Orientation-discriminating regression guard (requirement 3)

The task's own framing proved out directly: the exact-counter/pixel gate alone is blind to this
class of bug, because the committed reference was baked *from* the buggy render — a
self-consistent wrong answer passes a pixel-diff gate with `failingPixels=0`.

Added `samples/09_scene/tests/test_grid_layout.cpp` (2 `TEST_CASE`s, 51 assertions), registered
in `samples/09_scene/tests/CMakeLists.txt`. It hard-codes DamagedHelmet.gltf's own node
quaternion (`glm::quat(0.7071068286895752F, 0.7071068286895752F, 0.0F, -0.0F)`) — a constant
taken directly from the asset file, **independent of `gridInstanceTransform()`'s own
implementation** — and asserts:
- the composed instance's rotational 3x3 equals that quaternion's rotation matrix (not merely
  "some" rotation);
- it is explicitly **not** identity (the exact signature every form of "dropped rotation"
  produces);
- the translation part still lands exactly on the plain grid placement.

This is device-free (no GPU, no lavapipe, no reference PNG at all), so it fails deterministically
regardless of what any regenerated reference says.

### Revert-prove, composition-formula level

Temporarily reverted `gridInstanceTransform()` in `grid_layout.h` to ignore
`assetNodeTransform` and return plain `gridTransform(...)` (the pre-fix behavior). Rebuilt and
ran the new tests:

```
[doctest] test cases:  2 |  1 passed |  1 failed | 33 skipped
[doctest] assertions: 51 | 19 passed | 32 failed |
[doctest] Status: FAILURE!
```

32/51 assertions failed immediately, at exactly the discriminating checks (rotation-matches-known-
quaternion, rotation-is-not-identity), across all 16 grid cells. Restored the fix; rebuilt;
`sample_09_scene_tests`: **35/35 test cases, 157/157 assertions, SUCCESS**.

### Revert-prove, call-site level

Temporarily reverted both `populateHelmetGrid()` call sites in `main.cpp` to pass
`glm::mat4(1.0F)` instead of `helmetAssetNodeTransform(result)` (simulating a regression that
bypasses the composition entirely at the wiring level, which the header-level unit test above
cannot see since it never runs the real importer). Rebuilt `sample_09_scene` and ran the headless
gate against the **already-corrected** reference:

```
sample_09_scene: D17 grid_scene gate: failingPixels=2499/65536 (3.8132%) pass=false
sample_09_scene: D17 grid_scene gate FAILED on lavapipe (first mismatch at (80,127))
sample_09_scene: headless gate FAILED
```

2499/65536 (3.8132%) — the *exact same number* the pre-fix binary produced against the
corrected reference before any code changes (see "before/after" below), confirming the D17
pixel gate now genuinely discriminates a call-site regression once its own reference is correct.
Restored the fix; rebuilt; gate passed again (`failingPixels=0/65536, pass=true`).

Together, the transform-level unit test (composition-formula regressions) and the corrected D17
pixel gate (call-site/wiring regressions) cover both ways this bug could be reintroduced.

## Before / after

Old (buggy, pre-fix) committed reference vs. the render produced by the fixed code:

```
sample_09_scene: D17 grid_scene gate: failingPixels=2499/65536 (3.8132%) pass=false
sample_09_scene: D17 grid_scene gate FAILED on lavapipe (first mismatch at (80,127))
```

New reference regenerated from the fixed code; fixed code re-run against it:

```
sample_09_scene: D17 grid_scene gate: failingPixels=0/65536 (0.0000%) pass=true
sample_09_scene: C1 discrimination re-proof (shadows-on vs. forced-off): differingPixels=240/65536 (0.3662%)
sample_09_scene: headless gate PASSED
```

The shadow discrimination re-proof (C1, pre-existing) stayed meaningfully non-zero throughout
(240 with the fix in place, 338 with the call-site regression reinstated, back to 240 restored)
— shadows visibly track the corrected geometry, consistent with the single-source-of-transforms
finding above.

Captures saved to this SDD directory:
- `issue35-grid-scene-before-lavapipe.png` — old (buggy) D17 reference, lavapipe, 256x256.
- `issue35-grid-scene-after-lavapipe.png` — new (fixed) D17 reference, lavapipe, 256x256.
- `issue35-nvidia-present-grid-512.png` — real-NVIDIA present-mode grid run, HUD visible,
  helmets upright (visor domes facing the camera), scaled to 512px max dimension.

Visual comparison: the old reference's helmets are dark, near-uniform teardrop silhouettes from
this elevated downward-angled camera (consistent with lying on their backs, camera mostly seeing
the chin/underside); the new reference and the real-NVIDIA capture both show the same
distinctive teal-green domed visor patch per helmet, matching sample 08's known-good coloring.

## Verification

**Full serial lavapipe ctest** (`linux-native`, `VK_ICD_FILENAMES` forced to the system lavapipe
ICD, `xvfb-run`):

```
100% tests passed, 0 tests failed out of 29
Total Test time (real) =  74.52 sec
```

Includes `sample_09_scene_headless`, `sample_09_scene_stress_headless`, and
`sample_09_scene_tests` (the new `test_grid_layout.cpp` cases).

**Real-NVIDIA verification** (default/unforced ICD — `nvidia-smi`: GeForce RTX 2080, driver
580.82.07; `vulkaninfo --summary` confirms it as `GPU0`, `driverID = DRIVER_ID_NVIDIA_PROPRIETARY`;
independently confirmed "driver-labeled" via the headless run's own D17 log annotation, which
explicitly reports `[non-lavapipe driver -- informational only, not enforced]` when the default
loader selects it):

- Present-mode grid run, `--present --validate`: window opened, rendered, closed cleanly
  (`sample_09_scene: window closed cleanly`, exit 0).
- `grep -i "validation error" | grep -v "known false positive"` on the full run log: **zero
  matches** — every reported `Validation Error`/`SYNC-HAZARD` line carries this codebase's own
  pre-existing, documented false-positive annotation (`context.cpp`'s four known-guard comments);
  none introduced by this change.
- 512px frame capture saved (`issue35-nvidia-present-grid-512.png`, see above) — helmets render
  upright.
- Separately, a default-ICD **headless** run confirmed the same default-ICD selection picks a
  non-lavapipe (real) driver, and passed: `sample_09_scene: headless gate PASSED`.

**windows-cross-zig build**: `cmake --build --preset windows-cross-zig` — clean, zero warnings,
for `sample_09_scene`, `sample_09_scene_tests`, and the full project (`ninja: no work to do` on
the final full-preset build, i.e. everything already built clean with no diagnostics at any
incremental step).

**Wine convention**: matched CI's own invocation exactly
(`.github/workflows/ci.yml` "Test under wine" step):

```
xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample' --output-on-failure
...
100% tests passed, 0 tests failed out of 13
Total Test time (real) = 133.28 sec
```

CI's own exclusion regex excludes every `sample*`-named test (not specific to this change), so
as a bonus check `sample_09_scene_tests.exe` was also run directly under Wine outside ctest
(it is genuinely device-free, so this is a legitimate extra check even though CI's regex happens
to skip it):

```
[doctest] test cases:  35 |  35 passed | 0 failed | 0 skipped
[doctest] assertions: 157 | 157 passed | 0 failed |
[doctest] Status: SUCCESS!
```

**Zero warnings**: every build step (linux-native and windows-cross-zig, full and incremental,
including every revert-prove rebuild) produced only `Building`/`Linking` lines — no compiler
diagnostics at any point.

## Files changed

- `samples/09_scene/grid_layout.h` (new) — `gridTransform()`/`gridInstanceTransform()`.
- `samples/09_scene/main.cpp` — `#include "grid_layout.h"`; new `helmetAssetNodeTransform()`;
  `populateHelmetGrid()` takes `assetNodeTransform`; both call sites updated.
- `samples/09_scene/tests/test_grid_layout.cpp` (new) — the orientation-discriminating unit test.
- `samples/09_scene/tests/CMakeLists.txt` — registers `test_grid_layout.cpp`.
- `samples/09_scene/references/grid_scene.png` — regenerated D17 reference (lavapipe,
  `tools/regen_references.sh linux-native 09_scene`).
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/issue35-*.png` (new, force-added past this
  directory's own `*` `.gitignore`, matching the existing `helmet-before.png`/`helmet-after.png`
  precedent) — before/after evidence captures.

`setupShadow()` and the `--stress` path required **no code change** — both were verified
directly (not merely inferred) per requirement 4, as detailed above.
