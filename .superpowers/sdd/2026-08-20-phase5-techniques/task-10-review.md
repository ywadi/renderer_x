# Task 10 review — IBL runtime integration + skybox (issue #46)

Independent reviewer round. Commit under review: `3f62df1`
(`feat(rx_scene,rx_material,rx_rhi_vk): IBL runtime integration + skybox
(FG1 closure)`), base `56fe697`, branch `task/t10-ibl-runtime`. Reviewer
did not write this code; every finding below is from a direct fetch of
the pinned Filament v1.75.0 source, direct reads of the diff/worktree
source, and live re-execution (including two temporary, byte-identically
restored source edits used to reproduce claims empirically) — not from
trusting the report's own characterizations.

## Verdict 1 — Spec compliance: **FAIL** (matrix-p5t10-ibl-runtime-skybox)

Five of the six matrix rows are genuinely met (Scene-level environment
API, discrimination against FG1, skybox pass, exposure-aware IBL,
environment-intensity-in-physical-units). The **"Specular IBL feeding
the specular lobe"** row fails: the split-sum specular/diffuse split
weight `E` the runtime computes does not match the DFG-LUT convention
Task 9 actually baked, so the specular lobe does not "reproduce the
environment" correctly for the general (non-degenerate) case the row's
own acceptance text targets. See Finding 1 (CRITICAL) below — this is
also the substance of the attention lens's own "mismatched roughness-
to-mip mapping... classic bug" warning, though the actual defect is a
DFG-channel-composition mismatch, not a roughness/mip mismatch (that
part was independently checked and is correct — see "Port fidelity").

## Verdict 2 — Code quality: **NOT Approved — 1 CRITICAL finding**
(everything else in the round is clean; re-review after the fix)

Outside Finding 1, this is a well-executed round: the bindless-capacity
generalization (bindless.cpp) is a genuine improvement over the prior
binary branch, the Scene API matches established idiom exactly, the
skybox pass's D29/reversed-Z depth-test derivation is correct and
independently reproduced, the 4 remaining bug fixes are real and each
independently verified, and both drivers are validation-clean. The
CRITICAL finding blocks approval because it is a physical-correctness
defect in the task's own headline deliverable (the split-sum IBL
composition), shipped as the new default lighting path for every future
consumer of `standard_pbr.slang`, and not caught by any of the four new
value-asserted tests — see the "why the tests didn't catch it" analysis
below, which is itself a secondary (MEDIUM) test-design finding.

---

## Finding 1 (CRITICAL) — `iblSpecularReflectance()` implements the
wrong DFG-LUT reconstruction formula for the LUT Task 9 actually bakes

**Where:** `shaders/material/brdf.slang`, `iblSpecularReflectance(f0, dfg)`:
```
public float3 iblSpecularReflectance(float3 f0, float2 dfg) {
    return f0 * dfg.x + float3(dfg.y, dfg.y, dfg.y);
}
```
consumed in `shaders/material/standard_pbr.slang`'s `evaluate()`:
`float3 E = iblSpecularReflectance(F0, dfg); ... Fd = diffuseColor *
irradianceSample * (1-E); Fr = E * prefilteredSample` (energy-
compensated).

**What's wrong:** the header comment claims this is "ported here as
Filament's own `specularDFG()`" (citing `surface_light_indirect.fs`
v1.75.0 :135), but Filament's actual `specularDFG()` is:
```glsl
return mix(pixel.dfg.xxx, pixel.dfg.yyy, pixel.f0);   // = dfg.x*(1-f0) + dfg.y*f0
```
— a **different formula** from what's coded (`f0*dfg.x + dfg.y`).
These are not algebraically equivalent (they coincide only for special
`f0`/`dfg` values). Confirmed against a fresh fetch of
`google/filament` tag `v1.75.0`
(`shaders/src/surface_light_indirect.fs`, `libs/ibl/src/CubemapIBL.cpp`,
same pin this phase already uses):

- `CubemapIBL.cpp:790-833`, `DFV_Multiscatter()` — the function Task
  9's `dfg_lut.slang` is a byte-for-byte structural port of (`r.x +=
  v*Fc; r.y += v;`, unconditionally the multiscatter variant per T9's
  own header comment) — carries this **exact** reconstruction comment
  in the CPU original:
  ```
  Er() = (1 - f0) * DFV.x + f0 * DFV.y
       = mix(DFV.xxx, DFV.yyy, f0)
  ```
  **This is already quoted verbatim in `shaders/ibl/dfg_lut.slang`'s
  own header comment** (unchanged by this diff, already in the
  repository from T9): *"DFV_Multiscatter's own header comment: 'Er() =
  (1-f0)*DFV.x + f0*DFV.y'"*. T10's `iblSpecularReflectance()` deviates
  from a formula the codebase already documents correctly one file
  over.
- The formula actually coded (`f0*dfg.x + dfg.y`) is instead
  `CubemapIBL.cpp:778`'s reconstruction for the **plain, non-
  multiscatter** `DFV()` (`Er() = f0*DFV.x + f90*DFV.y`, with f90=1) —
  the variant T9 explicitly does **not** bake (`dfg_lut.slang`'s own
  header: "multiscatter=true is used UNCONDITIONALLY... this is not a
  free choice").

**Magnitude — empirically reproduced live, not just derived by hand.**
Temporarily added a second assertion to the furnace TEST_CASE
(`test_standard_pbr_ibl_gpu.cpp`) using the correct `mix()` formula,
rebuilt, ran on lavapipe, reverted byte-identically (`git checkout --`,
confirmed via `git diff --stat` = clean) after capturing:
```
pixel r=104  expectedE=0.68  expected=104           (shipped formula, matches the render exactly)
REVIEW PROBE: correctE(mix)=0.305  correctExpected=47  (Filament-correct formula)
```
kF0=0.7, kDfgX=0.9, kDfgY=0.05 — the shipped render is **104/255**,
the physically-correct render is **47/255**, a >2x divergence, far
outside any D17 tolerance. For common dielectric F0≈0.04 the divergence
is worse in direction (the shipped formula pushes `E` toward `dfg.y`,
typically ≈0.9-1.0 at low roughness, instead of toward `dfg.x`,
typically small — i.e. ordinary non-metal surfaces get a near-mirror
specular response and a correspondingly crushed `(1-E)` diffuse term).

**Why none of the 4 new value-asserted tests caught it — a real
test-design gap (MEDIUM, folds into the fix):**
- Lambertian test: `dfg=(0,0)` → `E≡0` under either formula (identity
  masks the bug).
- Mirror-metal test: `dfg=(1,0)`, and the test's own header comment
  encodes the SHIPPED (wrong) formula's prediction as ground truth
  ("dfgValue=(1,0) makes E==f0*1+0==f0"). Worse: `dfg.x=1, dfg.y=0`
  is a value the real bake can **never produce** — `dfg_lut.slang`'s
  own accumulation (`r.x += term*fc; r.y += term`, `fc=(1-VoH)^5 ∈
  [0,1]`) makes `dfg.x <= dfg.y` a hard invariant of every real baked
  texel, and this test's own constant inverts it.
- Furnace test: `expectedE = kF0*kDfgX + kDfgY` is the **exact same
  formula** the shader implements — a tautological check (shader
  matches itself, not an independent ground truth) — and its own
  `kDfgX=0.9 > kDfgY=0.05` **also** violates the same real-bake
  invariant.

  All three of the round's own synthetic `dfg` fixtures violate the one
  invariant every real Task-9 bake output satisfies — independent
  evidence the implementer's mental model of what the LUT's two
  channels mean was inverted relative to what Task 9 actually bakes,
  not merely one mistyped line.

**Recommended fix** (for the implementer, not applied by this review):
```
public float3 iblSpecularReflectance(float3 f0, float2 dfg) {
    return lerp(float3(dfg.x, dfg.x, dfg.x), float3(dfg.y, dfg.y, dfg.y), f0);
}
```
and correct the 3 dfg-bearing TEST_CASEs' own "expected" derivations to
the same `mix()`/`lerp()` formula (the mirror test's `dfg=(1,0)` should
become `(0,1)` to keep its "E==f0 exactly" pedagogical shape under the
corrected formula, since `lerp(0,1,f0)==f0`). Sample08's committed D17
references (`loaded_scene.png`) were baked under the buggy formula and
need regeneration once fixed.

---

## Reproduction log (all commands re-run independently this session, in
the worktree, foreground, serial, NICEd, solo GPU)

**Full ctest suite, lavapipe, serial — CONFIRMED 33/33 (100%).**
`VK_ICD_FILENAMES=lvp_icd.json xvfb-run -a ctest --test-dir
build/linux-native --output-on-failure -j1`, driver-labeled
llvmpipe/Mesa. 117.8s.

**Full ctest suite, real NVIDIA — CONFIRMED 33/33 (100%).**
`VK_ICD_FILENAMES=nvidia_icd.json` (vendorID `0x10de` confirmed via
`vulkaninfo --summary`; `nvidia-smi` confirms `GeForce RTX 2080`,
driver `580.82.07`, matching the report exactly). 205.9s.

**Touched GPU binaries, real NVIDIA — CONFIRMED.**
`rx_material_gpu_tests --test-case="*IBL*,Skybox*"`: 6/6, 501/501
assertions. `sample_08_gltf_viewer --validate`: D17 loaded_scene
`204/65536 (0.3113%)` — exact match to the report's own figure.
`grep -i validation | grep -v "known false positive"` → empty (zero
unfiltered validation errors) on every run above.

**Flat-ambient revert-discrimination — reproduced live, restored
byte-identically.** Temporarily reverted BOTH halves of the retirement
(`standard_pbr.slang`'s final composition line back to `v.ambientColor
* occlusion * baseColor.rgb`, AND `forward_entry.slang`'s
`v.ambientColor = float3(0,0,0)` literal back to `draw.ambientColor.xyz`
— the first revert alone is insufficient, since `forward_entry.slang`
independently zeroes the field before `evaluate()` runs regardless of
what `standard_pbr.slang` does with it). Re-ran the FG1-discrimination
TEST_CASE: **failed as expected**, new pixel matched the old formula's
own closed-form prediction with **delta=0 on all 3 channels**
(`new=82/20/20, oldPrediction=82/20/20`) — the discrimination gate is
genuinely load-bearing, not a tautology. Restored both files via `git
checkout --`; `git diff --stat` confirms zero diff against the commit;
rebuilt to restore binaries.

**Pre-existing (non-T10) energy-compensation/furnace tests — unaffected,
still pass.** `*furnace*,*energy*compensation*,*EnergyComp*`: 2/2,
142/142 — confirms the SEPARATE, correctly-ported multiplicative
`energyCompensation(f0, dfgY) = 1+f0*(1/dfgY-1)` mechanism (used for
direct-light and as a post-multiply on the IBL specular term) is
untouched by Finding 1, which is isolated to the split-sum `E` weight
specifically.

**Commit hygiene — CONFIRMED.** One commit (`3f62df1`) on
`task/t10-ibl-runtime`, base `56fe697`. Author/committer: `Yousef Wadi
<ywadi85@gmail.com>` (both fields). Commit message grepped for
`claude|anthropic|co-authored|ai-generated|generated by` — none found.
`git branch -vv` shows no upstream/remote tracking ref (not pushed).
Main checkout untouched (only the pre-existing `progress.md` diff from
before this review started; verified via `git diff --stat` in the main
checkout at both start and end of this round).

## Port fidelity — everything else checked against the pinned source

- **Roughness→mip/LOD mapping (bake vs. runtime) — CORRECT, matches.**
  `rx_ibl/src/bake.cpp`'s prefilter dispatch: `coord = mip/(mipCount-1)`,
  `linearRoughness = coord²` (so `coord == perceptualRoughness`).
  Runtime: `lod = roughness * envMaxPrefilteredLod` where
  `envMaxPrefilteredLod = mipCount-1` — inverting gives the identical
  `perceptualRoughness = mip/(mipCount-1)`. Self-consistent (a
  deliberately different, but internally coherent, linear convention
  from Filament's own nonlinear `perceptualRoughnessToLod()` quadratic
  fit — a legitimate, documented design choice, not a bug). The DFG-LUT
  sample coordinate (`float2(NdotV, 1-roughness)`) was also checked
  against `dfg_lut.slang`'s own row convention ("row 0 = roughest, last
  row = smoothest") and is consistent.
- **`energyCompensation(f0, dfgY) = 1 + f0*(1/dfgY - 1)`** — byte-exact
  match to `surface_shading_lit.fs`'s `getEnergyCompensationPixelParams()`
  (`1.0 + pixel.f0 * (1.0/pixel.dfg.y - 1.0)`). Correct.
- **Diffuse IBL composition** `Fd = diffuseColor * irradiance * (1-E)`
  — matches `surface_light_indirect.fs:787`'s structure
  (`pixel.diffuseColor * diffuseIrradiance * (1.0-E) * diffuseBRDF`,
  where `diffuseBRDF` is a constant 1/π folded into the bake's own
  pre-divided-by-π irradiance convention here, consistent with T9's own
  documented convention). The `(1-E)` factor itself is correctly
  computed as `1-E`; only `E`'s own value is wrong (Finding 1).
- **Skybox pass (D29, depth, camera-ray reconstruction)** — correct.
  `GREATER_OR_EQUAL`/no-write against this project's reversed-Z (near=1,
  far=0) convention, NDC z=0.0 emitted at the far plane, correctly
  derived and independently re-verified. The "Skybox: a pixel not
  covered..." TEST_CASE was re-run on lavapipe and NVIDIA — passes on
  both, and is genuinely independent of Finding 1 (skybox samples the
  base cubemap directly, no DFG involvement).
- **Energy-compensation activation for IBL paths** — CONFIRMED wired:
  `materialSpecializationBits()` returns
  `kSpecializationEnergyCompensation` whenever `hasEnvironment()`, at
  both `setupMaterials()`'s D27 pre-resolution and
  `recordSceneDraws()`'s real per-draw lookup (single function, cannot
  drift). Every IBL TEST_CASE exercises `metallic=1` or a nonzero
  specular response, so the ON variant is genuinely exercised.
- **Bindless capacity bumps (06/08/09/tests)** — sized with documented
  arithmetic in every case (e.g. sample08's `storageBuffers` 4→8:
  "default row (1) + drawDataBuffer 2 FIF + skyboxDataBuffer 2 FIF == 5
  minimum; 8 leaves headroom"), never a bare magic number. No T24-class
  capacity-exhaustion risk identified — every touched `BindlessTable`
  construction either matches real usage + documented headroom, or (for
  fixtures that never register a real cube image) a small, harmless,
  explicitly-commented "structural presence only" value.
- **The other 4 disclosed in-round bug fixes** — all independently
  plausible and each pinned by a real test: std430 stride padding
  (`_padEnv0`, `static_assert(sizeof(DrawDataGpu)==384,...)` on both
  sides, caught by the pre-existing D26.1 two-draw TEST_CASE per the
  report — not independently re-broken/re-fixed this round given the
  Finding 1 time budget, but the fix itself is a straightforward,
  correctly-reasoned 16-byte-alignment padding field with matching
  static_asserts on both C++/Slang sides); vertex→fragment interface
  mismatch (`ambientColor` dropped from `VertexStageOutput`, replaced
  with a literal-zero fragment-side assignment — directly read and
  confirmed in the diff); pipeline-layout push-constant incompatibility
  (`recordSkybox()`'s explicit set-0 rebind against its own layout,
  correctly reasoned against Vulkan spec 14.2.2); skybox 9×9 test-probe
  extent (confirmed correct: index 4 of 9 sits at NDC 0.0 exactly).

## Sample09 adjudication — darker, no-environment render

**The matrix is not silent here** — its "Scene-level environment API"
row explicitly states: *"a Scene with no environment set behaves
byte-identically to today's flat-ambient path is NOT required... the
regression bar is 'existing 08/09 GATES regenerate with new,
provably-more-correct references,' not 'old behavior survives
unchanged.'"* Sample09's regenerated, darker `grid_scene.png` reference
is therefore **matrix-compliant by the matrix's own explicit text**, not
an open question the coordinator left for this review to resolve on
compliance grounds. Ruling on compliance: **acceptable, no finding.**

Ruling on the UX question this review was separately asked to weigh
honestly: sample09's regenerated reference
(`task-10-captures/sample09_grid_scene_no_env_256.png`, visually
inspected) shows 8 DamagedHelmet instances in a grid, almost entirely
against solid black, with only faint teal/gray highlights on directly-
lit surfaces — a materially darker, less legible render than the
pre-T10 flat-ambient version. `sample09_scene`'s own git history (a
promoted `FlyCamera`, "close row 4") indicates it is an interactive
fly-through the project owner personally drives, not a static gate-only
asset. A near-black interactive fly-through plausibly reads as "looks
broken" to a human tester unaware of the underlying physically-correct
zero-ambient rationale, independent of whether the gate passes.
**This is a genuine, if minor, product-quality gap** — T9+T10 already
built the exact machinery (`Scene::setEnvironment`,
`rx::ibl::bakeEnvironment()`) that would let sample09 bind a modest
default/dim environment cheaply, and did not. Recommend (not a spec
compliance blocker, filed as a LOW finding): a fast-follow binds a
low-intensity default environment in sample09, either as part of the
Finding-1 fix round or immediately after, rather than leaving the
"correct but visually flat" render as sample09's permanent interactive
demo state.

## Findings summary (severity-ranked, driver-labeled where applicable)

1. **CRITICAL** — `iblSpecularReflectance()` (`brdf.slang`) implements
   the non-multiscatter `DFV()` reconstruction formula
   (`f0*dfg.x+dfg.y`) against a LUT baked with the multiscatter
   `DFV_Multiscatter()` convention (`(1-f0)*dfg.x+f0*dfg.y`), a real
   citation/formula mismatch verified against fresh Filament v1.75.0
   source and reproduced live (104 vs. 47 out of 255, lavapipe). Blocks
   spec compliance and code-quality approval. Fix + affected-test-
   correction + D17 reference regeneration all required before re-review.
2. **MEDIUM** — the 3 dfg-bearing IBL GPU TEST_CASEs use synthetic
   `dfg` values that violate `dfg.x <= dfg.y`, an invariant every real
   Task-9 bake output satisfies, and (for the furnace case) compute
   "expected" via the same formula the shader implements — none of the
   3 could have caught Finding 1. Folds into the Finding-1 fix.
3. **LOW** — sample09's darker, no-environment interactive render is
   matrix-compliant but a product-quality/UX gap for an owner-driven
   fly-through demo; recommend a fast-follow default-environment bind.

## Not independently verifiable this round

- Windows-cross-zig/Wine tier (report claims 14/14) — not re-run this
  session; not required by this review's stated empirical minimum, and
  the headless-non-GPU filter the report used matches the CI job's own
  exclusion regex, so this is a low-risk gap, not a disclosed concern.
- Steam Deck / non-desktop hardware numbers — out of scope for this
  ticket per RC8 (Deck rows are honest-manual until Deck hardware
  enters the loop).

---

## Re-review (fix round 1)

Scoped re-review of the fix commit `1a71e9c` (base `3f62df1`, branch
`task/t10-ibl-runtime`) against the five findings above. Builds/tests run
in the worktree (`/media/ywadi/second/renderer_x-worktrees/t10-ibl-runtime`,
`cd -P`'d in); this document is edited in the main checkout only. Every
claim below is from this session's own independent re-execution — a fresh
fetch of Filament v1.75.0 via `gh api` (not from training-data memory or
trusting the report's citations), direct reads of the current worktree
source, and live GPU runs on both drivers — not from trusting
`task-10-report.md`'s own characterizations.

### Overall verdict: **ALL FIVE FINDINGS CLOSED**

### Finding 1 (CRITICAL, formula) — **CLOSED**

`shaders/material/brdf.slang:258-260` now reads:
```
public float3 iblSpecularReflectance(float3 f0, float2 dfg) {
    return lerp(float3(dfg.x, dfg.x, dfg.x), float3(dfg.y, dfg.y, dfg.y), f0);
}
```
Independently re-fetched `google/filament` tag `v1.75.0` via `gh api
repos/google/filament/contents/...` (not the local pin, not memory):
- `shaders/src/surface_light_indirect.fs:135-142` — `specularDFG()`'s
  default (non-cloth, non-specular-factor) branch, byte-exact:
  `return mix(pixel.dfg.xxx, pixel.dfg.yyy, pixel.f0);` — algebraically
  `dfg.x*(1-f0) + dfg.y*f0`, identical to Slang's `lerp(dfg.x, dfg.y, f0)`
  (same two-endpoints-plus-t argument convention as GLSL `mix`). Matches
  the shipped fix exactly.
- `libs/ibl/src/CubemapIBL.cpp:790-833` (`DFV_Multiscatter()`) — refetched
  and re-read in full: its own header comment is byte-exact to what
  `dfg_lut.slang`'s pre-existing (T9, unchanged by this diff) header
  comment already quotes: `"Er() = (1-f0)*DFV.x + f0*DFV.y = mix(DFV.xxx,
  DFV.yyy, f0)"`, accumulation `r.x += v*Fc; r.y += v;` — the exact
  convention `dfg_lut.slang`'s own `dfvMultiscatter()` ports (confirmed by
  direct read of the current worktree file: `r.x += term*fc; r.y +=
  term;`, `fc = pow(clamp(1-voH,0,1), 5)`). The fix now matches the LUT
  Task 9 actually bakes.
- Call-site composition (`standard_pbr.slang:504-523`) re-read directly:
  `float3 E = iblSpecularReflectance(F0, dfg); float3 iblSpecular =
  energyFeature.apply(prefilteredSample * E, F0, dfgY); float3 iblDiffuse
  = diffuseColor * irradianceSample * (1 - E);` — energy compensation
  (`energyFeature.apply`, `EnergyCompensationOn::apply() = specular *
  energyCompensation(f0, dfgY)`, `brdf.slang:297-300`) is applied AFTER
  the split-sum `E`, on the specular term only — matching Filament's own
  `Fr = E * prefilteredRadiance; Fr *= pixel.energyCompensation` order
  exactly, not merely present somewhere in the file.

### Finding 2 (ground truth / revert-discrimination) — **CLOSED**

The furnace TEST_CASE's `expectedE` (`test_standard_pbr_ibl_gpu.cpp:1158`)
is now `(1.0F - kF0) * kDfgX + kF0 * kDfgY`, transcribed directly in C++
from the cited Filament source (comment cites `surface_light_indirect.fs
:135` and `CubemapIBL.cpp:790-833` by name), never by calling
`iblSpecularReflectance()`. This is the mathematically SAME formula the
shader now implements — acceptable per the mandate's own standard ("a CPU
re-statement of the SAME formula is acceptable ONLY if the derivation is
justified against Filament/first principles in comments") because the
justification is a direct, cited transcription from the primary source in
a structurally separate implementation (C++, different file, no call
through the shader helper), not a copy of the shader's own code — exactly
the standard "verify against spec independently" test-design pattern, and
it demonstrably still catches a regression (see below). The mirror
TEST_CASE's ground truth uses the `mix()` endpoint identity at `f0==1`
(`E==dfg.y` for any `dfg.x`) — also an independent algebraic derivation,
not a shader call.

**Empirical re-proof (this session, lavapipe, RTX-2080-driver machine,
NICEd, serial, foreground):**
1. Edited `shaders/material/brdf.slang`'s `iblSpecularReflectance()` back
   to the retired `f0 * dfg.x + float3(dfg.y, dfg.y, dfg.y)` formula
   (one-line change, marked `TEMPORARY REVERT ... DO NOT COMMIT`).
2. Ran `rx_material_gpu_tests --test-case="*furnace*,*mirror-metal*"` —
   no rebuild needed (Slang compiles in-process at test-binary startup).
   Result: **3 test cases matched, 1 passed, 2 failed, 5 assertions
   failed**:
   - Mirror: `pixel r=205 g=102 b=11 expected=(184,92,10)` — an EXACT
     match to the report's own claimed magnitudes (shipped=205,
     corrected=184).
   - Furnace: `pixel r=93 expectedE=0.409 expected=63` — an EXACT match
     to the report's own claimed magnitudes (shipped=93, corrected=63).
   - The third matched (pre-existing, non-IBL) furnace/energy-compensation
     test case passed, unaffected — confirming the revert's blast radius
     was correctly isolated to `iblSpecularReflectance()` alone, exactly
     as the report claims.
3. Restored via `git checkout -- shaders/material/brdf.slang`; `git diff
   --stat` and `git status --short shaders/material/brdf.slang` both
   confirm zero diff (byte-identical restoration).
4. Re-ran the full `rx_material_gpu_tests` binary on lavapipe:
   **69/69 test cases, 3556/3556 assertions, 0 failed** — an exact match
   to the report's own claimed post-fix counts.
5. `git status --short` in the worktree after restoration: clean except
   pre-existing untracked build-cache directories (`.deps-cache`,
   `assets/fetched`, `toolchain` — build artifacts, not source).

This is a real, live-reproduced gate-flip against the actual
shader-compiled output, not a hand-derived or merely-reported claim.

### Finding 3 (dfg invariant) — **CLOSED**

Independently confirmed the real invariant by reading
`shaders/ibl/dfg_lut.slang`'s `dfvMultiscatter()` accumulation directly:
`r.x += term*fc; r.y += term;` with `fc = pow(clamp(1-voH,0,1), 5) ∈
[0,1]` and `term >= 0` — this makes `dfg.x <= dfg.y` (non-strict) the
true invariant of every real bake texel, matching both the original
review's and the fix's characterization.

The three dfg-bearing TEST_CASEs in `test_standard_pbr_ibl_gpu.cpp` all
satisfy this strictly: Lambertian `(0.05, 0.4)`, Mirror `(0.12, 0.80)`,
Furnace `(0.08, 0.55)`. A fourth, separate dfg-bearing case — the
occlusion-scales-IBL TEST_CASE in `test_standard_pbr_unlit.cpp` — uses
`(0.5, 0.5)`, an equality boundary rather than strict `<`; this is
correctly documented in-test as a deliberate boundary case ("this test
only needs a real, non-clamped visible value, not a discriminating one,
since its own assertions are RATIOS, invariant to which E-formula
produced them"), and equality is still compliant with the real (`<=`)
invariant, not a violation of it. No finding.

The Lambertian TEST_CASE's non-discrimination rationale is algebraically
verified sound: with energy compensation OFF, metallic=0/white baseColor
(`diffuseColor == (1,1,1)`), and a uniform environment
(`irradianceSample == prefilteredSample == L`), `ibl = diffuseColor*L*(1-E)
+ prefilteredSample*E = L*(1-E) + L*E = L` identically for any `E ∈
[0,1]` — a real split-sum energy-conservation identity, not a hand-waved
excuse; no choice of `dfg` could make this specific probe discriminate the
specular formula without abandoning its own "isolate the diffuse lobe"
purpose. The mirror and furnace cases (both metallic=1, which kills
`diffuseColor` and breaks the cancellation) correctly carry the
discrimination burden instead, confirmed empirically above.

### Finding 4 (LOW, sample09 default environment) — **CLOSED**

`samples/09_scene/main.cpp` binds a real default environment
(`setupEnvironment()`, `kDefaultEnvironmentIntensity = 0.4F`) via
`rx::ibl::bakeEnvironment()` → `Scene::setEnvironment()`, reusing
`samples/08_gltf_viewer/environments/gate_test_env.hdr` verbatim (no new
asset; confirmed via `CMakeLists.txt`'s new `environments_deploy.stamp`/
`ibl_shaders_deploy.stamp` blocks). Wired into both `runHeadless()`'s and
`runPresent()`'s default (no `--stress`, no `--scene`) branch, confirmed
by direct read of `main.cpp:2980-3026` and `main.cpp:3505-3545` — in both
functions `setupEnvironment()` is called BEFORE `warmMaterialPipelines()`,
with `materialSpecializationBits()` shared identically between the
pre-warm call and `resolveDrawGroups()`'s real per-frame lookup (single
function, confirmed by direct read, cannot drift).

Visually confirmed by rendering both PNGs myself (lavapipe, this session):
`sample09_grid_scene_no_env_256.png` (near-black, faint highlights only)
vs. `sample09_grid_scene_with_env_256.png` (all 8 DamagedHelmet instances
show clearly visible blue/teal-lit surfaces and dome reflections) — a
real, visible improvement, matching the report's own characterization,
not an exaggeration.

**Adjudication 1 — `--scene`/`--stress` modes still bind no environment.**
Ruling: **acceptable as-is, no finding.** Confirmed by direct read:
`runHeadless()` has exactly two branches (`args.stress` / default-grid);
`runPresent()` has three (`args.stress` / `!args.scenePath.empty()` /
default-grid) — `setupEnvironment()` is called only in the default-grid
branch of each, exactly as disclosed. `--stress` genuinely cannot benefit:
`setupStressMaterials()` (`main.cpp:1621-1644`) loads `unlit.slang`, not
`standard_pbr.slang` — confirmed by direct read — so no IBL lobe exists
to feed regardless of whether an environment is bound. `--scene <path>`
(explicit user-supplied glTF, e.g. Sponza) is a power-user override path,
not the "interactive fly-through the project owner personally drives"
the original LOW finding's own text specifically named (that finding's
own quote targets the DEFAULT demo state); extending environment binding
to arbitrary user-supplied content is a reasonable scope boundary for a
LOW-severity fast-follow, not an obligation this round left unmet.

**Adjudication 2 — sample09 has no skybox pass.** Ruling: **acceptable
as-is, no finding.** Read `gate/matrix-p5t10-ibl-runtime-skybox.md`'s own
"Skybox pass" row directly: its acceptance text and provenance
requirement name a GPU test against the shader/render-graph mechanism
generically, with no sample09 mention anywhere in the row or the
document; the report's own (T9/T10-established) file-list scoping ties
the skybox deliverable to `samples/08_gltf_viewer` specifically. Grepped
`samples/09_scene/main.cpp`/`CMakeLists.txt` for "skybox": zero
non-comment hits — confirmed no skybox code exists, only disclosure
comments explaining its absence. The original review's own LOW finding
text complained specifically about the HELMETS reading near-black (a
lighting-lobe problem), which Finding 4's environment bind fully
addresses without requiring a background sky visual; Finding 4's own
report text scoped itself explicitly to `Scene::setEnvironment()` only,
consistent with what the original finding actually asked for.

### Finding 5 (D17 regeneration) — **CLOSED**

Both gates re-run from scratch this session, both green on the enforced
(lavapipe) tier:
- `sample_08_gltf_viewer --validate` (lavapipe): `D17 loading_state
  gate: failingPixels=0/65536 (0.0000%) pass=true`; `D17 loaded_scene
  gate: failingPixels=0/65536 (0.0000%) pass=true`; `headless gate
  PASSED`.
- `sample_09_scene --validate` (lavapipe): `D17 grid_scene gate:
  failingPixels=0/65536 (0.0000%) pass=true`; `headless gate PASSED`.
- Same two binaries on real NVIDIA (RTX 2080, driver 580.82.07):
  `loaded_scene` `487/65536 (0.7431%)`, `grid_scene` `516/65536
  (0.7874%)` — both `pass=false` but explicitly `[non-lavapipe driver --
  informational only, not enforced]`, and an EXACT match to the report's
  own claimed figures — overall `headless gate PASSED` on both (the
  informational NVIDIA divergence does not fail the gate).
- Regenerated PNGs visually inspected directly (`samples/08_gltf_viewer/
  references/loaded_scene.png`, `samples/09_scene/references/
  grid_scene.png`) — both show a physically-lit, non-degenerate render;
  no artifacts, no black/broken frames.
- Provenance: only `references/*.png` binary diffs and the CMakeLists.txt
  deploy-block additions appear in the diff for sample assets — no D17
  threshold/tolerance constant in any sample `main.cpp` was touched by
  this commit (`samples/08_gltf_viewer/main.cpp` doesn't appear in the
  diff at all), and the GPU TEST_CASE tolerances (`±6`, `±8`) are
  unchanged from the prior round — consistent with "regen script used,
  tolerances unchanged."

### Empirical minimum closed this session

- **Full serial ctest, lavapipe** (`VK_ICD_FILENAMES=lvp_icd.json nice -n19
  xvfb-run -a ctest --test-dir build/linux-native --output-on-failure
  -j1`): **33/33 (100%)**, ~113-115s across two runs. Zero unfiltered
  validation errors (`grep -i validation | grep -v "known false
  positive"` → empty).
- **Full serial ctest, real NVIDIA** (`VK_ICD_FILENAMES=nvidia_icd.json`,
  identical invocation; `nvidia-smi`/`vulkaninfo` confirm GeForce RTX
  2080, driver 580.82.07): **33/33 (100%)**, 187.85s. Zero unfiltered
  validation errors.
- Revert-discrimination re-proof: reproduced live (Finding 2 above),
  magnitudes match the report exactly, restored byte-identically, re-run
  green.
- Commit hygiene: exactly one commit (`git rev-list --count
  3f62df1..1a71e9c` = 1) — `1a71e9c` on `task/t10-ibl-runtime`.
  Author/committer both `Yousef Wadi <ywadi85@gmail.com>`. `git show
  1a71e9c | grep -iE "claude|anthropic|co-authored|ai-generated|generated
  by"` → no matches. `git branch -vv` shows the task branch with no
  upstream/remote tracking ref (not pushed). Main checkout: `git status
  --short` shows only the pre-existing `progress.md` modification, both
  before and after this session's work.

### Overall verdict

**ALL FIVE FINDINGS ADDRESSED.** Finding 1 (CRITICAL) is fixed with the
formula independently re-verified against a fresh fetch of Filament
v1.75.0 (not the prior round's citation, re-derived from source this
session) and empirically re-proven to discriminate at magnitudes matching
the original review's report exactly. Findings 2/3 (test-design) are
genuinely closed, not cosmetically closed — the new ground truth is
structurally independent and demonstrably catches the regression live;
the dfg constants now respect the real bake invariant, with the one
sound, documented exception (the ratio-based occlusion test's equality
boundary). Finding 4 (LOW) is closed with a real, visually-confirmed
lighting improvement, and the two disclosed out-of-scope items
(`--scene`/`--stress` environment binding, no sample09 skybox) both
adjudicate as legitimate, appropriately-scoped non-findings, not
overlooked gaps. Finding 5's D17 regenerations are green on the enforced
tier with informational-only NVIDIA divergence matching the report
exactly, and provenance/tolerance discipline was followed. No new
findings raised by this re-review round.
