# Independent review: issue #35 (helmet-grid orientation)

Reviewer had no part in authoring the change. Repo `main`, commits under review:
`86c68cc` (fix + test + reference regen) and `0bf1f79` (report/captures), on top of
`ab4f1a7`. Real path (`cd -P`) used throughout; all commands run in the foreground.
Environment: `linux-native` preset, real GPU present (NVIDIA GeForce RTX 2080,
driver 580.82.07, confirmed via `nvidia-smi`/`vulkaninfo`) alongside the system
lavapipe ICD.

## Verdicts

- **Spec compliance vs issue #35: PASS.** The fix composes DamagedHelmet's node
  rotation into every grid instance in the mathematically correct order, leaves
  `--scene`/sample 08 untouched, and shadows/`--stress` require no separate fix
  (both verified directly, not inferred).
- **Code quality: Approved**, with one low-severity/informational nit (below).
  No blocking findings.

## Findings

- **[Nit / informational]** `test_grid_layout.cpp`'s "rotation matches
  DamagedHelmet's known quaternion" assertion is **not**, by itself,
  composition-order-discriminating: for any `T * R` vs `R * T` where `T` is a
  pure translation and `R` is a pure rotation, the rotational 3x3 block of the
  product is identical regardless of order (only the translation column
  differs). I verified this by temporarily swapping the multiplication order
  in `gridInstanceTransform()` (`assetNodeTransform * gridTransform(...)`
  instead of the committed `gridTransform(...) * assetNodeTransform`) and
  rebuilding: the rotation-matches-quaternion `CHECK` still **passed** for
  every cell; only the translation `CHECK` failed (12/157 assertions, rows
  1-3 x 4 cols — row 0's translation is `(x,0,0)`, which this particular
  90°-about-X rotation leaves fixed, so row 0 alone can't discriminate order
  either). The D17 pixel gate also caught the swap independently
  (`failingPixels=2578/65536 (3.9337%)`, vs. `2499/65536 (3.8132%)` for the
  dropped-rotation case — a different but comparably-sized defect signature).
  **Net effect: the guard as a whole still catches an order regression on both
  layers**, so this is not a coverage gap, but the test's own top comment
  ("the composed instance's rotational 3x3 equals that quaternion's rotation
  matrix... not merely 'some' rotation") reads as if that specific assertion
  is what defends against a reordering bug, when the translation assertion two
  lines below is what's actually load-bearing for that case. Documentation-only
  nit; recommend a follow-up comment (not a re-open) clarifying which
  assertion defends against which regression shape. Does not block.

## Verification performed (all commands, my own environment)

### 1. Composition order — reasoned, not eyeballed

`gridInstanceTransform() = gridTransform(...) * assetNodeTransform`, i.e. `T *
R`. Applied to a mesh-local vertex `v`: `T*(R*v)` — the asset's own node
rotation acts on the mesh FIRST (in local space), then the result is
translated into its grid cell. This is exactly glTF/scene-graph composition
order (`parentWorld * childLocal`), with the grid cell acting as a synthetic
parent over the asset's own node transform — the same order
`populateImportedInstances()`/sample 08 get "for free" since their
`instance.worldTransform` already **is** that composed node transform.

Confirmed against the source data directly: `assets/fetched/DamagedHelmet/glTF/DamagedHelmet.gltf`
has exactly one node, one scene, and the node carries **only** a `rotation`
key (no `translation`/`scale`/`matrix`) — so `assetNodeTransform` is a pure
rotation matrix, `R`, with zero translation component. I also read
`src/rx_asset/import_gltf.cpp`'s `nodeLocalTransform()`/scene-flattening
code (`world = parentWorld * localTransforms[nodeIndex]`, `T*R*S` per node)
to confirm `result.scene.instances[0].worldTransform` for this single
root-node asset reduces to exactly this `R`, with no coordinate-system flip
or extra composition — the report's math checks out against the actual glTF
and importer.

**Why a wrong order is genuinely hard to eyeball here**: because `R` has no
translation part, `T*R` and `R*T` produce the **identical rotational block**
(only the translation differs — proven above). So a camera zoomed on any
*single* helmet cannot distinguish correct from swapped-order composition at
all — both show the mesh rotated identically. The difference only shows up in
*where the row is placed* (receding in -Z vs. shifted in +Y for this
90°-about-X node rotation), and only for rows 1-3 (row 0's translation has
z=0, which this rotation leaves invariant). Verified this substitution
directly (see Findings above) rather than assuming it from the diff.

### 2. `--scene`/sample 08 untouched — confirmed by code read

Diff touches only `samples/09_scene/*`; `samples/08_gltf_viewer/main.cpp` is
untouched in both commits. Within `09_scene/main.cpp`, `--scene` dispatches
to `populateImportedInstances()` (present-mode only; `runHeadless()` has no
`--scene` branch at all) — a function whose body is not part of this diff, and
which already read `instance.worldTransform` directly before and after this
change.

### 3. Two-layer regression guard — both reverts re-proven myself, byte-identical restores

**(a) Composition-formula revert** — edited `grid_layout.h`'s
`gridInstanceTransform()` to ignore `assetNodeTransform` and return plain
`gridTransform(...)`, rebuilt `sample_09_scene_tests`, ran it (full binary,
no doctest filter — all 35 cases/157 assertions in this binary, not just the
2 grid-layout cases): **32/157 assertions failed**, all in the grid-layout
test case, at exactly the rotation-matches / not-identity checks. This is the
same 32 failing assertions the report's filtered run (`51` total, `32`
failed) reports — cross-checked by a different invocation style, same
failure set. Restored `grid_layout.h`, rebuilt, reran: **157/157 pass**.
`git diff` on the file after restore: **0 lines** (byte-identical).

**(b) Call-site identity revert** — edited both `populateHelmetGrid()` call
sites in `main.cpp` (`runHeadless()` and `runPresent()`) to pass
`glm::mat4(1.0F)` instead of `helmetAssetNodeTransform(result)`, rebuilt
`sample_09_scene`, ran the headless D17 gate on lavapipe against the
already-corrected committed reference:

```
sample_09_scene: D17 grid_scene gate: failingPixels=2499/65536 (3.8132%) pass=false
sample_09_scene: D17 grid_scene gate FAILED on lavapipe (first mismatch at (80,127))
sample_09_scene: headless gate FAILED
```

Exact match to the report's claimed numbers (failing pixel count, percentage,
and first-mismatch coordinate). Restored `main.cpp`, rebuilt, reran: gate
passes again (`failingPixels=0/65536 (0.0000%) pass=true`). `git diff` on the
file after restore: **0 lines** (byte-identical).

**(c) Bonus: swapped-order regression** (my own addition, addressing the
composition-order instruction directly) — see Findings above; both the
device-free test and the D17 gate independently catch it too.

### 4. Shadows share the transform span — confirmed by code read

`updateSceneFrame()` (`main.cpp:1985`) computes `const auto transforms =
app.scene->transformsSpan();` **once**, then both the forward pass
(`row.model = glm::transpose(model)` where `model =
transforms[payload.instanceDataIndex]`, line 1988) and the shadow pass
(`row.model = glm::transpose(transforms[payload.instanceDataIndex]);`, line
2029) read from that same span. Neither path builds its own transform —
`populateHelmetGrid()`'s `desc.transform =
rx::samples9::gridInstanceTransform(...)` is the single source `Scene` stores
per renderable. No separate shadow fix needed; confirmed by reading the code,
not merely inferred.

### 5. `--stress` unaffected — confirmed by code read

`buildStressField()`/`generateStressCube()` builds a procedural cube from
hard-coded vertex data, Registry-free, no glTF import, no node/asset concept
at all — `grep` confirms zero overlap between the `stress` code path and
`populateHelmetGrid`/`gridInstanceTransform`/`helmetAssetNodeTransform`.
`sample_09_scene_stress_headless` passed in my own full ctest run (below).

### 6. Reference regen provenance + 09 headless gate on lavapipe — run myself

`tools/regen_references.sh` forces `VK_ICD_FILENAMES` to
`/usr/share/vulkan/icd.d/lvp_icd.json` (confirmed present on this machine) and
is explicitly "NEVER AUTO-RUN" (no CI/build/test invokes it) — a deliberate,
human-invoked, single mechanism. `git show --stat` on `86c68cc` confirms
`references/grid_scene.png` is touched by exactly one commit, alongside the
code fix and test (no separate/second regen commit). I ran the D17 gate
myself on lavapipe against the committed reference at HEAD:

```
sample_09_scene: D17 grid_scene gate: failingPixels=0/65536 (0.0000%) pass=true
sample_09_scene: C1 discrimination re-proof (shadows-on vs. forced-off): differingPixels=240/65536 (0.3662%)
sample_09_scene: headless gate PASSED
```

Matches the report's "after" numbers exactly.

### 7. Full serial lavapipe ctest — run myself

```
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a ctest --test-dir build/linux-native -j1 --output-on-failure
...
100% tests passed, 0 tests failed out of 29
Total Test time (real) =  78.37 sec
```

29/29, matching the report's claim (includes `sample_09_scene_headless`,
`sample_09_scene_stress_headless`, `sample_09_scene_tests`).

### 8. Real-NVIDIA verification — run myself (labeled, per standing rule)

This machine has a real discrete GPU: `nvidia-smi` reports GeForce RTX 2080,
driver 580.82.07; `vulkaninfo --summary` with both ICDs registered enumerates
it as `GPU0`, `deviceType = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU`, `driverID =
DRIVER_ID_NVIDIA_PROPRIETARY` — llvmpipe is `GPU1`. With `VK_ICD_FILENAMES`
unset (default loader), the headless D17 gate log carries the same
`[non-lavapipe driver -- informational only, not enforced]` annotation the
report cites, confirming the default loader picked the real driver:

```
sample_09_scene: D17 grid_scene gate: failingPixels=553/65536 (0.8438%) pass=false [non-lavapipe driver -- informational only, not enforced]
sample_09_scene: C1 discrimination re-proof (shadows-on vs. forced-off): differingPixels=240/65536 (0.3662%) [non-lavapipe driver -- informational only, not enforced]
sample_09_scene: headless gate PASSED
```

**[real-NVIDIA]** Present-mode grid run under this real driver (`--present
--validate`, `xvfb-run`, ~15s live render, terminated by timeout rather than a
window-close event — see caveat below):

```
grep -i "validation error" log | grep -vi "known false positive"   -> 0 matches
grep -ic "validation error" log                                     -> 2556
grep -ic "known false positive" log                                 -> 2557
grep -iE "segfault|fatal|abort|crash" log                           -> 0 matches
```

Every validation-error line in this real-NVIDIA run carries this codebase's
own pre-existing false-positive annotation; none introduced by this change.
I did not reproduce a natural "window closed cleanly, exit 0" event myself
(I terminated the run via `timeout` rather than sending a close signal to
the SDL window) — this is a gap in *my own* reproduction method, not a
finding against the change; the report's own log line for this is not
independently re-verified by me.

Also confirmed the defensive `helmetAssetNodeTransform()` identity-fallback
warning (`"...zero scene instances -- grid falling back..."`) never fired in
either my lavapipe or NVIDIA runs — the real `result.scene.instances[0]`
path is what's actually exercised, not the fallback.

**[lavapipe]** vs. **[real-NVIDIA]**, labeled per standing rule, both shown
above.

### 9. Orientation comparison — captures inspected directly

- `issue35-grid-scene-before-lavapipe.png`: dark, near-uniform teardrop
  silhouettes — no visible visor detail, consistent with "lying on its back,
  camera sees the chin/underside."
- `issue35-grid-scene-after-lavapipe.png` and `issue35-nvidia-present-grid-512.png`:
  every helmet shows the same distinctive teal-green domed-visor patch,
  facing the camera in a recognizable upright orientation.
- `samples/08_gltf_viewer/references/loaded_scene.png` (same DamagedHelmet
  asset, known-good, untouched by this diff): same teal-green domed visor,
  same upright orientation.

Visual comparison confirms the after/NVIDIA captures match sample 08's
known-good orientation for the same asset.

### 10. Commit hygiene

- Exactly 2 commits ahead of `origin/main` (`git status`:
  "ahead of 'origin/main' by 2 commits"); nothing pushed.
- `86c68cc`: `samples/09_scene/{grid_layout.h,main.cpp,references/grid_scene.png,tests/CMakeLists.txt,tests/test_grid_layout.cpp}`
  only — tightly scoped to the fix + its test + its reference.
- `0bf1f79`: `.superpowers/sdd/2026-08-11-phase4-scene-assets/{helmet-orientation-report.md,issue35-*.png}`
  only — tightly scoped to documentation/evidence, no code.
- Both commits authored as `Yousef Wadi <ywadi85@gmail.com>`, matching local
  `git config user.name`/`user.email` exactly.
- `grep -in "claude|anthropic|co-authored|chatgpt|openai"` over both commits'
  full diffs and messages: **no matches**. No AI attribution anywhere.
- The only uncommitted working-tree change is the pre-existing
  `.superpowers/sdd/2026-08-11-phase4-scene-assets/progress.md` modification,
  left untouched per instructions.

## CI Wine-regex adjudication

`.github/workflows/ci.yml`'s "Test under wine" step excludes `-E
'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample'` — a
substring match that sweeps **every** ctest name containing `sample`,
including the pre-existing, already-device-free `sample_09_scene_tests`
binary (which also contains `test_draw_recording.cpp`, `test_fly_camera.cpp`,
`test_mouse_capture*.cpp` — all predating this change). I confirmed `git diff
86c68cc~1..0bf1f79 -- .github/workflows/ci.yml` is empty: **this PR does not
touch CI at all**, and the sweeping regex is a pre-existing convention that
already excluded this same binary's other device-free tests before issue #35
added `test_grid_layout.cpp` to it. The new test is simply more content added
to an already-excluded binary, not a newly-created gap.

**Ruling: out of scope, does not block this review.** The implementer's own
characterization ("CI's own exclusion regex excludes every `sample*`-named
test — not specific to this change") is accurate. Tightening the CI regex to
distinguish device-free `*_tests` binaries from GPU-backed `*_headless` ones
is a reasonable future cleanup but belongs to a separate CI-hygiene task, not
this bugfix. The implementer's own direct Wine run of
`sample_09_scene_tests.exe` outside ctest (35/35, 157/157) is a legitimate
bonus check given the gap, not a substitute for fixing CI — noted as such,
correctly, in their own report.

## Not independently verified in this review

- **windows-cross-zig build / Wine ctest run**: not reproduced by me (not
  required by the task's empirical-verification list; trusting the report's
  clean-build and 13/13 Wine ctest claims).
- **Present-mode "window closed cleanly, exit 0"**: I ran present-mode on the
  real NVIDIA GPU myself and confirmed zero unfiltered validation errors over
  a live ~15s render, but terminated it via `timeout` rather than a window-close
  event, so I did not personally reproduce that specific log line/exit code.
- **512px capture generation mechanism**: not asked to reproduce the
  screenshot pipeline itself; verified the resulting image's content
  (orientation) directly instead.
