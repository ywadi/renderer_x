# Completeness matrix — ticket #44: [P5 T08] StandardPBR rework onto the module architecture

**Plan task:** Task 8, "StandardPBR rework onto the module architecture"
(`docs/superpowers/plans/2026-08-20-phase5-techniques.md:342-369`), Stage 1.
Depends on Task 7's `brdf.slang` module (ticket #43,
`gate/matrix-p5t07-brdf-module-port.md`) — this matrix cross-references
that one's Open Questions rather than re-deriving them.

**Binding sources:** techniques charter shader-architecture paragraph
(`toolchain-platform-rhi-design.md:390-402` — "feature permutation via the
existing specialization-bit system / Slang generics... D28's axis + Phase-3
D8 variant machinery are the prepaid seams"); D28 (fixed-function
pipeline-state axis, `phase4-scene-assets-design.md:492-512`); D8 (Phase-3,
parameters-vs-specialization split, `phase3-render-graph-materials-
design.md:190-199`); D22 (Phase-4 StandardPBR, same doc:360-373).

**Ticket body (`gh issue view 44`):** rebuild `standard_pbr.slang` on
Task 7's modules; grow toward the full glTF-extension parameter set —
baseColor/metallic/roughness/ior/specular/emissive+strength NOW,
clearcoat/anisotropy/sheen/transmission/thickness/attenuation/dispersion/
iridescence/diffuseTransmission as declared-but-gated slots; feature
permutation via specialization-bit system / Slang generics (D28 + Phase-3
D8); KHR_materials_ior/specular/emissive_strength consume here (already
parsed/preserved by the Phase-4 importer, no importer rework).

**Sources consulted (in-repo, read in full this session):**
`shaders/material/standard_pbr.slang` (current shipped baseline — see the
T7 matrix's own citation list for the exact lines), `shaders/material/
material.slang`, `shaders/material/forward_entry.slang`; `src/rx_material/
include/rx_material/material_system.h` (`PipelineRequest`, `MaterialFixed
FunctionState`, `AlphaMode`); `src/rx_material/material_system.cpp`
(`getPipeline()`/`PipelineKey`, :1920-1940); D28/D8 spec text (as above).

**Sources consulted (external, fetched 2026-08-20):** the same Filament
`shaders/src/surface_brdf.fs` fetch as the T7 matrix (pinned commit —
see that matrix for the resolved release SHA
`0e58877c09afb1aacd09ff640f74d2adcd2a7e80`, `v1.75.0`); Khronos extension
READMEs fetched directly (`gh api repos/KhronosGroup/glTF/contents/
extensions/2.0/Khronos/<ext>/README.md?ref=main`, base64-decoded, full
text read) for `KHR_materials_ior`, `KHR_materials_specular`,
`KHR_materials_emissive_strength`.

---

## The matrix

| Feature | glTF spec text (quoted, fetched 2026-08-20) | Current shipped state | Disposition | Proposed acceptance criterion |
|---|---|---|---|---|
| `KHR_materials_ior` | `ior` default **`1.5`**; *"for the default index of refraction ior = 1.5 this term evaluates to dielectric_f0 = 0.04"* (`dielectric_f0 = ((ior-1)/(ior+1))^2`). Special case: `ior=0` forces the Fresnel term to evaluate to exactly `1.0` regardless of view/light angle (a "specular-glossiness backward-compat mode", non-dynamically-toggleable). | `standard_pbr.slang:193` hardcodes `dielectricF0 = float3(0.04,0.04,0.04)` — exactly the `ior=1.5` DEFAULT value, byte-consistent with a future ior=1.5 no-op regression guard. | consume-now | Unit test: `ior` parameter absent (default 1.5) reproduces the existing hardcoded-0.04 gates byte-identically (regression guard, same pattern as Task 4's exposure=0.0 no-op precedent). GPU test: `ior=1.0` (a common glass/no-Fresnel-reflection value) produces `dielectric_f0=0.0` (a readback-checkable exact value from the closed-form quoted above). Edge case: `ior=0` produces a Fresnel term of exactly `1.0` at every angle (a dedicated probe, since this is a genuinely different code path — not just a different F0 constant — per the spec's own "MUST evaluate to 1.0 independently of view or light direction" language). Applies to the DIELECTRIC term only — metals (F0=baseColor) are unaffected (see `KHR_materials_specular` row below for the same metal-exclusion rule, spec-confirmed independently for each extension). |
| `KHR_materials_specular` | `specularFactor` default `1.0`, `specularColorFactor` default `[1,1,1]`. Formula (quoted): `dielectric_f0 = min(0.04 * specularColor, 1.0) * specular`, `dielectric_f90 = specular` (NOT 1.0), `dielectric_fresnel = mix(dielectric_f0, dielectric_f90, fresnel_w)`. Explicit: *"The metal BRDF is not affected by the parameter."* Explicit interaction rule (quoted): *"If KHR_materials_ior is used in combination with KHR_materials_specular, the constant 0.04 is replaced by the value computed from the IOR."* | Absent. Current Fresnel (`standard_pbr.slang:211-213`) is a two-parameter Schlick with implicit `f90=1.0` — cannot represent `dielectric_f90=specular` at all without a code change, independent of this ticket's own work. | consume-now | **Cross-ticket dependency on T7's Fresnel-overload decision (see T7 matrix Open Questions, "f90 convention"):** this extension's own spec text HARD-REQUIRES the three-argument `F_Schlick(f0, f90, VoH)` form (Filament's own `surface_brdf.fs` already has this exact overload) — `KHR_materials_specular`'s `dielectric_f90 = specular` is not an optional quality-tier nicety, it is spec-mandated behavior the moment this extension consumes. This STRENGTHENS T7's recommendation to port the explicit-f90 overload (independent of which f90 DEFAULT T7 picks for the no-extension case). GPU test: `specular=0.0` (spec-legal, "disables the specular reflection, resulting in a pure diffuse material") produces a fully diffuse response at grazing AND normal incidence (`dielectric_f0=0`, `dielectric_f90=0` — both Fresnel endpoints collapse to zero, a stronger, more discriminating test than F0 alone since it also exercises the grazing-angle term). `specularColorFactor` tints F0 independent of `specular`'s own scalar strength — a two-axis test (color axis × strength axis) distinguishes the two params from each other, not just from baseline. Metal-exclusion: a fully-metallic material with `specular=0` still renders its normal metallic reflectance (F0=baseColor unaffected) — the discrimination proof this row's "metal BRDF not affected" claim needs. |
| `KHR_materials_emissive_strength` | `emissiveStrength` default `1.0`. Formula (quoted): `color += emissiveFactor.rgb * sRGB_to_Linear(emissiveTexture.rgb) * emissiveStrength`. | Already consumed via a CPU-side pre-multiply into `emissiveFactorAndPad` (`standard_pbr.slang:53-58`'s own comment: "PRE-MULTIPLIED by KHR_materials_emissive_strength at bind time... folding it into the CPU-side bind avoids widening this struct") — this is a Phase-4-era gate-matrix disposition (matrix-issue08) already implemented, not new T8 work. | **already consume-now (verify unaffected by rework)** | The ticket's own acceptance line (*"emissive_strength 4.0 → 4× radiance probe pre-tonemap"*) is a REGRESSION test against already-shipped behavior, not new functionality — T8's rework must not accidentally regress the existing CPU-side pre-multiply while moving other fields into the new module architecture. No new shader-side change is implied unless T8's rework changes how `emissiveFactorAndPad` is populated (e.g., if T1's spec rules the pre-multiply should move on-shader for consistency with ior/specular's own on-shader math — a minor internal-consistency question, not a functional gap). |
| **Feature-permutation mechanism ("only pay for the features they use")** | N/A (architecture decision, not a glTF field). | `PipelineRequest::specializationBits` exists as a cache-key field, hardcoded to `0` at its one call site (`material_system.cpp:1725`); **zero** `[vk::constant_id]`/spec-constant declarations exist anywhere in this repo's Slang files (grep-verified). D28's `MaterialFixedFunctionState` is the ONLY currently-working per-material cache-key axis beyond content hash. | **Not yet built — this ticket's core architectural deliverable, not a config flip** | See T7 matrix's Open Question (spec constants vs Slang link-time generics) — RECOMMENDS link-time generic composition per feature-combination, with `specializationBits` repurposed as the resulting variant's cache-key selector rather than a live runtime toggle. Concrete acceptance criterion either way: (a) a material with ZERO optional features set produces IDENTICAL SPIR-V (byte-for-byte, or functionally proven via `getPipeline()`'s own cache behavior) to a hand-written "clearcoat/anisotropy/etc-free" StandardPBR; (b) a measured perf counter (Tracy zone, per CLAUDE.md's "measured claims only") shows no regression vs the Phase-4 baseline for the zero-feature case; (c) turning ONE optional feature on changes the cache key (a new, distinct `VkPipeline`) without perturbing materials that did not opt in (their `PipelineKey`s, and therefore their already-warm `VkPipeline`s, stay byte-identical — a real regression risk if the new key derivation touches every material's hash, not just the ones using new features). |
| **Declared-but-gated slots (clearcoat/anisotropy/sheen/transmission/thickness/attenuation/dispersion/iridescence/diffuseTransmission)** | Khronos glTF Sample Viewer's own supported-extensions list (`glTF-Sample-Viewer/README.md`, fetched — see the T11 matrix's own citation) confirms ALL NINE are live, shipped, non-experimental extensions in the reference viewer today — this is not a speculative feature set, it is the CURRENT state of the art the charter explicitly targets. | Absent (Stages 2-4 build the actual lighting math). | genuinely declared-but-gated per the ticket's own text | "Declared" must mean something concrete and testable, not just a comment: proposed acceptance criterion — each gated field has a real place in the material's parameter data (either present-but-zero-cost via the SAME permutation mechanism the row above builds — i.e., a Task-21-onward material simply flips a bit this ticket's mechanism already supports — or a documented Slang generic type parameter placeholder) AND a discrimination test proving a material NOT opting into any gated feature carries zero extra `ParameterBlock` bytes/zero extra bindless slots/zero extra SPIR-V for them (the same "pay for what you use" bar as the row above, extended to DATA layout, not just code path). This is where "declared but gated" could silently become "a dead struct field nobody ever removes" if the acceptance bar is not explicit now — flagging as an Open Question below since it is a real disposition choice, not obviously either way. |
| Energy conservation preserved through the rework | Ticket's own acceptance line: *"energy conservation preserved (Task 7 harness re-run on the composed material)."* | T7's white-furnace test operates on `brdf.slang`'s STANDALONE functions (per the T7 matrix's own recommendation — test-local DFG evaluation, no IBL dependency); StandardPBR COMPOSES those functions with its own metallic/roughness/F0 derivation, texture sampling, and (new, this ticket) ior/specular-driven F0/f90. | consume-now | The composed-material re-run must exercise the FULL StandardPBR F0/f90 derivation path (post-ior/specular consumption), not just re-invoke `brdf.slang`'s bare functions with hand-picked F0/roughness — otherwise a bug in HOW StandardPBR computes F0 from ior/specular (row above) could pass T7's harness while breaking energy conservation in the shipped material. Concrete criterion: a second white-furnace pass parameterized by `(ior, specular, specularColor)` combinations (not just `roughness`), asserting integrated response ≈ 1 at F0-derived-to-1 configurations across that combined space, not just the single-axis roughness sweep T7's own harness covers. |
| Variant discrimination cost claim | Ticket's acceptance line: *"pays no measured cost vs the Phase 4 baseline."* | No Tracy zone currently wraps `MaterialSystem::getPipeline()`/`bindInstance()` specifically for a per-variant cost breakdown (general Tracy instrumentation exists elsewhere in the codebase per the Phase-4 toolchain, but not scoped to this exact claim — not independently verified this session, flagged for the coordinator to confirm at implementation time). | consume-now | A dedicated Tracy zone (or counter) isolating the zero-feature-material draw path's CPU/GPU cost, compared against a Phase-4-era StandardPBR build (or a recorded baseline number from before this ticket lands) — "no measured cost" needs a BEFORE number to be a real claim, not just an AFTER number that looks fine in isolation. |

## Open Questions

- **Declared-but-gated slots: "present in the struct, zero-cost when
  unset" vs "not present until the owning Stage-2/3/4 task adds it" —
  RECOMMEND the latter (add fields only when the owning task lands),
  with the permutation MECHANISM (not the fields) built now.** Two
  readings of "declared-but-gated" are both defensible: (a) `Standard
  PbrParams` grows NINE new fields today (clearcoat factor/roughness,
  anisotropy strength/rotation, sheen color/roughness, transmission
  factor, thickness, attenuationColor/Distance, dispersion, iridescence
  factor/thickness/ior, diffuseTransmission), all zero/neutral by
  default, with Stages 2-4 flipping them on; (b) the STRUCT stays
  minimal (today's five-ish fields) and the PERMUTATION MECHANISM (the
  matrix row above) is what "prepared for... growth" means — each later
  task adds its own fields via the SAME Slang-generic composition
  pattern when it actually lands, rather than pre-declaring nine fields
  that sit unused (and, per D8's "bound parameters change per-instance
  with zero recompilation" contract, WOULD have real per-instance cost —
  every `ParameterBlock<StandardPbrParams>` instance pays for every
  declared field's storage regardless of whether that instance uses it,
  since `ParameterBlock` is not itself sparse). Recommend (b): this
  ticket builds the MECHANISM (module layout + permutation/variant-key
  plumbing) and grows the PARAMETER STRUCT only for what it actually
  consumes now (ior/specular/emissive-strength) — pre-declaring nine
  unused fields would violate the charter's own "materials only pay for
  the features they use" language at the DATA level even if the SHADER
  CODE path is correctly gated, and directly contradicts D8's own
  "bound parameters change... with zero recompilation" framing, which
  presumes the parameter block reflects what's actually wired, not a
  maximal superset. A short code comment at the extension point
  (matching this project's established D13/D7 "future task lands here"
  idiom, already used three times in the current `material.slang`) is
  the cheap, correctly-scoped alternative to nine live-but-inert fields.
- **Where does the ior→f0 / specular→(f0,f90) derivation live —
  `brdf.slang` (T7's module) or `standard_pbr.slang` (this ticket)? —
  RECOMMEND `standard_pbr.slang`, with `brdf.slang` exposing only the
  generic `F_Schlick(f0, f90, VoH)` primitive.** The ior/specular
  formulas above are glTF-EXTENSION-SPECIFIC derivations of F0/f90 from
  material factors — they are not general BRDF math the way `D_GGX`/
  `V_SmithGGXCorrelated` are (Filament itself does not implement
  `KHR_materials_ior`/`_specular` at all — these are Khronos glTF
  extensions, not part of Filament's own material model, which has its
  own separate `reflectance`/`ior` material properties with different
  defaults). Keeping the derivation in `standard_pbr.slang` (the glTF-
  vocabulary-aware module) and leaving `brdf.slang` purely as
  Filament-ported generic math keeps the module boundary honest about
  WHICH port source backs which code — a future non-glTF material
  consuming `brdf.slang` should not inherit glTF-specific F0 semantics
  by accident.
- **Metal-exclusion enforcement: is it a shared helper or duplicated
  per-extension logic?** Both `KHR_materials_ior` and
  `KHR_materials_specular` independently state "the metal BRDF is not
  affected" — recommend a single `computeDielectricF0F90(ior, specular,
  specularColor)` helper (returning both scalars) that StandardPBR
  calls ONCE, then `lerp`s with `baseColor` by `metallic` exactly as the
  existing code already does for the plain-0.04 case — this keeps the
  metal-exclusion invariant enforced in ONE place rather than trusting
  two independent call sites to each remember it, and gives the
  discrimination tests above a single function to target.

## New gaps

- **No existing precedent in this codebase for a Tracy zone scoped to
  "cost of a zero-feature material variant specifically"** (see the
  "variant discrimination cost claim" matrix row) — this is a new
  measurement surface T8 must add, not an existing facility to reuse;
  flagged so the coordinator doesn't assume it already exists from
  Phase 4's general Tracy adoption.
- **`ParameterBlock<T>` sparsity/cost model was not independently
  verified against Slang's own reflection/codegen docs this session**
  (the Open Question above asserts "every declared field has real
  per-instance storage cost regardless of use" based on how
  `ParameterBlock` behaves elsewhere in this codebase's own comments,
  not a freshly-fetched Slang spec citation) — a cheap follow-up before
  the coordinator treats this as settled fact rather than a
  well-grounded inference.

## Verification health

- glTF extension formulas (`KHR_materials_ior`, `KHR_materials_specular`,
  `KHR_materials_emissive_strength`) are QUOTED VERBATIM from each
  extension's own `README.md`, fetched directly via the GitHub Contents
  API this session (2026-08-20), not paraphrased from a search digest or
  inherited from the Phase-4 gate matrix (which cited these three
  extensions by name only, without quoting formulas, since Phase 4 only
  needed to confirm they were parsed/preserved at import, not consumed).
- The "zero `[vk::constant_id]` anywhere in this repo" and
  "`specializationBits` hardcoded to 0" claims are the SAME grep-verified
  findings cited in the T7 matrix (`gate/matrix-p5t07-brdf-module-
  port.md`) — not re-verified independently in this file, cross-
  referenced instead to avoid duplicate, possibly-drifting claims.
- The Khronos glTF Sample Viewer's supported-extension list (used to
  argue clearcoat/anisotropy/sheen/etc. are "current state of the art,
  not speculative") is cross-cited from the T11 matrix's own fetch of
  that same README — see that matrix for the full citation, not
  re-fetched independently here.
