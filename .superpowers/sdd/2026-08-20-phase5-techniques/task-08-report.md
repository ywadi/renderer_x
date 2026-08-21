# Task 8 report — StandardPBR rework onto the module architecture (issue #44)

Implementer round. Base: main `d8b8d46` (T7's SDD-records commit; tree
clean except SDD workspace files, not this task's). Order of authority
followed: rulings (`rulings-2026-08-20.md`, T7/T8/RC1/RC3) > plan (Task 8)
> gate matrix (`matrix-p5t08-standard-pbr-rework.md`) > ticket (#44).

## Status: COMPLETE

Every matrix row (as amended by the T8 per-ticket ruling) is satisfied.
Both presets build clean. Real-driver (NVIDIA RTX 2080, default ICD) and
lavapipe runs both pass with zero unfiltered validation errors. Full
ctest suites green: linux-native 32/32 (100%), windows-cross-zig/Wine
14/14 (100%, GPU/sample binaries correctly excluded per the existing CI
regex — this task added no new binary needing an exclusion-regex update).
Two independent revert-discrimination proofs performed for real this
session and recorded below, both restored to green.

## What shipped

- **`shaders/material/standard_pbr.slang`** (reworked) — the inline
  D_GGX/V_SmithGGXCorrelated/F_Schlick math is replaced with calls into
  Task 7's `brdf.slang` module; a new `computeDielectricF0F90()` helper
  derives the dielectric F0/F90 pair from `KHR_materials_ior`/
  `KHR_materials_specular` (kept in this file, not `brdf.slang`, per the
  gate matrix's own Open Question — these are glTF-extension-specific
  formulas, not general Filament-ported math); the module now
  `import brdf;` and declares `extern struct EnergyComp :
  IEnergyCompensationFeature;`, resolved at link time by whichever of the
  two new companion files `MaterialSystem` composes it against.
- **`shaders/material/energy_compensation_{off,on}.slang`** (new) — the
  two tiny companion modules that resolve `standard_pbr.slang`'s
  `EnergyComp` extern, extending Task 7's own
  `test_brdf_spirv_link_composition_gpu.cpp` proof mechanism into the real
  production `MaterialSystem::create()`/`compileMaterial()` pipeline.
- **`StandardPbrParams`** grows exactly FOUR new fields (per the T8 ruling
  — "no pre-added unused parameter fields", the mechanism's own minimum
  real cost, not a "future lobe"): `ior` (default 1.5), `specularFactor`
  (default 1.0), `specularColorFactorAndPad` (default (1,1,1,·)), and
  `dfgY` (the energy-compensation feature's own input — every current
  producer binds 1.0, the honest "no correction" neutral value, pending
  Task 9's baked DFG LUT). The other nine glTF-extension slots the ticket
  names (clearcoat/anisotropy/sheen/transmission/thickness/attenuation/
  dispersion/iridescence/diffuseTransmission) are **declared-but-gated via
  a documented extension-point comment only** (matrix's Open Question,
  recommendation (b)) — no struct fields for them yet; their owning tasks
  add fields when they land.
- **`src/rx_material/material_system.h`** — `kSpecializationEnergyCompensation`
  (`0x1u`), the first REAL value ever assigned to a `specializationBits`
  bit position in this codebase (T2's own compute-pipeline work had
  already confirmed this field was dead/hardcoded-0 everywhere).
- **`src/rx_material/material_system.cpp`** — `MaterialSystem::create()`
  now loads the two companion modules unconditionally (cheap, two
  one-line files); `compileMaterial()` content-sniffs a material's source
  for `IEnergyCompensationFeature` and, when present, compiles a SECOND
  (vertex, fragment) SPIR-V pair (link-time composed against
  `energy_compensation_on.slang`) alongside the base (off) pair — both
  eagerly, at `loadMaterial()` time, sharing ONE reflection pass (the
  `EnergyComp` choice never changes `StandardPbrParams`' own field
  shape). `getPipeline()` selects between the two pairs by
  `req.specializationBits & kSpecializationEnergyCompensation`,
  falling back to the base pair for every material that doesn't declare
  the extern (byte-identical behavior for Unlit and every test fixture).
  `reloadChanged()` mirrors the same two-pass compile against its own
  fresh per-reload session. The `compose→link→codegen→vkCreateShaderModule`
  tail (previously typed out once inline) is factored into
  `linkAndCreateShaderModules()`, reused by both passes.
- **`samples/{06_materials,08_gltf_viewer,09_scene}/CMakeLists.txt`** —
  deploy `brdf.slang` + the two new companion files alongside
  `material.slang`/`forward_entry.slang`/`standard_pbr.slang` (06 needed
  this fix too even though it never loads `standard_pbr.slang` at all —
  see the revert-discrimination-adjacent finding below).
- **`samples/{08_gltf_viewer,09_scene}/main.cpp`** — bind the four new
  `StandardPbrParams` fields at their glTF-neutral defaults (ior=1.5,
  specularFactor=1.0, specularColorFactorAndPad=(1,1,1,0), dfgY=1.0) at
  every StandardPbr material-setup call site (three total). **Load-bearing,
  not decorative**: a zero-filled `StandardPbrParams` blob (the pre-Task-8
  behavior for any unset field) would trip the `ior<=0` special case
  (Fresnel forced to exactly 1.0 at every angle) for every real asset —
  this was caught directly by the test suite before it ever reached a
  sample.
- **Tests** — see the dedicated test commit; summarized in its own section
  below.

## Ported-file / formula provenance

`brdf.slang`'s consumed functions were already ported+cited by Task 7
(`0e58877c0`, `google/filament` v1.75.0) — unchanged here, only newly
*called* from `standard_pbr.slang`. `computeDielectricF0F90()` itself is
**not a Filament port** (Filament has no `KHR_materials_ior`/`_specular`
equivalent — its own `reflectance`/`ior` material properties are a
different model with different defaults, per the gate matrix's Open
Question): it is derived directly from the two Khronos extension
`README.md` files' own quoted formulas (fetched by the gate matrix
session, re-derived independently for this implementation, cited in the
function's own header comment):

- `KHR_materials_ior`: `dielectric_f0 = ((ior-1)/(ior+1))^2`; `ior=0`
  forces the Fresnel term to exactly 1.0 at every angle.
- `KHR_materials_specular`: `dielectric_f0 = min(0.04*specularColor,
  1.0)*specular`; `dielectric_f90 = specular` (not 1.0); "the metal BRDF
  is not affected by the parameter." Interaction rule: combined with
  `KHR_materials_ior`, the `0.04` constant is replaced by the IOR-derived
  value.

## Regression-guard proof (matrix's own acceptance criterion, ior row)

**Claim**: at the glTF-default values every current asset binds (ior=1.5,
specularFactor=1.0, specularColorFactor=(1,1,1) — neither extension is
parsed from glTF content yet, ruling RC3), the new derivation reproduces
the pre-Task-8 hardcoded `float3(0.04,0.04,0.04)` + implicit-f90=1.0
Fresnel byte-identically.

**Verified empirically, not just algebraically**: both D17 pixel gates
(sample 08's `loading_state.png`/`loaded_scene.png`, sample 09's
`grid_scene.png`) were re-run on lavapipe against the EXISTING committed
references, post-rework, with **zero** regeneration:

```
sample_08_gltf_viewer: D17 loading_state gate: failingPixels=0/65536 (0.0000%) pass=true
sample_08_gltf_viewer: D17 loaded_scene gate:  failingPixels=0/65536 (0.0000%) pass=true
sample_09_scene:       D17 grid_scene gate:    failingPixels=0/65536 (0.0000%) pass=true
```

**Gates 08/09 stay identical — neither was regenerated.** No before/after
capture pair is attached for this reason: there is no "after" that
differs from the committed "before." This is the matrix's own required
outcome for the default-value case, not a missed opportunity — the
`brdf.slang` module refactor (D_GGX/V_SmithGGXCorrelated algebraically
identical to the pre-T8 inline forms, hand-verified during T7 and
re-confirmed here) and the ior/specular derivation's own byte-identical-
at-defaults design (T7 ruling's computed-f90 default also collapses to
the implicit-f90 form for every current dielectric/metal, per
`brdf.slang`'s own `fresnelDefault` header comment) combine to leave
every existing asset's rendered output untouched.

## The visible change (not in the shipped samples — by design)

The CRITICAL bar named "the first visible material-quality change of the
phase" — this task delivers the MECHANISM (energy compensation, real
`specializationBits`) and it IS visibly, measurably different when
exercised, but **not through either shipped sample's default path**,
deliberately: `dfgY` (the multi-scatter energy-compensation input) has no
real per-pixel source until Task 9's baked DFG LUT lands; wiring the ON
variant into a shipped sample today would mean picking an arbitrary
constant `dfgY`, which is exactly the kind of unbacked, non-physical
default this project's production-quality bar rejects. The mechanism's
real, non-trivial effect is instead demonstrated and value-asserted by
the new `energy-compensation permutation mechanism` TEST_CASE
(`test_standard_pbr_unlit.cpp`): at `metallic=1.0, roughness=1.0,
dfgY=0.5` (a physically-plausible "significant multi-scatter loss"
probe), the ON variant reads measurably brighter than OFF
(`CHECK(pixelOn.r > pixelOff.r)`, oracle-verified exact values both
sides) — real, GPU-rendered, non-tautological evidence the mechanism
works, available for the coordinator/owner to re-run
(`rx_material_gpu_tests --test-case="*energy-compensation permutation*"`)
without needing a static image. A follow-up task (Task 9/10) wiring a
real `dfgY` source will be the point this becomes visible in a shipped
sample's own committed reference.

## Per-row proof (matrix, T8-ruling-amended)

| Matrix row | Disposition | Evidence |
|---|---|---|
| `KHR_materials_ior` | consume-now | 3 new TEST_CASEs: default(1.5)-vs-1.0 discrimination (oracle-checked, roughness=0.28 sweet spot — see "roughness pitfall" below), `ior=0` special case (distinct from the ordinary formula at a non-1.0 specularFactor, not a coincidental clamp), the regression-guard proof above. |
| `KHR_materials_specular` | consume-now | `specular=0.0` fully-diffuse collapse; specularColorFactor's own two-axis (color × strength) discrimination test; metal-exclusion zero-delta test. |
| `KHR_materials_emissive_strength` | already consume-now, verify unaffected | Unaffected — `emissiveFactorAndPad`'s own CPU-side pre-multiply untouched by this rework; every existing emissive-strength assertion in the pre-Task-8 suite still passes unchanged. |
| Feature-permutation mechanism | built, the ticket's own core deliverable | `kSpecializationEnergyCompensation` is real: (a) zero-feature materials (every material besides `standard_pbr.slang`) produce IDENTICAL SPIR-V regardless of the bit — proven by construction (they never get a second pass at all); (b) SPIR-V presence/absence proof on the PRODUCTION path (new `rx_material_brdf_gpu_tests` TEST_CASE) — ON's fragment SPIR-V has strictly more division instructions (measured: 5 vs 4) and is measurably larger (measured: 3251 vs 3207 words) than OFF's; (c) turning ON does not perturb OFF's own cache entry — `pipelineOffSecond == pipelineOffFirst` (same `VkPipeline` handle, not merely equal-by-value). |
| Declared-but-gated slots (9 future lobes) | genuinely declared-but-gated, mechanism only | No struct fields added for any of the nine. Documented extension-point comments: two (clearcoat/anisotropy) already existed pre-Task-8, in `brdf.slang`'s own Task 21 comment; the other seven (transmission/volume/sheen/iridescence/dispersion/diffuseTransmission/emissive-strength-beyond-current) were added to `standard_pbr.slang` in a review fix-round this session (task-08-review.md finding 1) — matching D13/D7's own "future task lands here" idiom. |
| Energy conservation preserved through the rework | consume-now | New parameterized white-furnace pass (`test_standard_pbr_energy_compensation_gpu.cpp`) exercises `computeDielectricF0F90()`'s OWN derivation (not brdf.slang's bare F0=1 constant) across `(ior, specularFactor)` combinations reaching F0≈1 via BOTH the `ior<=0` special case (Q0: `f0=1, ess=0.988382, compensated=1` at low roughness; Q1: `f0=1, ess=0.427034, compensated=1` at HIGH roughness AND a non-1.0 `specularFactor=0.3` — both exact-cancel despite very different `ess`) AND the ordinary high-ior limit (Q2, `ior=1000`: `f0=0.996008, ess=0.427034, compensated=0.997713`, within the epsilon-0.02 tolerance the smaller-than-1 `f0` predicts) — measured, not estimated. The glTF-default low-F0 case (Q3, `ior=1.5`: `f0=0.04, ess=0.427034, compensated=0.449952`) does NOT restore near 1 (discrimination baseline, `|compensated-1|=0.550048 > 0.05`). |
| Variant discrimination cost claim | consume-now (Tracy zone) | `getPipeline()`'s existing `RX_ZONE` macro already wraps the whole call including the (now real) cache-key lookup; no new per-frame cost for the zero-feature path — the second compile pass runs at `loadMaterial()` (asset-load time), never in `getPipeline()`'s own steady-state cache-hit path, so the claim's actual binding scope (draw-time cost) is unaffected by construction, not merely measured-and-found-equal. See "Concerns" below for the coordinator on whether a DEDICATED new Tracy zone name is still wanted despite this. |

## SPIR-V absence/presence evidence (real, this session, production path)

Composed the REAL shipped `material.slang`/`forward_entry.slang`/
`standard_pbr.slang`/`brdf.slang` files (not a synthetic fixture) against
each companion variant file, extracted the fragment entry point's SPIR-V,
disassembled via `spirv-dis`:

```
OFF OpFDiv count: 4   (V_SmithGGXCorrelated's own division, the ior/specular
                        derivation's (ior-1)/(ior+1), KHR_texture_transform
                        UV-scale reciprocals -- legitimate, pre-existing
                        divisions unrelated to this feature)
ON  OpFDiv count: 5   (exactly one more -- energyCompensation()'s own
                        `1.0 / dfgY`)
OFF code size: 3207 words
ON  code size: 3251 words
```

T7's own standalone `brdf.slang` proof could assert a raw `offDivCount ==
0` (its whole compute kernel's only division came from the feature
itself); the real production fragment shader already has other
legitimate divisions, so the correct generalization is the delta
(`onDivCount > offDivCount`) — documented explicitly in the test's own
header comment as the reason this differs from T7's exact-absence form.

## Revert-discrimination proofs (both performed for real this session, both restored to green)

**Proof 1 — the `ior<=0` special case.** Mutated
`computeDielectricF0F90()`'s guard from `if (ior <= 0.0)` to `if (false)`
(standard_pbr.slang is read at runtime, no rebuild needed — just re-ran
the already-built test binary):

```
before mutation: StandardPBR Task 8: KHR_materials_ior ior=0 ... -- 1 passed, 61/61 assertions
after mutation:  StandardPBR Task 8: KHR_materials_ior ior=0 ... -- 1 FAILED, 58/61 assertions
                   (all 3 pixel-channel CHECKs against the oracle's F0=F90=1
                    prediction fail, since the mutated code now falls
                    through to the ordinary ((ior-1)/(ior+1))^2 formula)
```
Restored; re-ran: 1 passed, 61/61.

**Proof 2 — the SPIR-V delta test's own OFF/ON distinction.** Mutated
`energy_compensation_off.slang`'s own export to resolve
`EnergyCompensationOn` instead of `Off` (collapsing the two variants):

```
before mutation: onDivCount=5, offDivCount=4 -- CHECK(onDivCount > offDivCount) PASSES
after mutation:  onDivCount=5, offDivCount=5 -- CHECK(onDivCount > offDivCount) FAILS
                 (CHECK( 5 >  5 ) is NOT correct!); code-size CHECK also
                 fails identically (3251 == 3251 -- OFF now compiles to
                 exactly ON's own SPIR-V, since both resolve to
                 EnergyCompensationOn under the mutation)
```
Restored; re-ran: both CHECKs pass.

## A real regression found and fixed mid-round (recorded, not swept under)

`MaterialSystem::create()` now loads the two companion modules
UNCONDITIONALLY (every `MaterialSystem` instance, regardless of whether
its own materials reference the feature) — this broke
`sample_06_materials_headless` (`could not read shared shader file
'.../material_shaders/energy_compensation_off.slang'`), since that
sample's own `checker.slang`/`rim.slang` test materials never reference
`standard_pbr.slang`/`IEnergyCompensationFeature` at all but still build
a `MaterialSystem` against their own deployed `material_shaders/`
directory, which didn't have the two new files. Caught by the FULL
(un-scoped) ctest suite run, not by `rx_material`'s own tests (which use
the real `RX_MATERIAL_SHADER_DIR` directly, unaffected). Fixed by adding
`brdf.slang` + the two companion files to `samples/06_materials/
CMakeLists.txt`'s own shader-deploy list (same fix already applied to
08/09 proactively). Full suite re-run clean after the fix (32/32).

## Both-preset / both-driver verification (command tails)

```
$ cmake --build --preset linux-native
$ xvfb-run -a ctest --preset linux-native --output-on-failure
100% tests passed, 0 tests failed out of 32

$ cmake --build --preset windows-cross-zig
[24/24] ... (clean build, all 24 rebuilt targets link)
$ ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_material_brdf_gpu|rx_debug_ui_gpu|rx_frame_loop_gpu|sample' --output-on-failure
100% tests passed, 0 tests failed out of 14

# Real-driver (NVIDIA GeForce RTX 2080, default ICD -- vulkaninfo-confirmed
# this session) -- default ICD on this dev machine IS the real GPU, no
# VK_ICD_FILENAMES override needed:
$ ./build/linux-native/src/rx_material/tests/rx_material_gpu_tests --validate
[doctest] test cases: 65 | 65 passed | 0 failed
[doctest] assertions: 3293 | 3293 passed | 0 failed

$ ./build/linux-native/src/rx_material/tests/rx_material_brdf_gpu_tests --validate
[doctest] test cases: 8 | 8 passed | 0 failed
[doctest] assertions: 167 | 167 passed | 0 failed

# lavapipe (forced VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json):
same two binaries, same 65/65 and 8/8, zero failures.

# Zero unfiltered validation errors, every run (grep -a "Validation Error"
# | grep -v "known false positive" -> 0 matches, every log).
```

## Decisions made as implementer (not escalated — the T8 ruling and matrix's own Open Questions already resolved every contested question)

1. **Eager dual-compile at `loadMaterial()` time, not lazy per-`getPipeline()`-call compilation.** The ticket frames this as "the specialization axis made real"; the alternative (compile the ON variant lazily, on first `getPipeline()` request with the bit set) would need `getPipeline()` itself to gain Slang-compile capability it doesn't have today — a much larger, riskier change for a mechanism with exactly ONE real axis today. Eager-at-load matches this file's own existing design comment ("all of this happens here, eagerly, not lazily deferred to getPipeline()") and costs nothing at steady-state (only `loadMaterial()`, an asset-load-time call, pays the second compile).
2. **Content-sniffing (`source->find("IEnergyCompensationFeature")`), not a filename check or a link-failure-based detection.** Robust to a future second material also wanting this axis (unlike a `path.stem() == "standard_pbr"` check) and simpler/less fragile than compile-and-catch-failure branching.
3. **`dfgY` as a real `StandardPbrParams` field, not a compile-time constant passed to `.apply()`.** A constant `1.0` would let Slang's optimizer legally fold `energyCompensation()`'s own division away entirely for a constant-1.0 `dfgY`, undermining both the SPIR-V-presence proof (On's division might not even survive optimization) and the value-assertion test (a constant-folded no-op can't be value-asserted as "brighter than OFF"). A real field is the honest, minimum-necessary cost the T8 ruling's own "the mechanism, not the fields" carve-out anticipates.
4. **`makeDefaultStandardPbrBlob()`'s pre-existing `metallicFactor=1.0` default is a real gotcha this task's own new dielectric-focused tests had to route around explicitly** (three of six new TEST_CASEs needed an explicit `metallicFactor=0.0` override) — not a bug in that pre-existing default (every pre-Task-8 test already accounted for it), just a sharp edge newly-written ior/specular tests must know about; documented in-line at each call site for the next implementer.

## Roughness-choice pitfall (recorded for the next implementer touching this file)

`kMinRoughness` (0.045) is UNUSABLE for an exact oracle-vs-render pixel
comparison in this test rig: this rig's own default "flat" normal-map
texture is byte `(128,128,255)`, which decodes to tangent-space
`(0.502,0.502,~1.0)` — NOT an exact `(0,0,1)`. At ordinary roughness this
sub-milliradian tilt is negligible; at `kMinRoughness`, `D_GGX`'s
near-singular peak amplifies it into a >8x error (measured: idealized
776/255-saturating vs actual 93/255). `roughness=0.28` was chosen instead
for every new low-roughness discrimination test — low enough that the
F0-proportional specular lobe dominates the pixel, high enough that the
same normal-map imprecision stays a measured ~9% bias, absorbed by a
documented (not silently loosened) `near8` margin of 22, with every
discrimination `CHECK` requiring ≥40 levels of separation specifically so
that bias can never itself explain a false pass.

## Self-review

- [x] Every matrix row addressed with a concrete, cited proof (table
      above), not a bare "done."
- [x] Rulings followed in full: T8 (mechanism now, no pre-added unused
      lobe fields — 4 mechanism-cost fields added, 9 lobe fields NOT
      added), T7 context (IEnergyCompensationFeature extended into
      production, not reimplemented), RC3 (ior/specular importer
      plumbing NOT touched — `rx_asset`/`import_gltf.cpp` untouched by
      this round, confirmed via `git status`).
- [x] Byte-source invariant: every new `.slang` file lives under
      `shaders/material/`, deployed via the same tracked
      `add_custom_command` OUTPUT/DEPENDS mechanism every sibling file
      already uses (no inline string-literal shaders introduced).
- [x] No AI attribution anywhere in the two commits (checked directly,
      not just via a template default).
- [x] `git status` reviewed before every stage/commit — the pre-existing
      `progress.md` modification (present before this round started, not
      this task's to touch) was excluded from both commits via explicit
      pathspecs.
- [x] TDD in substance: every new TEST_CASE was run RED at least once
      during development (the `metallicFactor` blob bug, the
      `kMinRoughness` pitfall) before being made to pass correctly — not
      written-then-immediately-green.

## Concerns for the coordinator

1. **Variant-cost Tracy zone** (matrix row 5): I did not add a NEW,
   dedicated Tracy zone/counter for "zero-feature-material draw-path
   cost specifically" — `getPipeline()`'s existing `RX_ZONE` already
   covers the call, and this task's own design keeps the SECOND compile
   pass entirely out of the draw-time path (it runs once, at
   `loadMaterial()`), so there genuinely is no new steady-state cost to
   isolate. If the coordinator wants a literal before/after Tracy capture
   number regardless (not just an architectural argument), that is a
   small follow-up, not a gap in THIS task's own correctness.
2. **`dfgY`'s eventual real source is explicitly Task 9's job**, not
   ambiguous, but flagging directly: until that lands, the
   energy-compensation-ON variant is real, tested, and correct, but
   inert in every shipped sample (bound to the neutral 1.0). This is the
   intended, ruling-compliant scope boundary, not a partial delivery —
   restating it here so it doesn't read as an oversight when Task 9
   starts.
3. **No before/after PNG capture is attached** — see "The visible change"
   section above for why (gates 08/09 are correctly byte-identical; the
   real, working difference lives in a GPU test assertion, not a static
   image, until Task 9 supplies a real `dfgY`). Happy to produce a
   demonstration render on request if the owner wants one sooner.
