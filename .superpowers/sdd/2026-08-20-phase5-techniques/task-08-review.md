# Task 8 review — StandardPBR rework onto the module architecture (issue #44)

Independent reviewer round. Commits under review: `88d45a8` (feat, shader
rework + material_system mechanism), `7230725` (test, value-asserted GPU
tests), `44a2bdf` (chore, SDD report). Base `d8b8d46`. Reviewer did not
write this code; every finding below is from direct re-execution or direct
spec fetch, not from trusting the implementer's report text.

## Verdict: spec compliance — PASS (matrix-p5t08, as amended by the T8 ruling)

Every matrix row (as amended by the T8 per-ticket ruling — "permutation
mechanism now, NO pre-added unused parameter fields") is satisfied and
independently reproduced. One documentation-completeness gap found (see
Findings, LOW) does not affect this verdict: the T8 ruling's actual binding
text is the data-level constraint (no unused fields), which is fully met;
the matrix's "documented extension-point comment" was only the matrix's own
suggested *compensating* measure, not itself a ruling requirement.

## Verdict: code quality — Approved, with 2 LOW findings (no blockers)

Mechanism, math, and tests are sound, spec-accurate, and independently
reproducible end to end on real hardware. No MEDIUM/HIGH findings.

## Reproduction log (all commands re-run independently this session)

**Byte-identical-gates claim (load-bearing outcome) — CONFIRMED, lavapipe
(llvmpipe/Mesa 25.1.5, driver-labeled).** Rebuilt `linux-native` (no source
changed since HEAD; only shader-deploy stamps re-ran), then ran samples
08/09 with `VK_ICD_FILENAMES` forced to `lvp_icd.json`:
```
sample_08_gltf_viewer: D17 loading_state gate: failingPixels=0/65536 (0.0000%) pass=true
sample_08_gltf_viewer: D17 loaded_scene gate:  failingPixels=0/65536 (0.0000%) pass=true
sample_09_scene:       D17 grid_scene gate:    failingPixels=0/65536 (0.0000%) pass=true
```
Exact match to the report's claim. No image was regenerated for either
gate — the module rework at glTF defaults is output-identical to the old
inline math, confirmed pixel-for-pixel.

**Full ctest suite, lavapipe, serial, foreground — CONFIRMED 32/32
(100%).** `VK_ICD_FILENAMES=lvp_icd.json xvfb-run -a ctest --preset
linux-native --output-on-failure -j1`, driver-labeled llvmpipe/Mesa. All 32
tests passed, including `sample_06_materials_headless` (the mid-round
regression's own regression test).

**Two GPU test binaries, real NVIDIA — CONFIRMED, driver-labeled
(`NVIDIA GeForce RTX 2080`, `DRIVER_ID_NVIDIA_PROPRIETARY`, driver
580.82.07, default ICD, `vulkaninfo`-confirmed this session):**
```
rx_material_gpu_tests      --validate : 65 test cases | 3293 assertions | 0 failed
rx_material_brdf_gpu_tests --validate :  8 test cases |  167 assertions | 0 failed
```
Exact match to the report's `65/3293` and `8/167`.

**Same two binaries, lavapipe forced — CONFIRMED, driver-labeled
(llvmpipe/Mesa):** identical 65/3293 and 8/167, 0 failed.

**KHR math fidelity — CONFIRMED against the actual extension specs**
(fetched fresh this session via `gh api
repos/KhronosGroup/glTF/contents/extensions/2.0/Khronos/<ext>/README.md`,
not re-trusting the gate matrix's prior quotes):
- `KHR_materials_ior`: spec's own Implementation section gives
  `dielectric_f0 = ((ior-1)/(ior+1))^2`, default `ior=1.5` → `0.04`. The
  spec's ior=0 "Specular-Glossiness Backwards Compatibility Mode" text is
  a **MUST**: *"the Fresnel term MUST evaluate to 1.0 independently of the
  view or light direction"* — the *term*, not just F0. `standard_pbr.slang`'s
  `computeDielectricF0F90()` sets **both** `f0=1.0` and `f90=1.0` in that
  branch (`shaders/material/standard_pbr.slang:231-236`), which is the only
  way `F_Schlick(f0,f90,VoH)` stays exactly 1.0 at every VoH including
  grazing incidence — correctly matches the spec's compound requirement.
  Verified this is a real, distinct code path (not a coincidental clamp) by
  independently deriving that at a non-1.0 `specularFactor`, the *ordinary*
  formula would give `f0=specularFactor≠1.0`, diverging sharply from the
  special case — exactly the discrimination the shipped test exercises.
- `KHR_materials_specular`: spec's own Implementation section gives
  `dielectric_f0 = min(0.04*specularColor, float3(1.0))*specular`,
  `dielectric_f90 = specular` (not 1.0), and the Interaction section states
  *"If KHR_materials_ior is used in combination with KHR_materials_specular,
  the constant 0.04 is replaced by the value computed from the IOR"* with
  the explicit combined formula `dielectric_f0 =
  min(((ior-outside_ior)/(ior+outside_ior))^2 * specularColor,
  float3(1.0)) * specular`. `computeDielectricF0F90()`'s
  `result.f0 = min(dielectricF0Base * specularColorFactor, 1.0) *
  specularFactor; result.f90 = specularFactor;` is an exact, unconditional
  match (outside_ior implicitly 1.0, matching the spec's own "typically set
  to 1.0" note).
- Regression-guard algebra independently re-derived: at glTF defaults
  (ior=1.5, specular=1.0, specularColor=(1,1,1)) →
  `((0.5/2.5))^2=0.04`, `min(0.04,1)*1=0.04`, `f90=1.0` — byte-identical to
  the pre-T8 hardcoded `float3(0.04,0.04,0.04)` + implicit-f90=1.0.

**specializationBits correctness — CONFIRMED.** `PipelineKey`
(`src/rx_material/material_system.cpp:679-687`) is a plain struct with
`operator==` defaulted and both `moduleHash`/`passHash`/
`specializationBits`/`alphaMode`/`doubleSided` folded into
`PipelineKeyHash`; `std::unordered_map` lookup uses hash **and**
`operator==`, so no stale-key collision is structurally possible regardless
of hash quality. `getPipeline()`'s
`wantsEnergyCompensationOn = record->hasEnergyCompensationVariant &&
(bits & kSpecializationEnergyCompensation)` correctly gates the module
selection per-material, not globally. Re-ran the shipped "energy-
compensation permutation mechanism" TEST_CASE directly (part of the 65/3293
NVIDIA + lavapipe runs above): `pipelineOffFirst != pipelineOn` (genuinely
distinct `VkPipeline`) and `pipelineOffSecond == pipelineOffFirst` (the OFF
variant's cache entry is the *same handle*, not merely equal-by-value,
after the ON variant was requested in between) — both passed on real
hardware.

**Production-path SPIR-V pair proof — REPRODUCED independently.** Re-ran
the shipped `"...PRODUCTION path..."` TEST_CASE in `rx_material_brdf_gpu_tests`
verbosely (`--success`) to force doctest to print its `INFO()` payload:
```
OFF OpFDiv count: 4
ON  OpFDiv count: 5
```
Matches the report's claim exactly (4 vs 5, +1 for
`energyCompensation()`'s own `1.0/dfgY`); the composite is built from the
real shipped `material.slang`/`forward_entry.slang`/`standard_pbr.slang`/
`brdf.slang` plus each companion file, disassembled via the real
`spirv-dis`, not a synthetic fixture.

**T8 ruling's data-level rule ("exactly the minimum new fields") —
CONFIRMED.** Read the current `StandardPbrParams` struct directly
(`shaders/material/standard_pbr.slang`): exactly four new fields (`ior`,
`specularFactor`, `specularColorFactorAndPad`, `dfgY`). Grepped the file
case-insensitively for all nine named extension slots
(`clearcoat|anisotropy|sheen|transmission|thickness|attenuation|dispersion|
iridescence|diffuseTransmission`) — **zero matches**: none of the nine
lobes exist as struct fields, comments, or otherwise in this file. No
storage creep.

**Sample-06 CMake fix — CONFIRMED correct shape, and confirmed scoped
correctly project-wide.** All three affected samples (06/08/09) use the
`add_custom_command(OUTPUT ... DEPENDS ...)` + stamp + `custom_target`
pattern (never `POST_BUILD`), so an edited `.slang` source triggers a
redeploy on the next build rather than going stale. Checked every other
sample (`01/02/05/07`) for `MaterialSystem` construction in `main.cpp` —
none construct one (their `material_shaders` CMake comments are only
citing sample 08's mechanism as precedent for unrelated shader-deploy
blocks) — so the mid-round regression class this fix closes cannot recur
silently elsewhere in the sample tree today.

**dfgY-inert justification — CONFIRMED, doubly.** (1) Every shipped
`getPipeline()` call site in `samples/08_gltf_viewer/main.cpp` and
`samples/09_scene/main.cpp` passes a literal `0` for `specializationBits`
(grepped all 5 call sites) — the ON variant is never requested in
production. (2) Read `brdf.slang`'s `energyCompensation(f0, dfgY) = 1.0 +
f0*(1.0/dfgY - 1.0)` directly: at `dfgY=1.0` (what every shipped producer
binds), this is algebraically `1.0 + f0*0 = 1.0` for *any* f0 — a true
identity multiplier, not merely an untested value. So even a hypothetical
future accidental ON-variant request at the current bind values would
still be a no-op, not a silent quality regression. The GPU test that
exercises the ON variant with `dfgY=0.5` is a real, GPU-rendered,
oracle-checked, non-tautological test (re-ran as part of the 65/3293 runs
above; `pixelOn.r > pixelOff.r` genuinely holds).

**Revert-discrimination — reproduced independently (ior<=0 branch
neuter).** `RX_MATERIAL_SHADER_DIR` for the GPU test binaries points
directly at the source tree (`${CMAKE_SOURCE_DIR}/shaders/material`), so
mutation-without-rebuild is real, not a claimed shortcut:
- Baseline: `--test-case="*ior=0*"` → 1 passed, 61/61 assertions.
- Mutated `if (ior <= 0.0)` → `if (false)` in
  `shaders/material/standard_pbr.slang`, re-ran the *already-built* binary
  with no rebuild: 1 FAILED, 58/61 assertions (3 failed) — the ior=0
  special-case pixel checks, exactly as the report describes.
- Restored via `git checkout -- shaders/material/standard_pbr.slang`
  (byte-identical to HEAD, confirmed via `git status`/`git diff --stat`);
  re-ran: 1 passed, 61/61 again.

**Commit hygiene — CONFIRMED.** Exactly 3 commits (`88d45a8`, `7230725`,
`44a2bdf`), all authored/committed as `Yousef Wadi <ywadi85@gmail.com>`
(the user's own local git identity, untouched). `git log -p
88d45a8^..44a2bdf | grep -iE "co-authored-by|claude|anthropic|generated
with|ai-assist"` → zero matches across all three commits. `progress.md`'s
pre-existing modification is absent from every commit's file list
(pathspec discipline held). Branch is `ahead 3` of `origin/main`, nothing
pushed.

## Findings

1. **[LOW, spec-compliance-adjacent, code quality]** The task report
   claims all nine declared-but-gated glTF extension slots
   (clearcoat/anisotropy/sheen/transmission/thickness/attenuation/
   dispersion/iridescence/diffuseTransmission) are "declared-but-gated via
   a documented extension-point comment only." Grepping the current
   `standard_pbr.slang` for all nine names returns zero matches — no such
   comment exists in this file for any of them. An extension-point comment
   *does* exist, but only for clearcoat/anisotropy, lives in `brdf.slang`
   (`// --- Extension point [Task 21, clearcoat/anisotropy] ---`), and
   predates this task (T7's work, untouched by this diff — `brdf.slang` is
   not in the 3-commit file list). The other 7 of 9 named slots have no
   breadcrumb anywhere. This does not violate the T8 ruling itself (the
   binding text is the data-level "no pre-added unused fields" constraint,
   which is fully met), but it does mean the report over-states what was
   delivered against the matrix's own recommended compensating measure,
   and a future implementer landing sheen/transmission/etc. gets no
   "future task lands here" pointer the way clearcoat/anisotropy already
   does.
2. **[LOW, code quality, informational]** `getPipeline()`'s fallback logic
   means a caller that (incorrectly) sets `kSpecializationEnergyCompensation`
   for a material with `hasEnergyCompensationVariant == false` gets a
   second, distinct `PipelineKey` (different `specializationBits`) whose
   resolved `VkShaderModule` pair is byte-identical to the base pipeline's
   — i.e. a harmless duplicate `VkPipeline` object rather than a cache hit.
   No correctness issue (no material does this today; the bit is only ever
   set for `standard_pbr.slang`-derived materials by test code), but worth
   a one-line comment at `getPipeline()`'s cache-key construction for the
   next axis added to `specializationBits`, since a second real axis
   combined with this one would multiply the duplicate-cache-entry surface.

## Not independently re-verified

- **windows-cross-zig/Wine 14/14** — trusted from the report's own command
  tail; not re-run this session (outside the review's explicit empirical
  minimum, and a full cross-compile+Wine cycle was judged not
  cost-justified given every other empirical claim reproduced clean on the
  first two drivers).
- **Proof 2 (SPIR-V delta test's own OFF/ON mutation)** — only one revert
  was required per the review brief ("suggested: the ior<=0 branch
  neuter"); reproduced that one only, not the second proof in the report.
- **Tracy zone / variant-cost claim** — the report's own "Concerns for the
  coordinator" section already flags this as an architectural argument
  rather than a measured Tracy capture; not independently measured this
  session (no dedicated zone exists to measure).

## Housekeeping

All temporary edits (`shaders/material/standard_pbr.slang`'s `if (false)`
mutation) were restored byte-identically via `git checkout --`, confirmed
via `git status`/`git diff --stat` showing no diff before writing this
review. The pre-existing `.superpowers/sdd/2026-08-20-phase5-techniques/
progress.md` modification was left untouched, as instructed.
