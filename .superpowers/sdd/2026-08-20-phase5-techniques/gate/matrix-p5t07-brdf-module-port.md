# Completeness matrix — ticket #43: [P5 T07] Filament BRDF module port (Slang)

**Plan task:** Task 7, "Filament BRDF module port (Slang)"
(`docs/superpowers/plans/2026-08-20-phase5-techniques.md:314-340`), Stage 1
(Core PBR + IBL).

**Binding sources:** the techniques-phase charter
(`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:364-458`)
— reference sources, shader architecture, priority order (1)
"Filament-quality GGX PBR"; D28 (fixed-function pipeline-state axis,
`docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md:492-512`)
and D8 (Phase-3, "Parameters vs. specialization split at the API level",
`docs/superpowers/specs/2026-08-10-phase3-render-graph-materials-design.md:190-199`)
— the two decisions the ticket cites as the "prepaid seams" for feature
permutation (T8's mechanism, not this ticket's, but this ticket's module
boundaries must not foreclose it). Global Constraints (plan:55-111):
real-GPU verification, reference-vs-ground-truth discipline (no
render-success-only gates), content-scale-past-capacity testing,
performance-as-exit-criterion.

**Ticket body (`gh issue view 43`):** BRDF port — D_GGX, height-correlated
Smith visibility, Schlick Fresnel, energy compensation for
single-scattering (DFG-based multi-scattering), diffuse lobe
(Lambert/Burley per spec ruling); composable Slang modules
(`shaders/material/brdf.slang` + siblings); acceptance sketch names
port-parity tests, a white-furnace energy test, and standalone
compile-through-existing-Slang-path with existing gates unaffected.

**Sources consulted (in-repo, read in full this session):**
`shaders/material/material.slang`, `shaders/material/standard_pbr.slang`,
`shaders/material/forward_entry.slang` (current shipped BRDF baseline —
D_GGX/V_SmithGGXCorrelated/F_Schlick already present, unported, no
energy-compensation term); `src/rx_material/include/rx_material/
material_system.h` (`PipelineRequest::specializationBits`, D28's
`MaterialFixedFunctionState`); `src/rx_material/material_system.cpp`
(`getPipeline()`, `PipelineKey`, the attachment-free-signature rejection
at :1930-1936); `docs/superpowers/specs/2026-08-10-phase3-render-graph-
materials-design.md` D6-D9 (link-time-specialization contract, hot-reload
invalidation); `docs/superpowers/specs/2026-08-11-phase4-scene-assets-
design.md` D22, D28 (materials/alphaMode, fixed-function axis).

**Sources consulted (external, fetched 2026-08-20):**
- `github.com/google/filament` — repo metadata (`gh api repos/google/
  filament`): default branch `main`, license SPDX `Apache-2.0`, HEAD
  commit **`721ec800093de984cbee155e459298b6b2dbb855`** (committer date
  2026-08-20T07:34:01Z, i.e. same-day HEAD at research time). Latest
  tagged release **`v1.75.0`** (published 2026-08-04), built from a
  separate `release` branch (`target_commitish: "release"`, per `gh api
  repos/google/filament/releases/latest`) — Filament tags releases off a
  branch distinct from `main`; the pin recommendation below addresses
  this explicitly.
- `shaders/src/surface_brdf.fs` (fetched via `gh api repos/google/
  filament/contents/...?ref=main`, full content read): contains
  `D_GGX`, `D_GGX_Anisotropic`, `D_Charlie`, `V_SmithGGXCorrelated`
  (cites Heitz 2014, "Understanding the Masking-Shadowing Function in
  Microfacet-Based BRDFs"), `V_SmithGGXCorrelated_Fast`,
  `V_SmithGGXCorrelated_Anisotropic`, `V_Kelemen`, `V_Neubelt`,
  `F_Schlick` (three overloads — with/without explicit `f90`), the
  `distribution()`/`visibility()`/`fresnel()` dispatch functions gated by
  `BRDF_SPECULAR_D/V/F` macros, `Fd_Lambert()`, `Fd_Burley()` (cites
  Burley 2012), `Fd_Wrap()`.
- `shaders/src/surface_shading_lit.fs` (fetched, full content read for
  the energy-compensation section): `getEnergyCompensationPixelParams()`
  — `pixel.dfg = prefilteredDFG(pixel.perceptualRoughness, shading_NoV)`
  (a LUT texture sample, not a per-pixel closed-form), then
  `pixel.energyCompensation = 1.0 + pixel.f0 * (1.0 / pixel.dfg.y - 1.0)`
  for non-cloth shading models (comment cites "Multiple-Scattering
  Microfacet BSDFs with the Smith Model" — Heitz/Hery/Misty 2016,
  Fdez-Agüera's DFG-based derivation is the same family). Cloth model:
  `energyCompensation = vec3(1.0)` (no compensation).
- `shaders/src/surface_light_indirect.fs` (fetched): `PrefilteredDFG_LUT()`
  — `textureLod(sampler0_iblDFG, vec2(NoV, lod), 0.0)`, comment: *"coord
  = sqrt(linear_roughness), which is the mapping used by cmgen"* — the
  DFG term is baked offline/at load time into a 2D LUT, sampled at
  runtime, never evaluated analytically per-pixel.
- `libs/ibl/src/CubemapIBL.cpp` (fetched, header + `DFG()`/`brdf()`
  functions read): license header confirmed **Apache License 2.0,
  "Copyright (C) 2015 The Android Open Source Project"** (the literal
  boilerplate every `libs/ibl` and `shaders/src` file carries — spot-
  checked on this file, consistent with the repo-level SPDX field).
  `CubemapIBL::DFG(js, dst, multiscatter, cloth)` selects between
  `DFV`/`DFV_Multiscatter` importance-sampling integrators — the
  **discrimination mechanism** for T7's white-furnace ON/OFF test: the
  `multiscatter` bool is a real, named toggle in Filament's own port
  source, not an invented axis.
- Filament GitHub issues search (`gh api search/code`, `WebSearch`):
  Slang issues #3044 ("Support for SPIR-V/Vulkan specialization
  constants") and #5725 ("Specialization constants need to be supported
  on all backends") surfaced as open/recent — noted under Open Questions
  below (T8-adjacent, recorded here since T7's module boundary choices
  affect it).
- Slang docs (`docs.shader-slang.org`, WebSearch digest): `[vk::
  constant_id(N)] const T x = default;` is real, documented Slang syntax
  for Vulkan specialization constants — confirms the mechanism exists in
  the toolchain, independent of whether it is mature/portable enough to
  rely on (see Open Questions).

---

## The matrix

| BRDF term | Filament source (pinned commit `721ec80`, `shaders/src/surface_brdf.fs` unless noted) | Current shipped state (`standard_pbr.slang`) | Disposition | Proposed acceptance criterion |
|---|---|---|---|---|
| `D_GGX` (normal distribution) | `D_GGX(roughness, NoH, h)` — numerically-stable form (`k = min(roughness/(oneMinusNoHSquared + a*a), 453.5)`, `d = k*k/PI`), algebraically identical to the textbook Trowbridge-Reitz/GGX form `alpha^2 / (PI * (NoH^2*(alpha^2-1)+1)^2)` — verified by hand-expansion this session (both reduce to the same `1 - NoH^2*(1-alpha^2)` inner term). | Present, textbook form (`standard_pbr.slang:200-202`): `denomD = NdotH^2*(alpha2-1)+1; D = alpha2/(pi*denomD*denomD)`, with `alpha2 = (roughness^2)^2` — algebraically matches Filament's `a2 = roughness*roughness` convention where Filament's own "roughness" parameter is already the perceptual-roughness-squared `alpha`. | consume-now, port-parity re-verify | Compute-shader table test: for a grid of (NoV, NoL, roughness, F0) points, `D_GGX` output matches a value independently computed from Filament's own numerically-stable formula (not the textbook rearrangement) to a tight, documented tolerance (fp32 rounding-only, not an approximation gap — Filament's own comment explains its rearrangement is a *precision* improvement over the textbook form for `mediump`, not a different function). |
| `V_SmithGGXCorrelated` (height-correlated visibility) | `V_SmithGGXCorrelated(roughness, NoV, NoL)` — `lambdaV = NoL*sqrt((NoV-a2*NoV)*NoV+a2)`, `lambdaL = NoV*sqrt((NoL-a2*NoL)*NoL+a2)`, `v = 0.5/(lambdaV+lambdaL)` guarded by `PREVENT_DIV0`. Cites Heitz 2014. | Present (`standard_pbr.slang:207-209`), algebraically identical expansion (`NoV*NoV*(1-alpha2)+alpha2` vs Filament's `(NoV-a2*NoV)*NoV+a2` — same polynomial), divide-by-zero guarded by `max(lambdaV+lambdaL, 1e-5)` (a fixed epsilon, not Filament's `PREVENT_DIV0`'s fp16-`nextafter`-derived constant — irrelevant at fp32, this project has no mediump/fp16 target). | consume-now, port-parity re-verify | Same table-test harness as `D_GGX`, exact-tolerance assert. Discrimination: swap in the UN-height-correlated (separable) Smith form at one table point and confirm the test fails — proves the harness actually distinguishes the correlated term, not just "some visibility function ran." |
| `F_Schlick` (Fresnel) | THREE overloads: `F_Schlick(f0, VoH)` (implicit `f90=1.0`, the `FILAMENT_QUALITY_LOW` path); `F_Schlick(f0, f90, VoH)` (explicit f90); default (`FILAMENT_QUALITY >= HIGH`) branch in `fresnel()` computes `f90 = saturate(dot(f0, vec3(50.0*0.33)))` ≈ `saturate(dot(f0, 16.5))` — a computed, non-unity f90 driven by f0's own magnitude (this raises the grazing-angle reflectance above the base `F_Schlick` for high-f0/metallic surfaces). | Present (`standard_pbr.slang:211-213`) as the IMPLICIT-`f90=1.0` overload ONLY: `fresnel = F0 + (1-F0)*pow(1-VdotH,5)` — this is Filament's `FILAMENT_QUALITY_LOW` simplified path, **not** its default/HIGH-quality path. | **partial port — real discrepancy, not yet ported** | Decision needed (see Open Questions): does T7 port the computed-`f90` HIGH-quality Fresnel, or standardize on the simpler implicit-`f90=1.0` form already shipped? Whichever is chosen, a table test pins the EXACT formula (not "a Fresnel-shaped curve") at the same (NoV,NoL,roughness,F0) grid, with a discrimination case at grazing angle (VoH→0) where the two forms diverge measurably for high-F0 inputs. |
| Diffuse lobe: `Fd_Lambert` | `1.0/PI`. | Present as `diffuseColor/kPi` inline (`standard_pbr.slang:216`) — not yet a named, importable `Fd_Lambert()` function in a `brdf.slang` module. | consume-now (already correct value; needs modularization) | Unit test: `Fd_Lambert()` (post-port, in `brdf.slang`) returns exactly `1/PI`; StandardPBR's diffuse term composes it identically to the pre-port inline value (byte-identical regression on existing 08/09 gates, per the ticket's own "existing material gates unaffected" acceptance line). |
| Diffuse lobe: `Fd_Burley` | `Fd_Burley(roughness, NoV, NoL, LoH)` — retro-reflective grazing-angle term, cites Burley 2012 ("Physically-Based Shading at Disney"). | Absent. | **Spec ruling required** (ticket text: "plus the diffuse lobe (Lambert/Burley per spec ruling)" — explicitly deferred to Task 1's spec, not this matrix's call to make) | If ruled IN: table test against Filament's own `Fd_Burley` formula, discrimination case at low-NoL grazing angle where Burley and Lambert diverge measurably. If ruled OUT (Lambert-only, matching StandardPBR's existing choice and its own header comment's citation of Filament's *documentation* preferring Lambert — "delivers results close enough to more complex models"): `brdf.slang` still exposes `Fd_Burley()` as an unused, tested module function per the "composable module" architecture the ticket names, OR is explicitly not ported with a recorded rationale — either is acceptable, but silence is not (a future clearcoat/sheen consumer may want Burley's grazing term independently of StandardPBR's own choice). |
| **Energy compensation (single-scattering, DFG-based multi-scatter)** | `getEnergyCompensationPixelParams()` (`surface_shading_lit.fs`): `pixel.dfg = prefilteredDFG(perceptualRoughness, NoV)` (LUT sample — **depends on Task 9's DFG LUT bake**, see Open Questions/cross-ticket dependency below), then `energyCompensation = 1.0 + f0*(1.0/dfg.y - 1.0)`. `CubemapIBL::DFG()`'s `multiscatter` bool (`libs/ibl/src/CubemapIBL.cpp:1008`) is the toggle between `DFV` (single-scatter-only, `dfg.y` closer to the true single-scatter albedo) and `DFV_Multiscatter` (the compensated integral) — this is the concrete ON/OFF the white-furnace discrimination test needs. | **Absent entirely** — the ticket's own text names this as "the charter's explicit bar above 'basic GGX.'" | consume-now (this ticket's headline deliverable) | **Genuine sequencing dependency, not a T7-internal decision:** the runtime energy-compensation formula above is a straight-line application of Filament's shipped code, BUT it consumes `dfg.y`, a value this ticket has NO source for until Task 9's DFG LUT exists (Task 9, ticket #45, Stage 1, sequenced AFTER T7 in the plan's own numbering but the DEPENDENCY runs the other direction at the data level). T7's own acceptance sketch text ("integrated response at F0=1 ≈ 1 across roughness... discrimination") implies T7's white-furnace test computes `dfg.y` itself analytically/via its OWN compute-harness integration (a Monte-Carlo or closed-form DFG evaluation local to the test), NOT by consuming Task 9's baked LUT — this must be true for T7 to close standalone per the ticket's own "modules compile standalone" acceptance line. Proposed acceptance criterion: T7's compute test harness evaluates the DFG integral itself (small, test-local importance-sampling kernel, following `CubemapIBL::DFV`/`DFV_Multiscatter`'s own importance-sampling structure — `hammersley`-distributed samples, `VisibilityAshikhmin`-style accumulation) rather than depending on Task 9's production LUT; `brdf.slang`'s `energyCompensation()` function itself takes `dfg.y` (or a full `vec2 dfg`) as a PARAMETER (not a global/binding), so it has no runtime dependency on IBL infrastructure at all — Task 9/10 later supply the real baked value, Task 7's test supplies a locally-computed reference one. This decouples the two tickets' build order cleanly and should be recorded as the module's calling contract. |
| **Clearcoat lobe (GGX distribution + Kelemen visibility) — "prepared for," not built** | `distributionClearCoat()`/`visibilityClearCoat()` (`surface_brdf.fs`) — `D_GGX` reused, `V_Kelemen(LoH) = 0.25/(LoH*LoH)` (cites Kelemen 2001). Charter's own text flags a **June-2026 clearcoat documentation discrepancy**, resolved correctly only in shader code (`toolchain-platform-rhi-design.md:377-380`). | Absent (Task 21, Stage 3, out of this ticket's scope). | genuinely-N/A for T7's OWN acceptance bar, but the module LAYOUT decision below is in-scope | The charter's own instruction — "port from Filament's CURRENT `shaders/` implementation... never from its documentation prose" — is directly actionable HERE: `V_Kelemen` and `distributionClearCoat`'s dispatch macros ARE already visible in the currently-fetched `surface_brdf.fs` (this session, commit `721ec80`), so whatever the June-2026 doc discrepancy is, the shader-code answer is available NOW, not deferred to Task 21. Recorded acceptance criterion for T7 (not T21): `brdf.slang`'s module boundary leaves an unambiguous, named extension point for `visibilityClearCoat`/`distributionClearCoat` (e.g., a documented "Task 21 adds these two dispatch functions here" comment, matching this project's own established "code comment referencing the future task" idiom — D13's reversed-Z precedent, D7's TEXCOORD_1 precedent) — this ticket does NOT need to resolve the doc-vs-code discrepancy itself (that determination is scoped to whichever task actually implements clearcoat, Task 21), but must not build a module boundary that makes doing so awkward later. |
| Composable module layout (`brdf.slang` + siblings) | Filament's own file split (`surface_brdf.fs` = pure math, no material/lighting state; `surface_shading_lit.fs` = pixel-level composition consuming `PixelParams`) is the direct structural precedent the ticket cites ("Falcor's expression patterns... where idioms differ" — Falcor is the HLSL-idiom source, not the module-boundary source; Filament's OWN file split is). | `material.slang` currently mixes the engine ABI contract (`IMaterialShader`, `MaterialVertex`, bindless globals) with zero BRDF math at all (BRDF math lives inline inside `standard_pbr.slang`'s `evaluate()`). | consume-now (Task 1 spec's "module layout" decision item, plan:146) | `brdf.slang` exports pure math functions (`D_GGX`, `V_SmithGGXCorrelated`, `F_Schlick`, `Fd_Lambert`, energy-compensation helper) taking scalar/vector inputs ONLY — no `ParameterBlock`, no bindless global, no `IMaterialShader` dependency — verified by a standalone-compile test (the ticket's own "modules compile standalone through the existing Slang path" line) that imports `brdf.slang` alone with no `material.slang` import at all. |

## Open Questions

- **Fresnel f90 convention (Low-quality implicit-1.0 vs High-quality
  computed-from-F0) — RECOMMEND: port Filament's computed-f90 (HIGH
  quality) form as the new default, keep the current implicit-f90=1.0
  form as a named, tested fallback.** The currently-shipped
  `standard_pbr.slang` Fresnel is Filament's `FILAMENT_QUALITY_LOW`
  simplification, not its default — this was presumably an unintentional
  choice made before any Filament source was consulted (Phase 4 predates
  this port). The charter's own bar is "Filament-quality GGX PBR"
  (priority 1) — the computed-f90 form is Filament's actual DEFAULT
  quality tier, not an optional upgrade, and it measurably changes
  grazing-angle metal highlights (the exact case DamagedHelmet's
  sample-08 gate visually exercises). Recommend porting it as
  `brdf.slang`'s default `fresnel()`, with the plain-f90=1.0 overload
  kept and tested (cheap, already correct, useful for the LOW-quality/
  Steam-Deck-floor performance tier the charter's own Deck-floor
  requirement may eventually want). Both existing 08/09 gates need
  regeneration with provenance either way (the ticket's regression-guard
  language already anticipates this for the energy-compensation term;
  the Fresnel change is smaller in magnitude but not zero).
- **Vulkan specialization constants (`[vk::constant_id]`) vs Slang
  link-time generics as T8's actual "pay for what you use" mechanism —
  RECOMMEND: Slang link-time generic composition (module-level
  per-feature-combination linking), with `specializationBits` kept
  purely as the resulting VARIANT'S cache-key, not as a live
  `[vk::constant_id]` runtime toggle.** This is T8's decision, not T7's,
  but it directly constrains how `brdf.slang`'s clearcoat/sheen/
  anisotropy extension points (row above) should be shaped, so it is
  recorded here for the coordinator to carry forward. Evidence: (1)
  `PipelineRequest::specializationBits` (`material_system.h:60-64`) is a
  cache-key-only field TODAY — hardcoded to `0` at the one call site
  (`getPipeline()`'s `PipelineRequest req{..., /*specializationBits=*/0}`,
  `material_system.cpp:1725`) and NO Slang file in this repository
  contains a `[vk::constant_id]`/`SpecConstant`-decorated declaration
  anywhere (grepped `shaders/`, `src/rx_material/` — zero hits) — the
  mechanism is a reserved cache-key slot, not a working toggle. (2) Slang
  DOES support `[vk::constant_id(N)]` (confirmed, Slang docs, WebSearch
  digest 2026-08-20) but Filament's own toolchain does NOT use runtime
  Vulkan spec constants for its shading-model axes at all — it uses
  C-preprocessor `#define`/`#if` macros compiled per-variant offline
  (`BRDF_SPECULAR_D`/`_V`/`_F`, `SHADING_MODEL_*` — literal `#if defined`
  gates throughout `surface_brdf.fs`/`surface_shading_lit.fs`), i.e.
  Filament's own "pay for what you use" precedent is COMPILE-TIME
  variant generation, not runtime spec-constant branching. (3) This
  project's own `forward_entry.slang` already has a VERIFIED,
  working link-time-specialization mechanism (its own header comment:
  "Verified directly against the project's shipped Slang v2026.14.1
  build... a throwaway 4-part composite... compiles, links, and produces
  correct SPIR-V") — extending that SAME mechanism to feature axes
  WITHIN one material (not just material-selection) is the lowest-risk
  path to a mechanism this codebase has already proven works, versus
  spec constants, which carry two live open upstream Slang issues
  (#3044, #5725) about incomplete/inconsistent backend support — a real,
  cited maturity risk, not a hypothetical one. The acceptance
  criterion's own wording ("SPIR-V free of that feature's code path")
  is trivially, unconditionally true under link-time composition (the
  code literally never gets compiled in) and only CONDITIONALLY true
  under spec constants (depends on the SPIR-V optimizer's dead-code
  elimination actually firing through Slang's own lowering — an extra
  verification burden T8 would have to carry that composition avoids by
  construction).
- **Filament's release-vs-main branch mismatch — RECOMMEND: pin to the
  latest tagged release commit on the `release` branch (`v1.75.0`,
  published 2026-08-04), not `main`'s same-day HEAD
  (`721ec800093de984cbee155e459298b6b2dbb855`).** The charter's own text
  says "port from Filament's CURRENT `shaders/` implementation... never
  from its documentation prose" — but does not say "from `main`'s tip."
  `main` is Filament's active-development branch (pushed same day as
  this research, `pushed_at: 2026-08-20T19:21:18Z`) and its release
  process tags from a SEPARATE `release` branch
  (`target_commitish: "release"` on the `v1.75.0` GitHub Release object)
  — porting from an untagged `main` HEAD risks pulling in-flight,
  unreleased shader changes with no stable reference point to diff
  against on a future re-sync. **Resolved this session:** `v1.75.0`'s
  tag object (`gh api repos/google/filament/git/refs/tags/v1.75.0`)
  points to commit **`0e58877c09afb1aacd09ff640f74d2adcd2a7e80`** — this
  is the concrete SHA the recommendation pins. Recording BOTH the tag
  and this resolved SHA in the spec's decision entry satisfies "CURRENT
  `shaders/` implementation" (a real release, not stale documentation-
  era code) while giving future re-syncs a stable, tagged diff base. Any
  file citation in this matrix should be re-verified against
  `0e58877c0...` rather than `721ec80...` before the port lands, since
  the two commits were not diffed against each other this session (a
  real, cheap follow-up: `gh api repos/google/filament/compare/
  0e58877c09afb1aacd09ff640f74d2adcd2a7e80...721ec800093de984cbee155e
  459298b6b2dbb855`). If the coordinator instead wants
  the absolute freshest shader code regardless of release-tag stability
  (defensible — "CURRENT" arguably means "current", and clearcoat fixes
  may have landed on `main` after `v1.75.0` cut), the `main` SHA pinned
  above (`721ec80...`) is the fallback — either choice is workable, but
  it needs to be a RECORDED, deliberate choice, not an accidental
  same-session `main` HEAD grab that nobody can reproduce later.
- **`Fd_Burley` in/out — the ticket already defers this to Task 1's own
  spec ruling; not re-litigated here.** Recorded in the matrix row above
  with both dispositions' acceptance criteria pre-specified so whichever
  way the spec rules, T7 has a ready-made acceptance bar.

## New gaps

- **No compute-shader numerical test harness exists anywhere in this
  repository today** (grepped `src/rx_material/tests`, `shaders/tests` —
  no compute-dispatch-based value-table test precedent). T7's own
  acceptance sketch requires one ("compute-shader evaluation, exact-
  tolerance asserts") and is explicitly named as "a Task 2 consumer" in
  the plan's own file-list line (plan:329) — this is a genuine, already-
  tracked dependency (T7 cannot land before Task 2's compute pipeline
  capability exists), not a new finding, but worth restating plainly:
  T7's OWN acceptance criteria are unbuildable until Task 2 closes,
  which the plan's Stage 0→Stage 1 ordering already enforces correctly.
- **No SPIR-V-content inspection tooling (`spirv-dis`/`spirv-cross`
  invocation from a test) exists in this repository today** (grepped
  `tests/`, `tools/`, CMake test definitions — no hit). The Open
  Question above (spec constants vs link-time generics) partly turns on
  this: if the coordinator picks spec constants anyway, T8 will need to
  ADD spec-constant-content verification tooling (grep compiled SPIR-V
  disassembly for the absence of a specific opcode/decoration) to make
  the "SPIR-V free of that feature's code path" claim checkable at all —
  under link-time generics, the existing `MaterialSystem::getPipeline()`
  cache alone proves it (two distinct `VkPipeline`s exist or they don't).
  This belongs to T8's matrix as an acceptance-mechanism detail, flagged
  here since it originates from this ticket's Open Question.

## Verification health

- Filament source citations (`surface_brdf.fs`, `surface_shading_lit.fs`,
  `surface_light_indirect.fs`, `libs/ibl/src/CubemapIBL.cpp`) are FULL
  file fetches via the GitHub Contents API (`gh api .../contents/
  <path>?ref=main`, base64-decoded), not search digests — every quoted
  formula and function name above was read directly from the fetched
  file this session (2026-08-20), pinned to commit `721ec800093de984
  cbee155e459298b6b2dbb855`.
- The `D_GGX`/`V_SmithGGXCorrelated` algebraic-equivalence claims (shipped
  code vs Filament's rearranged forms) were hand-verified by polynomial
  expansion this session, not asserted by inspection alone — shown
  inline in the matrix rows above.
- The "no `[vk::constant_id]` anywhere in this repo" and "`specialization
  Bits` hardcoded to 0 at its one call site" claims are grep-verified
  against the current working tree (`shaders/`, `src/rx_material/`),
  not inferred from documentation.
- Slang's `[vk::constant_id]` syntax and the two cited open GitHub
  issues (#3044, #5725) are WebSearch-digest-sourced, not directly
  fetched from `github.com/shader-slang/slang`'s issue tracker — the
  ISSUE NUMBERS and titles are quoted from the search tool's own result
  list (a materially different confidence level than a fetched issue
  body would be; the coordinator may want to `gh issue view` both
  directly on `shader-slang/slang` before treating the "maturity risk"
  claim as fully settled — the DIRECTION of the finding, "spec constants
  are less battle-tested than link-time generics in this exact
  toolchain," is well-supported regardless by the zero-shipped-instances
  grep result, which IS first-hand).
- `Fd_Burley`'s citation ("Burley 2012, Physically-Based Shading at
  Disney") and `V_Kelemen`'s ("Kelemen 2001") are quoted verbatim from
  Filament's own source comments, not independently verified against the
  original papers.
- The `v1.75.0`/`release`-branch SHA was NOT resolved to a literal commit
  hash this session (only the tag name and `target_commitish` field were
  read) — the Open Question above names the exact follow-up API call.
