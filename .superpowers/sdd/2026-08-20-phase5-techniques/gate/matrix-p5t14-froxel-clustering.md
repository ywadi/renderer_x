# Completeness matrix — P5 T14 (issue #50): Froxel grid + clustered light assignment (compute; Filament port)

**Plan task:** Task 14, Stage 2 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:487-508`).
**Charter binding:** priority (3), *"Lighting: clustered Forward+ (Filament
froxel reference — its compute-shader light-assignment is published GLSL to
translate; translate GLSL→Slang)"*
(`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:414-416`).
Also binds Task 30 (Stage 4 volumetrics) as a second, day-one-designed
consumer of the SAME grid — this ticket's layout/binding decisions are not
single-consumer-scoped.

**Sources consulted (in-repo, 2026-08-20):** `src/rx_scene/include/
rx_scene/draw_list.h` (parallel-determinism precedent — "chunk-ordered
concatenation," fixed index-range partitioning, `--threads`-invariant
byte-identical output, the discipline this ticket's own compute-side
determinism criterion must match in spirit); Task 2 (compute-pipeline
capability, prerequisite, not yet landed at gate-research time — this
ticket is a Task-2 CONSUMER, first production one alongside Task 9's IBL
bake).

**Sources consulted (external, fetched 2026-08-20, pinned commit
`721ec800093de984cbee155e459298b6b2dbb855`, `google/filament`):**
`filament/src/Froxelizer.h` (full file, 321 lines), `filament/src/
Froxelizer.cpp` (full file read in relevant sections, 1060 lines),
`shaders/src/surface_light_punctual.fs:1-91` (froxel-buffer READ side,
GLSL, fetched in full).

---

## CRITICAL FINDING — the charter's premise is not accurate at the pinned commit

**Filament's froxelization is CPU-side, not a GPU compute shader.** The
charter's own text ("its compute-shader light-assignment is published GLSL
to translate") does not match what exists in Filament's current source at
the pinned commit. Verified directly:

- `Froxelizer::froxelizeLights()` → `froxelizeLoop()` →
  `froxelizePointAndSpotLight()` (Froxelizer.cpp, called from
  Froxelizer.h:131,220-226) run on the **CPU**, job-scheduled and
  SIMD-vectorized ("chosen so `froxelizePointAndSpotLight()` vectorizes 4
  froxel tests / spotlight," Froxelizer.h:168-169) — there is no
  `GL_COMPUTE_SHADER`/dispatch call anywhere in this file (grepped
  directly: zero hits for `compute`/`CS_`/`dispatch`/`workgroup` as a
  GPU-stage marker; the only "compute" hits are ordinary C++ method names
  like `computeFroxelLayout`/`computeLightTree`).
- The result (`mRecordsBuffer`/`mFroxelsBuffer`, a compressed
  per-froxel-index → light-index-list encoding, `FroxelEntry{offset,count}`
  packed into a `uint32_t`, Froxelizer.h:63-83,152-160) is **uploaded** to
  the GPU (`commit()`, Froxelizer.h:145) as a plain data buffer.
- `shaders/src/surface_light_punctual.fs` is the shader-side **consumer**
  of that buffer (`getFroxelParams()`/`getLightIndex()`,
  surface_light_punctual.fs:69-91) — it does zero light-assignment work
  itself, only reads what the CPU already computed.
- A GitHub code search across `google/filament` for `froxel`+`.comp` and
  `froxelize`+`compute` (2026-08-20) returns no compute-shader
  froxelization file anywhere in the repository.

This is not a documentation-vs-code discrepancy of the kind CLAUDE.md
warns about (the June-2026 clearcoat doc case) — it is the **charter's own
prose** describing a GPU compute pass Filament's actual, current source
does not have. See Open Questions #1 for the resolution this gate
recommends.

---

## The matrix

| Feature | First-tier precedent (cited) | Disposition | Library/source support (verified) | Acceptance criterion |
|---|---|---|---|---|
| Grid layout: tile-based XY, exponential/logarithmic Z slicing | Froxelizer.cpp:457 (`linearizer = log2(zFar/zNear) / (froxelCountZ-1)`), `findSliceZ()` (Froxelizer.cpp:585-592, quoted): `s = int(fast::log2(-viewSpaceZ/zFar) * mLinearizer[1] + froxelCountZ)` — the classic Ola Olsson-style exponential Z-slice distribution (denser slices near the camera, where perspective foreshortening makes screen-space froxels smallest in world space). | consume-now | VERIFIED via direct source read + the file's own derivation comment (Froxelizer.cpp:517-522: `i = log2(z_screen*(far/near)) * (-1/linearizer) + zcount`). XY tiling is power-of-two-dimensioned, sized from a target froxel-buffer entry count (`computeFroxelLayout()`, Froxelizer.cpp:277-303/Froxelizer.h:252-254) — grid resolution is DERIVED from a buffer-size budget, not a fixed tile-pixel-size constant. | Device-free math test (plan's own named criterion, plan:499-500): construct a froxel grid from known near/far/count parameters, assert `findSliceZ()`-equivalent index↔slice round-trips for a table of view-space Z values spanning the full range (including the near-plane and far-plane boundary cases), and that the SAME formula used to build a test light's slice range at authoring time matches the fragment-shader-side `getFroxelCoords()` formula (surface_light_punctual.fs:23-43) bit-for-bit in derivation (not just "close"). |
| Per-froxel light-list DATA STRUCTURE and its 256-light ceiling | Froxelizer.h's own ASCII diagram (:62-83): a `LightRecord` bitset (`utils::bitset<uint64_t, (CONFIG_MAX_LIGHT_COUNT+63)/64>`, Froxelizer.h:186) accumulates, per froxel, WHICH of up to 256 lights (`"256 lights max"`, Froxelizer.h:82) touch it; `froxelizeAssignRecordsCompress()` (Froxelizer.h:223) then compresses each froxel's bitset into a contiguous `[offset,count]` run in the record buffer. | **content-scale conflict — see Open Questions #2** | VERIFIED: `CONFIG_MAX_LIGHT_COUNT` bounds the light UBO Filament's froxelizer indexes into (Froxelizer.h:186's own bitset sizing) — a fixed, small-scene ceiling BY DESIGN (the bitset-per-froxel representation is only tractable at ~256 lights; it does not scale to "thousands"). | See Open Questions #2 — this row's "acceptance criterion" is the DECISION the coordinator must make before Task 14 can be scoped, not a test. |
| Determinism of per-froxel light-list CONTENTS across runs | RendererX's own `DrawListBuilder` precedent (draw_list.h:19-28,553-599): "chunk-ordered concatenation... never completion order," `--threads`-invariant byte-identical output — the CPU-parallel-culling discipline this project already enforces. Filament's OWN mechanism (bitset-accumulate-then-compress, see row above) achieves an analogous property FOR FREE: a light's presence in a froxel's record-buffer run is ordered by BIT INDEX (light index), never by job-completion/insertion race order — `froxelizeAssignRecordsCompress()` is a deterministic, order-independent compress over an already-complete bitset, not an atomic-append accumulator. | consume-now (the DESIGN PRINCIPLE, not the bitset mechanism itself — see Open Questions #2) | N/A — general parallel-determinism principle, cross-referenced against this project's own existing `DrawListBuilder` standard (matrix-issue06-drawlists-culling.md's "Determinism across thread counts" row, Phase 4) and independently corroborated by Filament's own unrelated design choice solving the identical problem a different way. | GPU test: dispatch the SAME light set + camera through the froxel-assignment compute pass TWICE (or with differing dispatch/workgroup-size configurations, if the implementation exposes any), assert the per-froxel light-list buffer readback is BYTE-IDENTICAL both times — the compute-pipeline analogue of `DrawListBuilder`'s own `--threads`-invariance test, and a genuinely new methodology this codebase has not needed before (its only prior compute-adjacent parallel-determinism precedent is CPU-side). A naive atomic-counter-append implementation (the common alternative to Filament's bitset approach, see Open Questions #2) is the design most likely to accidentally FAIL this test — flagging it explicitly so an implementer chooses an append order that survives it (e.g. sorting appended entries by light index post-hoc, or a two-pass count+prefix-sum+scatter compaction, the standard GPU-compute idiom for order-independent output). |
| GPU membership test cases (corner/spanning/behind-camera lights) | Plan's own named acceptance criterion (plan:502-503): *"synthetic light sets → readback of per-froxel lists asserts EXACT membership for hand-computed cases (corner lights, spanning lights, behind-camera culls)."* Froxelizer's own `froxelizePointAndSpotLight()` (Froxelizer.cpp, per-light-per-froxel sphere/cone test against precomputed froxel bounding volumes, `updateBoundingSpheres()` Froxelizer.cpp:232-237,330-376) is the algorithmic reference for WHICH geometric test to port (sphere-vs-froxel-bounding-sphere for point lights, an analogous cone test for spot). | consume-now | VERIFIED: Froxelizer.cpp:330-376 computes a bounding SPHERE per froxel (not just an AABB) specifically to support cheap point/spot-light-vs-froxel intersection tests (the comment at :331: "needed for spotlights"). | Exact-membership unit/GPU test battery, per the plan's own three named cases: (1) a light in the exact CORNER froxel of the grid is assigned only there; (2) a light whose radius spans multiple adjacent froxels is assigned to ALL of them, none omitted; (3) a light positioned BEHIND the camera (negative view-space Z) is excluded from every froxel — the near/far Z-slicing formula's own domain boundary (row above) must not wrap/alias a behind-camera light into slice 0. |
| Capacity+1 behavior (content-scale rule) | CLAUDE.md/plan's own standing rule (plan:82-87): *"every capacity a task declares... gets a test PAST it — behavior at capacity+1 is loud and defined, never corrupt."* Directly named for this ticket (plan:504-505): *"max lights per froxel / total... counters exact and CI-gateable."* | consume-now | N/A — project-standing rule, not a library claim; concretized here against the SPECIFIC capacity the coordinator picks per Open Questions #2 (whatever RendererX's own light-buffer/per-froxel-list ceiling ends up being — almost certainly NOT Filament's literal 256, per that Open Question). | GPU test: a synthetic scene with exactly (declared-max + 1) lights all overlapping ONE froxel — the froxel's own list is TRUNCATED (not corrupted/wrapped/UB) at the declared max, a counter reports the overflow count exactly, and every OTHER froxel's unrelated light lists are unaffected (proving the overflow is contained, not a buffer-wide corruption). |
| Slang compute-shader mechanics (workgroup size, dispatch, storage-buffer binding) | Falcor patterns (charter's own named "how-to-express-it-in-Slang" reference — not independently re-fetched this session, see Verification health) + this project's OWN Task 2 (compute-pipeline capability), which this ticket is a direct CONSUMER of. | **hard dependency, not this ticket's own scope** | Task 2 has not landed at gate-research time — VERIFIED (2026-08-18 grep, carried from the plan's own Task 2 text) zero `vkCreateComputePipelines`/`vkCmdDispatch` exist in this codebase today. | Not this ticket's acceptance criterion — flagged so the coordinator confirms Task 2's compute-PSO/dispatch/reflection surface is ACTUALLY sufficient for a two-pass (or count+scatter) compute froxelizer BEFORE Task 14 dispatches (e.g. does Task 2's API support multiple sequential compute dispatches with a barrier between them, needed for any prefix-sum/compaction approach — see Open Questions #2). |
| Grid shared with Task 30 (volumetrics) from day one | Plan's own explicit binding (plan:492-494): *"the grid's layout/bindings are authored for two consumers from day one (a Task 1 spec decision records the shared shape)."* | consume-now, dependent on the Task 1 spec ruling | N/A — this gate does not itself author the shared-shape ruling (that is explicitly Task 1's own coordinator-authored spec decision, per the plan's own text) — flagged here only so Task 14's OWN implementation does not bake in an assumption (e.g. "froxel buffer only ever read by opaque lighting") that would force Task 30 to duplicate the grid later. | Acceptance criterion: the froxel grid's own compute-pass OUTPUT (the per-froxel light-list buffer, or an equivalent structure) is a standalone, independently-bindable resource — not embedded inside a larger pass-specific descriptor set only the opaque-lighting pass can reach — so Task 30's froxel-marched fog can bind the SAME buffer without Task 14's own code needing to change. |

---

## Open Questions

1. **The charter's "port Filament's compute-shader froxelizer" premise does
   not match Filament's actual current source (CPU-side, not compute) —
   see the CRITICAL FINDING above.** This is not a minor framing slip: it
   changes what "port" even means for this ticket. **Recommendation: treat
   Filament's `Froxelizer.cpp` as the ALGORITHM/DATA-LAYOUT reference (grid
   sizing math, exponential Z-slicing formula, bounding-sphere-per-froxel
   intersection test, record-buffer compression SHAPE), and independently
   design the GPU-compute EXPRESSION of that algorithm** — this is squarely
   still "port, don't reinvent" in spirit (the hard geometric/numerical
   work is Filament's, verified and cited above), just not a literal
   line-for-line GLSL→Slang translation, because no GLSL compute source
   exists to translate. Falcor (the charter's own named "how to express it
   in Slang" reference) or a well-known clustered-forward compute writeup
   (e.g. Doom 2016's public GDC talk on GPU-driven clustered light
   culling, or Ola Olsson's own "Practical Clustered Shading" paper —
   Filament's Froxelizer.cpp does not cite either by name in-file, but the
   exponential-Z-slice technique is the same one both describe) is a
   reasonable secondary source for the COMPUTE-SHADER EXPRESSION
   specifically, if the coordinator wants a second citation beyond this
   gate's own derivation. Recorded as a phase-fit correction, not a
   silent drop: the plan's own Task 14 text ("ported GLSL→Slang from
   Filament's published froxelizer") should be corrected at spec-authoring
   time to "ported from Filament's froxelization algorithm, expressed as a
   new compute shader" — a materially different (smaller but real) scope
   statement.

2. **Filament's 256-light bitset ceiling directly conflicts with the
   charter's own "hundreds-to-thousands of local lights" target** (plan:490,
   charter :417) and Task 15's own named scaling test (100/1k/5k lights,
   plan:523). A literal port of Filament's per-froxel `LightRecord` bitset
   (`utils::bitset<uint64_t,(256+63)/64>` — a FIXED 256-bit-wide structure,
   Froxelizer.h:186) cannot represent "thousands" of candidate lights at
   all, let alone assign them. **Recommendation: do NOT port the bitset
   data structure — use a wider-capacity GPU-compute idiom instead**: the
   standard two-pass "count then scatter" (prefix-sum/compaction) approach
   used by most GPU-compute clustered-forward implementations at
   thousands-of-lights scale (per-froxel light COUNT computed in pass 1,
   an exclusive prefix-sum over per-froxel counts produces each froxel's
   offset into a global light-index buffer, pass 2 scatters light indices
   into their froxels' ranges) — this generalizes to an arbitrary total
   light count bounded only by the global light-index buffer's own
   declared capacity (itself a content-scale-tested capacity per the
   matrix row above), unlike Filament's fixed 256-bit-per-froxel
   representation. This is the single most consequential, load-bearing
   decision in this ticket's entire scope — it determines the compute
   pass's whole shape (one dispatch vs. two with a barrier) and should be
   a named Task 1 spec decision (D-series), not left implicit.

## Verification health

**Verified first-hand this session:** `Froxelizer.h` (full file) and the
cited sections of `Froxelizer.cpp` (grid layout, Z-slicing math,
bounding-sphere computation, class-level ASCII diagram) were fetched
directly from `google/filament` at commit
`721ec800093de984cbee155e459298b6b2dbb855` and read in full/relevant
sections — not search-digested. The "CPU, not compute" finding is a
direct grep result against the fetched files (zero `compute`/`CS_`/
`dispatch`/`workgroup` GPU-stage markers) corroborated by a GitHub code
search across the whole `google/filament` repository for a
froxelization compute shader (zero hits) — both run this session, not
inherited. `shaders/src/surface_light_punctual.fs` (froxel-buffer
read-side) fetched and read in full.

**Not independently re-verified:** Falcor's own "how to express clustered
light assignment in Slang" pattern (the charter's named reference for
this specific translation problem) was not fetched this session — Open
Question #1's recommendation names it as a reasonable secondary source
but does not depend on it (this gate's own derivation from Filament's
Froxelizer.cpp + the standard prefix-sum/scatter GPU idiom is
self-sufficient). Whether Task 2's compute-pipeline API (not yet built)
actually supports the multi-dispatch-with-barrier shape a prefix-sum
compaction needs was flagged as a dependency, not resolved here (out of
this ticket's own scope to decide Task 2's API shape).
