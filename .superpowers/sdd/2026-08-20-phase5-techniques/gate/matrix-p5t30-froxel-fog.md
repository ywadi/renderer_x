# Completeness matrix — P5 T30 (issue #66): Froxel volume fog — volumetrics tier (b)

**Plan task:** Task 30, Stage 4 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:808-832`).
**Charter binding:** *"True volumetrics (committed 2026-08-19): froxel-marched
participating media (Frostbite/id-style volume fog), riding the SAME
camera-frustum froxel grid the clustered light assignment already builds —
per-froxel scattering/extinction accumulation fed by the clustered light
lists (shadowed sun + local lights), temporal reprojection for stability,
then a full-screen apply"* (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:433-446`).
Depends on Task 14's froxel grid (Stage 2) and Task 2's compute capability
(Stage 0) — neither landed at gate-research time (verified below).

**Sources consulted (in-repo, 2026-08-20):**
- Repo-wide grep: zero hits for `froxel`/`Froxel`/`cluster.*light` under
  `src/` or `shaders/`, and zero hits for `vkCreateComputePipelines`/
  `vkCmdDispatch` under `src/` (excluding tests) — **confirms Task 2
  (compute capability) and Task 14 (froxel grid) are both wholly unbuilt at
  this gate's research time**; T30 is a pure Stage-2/Stage-0 consumer with
  nothing yet to consume. This matches Task 2's own plan text ("verified
  2026-08-18").
- `.superpowers/sdd/2026-08-20-phase5-techniques/gate/matrix-p5t14-froxel-clustering.md:30-60`
  (a concurrent gate matrix from this same primary-gate round, read for
  cross-stage dependency accuracy): **CRITICAL FINDING carried forward** —
  that matrix proves Filament's own froxelization (`Froxelizer.cpp`) runs
  **CPU-side**, not as a GPU compute shader, contradicting the charter's
  "its compute-shader light-assignment is published GLSL" framing for
  Task 14. This directly affects T30: "the SAME camera-frustum froxel grid"
  T30 rides is Task 14's OWN grid, whose GPU-vs-CPU-build shape is still an
  open ruling at Task 14, not a settled fact T30 can assume. T30's own
  froxel **march** (light-accumulation-in-3D-texture / raymarch-and-apply)
  is unambiguously compute-shader work regardless of how Task 14 resolves
  (verified below, row 2) — only the shared GRID's *construction* method is
  in question, not T30's own consumption of it.
- `src/rx_graph/include/rx_graph/pass.h:67-138` — `addHistoryInput`/
  `setHistoryOutput` (ping-pong pinned physical images, cross-frame
  persistence) — the render-graph history-resource mechanism T30's
  "temporal reprojection for stability (history resources)" text names;
  confirmed genuinely delivered (Phase 4 Task 1), not aspirational.
- `src/rx_core/include/rx_core/profile.h` — Tracy, confirmed real (see
  T29's matrix, same finding).

**Sources consulted (external, fetched 2026-08-20):**
- `google/filament`, pinned tag `v1.75.0`: `filament/src/materials/fog/`
  (`fog.cpp`, `fog.h`, `fog.mat`) — **Filament's ENTIRE fog feature is a
  per-fragment analytic function** (`shaders/src/surface_fog.fs`, applied
  in the shading pass), not a 3D froxel volume, not compute-driven, and
  carries no shadowed in-scattering or temporal reprojection of any kind.
  GitHub code search across `google/filament` for
  `VolumetricFog`/`"god ray"`/`LightScattering` (2026-08-20): **zero
  matches.** Filament is NOT a valid port source for this ticket, despite
  the charter's general "Filament as canonical... froxel-based clustered
  lighting, ... HDR post" framing appearing to cover it by proximity.
- `godotengine/godot`, pinned stable tag `4.7.2-stable` (also verified
  present at `master`@`73fa32f`, MIT license,
  `raw.githubusercontent.com/godotengine/godot/master/LICENSE.txt`):
  `servers/rendering/renderer_rd/shaders/environment/volumetric_fog.glsl`
  (light-injection compute shader, writes a 3D froxel texture) and
  `.../volumetric_fog_process.glsl` (889 lines, full file fetched —
  integration/raymarch + temporal-reprojection compute shader). Read in
  full; see rows 2-4 for the exact cited lines.
- `NVIDIAGameWorks/Falcor`: GitHub code search for `VolumetricFog` — zero
  matches (Falcor is not a source for this feature either).
- Sébastien Hillaire & Benjamin Neyret, "Physically Based and Unified
  Volumetric Rendering in Frostbite" (SIGGRAPH 2015 course notes, publicly
  distributed PDF) — the charter's own named "Frostbite/id-style" algorithm
  reference; algorithm/paper only, not a code source (Frostbite is
  EA-proprietary, unavailable). Godot's implementation is independently
  written but follows the same froxel-grid + compute-injection +
  raymarch-integrate shape this paper popularized.

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/port-source support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------|-------------------------------|
| 1 | Shared froxel grid consumption (same layout/bindings as Task 14) | Task 14's own gate matrix (cited above) — CPU-vs-GPU build method of the grid itself is UNRESOLVED at Task 14. | preserve-later (hard dependency on Task 14's actual delivered shape) | N/A — cross-ticket dependency, not a library question. | T30's froxel-fog compute pass must consume Task 14's grid through whatever binding layout Task 14's OWN Task-1 spec ruling fixes (spec:493-495's "grid's layout/bindings are authored for two consumers from day one" — a Task 1 spec obligation, not a T30 one); T30's acceptance criteria should reference "Task 14's committed grid layout" rather than assuming a specific shape. |
| 2 | Per-froxel light-injection compute pass (scattering/extinction accumulation into a 3D froxel texture, fed by clustered light lists incl. shadowed sun + local lights) | Godot 4 `volumetric_fog.glsl` (light-injection compute shader; MIT, `godotengine/godot`@`4.7.2-stable`). **Filament has NO equivalent** (verified: `materials/fog/` is analytic-only). | consume-now — **recommend Godot as the primary port source**, correcting the charter's implicit Filament-covers-everything framing for this specific sub-feature. | Verified directly: Godot's shader writes a `light_only_map` 3D image accumulating per-froxel `scattering`/`base_scattering`/`base_density` (`volumetric_fog_process.glsl:170,379-384`) fed by iterating light lists with per-light shadow lookups (row 3) — exactly T30's "per-froxel scattering/extinction accumulation fed by the clustered light lists" text. | A GPU test asserts exact per-froxel scattering VALUES for a synthetic single-light, single-froxel-column configuration against a hand-computed analytic expectation (extends the ticket's own single-slab transmittance probe to the injection stage specifically, not just the apply stage). |
| 3 | Shadowed sun in-scattering (light shaft appears exactly where the shadow map says the sun reaches the medium) | Godot's own directional-light PSSM/cascade sampling inline in the injection shader (`volumetric_fog_process.glsl:400-420`: `shadow_attenuation`, `directional_lights.data[i].shadow_opacity`, per-split `pssm_coord`/`shadow_matrix1..3`, `shadow_z_range`). | consume-now | Verified: this is a DIRECT, cited, working reference for exactly the ticket's headline discrimination probe — Godot's own comment at the equivalent site reads "Higher values will make light in volumetric fog fade out sooner when it's occluded by shadow" (`volumetric_fog_process.glsl:291`), i.e. the exact effect the ticket's acceptance criterion measures. RendererX substitutes its own CSM/PCSS shadow sampling (Task 16/17, Stage 2) for Godot's PSSM lookup at the equivalent call site — the STRUCTURE (per-light shadow-test inside the injection loop) ports; the shadow-sampling call itself is this codebase's own. | Ticket's own probe stands as written: shadowed vs. unshadowed control pair, value-asserted. |
| 4 | Temporal reprojection for stability (history resources) + Halton jitter per froxel cell | Godot `volumetric_fog_process.glsl:183-349`: `use_temporal_reprojection`/`temporal_frame`/`temporal_blend` flags, `prev_density_texture` reprojected via a previous-frame view matrix, current froxel's world position jittered by `halton_map[params.temporal_frame]` ONLY when reprojection succeeds ("cells that can't reproject should not jitter" — own comment, line 347). | consume-now | Verified: this is a genuine ping-pong-texture temporal scheme — the RendererX-side mechanism it binds to is `rx_graph`'s ALREADY-DELIVERED `addHistoryInput`/`setHistoryOutput` (`pass.h:67-138`, confirmed real, Phase 4 Task 1) declaring the 3D froxel-scattering texture as a history resource. No new render-graph primitive is needed; T30 is a straightforward history-resource CONSUMER, same mechanism Task 33 (TAA) uses for its own history buffer. | Ticket's own two-frame static-camera variance-bound probe stands; additionally assert (per Godot's own "don't jitter on reprojection failure" rule) that the FIRST frame after a camera cut/disocclusion event does not jitter — a direct port-parity check against Godot's own stated rationale, not an invented rule. |
| 5 | Physical units: extinction/scattering in 1/m; analytic single-slab transmittance `exp(-σd)` | Textbook Beer-Lambert (Preetham/Hillaire — same SIGGRAPH course notes cited above give the exact `T = exp(-(σ_a+σ_s)·d)` form); Godot's own `base_scattering`/`base_density` fields are unit-carrying the same way. | consume-now | N/A — analytic physics, not a library dependency. | Ticket's own probe stands as written. |
| 6 | Full-screen apply pass, pipeline slot "before transparency, so glass sees fog" | Charter frame-pipeline text (spec:447-451). | consume-now | Godot's `volumetric_fog_process.glsl` integrate-and-apply step is the direct structural precedent (raymarch front-to-back accumulating scattered light + transmittance, single fullscreen composite at the end). | Wired at the documented pipeline point, ahead of Task 23/24's transmission passes (Stage 3, already landed by the time Stage 4 dispatches per plan sequencing). |
| 7 | Compute-pipeline prerequisite (Task 2) | N/A — internal dependency, not a precedent row. | preserve-later (hard blocking dependency, confirmed absent today) | Verified absent: zero `vkCreateComputePipelines`/`vkCmdDispatch` in `src/` today. | Plan's own sequencing already encodes this correctly ("T30 after Stage 2's grid" implicitly assumes Task 2 has landed in Stage 0 first, plan:989) — no correction needed, just confirming the dependency is real and currently unmet, not yet a defect. |
| 8 | Tier (c) — local fog volumes / height fog on the same grid | Plan's own text: lands "if schedule permits, else is registry-deferred at the stage checkpoint BY THIS PLAN'S TEXT" (plan:815-817, ticket body). Godot's SAME shader also supports per-`FogVolume` local density injection into the same 3D texture (the injection shader iterates both directional/global fog AND local volume primitives in one pass) — so if tier (c) lands, it is additive to the SAME Godot-derived injection kernel, not a separate port. | preserve-later / registry-fit, exactly per the plan's own already-correct text | Confirmed available in the same cited source if the schedule allows pulling it in. | No change from the plan/ticket text — already correctly scoped; recorded here only to confirm the "additive to the same kernel" framing for whoever makes the schedule call. |
| 9 | Cost measured (Deck-tier target carried to Task 36) | N/A — tooling precedent. | consume-now | Tracy confirmed real (see T29 matrix). | Ticket's own criterion stands; desktop number published at T30's own checkpoint, Deck row tracked per the standing MANUAL_VERIFICATION convention until Task 36's owner-executed Deck run. |

---

## Conflicts

**The charter's blanket "Filament as canonical... froxel-based clustered
lighting... HDR post" framing (spec:371-376) does not extend to volumetric
fog specifically** — verified Filament ships zero froxel-marched
participating-media code (its `fog.mat` is a per-fragment analytic term).
This is not a contradiction of the PLAN text, which correctly attributes
tier (b) to "Frostbite/id-style" rather than Filament (plan:809) — it is a
correction to the charter's more general framing, worth recording so a
future reader does not go looking for froxel-fog code in Filament and
conclude it was removed/missed.

## New gaps

None beyond what the plan/registry already track (tier (c) deferral is
already explicitly plan-scoped, row 8).

## Open Questions (for the coordinator's binding ruling)

1. **Port source for the froxel-fog injection/raymarch kernels.**
   Recommendation: **name Godot 4's `volumetric_fog.glsl` +
   `volumetric_fog_process.glsl` (MIT license) as the pinned port source**
   for Task 30, pinned at tag `4.7.2-stable` — a real, working,
   shadow-aware, temporally-reprojected implementation exists there and
   nowhere else searched (Filament/Falcor both verified absent). This is a
   genuine "ready-made implementation to port" per CLAUDE.md's standing
   rule, not a from-scratch case like T29's god rays.
