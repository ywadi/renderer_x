# Completeness matrix — P5 T15 (issue #51): Clustered shading integration + frame-pipeline adoption

**Plan task:** Task 15, Stage 2 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:509-528`).
Depends sequentially on T13 (physical units) and T14 (froxel grid) —
`T13→T14→T15 sequential` (plan:986). This ticket is the LIT-PATH consumer
of both.

**Charter binding:** frame-pipeline target (`docs/superpowers/specs/
2026-08-09-toolchain-platform-rhi-design.md:447-451`): *"depth → shadows →
clustered light assignment → opaque lighting → volumetrics... "* — this
ticket lands the first three-quarters of that spine for the scene path.

**Sources consulted (in-repo, 2026-08-20):**
`src/rx_material/include/rx_material/draw_data.h` (full `DrawDataGpu`
layout — the per-draw uniform row this ticket must grow to carry a froxel
buffer index, mirroring the exact pattern the Phase-4 shadow bridge already
used to add `lightViewProj`/`shadowMapTextureIndex` fields);
`shaders/material/material.slang:1-330` (existing `RxDrawData`/
`gDrawData`/`rx_sampleShadowPCF` — the bindless-array-of-buffers idiom this
ticket's froxel-buffer binding must follow); `shaders/material/
forward_entry.slang:183` (existing single-directional-light + shadow call
site — the exact insertion point clustered point/spot evaluation is added
next to). Cross-referenced against matrix-p5t14's own findings (froxel
buffer shape, determinism, capacity).

**Sources consulted (external, fetched 2026-08-20, pinned commit
`721ec800093de984cbee155e459298b6b2dbb855`, `google/filament`):**
`shaders/src/surface_light_punctual.fs:117-225` (full
`getLight()`/`evaluatePunctualLights()` — the exact loop-shape this
ticket's lit-path integration ports, read in full this session, cited
above in matrix-p5t14).

---

## The matrix

| Feature | First-tier precedent (cited) | Disposition | Library/source support (verified) | Acceptance criterion |
|---|---|---|---|---|
| Directional light stays DIRECT (never froxel-clustered) | Plan's own explicit text (plan:511): *"directional stays direct."* Filament's own architecture agrees structurally: `getLight()`'s froxel/record-buffer path (surface_light_punctual.fs:117-173) serves ONLY point/spot (`Type::POINT`/`SPOT`/`FOCUSED_SPOT`) — Filament evaluates its (singular, in practice) directional/sun light via a SEPARATE code path entirely (`surface_light_directional.fs`, named but not fetched this session — not needed to corroborate a claim already explicit in both the plan and Filament's own file-naming split). | consume-now | VERIFIED in-repo: RendererX's existing `RxDrawData::lightDirWorld`/`lightColor` (material.slang:240-241, draw_data.h:91-92) is ALREADY a direct, non-clustered per-pass field — this ticket does not change how the directional light reaches the shader, only ADDS a parallel point/spot path alongside it. | Regression test: a scene with ONLY a directional light (no point/spot) renders BYTE-IDENTICAL to the pre-Task-15 gates — proving the new clustered path is purely additive, never touching the existing directional term. |
| Per-draw froxel-buffer binding — extend `RxDrawData`, don't invent a new mechanism | RendererX's OWN established precedent: the Phase-4 shadow bridge added `lightViewProj`/`shadowMapTextureIndex`/`shadowCompareSamplerIndex`/`shadowTexelSize` to `RxDrawData`'s STRUCT TAIL on both the C++ (`DrawDataGpu`, draw_data.h:116-131) and Slang (`material.slang:258-275`) sides simultaneously, with an explicit "must stay in sync — cross-file drift risk" comment convention on both. | consume-now | VERIFIED this is the codebase's OWN idiom, not merely a precedent to consider adopting — `DrawDataGpu`/`RxDrawData`'s existing "per-pass, repeated per row, simpler than a second bindless buffer" convention (draw_data.h:116-118's own comment) is stated as the general pattern for exactly this kind of per-pass-constant addition. | Acceptance criterion: the froxel grid's own per-view parameters (grid dimensions, Z-slicing constants, the bindless STORAGE-BUFFER index of the per-froxel light-list buffer T14 produces) are added to `RxDrawData`'s struct tail on BOTH sides with the SAME `static_assert(sizeof(...) == N)` discipline the existing struct already enforces (draw_data.h:132) — a mismatched-size build failure, not a silent layout drift, is the test. |
| Clustered evaluation loop shape (fragment-shader side) | Filament `evaluatePunctualLights()` (surface_light_punctual.fs:180-225, fetched in full): fetch this fragment's froxel via `getFroxelIndex(screenCoord)`, iterate `[recordOffset, recordOffset+count)`, per light: channel-mask AND-test (`(light.channels & channels)==0 → skip`, matching RendererX's OWN existing D15/RC5 channel-coupling convention verbatim — see matrix-issue06's own "Alpha-MASK draws"/channel rows, Phase 4), early-out on `NoL<=0` or zero attenuation, THEN shadow-test only if `NoL>0` (shadow lookups are the expensive step — gated last). | consume-now | VERIFIED via direct source read, cited above — this is a well-formed, directly portable loop shape; RendererX's `channels` AND-test convention (`uint8_t channels`, scene.h:155-158) is ALREADY architecturally identical to Filament's own `light.channels & channels` gate, so this is a clean, low-risk port with no unit-convention translation needed on this specific sub-feature. | GPU test: a scene with N point/spot lights across several distinct froxels — a fragment's accumulated lighting includes ONLY the lights in ITS OWN froxel's record range (a light entirely outside a fragment's froxel must contribute exactly zero, not merely "small" — an exact-membership test, mirroring T14's own froxel-assignment test but exercised from the SHADING side of the same data). |
| Clustered-vs-unclustered equivalence (the plan's own named discrimination test) | Plan's own text (plan:519-520): *"An N-light scene renders within tolerance of a brute-force all-lights reference path (discrimination: clustering changes cost, never the image)."* | consume-now | N/A — a methodology requirement, not a library claim; this is the standard, necessary correctness harness for ANY clustered/tiled lighting scheme (the whole point of clustering is a PURE performance optimization over the brute-force "loop every light per fragment" ground truth — any visible difference is a bug, not a stylistic variant). | Acceptance criterion (concretizing the plan's own one-line sketch): implement the brute-force reference path as a SEPARATE, simple shader/compute permutation (loop over ALL scene lights per fragment, no froxel indirection) reachable via a build/runtime flag — NOT deleted after the comparison is made once, since it is this ticket's own permanent regression harness (every future point/spot lighting change re-runs this equivalence check, not just Task 15's own initial landing). Tolerance derives from float-accumulation-order differences ONLY (the two paths may sum the same lights' contributions in a different order) — any discrepancy exceeding that floor is a real assignment bug. |
| Depth prepass policy | Charter frame-pipeline target names `depth →` as the FIRST stage (charter:447-451) — implying a depth-prepass-before-lighting structure, but the plan itself defers the actual ruling: *"the scene path adopts the charter frame-pipeline spine this stage needs (depth prepass policy per the Task 1 ruling..."* (plan:513-514). | **cross-task dependency, not this ticket's own decision** | N/A — explicitly a Task 1 spec decision this ticket CONSUMES, not one this gate resolves. Cross-referenced against matrix-p5t14's own froxel-Z-slicing row: whichever depth source feeds froxel-Z-slice computation (a real depth prepass buffer vs. the camera's own near/far planes alone) is DOWNSTREAM of this same ruling — Filament's OWN `Froxelizer::prepare()` (Froxelizer.h:117-120) takes `projectionNear`/`projectionFar` as PLAIN SCALARS, not a depth-buffer sample, meaning Filament's froxel Z-slicing does NOT require a depth prepass to already exist (a relevant, concrete data point for the Task 1 ruling, even though this gate does not make the ruling itself). | Not this ticket's acceptance criterion to define — flagged with the concrete cross-reference above so the Task 1 ruling is made with the relevant fact in hand (a depth prepass is not a HARD prerequisite for froxel Z-slicing specifically, per Filament's own architecture — it may still be independently justified for early-Z/overdraw reasons, but that is a separate argument). |
| Scaling numbers published (100/1k/5k lights) | Plan's own named acceptance criterion (plan:521-522): *"Scaling numbers published: 100 / 1k / 5k synthetic lights, desktop driver-labeled."* Cross-references CLAUDE.md's own binding "performance is an exit criterion" rule (measured claims only, Tracy/counters, driver-labeled) and this project's real-GPU-verification standing corrective (lavapipe-only is not verification). | consume-now | N/A — a measurement requirement. Cross-references matrix-p5t14's Open Question #2 (the 256-light Filament ceiling vs. this project's own thousands-of-lights target) — the 5k-light scaling row is DIRECTLY the test that would expose a wrongly-scoped T14 data structure (a Filament-literal 256-bit-bitset port would simply be incorrect/non-functional at 5k lights, not merely slow) — so this row is this ticket's own EMPIRICAL confirmation that T14's Open Question #2 was resolved correctly. | Acceptance criterion: `--lights N` scaling table (desktop, driver-labeled per the standing rule) at N=100/1000/5000, published in the ledger; a CI perf regression gate wired on these numbers (Task 20's own scope closes this, but Task 15 is where the FIRST measurement must be taken, not deferred to the sample task). |
| Zero validation errors on the new pass chain (sync validation) | Standing Phase 4→5 carried-forward global constraint (plan:57-60): *"zero validation errors with sync validation active."* | consume-now | N/A — standing rule, not a new claim; flagged because a compute-then-graphics pass chain (T14's froxel-assignment compute pass feeding this ticket's forward-lighting graphics pass) is a GENUINELY NEW barrier shape for this codebase's render graph (Task 2's own compute-pass barrier derivation is itself new, unexercised-in-production infrastructure at the time T15 consumes it). | Acceptance criterion: the froxel-buffer write (T14's compute pass) → froxel-buffer read (this ticket's forward pass) dependency is expressed through the render graph's OWN barrier-derivation mechanism (no hand-rolled `vkCmdPipelineBarrier` in sample/pass-orchestration code, per the standing "compute passes go through the render graph's compute-class pass machinery" rule, plan:109-111) — validated with sync validation active, both drivers, per the standing real-GPU-verification corrective. |

---

## Open Questions

1. **None genuinely novel to THIS ticket beyond what T14/Task 1 already
   own.** T15 is primarily an INTEGRATION ticket — its two real open
   questions (depth-prepass policy, froxel data-structure capacity) are
   both already-flagged dependencies on Task 1's spec ruling and T14's own
   Open Question #2, respectively, not new forks this ticket introduces.
   Recorded here (rather than omitting an Open Questions section) to make
   explicit that T15's own scope is CLEAN pending those two upstream
   resolutions — the coordinator should confirm both are actually settled
   (in the Task 1 spec, and in T14's delivered PR) before T15 dispatches,
   since T15 cannot independently discover a T14-shaped defect except via
   its own scaling-numbers row (see matrix row above) — which only catches
   it AFTER the fact, at 5k lights, not at design time.

## Verification health

**Verified first-hand this session:** `draw_data.h`, `material.slang`,
`forward_entry.slang`'s cited call site were read directly from the
working tree 2026-08-20. Filament's `evaluatePunctualLights()`/`getLight()`
(surface_light_punctual.fs:117-225) were fetched and read in full at the
pinned commit as part of the SAME fetch used for matrix-p5t14 (not a
second, independent fetch — no risk of commit drift between the two
matrices' citations of the same file).

**Not independently re-verified:** `surface_light_directional.fs` (cited
by name only, to corroborate "directional stays a separate path in
Filament too") — not fetched this session; the claim it supports is
already independently true from the plan's own explicit text and this
project's own existing `RxDrawData` shape, so the Filament citation is
corroborating, not load-bearing.
