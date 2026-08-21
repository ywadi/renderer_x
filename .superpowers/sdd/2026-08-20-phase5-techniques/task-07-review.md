# Task 7 review — Filament BRDF module port to Slang (issue #43)

Independent reviewer round. Commits under review: `3eaaa0b` (module +
tests + CMake), `59b598e` (CI wiring), `ff94cca` (report) — base `87ac4d1`.
Order of authority: `rulings-2026-08-20.md` ("T7 (#43)") > plan (Task 7) >
gate matrix (`matrix-p5t07-brdf-module-port.md`) > ticket (#43).

## Verdict 1 — Spec compliance: **PASS**

Every matrix row, as amended by the T7 per-ticket ruling
("computed-from-F0 f90 becomes the default... permutations via Slang
link-time composition, not runtime spec constants"), is satisfied:

- **D_GGX, V_SmithGGXCorrelated, F_Schlick (3 overloads), fresnelDefault,
  Fd_Lambert, Fd_Burley, energyCompensation** — all present in
  `shaders/material/brdf.slang`, all formula-verified against a fresh
  fetch of the pinned Filament source (below), all table-tested against
  independently-typed fp64 references, all passing on both drivers.
- **f90 convention** — `fresnelDefault()` (computed f90) ships as the
  default; the implicit-f90=1.0 form is kept as a named, tested
  `F_Schlick(f0,VoH)` fallback. Matches the T7 ruling exactly.
- **Link-time composition, not spec constants** — `IEnergyCompensationFeature`
  / `EnergyCompensationOn` / `EnergyCompensationOff`, linked via
  `forward_entry.slang`'s already-verified extern/export +
  `createCompositeComponentType()`/`link()` contract. No
  `[vk::constant_id]` anywhere in the new files (grepped). Matches the
  ruling exactly.
- **Composable module layout** — `brdf.slang` takes scalar/vector inputs
  only (no `ParameterBlock`, no bindless global, no `IMaterialShader`
  dependency), compiles standalone (device-free test, confirmed).
- **Production path untouched** — `standard_pbr.slang`/`material.slang`/
  `forward_entry.slang` are **0 lines changed** in this diff range
  (`git diff 87ac4d1..ff94cca -- <those 3 files>` is empty); 08/09 gates
  reran unaffected (see Verification below).
- **Clearcoat extension point** — a named, task-referencing comment
  ("Extension point [Task 21...]") is present in `brdf.slang`, matching
  this codebase's D13/D7 "future-task comment" precedent.

## The matrix-correction adjudication: **the implementer's correction is
mathematically correct; I independently re-derived it and reproduce the
same result.**

The gate matrix's own row text frames the f90/implicit divergence as a
"high-F0" effect ("raises the grazing-angle reflectance ... for
high-f0/metallic surfaces"). Re-deriving directly from the pinned
formula (`f90 = saturate(dot(f0, vec3(50.0*0.33)))`, i.e.
`saturate(16.5 * (f0.r+f0.g+f0.b))`):

- The dot saturates to exactly `1.0` (bit-identical to the implicit
  `f90=1` form) whenever `f0.r+f0.g+f0.b >= 1/16.5 ≈ 0.06061`. Any
  physically-plausible metal (F0 well above 0.06 summed) and the
  glTF/Disney dielectric baseline (0.04×3 = 0.12) both clear this
  threshold — **high-F0/metallic surfaces show NO divergence**, the
  opposite of the matrix's own framing.
- Divergence only exists for **low total reflectance** (e.g. a
  `KHR_materials_specular`-attenuated dielectric pushed below ~0.0606
  summed F0).
- I hand-verified the chosen discrimination point (P6: f0=(0.01,0.01,0.01),
  VoH=0.02) independently: `f90_computed = saturate(16.5*0.03) = 0.495`
  (not saturated — genuinely below 1.0); `F_Schlick` implicit form gives
  `0.01 + 0.99·(0.98)^5 ≈ 0.9049`; computed form gives
  `0.01 + 0.485·(0.98)^5 ≈ 0.4484`. Divergence ≈ **0.456**, matching the
  report's claimed "~0.456" exactly. The test point genuinely
  discriminates, and it does so for the low-F0 reason the corrected
  framing gives, not the matrix's original high-F0 framing.

**Ruling: the correction is right, well-evidenced, and the committed
test's chosen F0 does genuinely discriminate.**

## Port fidelity — re-verified against a fresh fetch of the pin

Re-fetched `shaders/src/surface_brdf.fs`, `surface_shading_lit.fs`,
`surface_shading_model_standard.fs`, `libs/ibl/src/CubemapIBL.cpp`,
`libs/ibl/include/ibl/utilities.h`, and `shaders/src/common_math.glsl`
(for `PREVENT_DIV0`/`pow5`) directly from `google/filament` at the pinned
commit `0e58877c09afb1aacd09ff640f74d2adcd2a7e80` (confirmed via
`gh api repos/google/filament/commits/<sha>`: committed
2026-08-04T04:53:24Z, consistent with the v1.75.0 tag date) — not trusted
from the matrix's or report's own citations.

- **D_GGX**: formula-for-formula match (Filament's `min(...,453.5)` fp16
  clamp correctly dropped, per the module's own documented fp32-only
  rationale). Independently hand-verified the claimed algebraic identity
  between this numerically-stable form and the pre-existing
  `standard_pbr.slang` textbook rearrangement — both reduce to
  `alpha^2 / (PI·(NoH^2·(alpha^2-1)+1)^2)` under the shared "alpha =
  roughness^2" convention. Confirmed exact.
- **V_SmithGGXCorrelated**: formula-for-formula match, including
  `PREVENT_DIV0`'s real macro definition (fetched from
  `common_math.glsl`: `n/max(d,magic)`) — the port's
  `0.5/max(lambdaV+lambdaL, 1e-5)` is structurally identical, only the
  epsilon differs (1e-5 vs Filament's fp16-tuned 0.0000077), which is the
  documented, disclosed fp32-vs-fp16 substitution already used by the
  pre-existing `standard_pbr.slang` inline form.
- **F_Schlick (three overloads)** and **fresnelDefault**: core formulas
  match exactly. One deviation found and independently traced: all three
  overloads compute `pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0)`, but Filament's
  own source uses the **unclamped** `pow5(1.0-VoH)`/`pow(1.0-VoH,5.0)`
  (confirmed by reading `surface_brdf.fs:145-157` directly — no clamp
  anywhere in any of the three). Traced this to this project's own
  pre-existing `standard_pbr.slang:211` inline Fresnel, which **already**
  uses the identical `pow(clamp(1.0-VdotH,0.0,1.0),5.0)` form before this
  port — so this is inherited project convention, not a deviation
  invented by this task, and it is a no-op for every table point tested
  (all 7 points have VoH∈[0,1], where the clamp never engages) and for
  every real call site (VoH/LoH is always pre-saturated by the caller in
  both Filament's and this project's shading code). See Findings below —
  flagged as a documentation gap, not a correctness bug.
- **Fd_Lambert**, **Fd_Burley**: exact formula match, byte-for-byte
  against the fetched source (including citations: `Fd_Burley` at
  `surface_brdf.fs:241-247`, exact line range).
- **energyCompensation**: exact match to
  `getEnergyCompensationPixelParams()`'s
  `1.0 + f0*(1.0/dfg.y - 1.0)` (`surface_shading_lit.fs:237`). The
  multiplicative contract comment (`color = Fd + Fr*pixel.energyCompensation`)
  is accurate but off by 7 lines from the report's own citation
  (`:135` cited, actual pinned line is `:128`) — cosmetic, not
  substantive; the surrounding function and formula are otherwise
  identical.
- **White-furnace Monte-Carlo kernel** (`hammersley`, 
  `hemisphereImportanceSampleDggx`, `dfvMultiscatter`): re-fetched
  `CubemapIBL.cpp`/`utilities.h` directly and diffed by hand against the
  test's Slang kernel. `hammersley`'s bit-reversal and `1/2^32` scale
  factor match exactly. `hemisphereImportanceSampleDggx`'s
  `cosTheta2`/`phi` formulas match exactly (the port adds `max(0.0, ...)`
  NaN guards around the two `sqrt()` calls — provably no-ops for any
  valid input in [0,1], since `cosTheta2` is mathematically bounded
  there). `DFV_Multiscatter`'s accumulation (`r.x += v*Fc; r.y += v;
  return r*(4/N)`) matches exactly, line-range citation (790-833)
  confirmed exact. The kernel correctly reuses the module's own
  already-verified `V_SmithGGXCorrelated()` in place of re-porting
  `Visibility()` — I confirmed `Visibility()`
  (`CubemapIBL.cpp:170-176`) is the algebraically identical formula
  minus the div-zero guard, so this substitution is sound, not a
  shortcut that changes behavior.

**No silent formula drift found.** The one real deviation (the F_Schlick
clamp) is inherited, harmless-in-practice, and only under-disclosed, not
hidden — see Findings.

## White-furnace test — methodology verified

The test ports `CubemapIBL::DFV_Multiscatter` (not the production DFG
LUT, which doesn't exist yet — Task 9) as a test-local Monte-Carlo
kernel, 16384 samples/point, matching the gate matrix's own resolved
calling contract (`dfgY` as a plain parameter, no IBL binding
dependency). "Energy conservation" is asserted over **both** the
uncompensated `Ess` (checked for near-1.0 at near-mirror roughness and
strict monotonic decrease across a roughness sweep — a real, independent
physical check that does not depend on the compensation formula) **and**
the compensated response (checked at ≈1.0 to fp32-rounding tolerance).
The test's own header comment candidly discloses that
`Ess·energyCompensation(1,Ess) = 1` is an algebraic tautology for any
nonzero `Ess` and that the real discriminating power comes from
combining it with the monotonicity checks — this is accurate and matches
what the test actually asserts, not an unearned claim. Independently
verified the reported Ess values are internally consistent (monotonic
Q0>Q1>Q2>Q3, Q4 shows a real off-normal deficit) and re-ran the exact
suite on both drivers (Verification below) — same 131/131 pass on both.

## SPIR-V absence — reproduced independently

Ran the full suite (which includes the SPIR-V-composition test) on real
NVIDIA hardware and inspected the resulting temp files myself, without
trusting the test's own assertions:

```
$ grep -n OpFDiv /tmp/rx_brdf_energy_feature_on.dis.txt
68:         %74 = OpFDiv %float %float_1 %27
$ grep -c OpFDiv /tmp/rx_brdf_energy_feature_off.dis.txt
0
$ wc -l /tmp/rx_brdf_energy_feature_{on,off}.dis.txt
  77 .../rx_brdf_energy_feature_on.dis.txt
  64 .../rx_brdf_energy_feature_off.dis.txt
$ ls -la /tmp/rx_brdf_energy_feature_{on,off}.spv
1252 bytes (on) / 1000 bytes (off)
```

Byte-for-byte identical to the report's claimed numbers (line 68, exact
opcode, exact file sizes). The ON variant's single `OpFDiv` is
`EnergyCompensationOn`'s `1.0/dfgY`; the OFF variant has zero division
instructions anywhere — the feature's code is provably absent, not
merely dead-code-eliminated after the fact.

## Revert-discrimination reproduction (f90 discriminator, my choice)

Mutated `brdf.slang`'s `fresnelDefault()` to hardcode `f90 = 1.0`
(collapsing it to the implicit form), rebuilt, ran on lavapipe:

```
TEST CASE: brdf.slang fresnelDefault() (computed f90) discrimination...
test_brdf_module_gpu.cpp:360: ERROR: CHECK( std::abs(implicitGpu - computedGpu) > 0.05 ) is NOT correct!
  values: CHECK( 0 >  0.05 )
[doctest] test cases:   6 |   4 passed | 2 failed | 0 skipped
[doctest] assertions: 131 | 128 passed | 3 failed |
```

Exactly reproduces the report's claimed failure text
(`CHECK( 0 > 0.05 )`). (The mutation also fails one assertion in the
port-parity table test, since the P6 reference value no longer matches
the mutated GPU output — a correct, expected side effect the report
didn't call out explicitly but which does not contradict it.) Restored
the file; `git diff shaders/material/brdf.slang` is empty; rebuilt;
suite back to 6/6, 131/131 on lavapipe.

## Verdict 2 — Code quality: **Approved**, findings below

### Findings

1. **[MINOR]** `brdf.slang`'s otherwise-thorough per-function deviation
   comments (which explicitly call out the fp16-epsilon substitution and
   the dropped 453.5 D_GGX clamp) do not mention that all three
   `F_Schlick` overloads clamp `1.0-VoH` to `[0,1]` before the `pow(...,
   5.0)` call, where Filament's own source does not. This is not a
   correctness bug (see Port fidelity above — inherited from
   `standard_pbr.slang`'s own pre-existing convention, and provably a
   no-op for every tested/production call site since VoH/LoH is always
   pre-saturated by callers before reaching this function), but it is an
   undisclosed deviation from the literal pinned formula in a file whose
   whole design intent is "every deviation is called out." The test
   file's "independently-typed fp64 reference, never brdf.slang's own
   source text" claim (`test_brdf_module_gpu.cpp`'s header comment) is
   also technically inexact for this reason — the fp64 references
   (`refFSchlickImplicit`, `refFresnelComputed`) bake in the same clamp
   rather than transcribing Filament's literal unclamped formula. Zero
   effect on any delivered result (no test point exercises VoH outside
   [0,1]). Recommend a one-line addition to `brdf.slang`'s F_Schlick
   header comment noting the clamp and its provenance (matches this
   project's existing `standard_pbr.slang` convention), the same
   treatment already given to the other two documented deviations.
2. **[NIT]** `surface_shading_model_standard.fs`'s `color = Fd + Fr *
   pixel.energyCompensation` citation in both the module header and the
   report's provenance table (`:135`) is off by 7 lines against the
   actual pinned commit (`:128` — confirmed by direct fetch). Same
   function, same formula, harmless; a pin-drift citation slip, not a
   sourcing problem.

No other quality issues found. Module boundary discipline is real (no
`ParameterBlock`/bindless/`IMaterialShader` dependency anywhere in
`brdf.slang`, verified by reading the whole file). Test harness code
(`brdf_test_harness.h`, `brdf_gpu_fixture.h`) correctly mirrors
`MaterialSystem`'s own session-creation and fixture idioms rather than
reinventing them, matching this codebase's established
per-file-duplicated-helper convention (cited precedent files checked:
the pattern is real, not invented for this task). The white-furnace
test's own header comment is a model of intellectual honesty about what
the test can and cannot prove — better self-scrutiny than most of this
phase's other tasks have shown. No dead code, no stray debug output, no
scope creep into production shader files.

## Verification performed (empirical minimum)

All performed directly by me this session, not taken from the report:

- **Fresh Filament fetch** — `gh api repos/google/filament/commits/<pin>`
  confirms the pinned SHA is real and dated consistent with v1.75.0
  (2026-08-04T04:53:24Z). Six files fetched in full
  (`surface_brdf.fs`, `surface_shading_lit.fs`,
  `surface_shading_model_standard.fs`, `CubemapIBL.cpp`, `utilities.h`,
  `common_math.glsl`) and read/diffed by hand against the shipped Slang,
  not sampled.
- **Build** — `cmake --preset linux-native` (spirv-dis found, no
  `FATAL_ERROR`); `cmake --build build/linux-native -j8` clean.
- **Lavapipe, full serial suite** —
  `VK_ICD_FILENAMES=.../lvp_icd.json xvfb-run -a ctest --test-dir
  build/linux-native --output-on-failure -j1`: **32/32 passed**,
  including `sample_08_gltf_viewer_headless`,
  `sample_08_gltf_viewer_quit_during_load`, `sample_09_scene_headless`,
  `sample_09_scene_stress_headless`, `sample_09_scene_tests` — the 08/09
  gates the production-untouched claim depends on, unaffected.
  `rx_material_brdf_gpu_tests` is CTest #15 on the linux-native path,
  confirming the new binary is registered and runs there, not silently
  skipped.
- **Lavapipe, new binary alone** — 6/6 test cases, 131/131 assertions,
  `SUCCESS`.
- **Real NVIDIA (driver-labeled)** — GeForce RTX 2080, driver 580.82.07,
  `DISPLAY=:1 VK_ICD_FILENAMES=.../nvidia_icd.json nice -n 15
  ./rx_material_brdf_gpu_tests --validate`: **6/6 test cases, 131/131
  assertions, SUCCESS**. Zero unfiltered validation errors on either
  driver (only the codebase's own pre-existing documented
  false-positive filter entries — `InconsistentSpirv`/Slang
  source-language and `VK_KHR_portability_enumeration` — appear in the
  logs on both runs).
- **SPIR-V-absence reproduction** — done independently, see above,
  byte-for-byte match to the report.
- **Revert-discrimination reproduction** — the f90 discriminator (my
  choice of the four), done independently, see above; restored
  byte-identically (`git diff shaders/material/brdf.slang` empty after
  restore) and reconfirmed green (6/6, 131/131) on lavapipe.
- **Production-path untouched** — `git diff 87ac4d1..ff94cca --
  shaders/material/standard_pbr.slang shaders/material/material.slang
  shaders/material/forward_entry.slang` is empty (0 lines).
- **CI wiring** — `spirv-tools` present in both jobs' package-install
  steps (`.github/workflows/ci.yml:126` linux-native,
  `:488` windows-cross-zig, confirmed by direct grep of the current
  file, not the diff). `rx_material_brdf_gpu` present in the Wine-job
  ctest exclusion regex (`:706`). Linux-native's own ctest invocation
  (`:337`) carries no exclusion pattern at all, so the new binary's
  `add_test()` registration alone is what gates it into CI — confirmed
  registered (CTest #15 above) and passing.
- **Commit hygiene** — 3 commits (`3eaaa0b`/`59b598e`/`ff94cca`), each
  pathspec-scoped to exactly what its own message claims (module+tests+
  CMake / CI yaml only / report only — confirmed via `git show --stat`
  on each). Author/committer `Yousef Wadi <ywadi85@gmail.com>` on all
  three, no override. `git log -p` across the range grepped for
  `claude|anthropic|co-authored|generated` — no attribution hits (the
  only matches are the report's own text describing that it checked).
  3 commits ahead of `origin/main`, nothing pushed. `progress.md` is not
  touched by any of the 3 commits under review (its current working-tree
  modification predates this round and was left untouched throughout,
  per instruction).
- **Windows-cross-zig build/Wine ctest** — **not independently
  reproduced this session** (not required by the empirical-minimum list,
  which asks only that CI wiring facts be verified statically — done
  above). The report's own windows-cross-zig configure/build and Wine
  ctest command tails were not re-run by me.
- **The other 3 of 4 revert-discrimination proofs** (D_GGX parity,
  white-furnace formula sabotage, SPIR-V link-composition harness-bug)
  were reviewed by reading the test code's structure (sound in each
  case — the assertions genuinely fail under the described mutation) but
  **not independently re-run**, per the task's "reproduce ONE" scope.

## Restoration

All temporary edits (`brdf.slang`'s `fresnelDefault()` mutation) were
restored byte-identically and reconfirmed via `git diff` (empty) before
this review was written. `git status` at the end of this session shows
only the pre-existing, out-of-scope `progress.md` modification —
untouched, left alone per instruction.
