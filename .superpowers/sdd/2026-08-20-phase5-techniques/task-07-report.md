# Task 7 report — Filament BRDF module port to Slang (issue #43)

Implementer round. Base: main `87ac4d1` (one ledger commit ahead of the
dispatch note's stated `06e6488`; no conflicting work). Order of authority
followed: rulings (`rulings-2026-08-20.md`) > plan (Task 7) > gate matrix
(`matrix-p5t07-brdf-module-port.md`) > ticket (#43).

## Status: COMPLETE

Every matrix row (as amended by the T7 per-ticket ruling) is satisfied.
Both presets build clean. Real-driver (NVIDIA RTX 2080) and lavapipe runs
both pass with zero unfiltered validation errors. Windows-cross-zig/Wine
suite unaffected (14/14, unchanged from before this round). Four
independent revert-discrimination proofs performed and recorded (D_GGX
parity, the f90 discriminator, the white-furnace energy-compensation
formula, and the SPIR-V link-composition mechanism) — every one reproduced
the expected failure, then was restored to green.

## What shipped

- **`shaders/material/brdf.slang`** (new) — the composable BRDF math
  module: `D_GGX`, `V_SmithGGXCorrelated`, `F_Schlick` (three overloads:
  vec3-f0/f90, scalar-f0/f90, vec3-f0-implicit-f90=1.0), `fresnelDefault`
  (computed-f90, the T7-ruled default), `Fd_Lambert`, `Fd_Burley`,
  `energyCompensation`, plus the link-time feature-composition machinery
  (`IEnergyCompensationFeature`, `EnergyCompensationOn`/
  `EnergyCompensationOff`) and a named clearcoat extension-point comment for
  Task 21. Every function takes scalar/vector inputs only — no
  `ParameterBlock`, no bindless global, no `IMaterialShader` dependency.
  **`standard_pbr.slang`/`material.slang` are NOT modified** — per the
  ticket's own scope line ("this task is the MATH + MODULES + MECHANISM...
  do not rewrite the production material path here"), so the existing
  08/09 D17 gates stay byte-identical trivially (confirmed: full suite
  rerun below, `sample_08_gltf_viewer_headless` still passes unchanged).
- **`src/rx_material/tests/brdf_test_harness.h`** (new) — shared
  `slang::ISession` plumbing (mirrors `MaterialSystem::
  createMaterialSession()`/`compileMaterial()`, not shared with it — this
  codebase's own established per-file-duplicated-helper idiom) for single-
  module and two-module (link-time-composed) compute compiles with a real
  search path into `shaders/material/`.
- **`src/rx_material/tests/brdf_gpu_fixture.h`** (new) — skip-guarded
  windowed-device fixture + one shared "one input buffer, one output
  buffer, N-wide dispatch" helper.
- **`src/rx_material/tests/test_brdf_module_gpu.cpp`** (new) — standalone-
  compile test, the 7-point port-parity table, the Smith-correlated
  discrimination case, the f90 discrimination case.
- **`src/rx_material/tests/test_brdf_white_furnace_gpu.cpp`** (new) — the
  white-furnace energy-conservation test (Monte-Carlo `DFV_Multiscatter`
  port + `energyCompensation()` ON/OFF discrimination).
- **`src/rx_material/tests/test_brdf_spirv_link_composition_gpu.cpp`**
  (new) — the SPIR-V-absence proof for Slang link-time generic composition.
- **`src/rx_material/tests/CMakeLists.txt`** — new `rx_material_brdf_gpu_tests`
  binary; `find_program(spirv-dis)` with a `FATAL_ERROR` if absent.
- **`.github/workflows/ci.yml`** — `spirv-tools` apt package (both jobs —
  `find_program` runs at CMake-configure time on the same Linux host
  regardless of cross-compile target); `rx_material_brdf_gpu` added to the
  Wine-job ctest exclusion pattern (two of its three files build a real
  windowed `VkDevice`; the third shells out to the host's own `spirv-dis`
  via `std::system()`, which a Wine-hosted cross-compiled `.exe` cannot
  reliably do — excluded as one whole binary, matching every other GPU-
  backed binary's own exclusion precedent in that same regex).

## Ported-file provenance

All from `google/filament` **v1.75.0** (tag; commit
`0e58877c09afb1aacd09ff640f74d2adcd2a7e80`, Apache-2.0, whole-repo LICENSE)
— RC1's phase-wide pin. Re-fetched and re-read in full against this exact
commit this session (the Stage-1 gate matrix's own citations were against
main HEAD `721ec80`; content diffed identical for every function ported
below — no drift found).

| Filament source (pinned `0e58877c0`) | Our module |
|---|---|
| `shaders/src/surface_brdf.fs:54-79` — `D_GGX(roughness, NoH, h)` | `brdf.slang::D_GGX(alpha, NoH)` |
| `shaders/src/surface_brdf.fs:102-113` — `V_SmithGGXCorrelated(roughness, NoV, NoL)` | `brdf.slang::V_SmithGGXCorrelated(alpha, NoV, NoL)` |
| `shaders/src/surface_brdf.fs:145-157` — `F_Schlick` (three overloads) | `brdf.slang::F_Schlick` (matching three overloads) |
| `shaders/src/surface_brdf.fs:177-186` — `fresnel()`'s computed-f90 (`FILAMENT_QUALITY_HIGH`) dispatch | `brdf.slang::fresnelDefault(f0, VoH)` |
| `shaders/src/surface_brdf.fs:237-239` — `Fd_Lambert()` | `brdf.slang::Fd_Lambert()` |
| `shaders/src/surface_brdf.fs:241-247` — `Fd_Burley(roughness, NoV, NoL, LoH)` | `brdf.slang::Fd_Burley(alpha, NoV, NoL, LoH)` |
| `shaders/src/surface_shading_lit.fs:230-238` — `getEnergyCompensationPixelParams()`'s `1.0 + f0*(1.0/dfg.y - 1.0)` | `brdf.slang::energyCompensation(f0, dfgY)` |
| `shaders/src/surface_shading_model_standard.fs:135` — `color = Fd + Fr * pixel.energyCompensation` (the multiplicative contract) | Documented as `energyCompensation()`'s own calling contract; not itself wired into any production path this task |
| `libs/ibl/src/CubemapIBL.cpp:790-833` — `DFV_Multiscatter()` (+`hammersley()`, `hemisphereImportanceSampleDggx()`, `Visibility()`) | Test-local Monte-Carlo kernel in `test_brdf_white_furnace_gpu.cpp` (ported verbatim; `Visibility()` itself is not re-ported — the kernel calls `brdf.slang`'s own already-verified `V_SmithGGXCorrelated`, the identical formula) |
| `libs/ibl/include/ibl/utilities.h:44-53` — `hammersley(i, iN)` | Same test-local kernel |

## Per-row proof (matrix, T7-ruling-amended)

| Row | Disposition | Evidence |
|---|---|---|
| `D_GGX` | consume-now, port-parity verified | `test_brdf_module_gpu.cpp`, 7-point table, exact-tolerance (`epsilon(1e-4)`) match against an independent fp64 C++ transcription |
| `V_SmithGGXCorrelated` | consume-now, port-parity verified + discrimination | Same table; discrimination case swaps in the textbook SEPARABLE Smith form at P3 (alpha=1.0, NoV=0.1, NoL=0.2) — diverges by ~0.91, four orders of magnitude past the parity tolerance |
| `F_Schlick` (f90 convention) | **resolved: computed-f90 (`fresnelDefault`) ships as the new default; implicit-f90=1.0 kept as a named, tested fallback (`F_Schlick(f0,VoH)`)** — per the T7 per-ticket ruling | Both forms table-tested; discrimination case at P6 (f0=(0.01,0.01,0.01), VoH=0.02) — diverges by ~0.456. **Correction to the matrix's own "high-F0" framing**: re-derived directly against the pinned formula and found the OPPOSITE — `f90 = saturate(dot(f0,16.5))` saturates to exactly 1.0 (byte-identical to the implicit form) for any f0 with `sum(f0) >= ~0.0606`, true of the glTF/Disney 0.04 dielectric baseline and essentially every physically-plausible metal; the two forms only diverge for LOW total reflectance (e.g. a `KHR_materials_specular`-attenuated dielectric). Documented in `brdf.slang`'s own `fresnelDefault()` header comment |
| `Fd_Lambert` | consume-now, modularized | Table test, exact match to `1/PI` |
| `Fd_Burley` | **ruled: shipped alongside Lambert, exported and tested, not wired into any production material** (no dedicated Task 1 spec doc landed; the charter's own binding "Shader architecture" bullet already names Lambert as the flagship's lobe choice) | Table test, exact-tolerance match |
| Energy compensation | consume-now (this ticket's headline deliverable) | `energyCompensation(f0, dfgY)` takes `dfgY` as a plain parameter (no IBL/LUT dependency) per the matrix's own resolved calling contract; white-furnace test below |
| Clearcoat extension point | N/A for T7's own bar; module-boundary comment delivered | `brdf.slang`'s own "Extension point [Task 21...]" comment |
| Composable module layout | consume-now | `brdf.slang` compiles standalone (device-free test, `rx_shader::Compiler::compileFromFile`, zero entry points, `import`-free) |

## SPIR-V absence evidence (real, this session)

`spirv-dis` output on the two link-time-composed variants (temp files,
`/tmp/rx_brdf_energy_feature_{on,off}.{spv,dis.txt}`):

```
$ grep -c OpFDiv /tmp/rx_brdf_energy_feature_on.dis.txt
1
$ grep -n OpFDiv /tmp/rx_brdf_energy_feature_on.dis.txt
68:         %74 = OpFDiv %float %float_1 %27
$ grep -c OpFDiv /tmp/rx_brdf_energy_feature_off.dis.txt
0
$ wc -l /tmp/rx_brdf_energy_feature_{on,off}.dis.txt
  77 /tmp/rx_brdf_energy_feature_on.dis.txt
  64 /tmp/rx_brdf_energy_feature_off.dis.txt
$ ls -la /tmp/rx_brdf_energy_feature_{on,off}.spv
-rw-rw-r-- 1 ywadi ywadi 1252 ... rx_brdf_energy_feature_on.spv   (313 words)
-rw-rw-r-- 1 ywadi ywadi 1000 ... rx_brdf_energy_feature_off.spv  (250 words)
```

The ON variant's single `OpFDiv` is exactly `EnergyCompensationOn::apply()`'s
`1.0 / dfgY`; the OFF variant (linked against `EnergyCompensationOff`,
which returns `specular` unchanged and never reads `dfgY` at all) contains
**zero** division instructions anywhere in its compiled module — the
feature's code is provably absent, not merely dead-code-eliminated after
generation.

## White-furnace energy-conservation numbers (real GPU output, NVIDIA-equivalent formula, lavapipe run)

Monte-Carlo `DFV_Multiscatter` port, 16384 samples/point:

| Query | NoV | alpha | Ess (uncompensated, GPU) | compensated (GPU) |
|---|---|---|---|---|
| Q0 | 1.0 | 0.02 | ≈0.9996 | ≈1.000 |
| Q1 | 1.0 | 0.30 | 0.877392 | ≈1.000 |
| Q2 | 1.0 | 0.70 | 0.503748 | ≈1.000 |
| Q3 | 1.0 | 1.00 | 0.306884 | ≈1.000 |
| Q4 | 0.3 | 0.70 | 0.669238 | ≈1.000 |

(Uncompensated/Ess values captured directly from the deliberate
`energyCompensation() -> 1.0` mutation's own real failure output during the
revert-discrimination proof below — not synthetic. Independently
cross-checked this session against a from-scratch fp64 Python transcription
of the exact same formulas: 0.99963 / 0.87739 / 0.50375 / 0.30688 / 0.66924
— matches to within GPU fp32 noise.) Monotonic decrease with roughness
confirmed (Q0 > Q1 > Q2 > Q3); off-normal view (Q4) also shows a real,
non-trivial deficit. See `test_brdf_white_furnace_gpu.cpp`'s own header
comment for why "compensated ≈ 1" alone would be a tautological check, and
why the Ess-monotonicity assertions are the test's real, non-tautological
discriminating power.

## Revert-discrimination proofs (all performed for real this session, all restored to green)

1. **D_GGX parity** — mutated `1.0/kBrdfPi` → `2.0/kBrdfPi`: 7/7 table
   points failed at exactly 2x the expected value (e.g. `24.868 ==
   Approx(12.434)`). Reverted; suite green.
2. **f90 discriminator** — mutated `fresnelDefault()` to hardcode `f90 =
   1.0` (the implicit form): the discrimination assertion failed exactly
   as expected (`CHECK( 0 > 0.05 )`, the two forms became bit-identical).
   Reverted; suite green.
3. **White-furnace formula** — mutated `energyCompensation()` to
   unconditionally `return float3(1,1,1)`: 4/5 "compensated ≈ 1"
   assertions failed with the real Ess values (0.877392/0.503748/0.306884/
   0.669238 — the numbers tabled above), plus the ON-vs-OFF gap assertion.
   Reverted; suite green.
4. **SPIR-V link composition** — mutated the OFF impl module to link
   `EnergyCompensationOn` too (simulating a harness bug): `offDivCount`
   became `1` (matching ON), failing `CHECK(offDivCount == 0)` and the
   size-comparison sanity check. Reverted; suite green.

## Both-preset / both-driver verification (command tails)

**Lavapipe, new binary alone:**
```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a \
  ./build/linux-native/src/rx_material/tests/rx_material_brdf_gpu_tests --validate
...
[doctest] test cases:   6 |   6 passed | 0 failed | 0 skipped
[doctest] assertions: 131 | 131 passed | 0 failed |
[doctest] Status: SUCCESS!
```

**Lavapipe, full linux-native suite (regression check — existing gates
unaffected):**
```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a \
  ctest --test-dir build/linux-native --output-on-failure -j1
...
100% tests passed, 0 tests failed out of 32
Total Test time (real) =  81.58 sec
```
(32 = the prior 31-test baseline + this task's one new binary.)

**Real driver (NVIDIA GeForce RTX 2080, driver 580.82.07, default ICD —
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json`, `DISPLAY=:1`,
on-desktop, NICEd per the standing owner rule, non-interactive headless
compute dispatch only):**
```
$ DISPLAY=:1 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json nice -n 15 \
  ./build/linux-native/src/rx_material/tests/rx_material_brdf_gpu_tests --validate
...
[doctest] test cases:   6 |   6 passed | 0 failed | 0 skipped
[doctest] assertions: 131 | 131 passed | 0 failed |
[doctest] Status: SUCCESS!
```

**Windows-cross-zig: configure + build (both `find_program(spirv-dis)` and
the new target compile cleanly):**
```
$ cmake --preset windows-cross-zig            # spirv-dis found, no FATAL_ERROR
$ cmake --build build/windows-cross-zig --target rx_material_brdf_gpu_tests -j
[5/5] Linking CXX executable src/rx_material/tests/rx_material_brdf_gpu_tests.exe
```

**Wine ctest (CI's own exclusion pattern, confirming
`rx_material_brdf_gpu_tests` is correctly excluded and nothing else
regressed):**
```
$ xvfb-run -a ctest --test-dir build/windows-cross-zig -E \
  'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_material_brdf_gpu|rx_debug_ui_gpu|rx_frame_loop_gpu|sample' \
  --output-on-failure -j1
...
100% tests passed, 0 tests failed out of 14
Total Test time (real) = 161.25 sec
```
(14 = unchanged from the pre-existing baseline; confirms the new binary is
excluded, not silently skipped-and-passing.)

Every "[vulkan validation]" line observed across every run above matches
this codebase's own pre-existing, documented false-positive filter set
(`context.cpp`'s `debugCallback()`); zero unfiltered validation errors on
any run (`hasValidationErrors()` false throughout).

## Decisions made as implementer (not escalated — none met the owner-escalation bar)

- **Fd_Burley in/out**: shipped alongside Lambert, unused by production —
  see matrix row above.
- **f90 divergence condition**: corrected the matrix's own "high-F0"
  framing after re-deriving directly against the pinned formula (see
  matrix-row entry above and `brdf.slang`'s own header comment) — a
  factual correction to a research artifact, not a design decision.
- **Test target location**: a new sibling binary
  (`rx_material_brdf_gpu_tests`) rather than folding into
  `rx_material_gpu_tests` — these tests drive `slang::ISession` directly
  and need `rx::rhi::ComputePipelineCache` (Task 2), neither of which any
  existing file in that directory touches; MaterialSystem/ParameterBlock
  machinery is irrelevant to this module's own tests.
- **spirv-dis invocation mechanism**: shells out to the CMake-located
  binary via `std::system()` (test-only code) rather than linking
  libSPIRV-Tools — no existing CMake integration for that library exists
  in this repo, and the CLI tool is this codebase's own already-established
  manual-verification precedent (multiple header comments cite direct
  `spirv-dis` use); this task makes that precedent an automated CI check
  for the first time.

## Self-review

- **TDD**: iterated for real — the SPIR-V-absence test's own positive
  control (`onDivCount >= 1`) and the white-furnace test's Ess-monotonicity
  design were arrived at only after the naive "compensated ≈ 1 alone"
  framing was recognized as tautological during this task's own
  development (documented candidly in the test file's own header comment,
  not hidden).
- **No deferred fixes**: none found requiring deferral; the matrix's own
  "New gaps" (no compute-numerical-test-harness precedent, no scripted
  SPIR-V-inspection tooling) are both closed by this task's own delivery,
  not left open.
- **No AI attribution**: verified directly (`git show <both SHAs> | grep
  -iE "claude|anthropic|co-authored|generated"` — clean) in both commits'
  messages and diffs, not just assumed.
- **Commit scope**: two pathspec-scoped commits — `3eaaa0b` (module + tests
  + CMake) and `59b598e` (CI wiring) — matching this repo's own
  feat/ci-commit-split precedent (e.g. `aa7e3e1`/`2badf74`).
  `.superpowers/sdd/.../progress.md` was untouched by `git status` at every
  check this session (no concurrent-agent conflict observed) and is
  deliberately excluded from both commits per this task's own no-ledger-
  edits constraint.
- **Not pushed**: per the task's own instruction ("no push").

## Concerns for the coordinator

- None blocking. One forward note: Task 9's real DFG LUT will eventually
  replace this task's test-local Monte-Carlo `dfgY` source at production
  call sites — `energyCompensation()`'s own calling contract (plain
  parameter, no binding) already accommodates that without any signature
  change, per the matrix's own resolved cross-ticket dependency.
- `brdf.slang`'s `IEnergyCompensationFeature`/`EnergyCompensationOn`/
  `EnergyCompensationOff` are shipped as real, importable module surface
  (not test-only scaffolding) since Task 8 will need this exact link-time-
  composition pattern for its own permutation axes and can consume these
  two structs directly rather than reinventing them — flagged here in case
  Task 8's own implementer wants to extend rather than duplicate.
