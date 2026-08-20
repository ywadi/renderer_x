# Completeness matrix — P5 T29 (issue #65): God rays — volumetrics tier (a)

**Plan task:** Task 29, Stage 4 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:792-806`).
**Charter binding:** volumetrics ladder tier (a), *"screen-space radial god
rays (cheap post pass, expressible against the Phase 3 graph today) as the
entry tier"* (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:433-446`);
frame-pipeline slot *"...opaque lighting → volumetrics (froxel march + apply)
→ SSR → ... → bloom → tone mapping..."* (same file, :447-451) — tier (a) sits
at the same pipeline slot as tier (b) (Task 30), both pre-tonemap.

**Sources consulted (in-repo, 2026-08-20):**
- `src/rx_graph/include/rx_graph/pass.h:1-360` — Graphics-class fullscreen
  pass API (`addColorOutput`/`addTextureInput`), confirmed sufficient for a
  post pass with zero new graph primitives (see row 4).
- `shaders/multipass/tonemap.frag.slang` (full file, 19 lines) — the
  fullscreen-triangle post-pass pattern this codebase already ships
  (sampled HDR input → per-pixel output), the structural precedent T29's
  pass follows; also the CURRENT tonemap this task must sit upstream of at
  its own dispatch time (see row 5).
- `src/rx_scene/include/rx_scene/camera.h:64-186` — `Camera::viewProj()`,
  the source of a world-space sun direction's screen-space projection this
  pass needs; no engine-side "project world point to screen UV" helper
  exists yet as a named API (grep: zero hits for a dedicated
  world-to-screen helper outside ad hoc per-sample math) — flagged as a
  small, in-task API-gap row (row 2).
- `src/rx_core/include/rx_core/profile.h` — `RX_ZONE`/`RX_PLOT` (Tracy
  v0.14.0, vendored `third_party/CMakeLists.txt:302-331`, confirmed real,
  not a stub) — the cost-measurement mechanism the ticket's "cost bounded +
  measured" criterion rides.
- Repo-wide grep, 2026-08-20: zero hits for `godray`/`crepuscular`/
  `lightshaft`/`light_shaft`/`sun_shaft`/`volumetric` under `shaders/` or
  `src/` — confirms this is a fully greenfield task with no in-repo partial
  implementation to build on or contradict.

**Sources consulted (external, fetched 2026-08-20):**
- GitHub code search (`gh api search/code`, 2026-08-20) across
  `google/filament`, `NVIDIAGameWorks/Falcor`, `godotengine/godot`,
  `bevyengine/bevy`, `o3de/o3de` for `godray`/`crepuscular`/`"light shaft"`/
  `sun_shafts`/`LightShaft` — **zero matches in every repo.** None of the
  four charter-named or otherwise-permissive reference engines ships a
  redistributable screen-space radial-scattering post effect under any of
  its common names.
- NVIDIA GPU Gems 3, Chapter 13, "Volumetric Light Scattering as a
  Post-Process" (Kenny Mitchell, 2008) — the canonical published algorithm
  for exactly this technique (occlusion-mask input → radial sample march
  toward the light's screen-space position → decay/weight/exposure
  accumulate). Publicly available NVIDIA developer documentation, algorithm
  description only — no redistributable source file ships with it.

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/port-source support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------|-------------------------------|
| 1 | Radial-march light-scattering kernel (occlusion mask → N samples toward light's screen-space position, per-sample decay/weight, exposure scale) | NVIDIA GPU Gems 3 Ch. 13 (Mitchell, 2008) — the standard, widely-cited technique; **no ready-made permissively-licensed CODE port source exists** (see Sources: zero hits across Filament/Falcor/Godot/Bevy/O3DE). | consume-now — **explicit from-scratch call, per CLAUDE.md's own carve-out** ("only write it from scratch when no reasonable ready-made option exists, and say so explicitly") | N/A — no library/engine ships this as redistributable code under a compatible license at the time of this search. | Implement the Mitchell 2008 algorithm directly in a `shaders/volumetric/godrays.slang` fullscreen pass; unit-cite the source in the shader's header comment (as this codebase already does for e.g. Gribb-Hartmann in `camera.h:76-78`) rather than presenting it as ported code. |
| 2 | World-to-screen projection of the sun direction (the pass's own "light screen position" input) | N/A — internal engine-API gap, not a renderer precedent. | consume-now | Verified: `Camera` (`rx_scene/camera.h`) exposes `viewProj()`/`cullingViewProj()` but no named "project point/direction to NDC or screen UV" helper; every existing sample does this ad hoc inline (not a reusable API). A directional light has no world position, so the pass needs the sun's direction projected as a point-at-infinity (`viewProj() * float4(dir, 0)`, then perspective-divide) — a small, cheap, single-function addition. | A device-free unit test asserts the projected screen position for a handful of known (camera, sun-direction) pairs against hand-computed NDC/UV expectations, including the degenerate case (row 3) where `w <= 0` (light behind the camera plane). |
| 3 | Degenerate handling: sun behind camera / fully off-screen | N/A — engine-specific contract. | consume-now | N/A — contract, not a library question. | When the projected sun position's `w <= 0` (behind the camera's near plane) or its NDC.xy falls far enough outside `[-1,1]` that no on-screen occluder geometry could contribute, the pass short-circuits to an identity copy of its input — byte-stable gate, discriminated by a paired test that DOES show shafts when the sun re-enters a visible configuration (reuses the ticket's own occluder-shaft discrimination probe with the camera/sun pose swapped). |
| 4 | Graph integration: Graphics-class fullscreen pass (NOT Compute-class) | N/A — internal graph-API question. | consume-now | Verified: `Pass::addColorOutput`/`addTextureInput` (`pass.h:55,65`) are delivered TODAY (Phase 3) and are exactly what a fullscreen-triangle radial-scatter pass needs — **this task has NO dependency on Task 2's compute-pipeline capability**, unlike Task 30's froxel march. `shaders/multipass/tonemap.frag.slang` is the live in-repo precedent for the same "sample one input texture, write one output" pass shape. | Pass expressed entirely through existing, already-delivered `rx_graph` primitives; zero new graph API surface required (a scope-shrinking finding worth recording explicitly so the task isn't over-scheduled against Task 2). |
| 5 | Pipeline placement: "applied before tonemap" | Charter frame-pipeline text (`...bloom → tone mapping...`, spec:447-451) places both volumetrics tiers pre-tonemap; T29 is scheduled `independent` within Stage 4 (plan:989) and MAY land before Task 32 replaces the Phase 4 utility tonemap. | consume-now | Verified: today's only tonemap pass is `shaders/multipass/tonemap.frag.slang`'s plain Reinhard (`c/(1+c)`) — the pass T29 must sit upstream of at ITS OWN dispatch time is whichever tonemap pass currently exists in the graph, not necessarily Task 32's AgX/ACES replacement. | Acceptance text should say "wired immediately upstream of the scene's current tonemap pass" rather than naming Task 32's tonemapper by name, so the criterion holds regardless of T29/T32 relative landing order (both are independent within Stage 4 per the plan's own sequencing notes, plan:989-990). |
| 6 | Cost budget: measured + published | N/A — tooling precedent already delivered. | consume-now | Verified real: Tracy v0.14.0 vendored (`third_party/CMakeLists.txt:302-331`), `RX_ZONE`/`RX_PLOT` macros live (`rx_core/include/rx_core/profile.h:42-110`). | Per-pass Tracy zone + a published desktop timing number at the task's own checkpoint (carried forward into Task 36's benchmark rows), following the same "measured, not asserted" mandate CLAUDE.md and every other Stage-4 ticket already requires. |

---

## Conflicts

None against the plan/charter text (the plan itself only says "expressible
against the Phase 3 graph today" and names no specific port source for
tier (a) — unlike its explicit Filament/Falcor/LTC naming elsewhere — so
the absence of a ready-made source is not a contradiction of anything
written, just a fact this gate surfaces before dispatch so the from-scratch
call is made deliberately and recorded, not discovered mid-task).

## New gaps

- **World-to-screen projection helper** (row 2): a small, reusable
  `Camera`-adjacent utility with no current home. Not large enough to
  warrant its own ticket; recommend it lands inside T29 itself and gets
  reused by Task 26 (SSR, Stage 3 — mirror-plane alignment probes need the
  same projection) if Task 26 has not already grown one by the time T29
  dispatches (sequencing note: T26 lands in Stage 3, strictly before
  Stage 4, so by T29's dispatch time this may already exist — implementer
  should check before re-adding it).

## Open Questions (for the coordinator's binding ruling)

1. **From-scratch implementation for god rays.** No permissively-licensed
   ready-made source exists in any of the five searched reference engines
   (Filament/Falcor/Godot/Bevy/O3DE) for screen-space radial light
   scattering. **Recommendation: rule this an explicit from-scratch
   implementation** citing NVIDIA GPU Gems 3 Ch. 13 (Mitchell, 2008) as the
   algorithm source in the shader's header comment, per CLAUDE.md's own
   "only write it from scratch when no reasonable ready-made option exists,
   and say so explicitly" carve-out — do not force a strained "port" from
   an unrelated Filament/Falcor file just to satisfy the port-don't-reinvent
   framing.
