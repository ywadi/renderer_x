# Completeness matrix — issue #15: Sample 09 (scene fly-through + stress-v2 + release prep)

## Header

- **Ticket:** #15, "Sample 09: scene fly-through + stress-v2 + release prep"
  (labels: `phase-4`, `stage-2`).
- **Original body:** "Plan Task 19 ... Sample 09_scene (phase exit):
  Registry->Scene->DrawListBuilder->graph; fly-through (mouse capture +
  gamepad); ImGui HUD (frame stats, cull counters, vsync toggle,
  layer/channel toggles, pool stats); --stress mode publishing A/B numbers
  vs sample 07's direct path; headless counter+tolerance gate;
  packaging/CI/MANUAL_VERIFICATION/README; then final review -> v0.4.0-phase4."
- **Correction (2026-08-18), treated as authoritative over the original body
  per the research-brief's instruction — the two differ only by making three
  items explicit that the original left implicit (quoted both below,
  Conflicts section notes the one place they diverge in emphasis, not
  substance):** "this card is **sample 09_scene** (plan Task 24), the phase
  finale: fly-through (mouse capture + gamepad), ImGui HUD (FPS, cull
  counters incl. instancing-collapse ratio, vsync/layer toggles, #27 memory
  report), `--stress` mode publishing A/B numbers vs sample 07's direct
  path, headless counter + tolerance-pixel gates, packaging, release
  v0.4.0-phase4."
- **Plan task:** Task 24, `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:452-455`
  (Steps: "TDD gate → implement → numbers (desktop; Deck rows added to
  MANUAL_VERIFICATION as unchecked) → packaging → commit(s)."). Sequencing
  (same file, "Execution notes" section, line 462): Stage 2 order is
  T18→T19→{T20,T21 parallel}→T22→T24; T23 (executor per-frame allocation
  elimination, card #29) is parallelizable but **must land before T24's
  stress-v2 numbers** since it directly affects the honesty of the A/B
  comparison's per-frame allocation profile.
- **Binding spec decisions (D-numbers) this ticket touches:** D5 (threading
  contract — main-thread GPU-object mutation), D7 (import core interfaces,
  Task 13), D8/D9 (GeometryPool growth/stats, consumed for pool-stats HUD),
  D10 (KTX2/sampler cache, Task 14), D11 (fallback assets), D12 (flattened
  world-transform InstanceRecords), D13 (reversed-Z main camera — migrates
  to the scene path in Task 22, consumed here), D14 (draw-list sort keys),
  D15 (frustum + shadow-caster culling, layer/channel masks), D16 (test
  content strategy — DamagedHelmet/Sponza sourcing), D17 (tolerance-based
  pixel gates), D18 (counters-gate/wall-clock-trend performance policy),
  D19 (scene data model), D20 (ImGui overlay, consumed from #16), D21
  (shadow quality bridge), D22 (StandardPBR/Unlit materials), **D24**
  (memory budget/eviction invariant — mandatory row below), **D25**
  (upload-ticket non-blocking flush — mandatory row below), **D26**
  (GPU-driven readiness invariants, especially D26.3 instancing collapse —
  mandatory row below), **D27** (main-thread pipeline pre-resolution before
  chunked fan-out — mandatory row below).
- **Sources consulted this session:**
  - `gh issue view 15 --json title,body,comments` (fetched directly this
    session; full body quoted above).
  - `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md` — Global
    Constraints (13-21), Task 7 (82-93, sample 07_stress baseline), Task 13
    (276-302, Registry/import interfaces), Task 14 (304-309, KTX2), Task 15
    (311-314, async import + D25 wall-clock assertion), Task 16 (316-326,
    StandardPBR/Unlit + sample 08_gltf_viewer + `tools/regen_references.sh`),
    Task 18 (350-366, Scene proxies + Camera), Task 19 (368-406,
    DrawListBuilder + D26 amendments), Task 20 (408-411, input), Task 21
    (413-417, ImGui overlay), Task 22 (419-422, shadow bridge), Task 23
    (424-450, executor allocation elimination), Task 24 (452-455, this
    ticket), Execution notes (459-466).
  - `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md` — D13
    through D27 (225-441), Stage exit criteria (442-end).
  - `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md` —
    the layer table (24-38) and the deferred-details "layer 8" paragraph
    (137-190), read in full for the registry-tick Conflicts finding below.
  - `.superpowers/sdd/2026-08-11-phase4-scene-assets/feature-gap-audit.md`
    — full file (FG1-FG12 table, "near-misses checked and found already
    covered" section, "judgment calls" section) for new-gap cross-check.
  - In-tree: `.github/workflows/ci.yml` (~195-220, sample 07_stress CI
    pattern), `tools/package_samples.sh` (full file, 219 lines),
    `MANUAL_VERIFICATION.md` (full file, 202 lines), `README.md` (70-102,
    Roadmap section), `tools/` directory listing (no `fetch_assets.sh` or
    `regen_references.sh` yet — both are Task 13/16 deliverables that must
    land before this ticket can consume them), `src/` directory listing
    (confirms Stage 1/2 do not exist in code yet: only `rx_core, rx_graph,
    rx_material, rx_platform, rx_rhi_vk, rx_shader, rx_task`).
  - `.superpowers/sdd/2026-08-11-phase4-scene-assets/claim-validation-2026-08-18.md`
    — claim #2 (culling/submission "already covered," sub-gap D26.3),
    claim #7 (executor hot-path allocations, new Task 23), consulted for
    the stress-v2 comparability-contract row's grounding.
  - `src/rx_platform/include/rx_platform/window.h` — full file read (31
    lines): confirms the ONLY input surface today is `pumpEvents()`,
    `sdlWindow()`, and Vulkan-surface helpers — no keyboard state query of
    any kind exists yet. Cross-checked against a repo-wide grep for
    `SDL_GetKeyboardState`/`SDL_SCANCODE` across `samples/`, which returns
    zero hits — no sample in this codebase polls the keyboard today (the
    existing orbit cameras in 06_materials/08_gltf_viewer are
    time-driven auto-orbits, not input-driven; verified in
    `samples/06_materials/main.cpp`). Grounds the new Conflicts entry
    below on fly-through keyboard movement.
  - `src/rx_rhi_vk/include/rx_rhi_vk/device.h` (lines 27-38, 103-118) —
    cross-checked `setPresentMode`/`presentMode()` declarations against
    `device.cpp` line numbers cited in row 21 below; both agree.
  - **Live external fetches this session** (first-tier-precedent grounding
    for rows 1, 8, 18, 24-26 — upgrades the prior draft's "not fetched,
    relying on general knowledge" posture to directly quoted source):
    `github.com/bkaradzic/bgfx` `examples/05-instancing/instancing.cpp`
    (real quotes: instanced-vs-per-cube-loop A/B structure, on-screen
    `bgfx::getStats()->numDraw` draw-call HUD counter, "Draw call limit
    reached!" / "Couldn't draw %d cubes last frame" overflow warnings);
    `github.com/google/filament` `samples/gltf_viewer.cpp` (real quotes:
    ImGui-based `ViewerGui` HUD showing entity/renderable/skipped-frame
    counts and a `PlotLinesSeries` frame-timing history plot, a
    `camutils::Manipulator`-driven camera rather than bespoke fly-through
    code, and `--batch`/`--screenshotAsPPM`/`--settings` flags driving an
    `AutomationEngine` for headless/automated multi-frame runs — the
    closest published first-tier analogue to this ticket's own headless
    counter+tolerance-pixel gate). Two further fetches (Filament's
    `Materials.md.html` doc and its GitHub release notes, and Godot's
    debugger-overview and MultiMesh-fish docs) returned no usable content
    on automatic-instancing/HUD-monitor specifics for THIS ticket's
    claims — not cited as precedent for any row; recorded here so the
    absence is visible rather than silently dropped.

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-4 disposition | Library support (verified, cited) | Proposed acceptance criterion |
|---|---|---|---|---|---|
| 1 | Scene composition — headless instanced DamagedHelmet grid | bgfx `examples/05-instancing/instancing.cpp` (bkaradzic/bgfx, fetched and quoted this session): submits a single instanced draw call for the whole grid versus a nested-loop per-cube path ("Without instancing, it generates one draw call per cube"), and the on-screen debug overlay literally prints `"%d draw calls", bgfx::getStats()->numDraw` plus overflow warnings ("Draw call limit reached!", "Couldn't draw %d cubes last frame") — the canonical "GPU instancing showcase with a visible draw-count HUD counter" pattern in a first-tier open renderer, directly analogous to this row's grid and to the HUD's collapse-ratio/draws-submitted counters (rows 11, 19). | consume-now | UNVERIFIED — not yet implemented; grounded in spec text only (D26.3, `...design.md:408-412`: "runs of identical (pipeline, material, mesh range, block) collapse into instanced draws... counters report records-in vs draws-submitted, CI-gateable"). No code exists yet (`src/rx_scene`, `src/rx_asset` absent from the tree). | Headless run of `sample_09_scene` (default args) imports the field once, asserts `recordsIn > drawsSubmitted` (collapse actually happened, not just "same count coincidentally"), and asserts `drawsSubmitted` is deterministic frame-to-frame for a static camera (same test posture as sample 07's exact-count assertions). |
| 2 | Scene composition — present-mode Sponza (`--scene sponza`) | Sponza is the de facto standard "hero present-mode" scene across Filament, bgfx, Godot demo projects, and Khronos' own glTF-Sample-Viewer — a scene complex enough to be a real stress/quality showcase, not a toy. | consume-now | Verified sourcing decision only: D16 (`...design.md:263-270`) — "Sponza for local/present-mode 'wow' ... CI never downloads Sponza"; fetch mechanism is Task 13's `tools/fetch_assets.sh` (plan line 278: "DamagedHelmet mandatory + `--sponza` optional; checksums; CI caches like slang-prebuilt") — **this script does not exist yet** (`tools/` listing today: `check_build_budget.sh, dep_cache_smoketest, fetch_slang.cmake, fetch_slang_test.sh, package_samples.sh, toolchain_check`). Sample 09 has a hard precondition on Task 13 landing first (already sequenced correctly: T13 is Stage 1, T24 is Stage 2). | `--scene sponza` fails loudly with a clear "run `tools/fetch_assets.sh --sponza` first" message (not a crash/hang) when the asset is absent; when present, loads and renders without validation errors; CI never exercises this path (matches D16's explicit CI-never-downloads-Sponza rule) — only the headless DamagedHelmet-grid path is CI-gated. |
| 3 | Full pipeline exercise — Registry/import (`rx_asset::Registry::importGltf`) | Every first-tier engine's "capstone sample" loads real content through the actual production import path, not a shortcut loader (Filament's `gltf_viewer`, Godot's own scene importer used by every demo project). | consume-now | UNVERIFIED — Task 13 not yet implemented; interface per plan (`...plan.md:281-289`): `Registry::importGltf(path, GeometryPool&, TextureCache*)`. | Sample 09's startup log (or a `--stats` flag) prints import counts (meshes/submeshes/instances) that the headless gate asserts against the known DamagedHelmet-grid composition (N helmets × submesh count) — a silent/unverified import doesn't satisfy "visibly exercised." |
| 4 | Full pipeline exercise — GeometryPool residency/pool stats | Filament's `RenderableManager`/backend pooling exposes pool occupancy stats to its own ImGui-based sample tooling as standard practice. | consume-now | UNVERIFIED — GeometryPool (Task 12) not yet implemented; D8/D9 referenced in Task 12's heading only (plan line 244) — full pool-stats contract not read in this session (out of my ticket's required-reading scope), flagged here as a dependency, not independently verified. | HUD "pool stats" panel shows at minimum block count and bytes-committed for the geometry pool, sourced from a real accessor (not a hardcoded/mocked number) — headless test asserts the reported bytes are nonzero after import and unchanged across static frames (no per-frame growth = no leak). |
| 5 | Full pipeline exercise — Uploader tickets (D25 non-blocking flush) | bgfx's async resource-creation model (submit now, fence-poll completion) is the named precedent D25 itself cites implicitly via the "non-blocking flush invariant" framing; this is standard practice in every engine that streams assets without stalling the render loop. | consume-now | UNVERIFIED — Task 11 not yet implemented; contract per D25 (`...design.md:369-389`): `flush()` returns a pollable `UploadTicket`; `isComplete()/wait()` added; `MeshBuffers::create` keeps blocking behavior explicitly via `wait(ticket)`. | Sample 09's import path (both sync startup import and, if async import is exercised, Task 15's `importGltfAsync`) never calls the old unconditional `vkWaitForFences(...)` pattern directly — a wall-clock main-thread-block assertion (per D25's own added criterion on Task 15) carries forward into sample 09's frame loop: no single frame's `pumpMain()`/upload-poll call blocks beyond a documented threshold, asserted in the headless gate. |
| 6 | Full pipeline exercise — KTX2 TextureCache + sampler cache | Godot/Filament both route all runtime textures through a single format-negotiating cache with sampler deduplication; DamagedHelmet's own textures (baseColor/normal/MR/AO/emissive) are the standard PBR round-trip test set (used by Khronos' own sample viewer). | consume-now | UNVERIFIED — Task 14 not yet implemented. | DamagedHelmet's five glTF textures visibly affect rendered output (a tolerance-pixel probe on the helmet distinguishes it from an untextured/fallback-gray render) — reuses D17's methodology, not a new one. |
| 7 | Full pipeline exercise — StandardPBR/Unlit materials, alpha modes | Same DamagedHelmet-as-PBR-reference-asset precedent as row 6; Sponza additionally exercises alphaMode MASK (foliage/fabric cutouts) and BLEND (some Sponza variants have glass), which DamagedHelmet alone does not. | consume-now | UNVERIFIED — Task 16 not yet implemented; D22 (`...design.md:322-336`) defines the material feature set (baseColor/MR/normal/occlusion/emissive/alphaMode/doubleSided + interim flat-ambient term per FG1 amendment). | Sample 09 (with Sponza present-mode) visibly renders at least one MASK-cutout surface and, if the fetched Sponza variant has one, one BLEND surface, without a MANUAL_VERIFICATION row that only ever exercises DamagedHelmet's OPAQUE-only material set — otherwise MASK/BLEND remain untested by this ticket despite being D22-committed features (flag as a scope gap if the chosen Sponza asset variant lacks both). |
| 8 | Full pipeline exercise — Scene proxies (RenderableManager/TransformManager/LightManager) | Filament precedent explicitly named in D19 itself (`...design.md:293-305`: "`src/rx_scene` implements Filament-precedent managers"); independently corroborated this session by fetching Filament's own capstone sample, `samples/gltf_viewer.cpp` — its camera is delegated to a `camutils::Manipulator` rather than bespoke per-sample code (the manager/handle-driven pattern D19 commits to), and its own HUD ("Stats" ImGui panel: entity count, renderable count, skipped-frame count) is exactly the shape row 19's HUD proposes. | consume-now | UNVERIFIED — Task 18 not yet implemented; interface per plan (`...plan.md:350-364`): `Scene::createRenderable(RenderableDesc)`, `setTransform`, `setLayers`, light equivalents. | Sample 09's fly-through camera changing frame-to-frame drives `setTransform` calls at minimum for its own camera-relative debug gizmos if any, and definitely exercises `createRenderable` for every imported instance at startup — headless test asserts renderable count matches import count from row 3. |
| 9 | Full pipeline exercise — Camera reversed-Z | D13 (`...design.md:225-234`): "Camera projection uses reversed-Z... `rx::scene::Camera` owns the projection helpers so samples cannot get it inconsistently wrong." Migration to the scene path is explicitly Task 22's job ("Reversed-Z main-camera migration lands here for the scene path," plan line 421), landing before Task 24. | consume-now | UNVERIFIED — Task 18 (Camera helpers) and Task 22 (migration) not yet implemented. Sample 08_gltf_viewer explicitly does NOT get reversed-Z yet (plan line 318: "reversed-Z NOT yet — camera helpers arrive Stage 2") — sample 09 is the FIRST sample to ship it. | Sample 09's depth-attachment clear value is 0.0 and its depth compare op is GREATER_OR_EQUAL (matches D13's stated convention) — a device-free or GPU test asserts the pipeline state directly, not just "it looks right," since a silently-wrong reversed-Z convention still often renders plausibly. |
| 10 | Full pipeline exercise — DrawListBuilder frustum + shadow-caster culling | D15 (`...design.md:247-262`): camera-plane AABB culling, ortho-fitted shadow frustum with conservative caster extrusion, layer (`u32`)/channel (`u8`) masks. | consume-now | UNVERIFIED — Task 19 not yet implemented. | Sample 09's HUD-displayed "culled" counter is nonzero for at least one camera position in the default headless composition (proves culling is actually active, not a pass-through no-op) — the headless gate asserts an exact culled/visible split for the fixed startup camera+scene, same posture as row 1's collapse assertion. |
| 11 | Full pipeline exercise — DrawListBuilder sort + **D26.3 instancing collapse** | Same Filament/bgfx precedent as rows 1 and 8. D26.3 (`...design.md:408-412`) is the exact mechanism the ticket's "instancing-collapse ratio" HUD item names. | consume-now | UNVERIFIED — Task 19 not yet implemented. | The HUD's "instancing-collapse ratio" has a concrete, testable formula — proposed here since no artifact defines one: `ratio = 1 − (drawsSubmitted / recordsIn)`, displayed as a percentage. Headless gate asserts this value against a hand-computed expectation for the fixed helmet-grid composition (identical mesh+material+block per instance ⇒ expect near-100% collapse for that grid, distinct from Sponza's mixed-material scene where it should be much lower) — this formula itself did not previously exist in any artifact; see New gaps. |
| 12 | Full pipeline exercise — `recordDrawList` **D27 main-thread pre-resolution** | D27 (`...design.md:424-440`) is itself framed as fixing "the sample-06 collision" — i.e., this is a correctness invariant already proven necessary by this project's own prior incident, not merely best practice. | consume-now | UNVERIFIED — Task 19 not yet implemented; the guard mechanism ("`getPipeline`/`bindInstance` are main-thread-guarded and MUST NOT be called from chunks ≥ 1") is described in the DrawListBuilder task text (plan line 399). | Sample 09's stress-v2 mode (`--threads N > 1`) runs under the same debug hook Task 19's own unit test uses to assert worker chunks never trip the main-thread guard — this is a debug-build assertion, not merely "it didn't crash," since a silent main-thread violation under Release could ship undetected. |
| 13 | Full pipeline exercise — shadow quality bridge (D21) | D21 (`...design.md:315-320`): light-frustum-fitted, slope-scaled bias, 3×3 PCF — "the production-credible single-map baseline." | consume-now | UNVERIFIED — Task 22 not yet implemented. | Sponza present-mode visibly shows soft (≥2-texel-gradient) shadow edges without acne on grazing-angle surfaces — reuses Task 22's own GPU test methodology (acne/peter-panning/PCF-softness probes) rather than inventing a new one for sample 09; sample 09 itself does not need its OWN shadow-quality gate, only to visibly exercise the already-gated path. |
| 14 | Full pipeline exercise — **D24 memory budget/eviction + host-facing report** | Middleware-shares-GPU-with-host framing is D24's own stated rationale (`...design.md:344-367`); this is card #27. | consume-now | UNVERIFIED — Task 10 not yet implemented; report shape per D24(c): "a host-facing POD memory report (per-category bytes, heap budget/usage) feeding HUD + Tracy plots." | HUD's "#27 memory report" panel shows at least: per-category bytes (geometry pool, textures, transients, staging, internal) and heap budget/usage from `vmaGetHeapBudgets` — headless test asserts the reported total is nonzero and monotonically consistent with import (grows after DamagedHelmet-grid import, does not silently read zero/uninitialized). |
| 15 | Full pipeline exercise — input surface (#14) | N/A — internal ticket dependency, not a first-tier-renderer comparison; covered in full by the sibling matrix `gate/matrix-issue14-input.md`. | consume-now | UNVERIFIED from this ticket's vantage — depends on #14/Task 20 landing first (already correctly sequenced). | Sample 09's fly-through camera responds to mouse-look (relative mode) and at least one gamepad stick simultaneously available (hot-plug tested manually, not headless) — see row 20 (fly-through camera spec) for the full row; this row exists only to mark input as one of the "every subsystem visibly exercised" checklist items. |
| 16 | Full pipeline exercise — ImGui overlay (#16) | N/A — internal ticket dependency; covered in full by `gate/matrix-issue16-imgui-overlay.md`. | consume-now | UNVERIFIED from this ticket's vantage — depends on #16/Task 21 landing first (already correctly sequenced, parallel with T20). | Sample 09's HUD is a real `rx_debug_ui::Overlay` graph pass (per D20), not a bespoke non-ImGui debug text dump — headless smoke test (reusing #16's own methodology) confirms the overlay pass renders and the HUD's toggle widgets are present in the draw data. |
| 17 | Full pipeline exercise — executor per-frame allocation elimination (#29/Task 23) | Standard "steady-state zero-allocation" hot-path discipline (Filament/id Tech-class engines all treat per-frame heap churn as a correctness bug in a middleware renderer, not just an optimization). | consume-now | Verified as a real, confirmed-present bug today: claim-validation #7 (`claim-validation-2026-08-18.md:24,45-51`) — 7 distinct per-frame heap-allocation sites confirmed in `Executor::execute()` at HEAD `bf5b853`. Task 23 fixes it; MUST land before Task 24's stress-v2 numbers per the plan's own sequencing note. | Stress-v2's published wall-clock numbers are only valid once Task 23's allocation-count test (plan line 449: "allocation-count test first (fails on current code)") is green — the release notes' A/B comparison must not be computed against a build where the scene-path executor still allocates per frame, or the "vs sample 07" comparison partly measures allocator noise instead of the scene-path overhead it claims to measure. |
| 18 | HUD content — frame timing (FPS/frame-ms) | Feature-gap-audit's own "near-misses checked and found already covered" section explicitly credits this ticket: "**Frame-time HUD** — seed 1 (profiling phase); sample 09's ImGui HUD covers the dev-side interim." (`feature-gap-audit.md:71-72`). Already-ruled — not re-litigated here, only deepened into a criterion. Filament's own `gltf_viewer.cpp` (fetched this session) plots frame-timing HISTORY, not just an instant value — `ImGuiExt::PlotLinesSeries` over target-frame-time and PID-controller series — a stronger bar than a single instant-FPS number; cited as the concrete first-tier shape a rolling window should aim for, not merely a raw counter. | consume-now | N/A — no external library beyond ImGui itself (#16's dependency, not this ticket's). | HUD displays instantaneous frame-ms and a rolling FPS average (not a single-sample-noisy instant FPS) — the specific averaging window (e.g., 1s or N-frame) is unspecified anywhere; propose N=60-frame rolling average as the concrete, testable default, headless-asserted to be present in `--stats`/log output at minimum (visual HUD content itself is not headlessly assertable, per D17's own tolerance-pixel-not-pixel-perfect posture for dynamic content). |
| 19 | HUD content — cull counters incl. instancing-collapse ratio, pool stats, memory report | Covered by rows 10, 11, 4, 14 above respectively; this row exists only to assert the HUD actually SURFACES those counters (a correct counter that's computed but never displayed does not satisfy the ticket's explicit HUD-content list). | consume-now | UNVERIFIED — depends on rows 4, 10, 11, 14 landing. | A single ImGui panel (or clearly labeled group of panels) shows, at minimum: visible/culled instance counts, drawsSubmitted/recordsIn + the collapse ratio (row 11's formula), pool bytes-committed, and the D24 memory report's per-category breakdown — headless smoke test (font-forced-demo-window style per #16's methodology) confirms non-empty draw data in that region, not pixel-perfect content. |
| 20 | HUD content — vsync + layer-mask + light-channel toggles, and their model fidelity vs D15 | D15 (`...design.md:257-261`) defines TWO distinct masking axes: renderable `layers: u32` vs camera `cullMask: u32` (visibility), and light `channels: u8` vs renderable `channels: u8` (lighting/shadow-caster filtering) — "32-bit layers à la Unity/Godot; 8 channels is deliberately more generous than Unreal's 3." | consume-now | UNVERIFIED — Task 18/19 not yet implemented; the HUD's "layer-mask toggles (hide/show instance groups)" and "light-channel demo toggle" from the plan text (line 454) map onto these two DIFFERENT axes — a HUD that conflates them (e.g., one shared bitmask control) would misrepresent the actual data model. | HUD ships two visibly distinct controls: a layer-mask control that edits the camera's `cullMask` (u32, hide/show instance groups — visibility only, no lighting effect) and a separate light-channel control that edits a light's `channels` (u8, affects which renderables that light illuminates/casts shadows for) — a manual verification row confirms toggling layer-mask hides geometry outright while toggling light-channel only changes lighting/shadowing on geometry that remains visible. |
| 21 | HUD content — vsync toggle wiring | Present-mode control already delivered (Stage 0 Task 6, `device.cpp:461-465` `Device::setPresentMode`); every existing sample already exposes `--vsync on|off` as a CLI flag (plan line 79/`Global Constraints`). | consume-now | Verified: `Device::setPresentMode(PresentMode)` + `recreateSwapchain()` exist today (`src/rx_rhi_vk/src/device.cpp:461-528`). | HUD's vsync toggle calls the SAME `setPresentMode`+`recreateSwapchain` pair the existing `--vsync` CLI flag uses (no second implementation of the present-mode ladder) — a runtime toggle from the HUD produces the identical `RX_LOG_INFO` "present mode in use" line the CLI-flag path already produces, asserted by grep in a present-mode manual-verification row (headless mode has no live present target to toggle, so this row is MANUAL_VERIFICATION-only, not headless-gated). |
| 22 | Stress-v2 A/B comparability contract | Sample 07_stress itself is the baseline (plan lines 82-93): 30,000 draws, cubes/spheres mix, 4 pipeline/material variations, per-instance transform+color via bindless arena, `--draws N --threads N --vsync on|off --validate`. | consume-now | Verified sample 07's exact baseline shape from its own task text; sample 09's scene-path equivalent is UNVERIFIED (not yet implemented). No artifact anywhere defines the comparability contract precisely — see New gaps. | Proposed contract (fills the gap): **held identical** — total instance count (30,000), the 4 pipeline/material variations, per-instance transform+color data, `--threads N` semantics, `--vsync` flag. **Expected to differ, and why** — draws SUBMITTED to the GPU (sample 07 submits all 30,000 unconditionally, confirmed absent-culling per claim-validation #2; sample 09 goes through cull→sort→D26.3-collapse, so post-collapse draw count is legitimately lower for a scene with repeated instances, and culled count is nonzero for any camera not framing the entire field). The published A/B numbers must report BOTH wall-clock time AND the draws-submitted count side by side, so a reader can see whether sample 09 is faster because of genuinely less GPU work (culling+collapse) versus purely CPU-recording overhead differences — reporting wall-clock alone without the draw-count context (as sample 07's own `stress-numbers.txt` does today, since it has no culling to report) would misrepresent the comparison. |
| 23 | Stress-v2 — what's CI-gated vs artifact-only | D18 (`...design.md:282-291`): "CI perf gates assert deterministic counters... exact and noise-free... wall-clock/Tracy numbers are published as artifacts... never CI-blocking." | consume-now | Verified precedent: `.github/workflows/ci.yml` (~195-220) — sample 07's exact pattern: ctest-registered `sample_07_stress_headless` (counter gate) runs in the normal test suite; a SEPARATE CI step runs `xvfb-run -a .../sample_07_stress --draws 30000 --threads 1` and the default-threads variant, piping stdout to `stress-numbers.txt`, uploaded as artifact `stress-numbers`. | Sample 09's stress-v2 mode gets the identical two-tier treatment: a `sample_09_scene_stress_headless` ctest target asserting exact counters (drawsSubmitted, culled, collapse ratio, chunk count = threads) for a fixed 3-frame run — CI-blocking; a separate non-blocking CI step running `--stress --threads 1` and `--stress` (default threads), piping to a new `stress-v2-numbers.txt` artifact alongside (not replacing) sample 07's own `stress-numbers.txt`, so both baselines remain independently inspectable in every CI run's artifacts. |
| 24 | Headless gate — counter assertions (D17/D18 combined) | Reuses sample 07's and sample 08's own established headless-gate conventions rather than inventing a third pattern; Filament's `gltf_viewer.cpp` `--batch` mode (fetched this session: an `AutomationEngine` running configurable-frame-count, non-interactive test cases with PPM screenshot export) is the first-tier analogue confirming "fixed-frame-count headless run with assertions" is standard practice for a full-pipeline showcase sample, not a project-specific shortcut. | consume-now | Verified conventions: sample 07 (fixed 3 frames, exact counter assertions + 4-pixel analytic probe, `...plan.md:92`); D17 tolerance-pixel methodology (`...design.md:272-280`). | Sample 09's headless gate, for the fixed default DamagedHelmet-grid composition and a fixed startup camera pose, asserts EXACT values (not tolerance ranges — these are deterministic per D14/D15/D26) for: instances imported, renderables visible, renderables culled, drawsSubmitted post-collapse, recordsIn pre-collapse, zero validation errors (sync validation active) — any nondeterminism discovered here (e.g., `std::sort` producing different orders across `--threads` counts on equal-key entries) is itself a bug the gate must catch, per D19/Task 19's own "determinism across thread counts" test posture already established for DrawListBuilder unit tests. |
| 25 | Headless gate — tolerance-pixel probe | D17 (`...design.md:272-280`): committed reference PNGs at 256×256 rendered on lavapipe, ±4/255 per-channel tolerance, <0.5% failing-pixel budget, references lavapipe-only, regeneration via an explicit script never automatic. | consume-now | Verified: `tools/regen_references.sh` is named as Task 16's deliverable (plan line 318) — does not exist yet in `tools/` (checked this session); sample 09 has a precondition on Task 16 landing (already correctly sequenced, Stage 1 before Stage 2). | Sample 09's headless gate adds its own committed 256×256 lavapipe reference PNG for the default DamagedHelmet-grid composition, regenerated only via the same documented script Task 16 introduces (no new, second regeneration mechanism invented for this one sample) — local-GPU divergence from the reference is reported as info, not a failure, matching D17's existing local-vs-CI posture exactly. |
| 26 | Fly-through camera — input mapping from #14's surface | N/A — internal ticket dependency; the input API itself is covered in full by `gate/matrix-issue14-input.md`. Named here only to state what #15 needs and check it against #14's OWN plan text, per this ticket's explicit brief. | consume-now, **with an unresolved cross-ticket gap — see Conflicts** | **Verified gap, not assumed:** Task 20's full text (`...plan.md:408-411`, quoted in Conflicts) commits to exactly three input surfaces — relative mouse mode + per-frame mouse deltas + cursor show/hide, and gamepad hot-plug + `GamepadState poll()` (stick float2 × 2, triggers, A/B buttons). **It names no keyboard API of any kind.** Independently confirmed today's codebase has none either: `src/rx_platform/include/rx_platform/window.h` (31 lines, read in full) exposes only `pumpEvents()`/`sdlWindow()`/Vulkan-surface helpers; a repo-wide grep for `SDL_GetKeyboardState`/`SDL_SCANCODE` across `samples/` returns zero hits; the existing "camera" samples (06_materials) animate azimuth by elapsed time, not input, so there is no incidental keyboard-polling precedent to fall back on either. | Sample 09 needs continuous WASD-or-equivalent keyboard movement (this ticket's own brief states it explicitly) plus mouse-look and gamepad-stick move/look — but as currently scoped, #14/Task 20 supplies only the mouse and gamepad halves. Proposed criterion, contingent on the gap being closed one way or the other: EITHER #14/Task 20 is amended to add a keyboard key-state query (`isKeyDown(SDL_Scancode)` or equivalent) before T24 starts, OR sample 09 itself vendors a small SDL-direct keyboard poll as sample-local code (not a public rx_platform API) — the coordinator must choose one, since neither choice is currently recorded anywhere. Separately, HUD toggle-button budget: sample 09 needs at least three independent discrete gamepad actions (vsync toggle, layer-mask toggle(s), light-channel toggle) but Task 20 commits only two buttons (A/B) beyond the movement/look axes — also unresolved, also flagged in Conflicts. |
| 27 | MANUAL_VERIFICATION + Deck rows | Existing convention: `MANUAL_VERIFICATION.md` today has a shared top-level Windows/Steam-Deck section (written against 01_triangle) plus per-sample `## 05_multipass` / `## 06_materials` sections that add only a Linux checklist + a "Last run" note referencing functional-but-not-hardware-observed verification (file read in full this session, 202 lines) — **07_stress currently has no section at all**, an existing gap in a sibling ticket's scope, not this one's, noted here only for pattern continuity. | consume-now | Verified: exact existing section structure and wording conventions cited above. | Sample 09 gets its own `## 09_scene` section following the 05/06 pattern exactly: a "What 'pass' means" subsection (fly-through moves smoothly, HUD toggles work, Sponza — if fetched — shows shadows/materials correctly, `--validate` clean), a Linux checklist (unchecked initially, per the plan's explicit "Deck rows added to MANUAL_VERIFICATION as unchecked" instruction), and an explicit Steam Deck subsection (unchecked, following the existing 01_triangle Deck-section wording pattern: "not yet performed on real Steam Deck hardware... nothing in this codebase depends on desktop-only APIs... but that has not yet been confirmed on an actual Deck"). |
| 28 | Packaging — `tools/package_samples.sh` | Existing convention verified in full this session (219-line file read): per-sample staged directory, `copy_required()` fails loudly on any missing expected file, samples 01-07 currently hardcoded by name in two places (the per-sample copy block and the final `zip -r` command line 216). | consume-now | Verified exact insertion points: a `09_scene` entry needs (a) a `copy_required` block modeled on 07_stress's own (binary + Slang runtime libs + LICENSE, since sample 09 compiles Slang in-process like 06/07) plus DamagedHelmet's imported-asset files if NOT embedded/fetched-at-runtime (needs a Task-24-time decision: does the packaged sample re-fetch DamagedHelmet at first run, or ship it pre-staged? — flagged as a Conflicts-adjacent open question, not answered by any artifact read this session), and (b) an addition to the `zip -r ... 01_triangle 02_hotreload ... 07_stress` argument list (line 216) and the sample-count comment at the top of the file (line 2: "seven sample binaries" becomes stale the moment 08 and 09 both land — 08_gltf_viewer already isn't reflected here either, a pre-existing staleness this ticket should fix while touching the file, not just add to). | `tools/package_samples.sh` successfully stages and zips `09_scene` alongside all other samples with zero missing-file failures on both presets; the file's header comment count and the explicit sample list are updated to include 08 and 09 (not just 09), verified by running the script end-to-end on both `linux-native` and `windows-cross-zig` build outputs. |
| 29 | Packaging — CI wiring | Same precedent as row 23 (sample 07's `.github/workflows/ci.yml` pattern). | consume-now | Verified exact lines (`.github/workflows/ci.yml:~195-220`). | CI gains a `sample_09_scene_headless` ctest registration (blocking) and a stress-v2-numbers artifact step (non-blocking) as described in row 23, plus `09_scene` added to whatever CI step invokes `tools/package_samples.sh` (not independently located this session — verify the exact invocation site when implementing; flagged as a small residual verification gap, not a blocker). | CI run shows `sample_09_scene_headless` passing in the same job sample 07/08's headless gates run in, and `stress-v2-numbers.txt` uploaded as an artifact on every run. |
| 30 | Packaging — README/roadmap update | README.md's "Roadmap" section (lines 83-91, read in full this session) is the ONLY roadmap document in this repository — no separate `roadmap.md` exists. Phase 1/2/3 each get one descriptive paragraph naming their delivered modules/samples; "Phase 4 and beyond" (line 91) is currently a single forward-looking paragraph with no delivered-samples list, since Phase 4 isn't complete yet. | consume-now | Verified: exact section and line numbers above. | On Phase 4's exit, README.md gains a "Phase 4 (complete)" paragraph in the same style as Phases 1-3 (line 85-89), naming `rx_asset`, `rx_scene`, `rx_debug_ui`, and samples 08/09, and the file-tree section (lines 70-81) gains entries for the new `src/rx_asset/`, `src/rx_scene/`, `src/rx_debug_ui/` directories and the two new samples — modeled exactly on how Phase 3's entries were added (render graph/materials/05/06, visible in the current file). |
| 31 | Packaging — registry "layer table tick for layer 8" | See Conflicts below — this criterion as literally worded does not map cleanly onto either candidate artifact. | consume-now (with the caveat in Conflicts) | Verified: TWO distinct "layer 8" artifacts exist in `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md` — the summary table (line 34: `| 8 | Scene Submission | Render items, transforms, cameras, lights; culling/LOD |`, no "(delivered: Phase N)" annotation yet, unlike lines 32-33 which already say "(delivered: Phase 3)") and a separate deferred-details paragraph (lines 148-155, "**Geometry processing** (layer 8, scene submission): meshlet generation, virtual geometry, LOD management, skeletal mesh skinning, morph targets"). | The literal, artifact-grounded action is: annotate line 34's table row with "(delivered: Phase 4)" **for the Scene Submission responsibilities Phase 4 actually delivers (render items/transforms/cameras/lights/culling)**, while explicitly NOT claiming LOD is delivered (LOD remains covered by the separate deferred paragraph at lines 148-155, itself confirmed still-deferred by `feature-gap-audit.md:80-81`: "LOD management, skinning, morph targets — registry layer 8... registry" under "near-misses... already covered," i.e., already correctly tracked as NOT part of Phase 4). A literal "tick" (checkbox) mechanism does not exist anywhere in this table — the existing convention is the parenthetical "(delivered: Phase N)" annotation, which this criterion should follow rather than inventing a new marking style. |
| 32 | Release — v0.4.0-phase4 tag + release notes | Standard practice, also this project's own established pattern for Phases 1-3 (each phase's README paragraph names what shipped; git tags are the implied mechanism though not separately verified this session). | consume-now | Verified: no `v0.4.0-phase4` tag exists yet (this is prep work). Plan's own exit sequence (`...plan.md:465`): "final whole-phase review... one fix wave, push, CI green, tag v0.4.0-phase4, release with both packages + published numbers, board cards closed." | Preconditions enumerated as testable gates, not claimed done: (1) both `linux-native` and `windows-cross-zig` packages built via row 28's updated `package_samples.sh`; (2) CI green on both presets including sample 09's new gates; (3) release notes contain the row 22 A/B numbers (wall-clock AND draw-count context, not wall-clock alone) plus sample 07's own numbers for continuity; (4) Deck rows in MANUAL_VERIFICATION remain explicitly unchecked with the standard "not yet performed on real hardware" wording (per the plan's own instruction — an unchecked Deck row is not a release blocker, consistent with 01_triangle's existing precedent) unless a real Deck run happens before the tag. |

## Conflicts

- **Plan-text "D16 input" citation is not a real D-number for input.** Task
  24's own body (`...plan.md:454`) reads: "fly-through camera (mouse
  capture + gamepad, D16 input)". Spec D16 (`...design.md:263-270`) is
  titled "Test content strategy" and is entirely about DamagedHelmet/Sponza
  asset sourcing — it says nothing about input. The input ticket itself
  (Task 20, plan line 408) is headed "Input expansion (**seed 6**)", not
  bound to any D-number in this session's required reading. This reads as
  either a typo (D16 meant as a cross-reference to the seed-6 input work,
  mis-rendered) or a copy-paste artifact from an adjacent clause. Does not
  block anything (the actual input requirement is unambiguous from
  context), but the coordinator should correct the citation so a future
  reader doesn't go looking for an input contract inside D16.
- **"Registry layer table tick for layer 8" does not map to a single, clean
  artifact action.** See matrix row 31 in full — the plan instruction
  implies a simple checkbox-style completion, but the two candidate
  artifacts (summary table line 34 vs. deferred-details paragraph lines
  148-155) cover DIFFERENT scope: the table row's stated responsibility
  bundles "culling/LOD" together, while Phase 4 delivers only culling
  (LOD stays explicitly deferred per `feature-gap-audit.md:80-81`).
  Ticking the row as flatly "(delivered: Phase 4)" without qualifying that
  LOD is excluded would overclaim relative to the feature-gap audit's own,
  already-ruled position. Quoted both sides in row 31; recommend the
  coordinator either accept the qualified annotation proposed there or
  clarify which artifact the plan actually meant.
- **Sample 09's fly-through needs keyboard movement; #14/Task 20's own text
  supplies none.** This ticket's brief (and the plan's own Task 24 line,
  "fly-through camera") requires continuous stick/WASD-style movement.
  Task 20 (`...plan.md:408-411`) reads in full: "relative mouse mode
  (`SDL_SetWindowRelativeMouseMode`), per-frame accumulated mouse deltas
  from `SDL_EVENT_MOUSE_MOTION` xrel/yrel, cursor show/hide, gamepad:
  hot-plug via `SDL_EVENT_GAMEPAD_ADDED/REMOVED`, `GamepadState poll()`
  (left/right stick float2 with 8000/32768 deadzone, triggers, A/B
  buttons)" — mouse-look and gamepad-stick movement are both present, but
  no keyboard scancode/key-state query appears anywhere in that sentence,
  and none exists in `src/rx_platform/include/rx_platform/window.h`
  today (verified: 31 lines, no keyboard surface of any kind). Quoting
  the other side: nothing in Task 20's text says keyboard input is
  explicitly OUT of scope either — it is simply absent, which reads as an
  oversight rather than a deliberate "mouse+gamepad only" decision, since
  "WASD" is the default PC expectation for a fly-through camera and the
  ticket's own brief names it. Not resolved here per the brief's
  instruction — the coordinator decides whether Task 20 grows a keyboard
  API or Task 24 vendors one locally. A second, smaller instance of the
  same pattern: Task 20 commits exactly two gamepad face buttons (A/B),
  while sample 09's HUD needs at least three independent discrete toggle
  actions (vsync, layer-mask, light-channel) — also unaddressed by either
  ticket's current text.
- **Sample 07's own `stress-numbers.txt` precedent has no draws-submitted
  context, but stress-v2's proposed contract (row 22) requires one.** This
  is not a contradiction in existing artifacts so much as a note that row
  22's proposed reporting format is a strict superset of sample 07's
  existing one, not a drop-in replacement — the coordinator may want sample
  07's own report format left untouched (it has nothing to cull, so
  draws-submitted always equals `--draws N` trivially) while only sample
  09's stress-v2 report gains the extra column.

## New gaps

- **No artifact anywhere defines the stress-v2 A/B "comparability contract"
  precisely** (what must stay identical between sample 07 and sample 09's
  `--stress` mode, what may legitimately differ and why, and what the
  published numbers must jointly report to avoid a misleading comparison).
  The plan text only says "publishes A/B numbers vs 07's direct path" —
  proposed and filled in as row 22 above. Phase fit: belongs in Task 24
  itself (this is the ticket that will implement and publish the numbers),
  but should be settled at gate/spec time rather than implementation time,
  since a wrong contract produces release-notes numbers that are frozen in
  project history once published — retrofitting a comparison methodology
  after a misleading number has already shipped in `v0.4.0-phase4`'s
  release notes is exactly the "expensive to change later" shape this
  gate exists to catch.
- **No artifact defines the exact "instancing-collapse ratio" formula/unit**
  the HUD is supposed to display (D26.3 only commits to the underlying
  counters, records-in vs draws-submitted, being CI-gateable — not to a
  derived ratio's formula). Proposed in row 11 above (`1 −
  drawsSubmitted/recordsIn`, as a percentage). Phase fit: trivial,
  belongs directly in Task 24's HUD implementation; flagged only so the
  coordinator can bless (or replace) the specific formula before
  implementation rather than leaving it to implementer discretion, since
  it becomes a published, citable number in release notes.

## Verification health

- **Verified first-hand this session (file:line read directly):** ticket
  #15's full body + correction note (`gh issue view 15`); plan Tasks 7, 13,
  14, 15, 16, 18, 19, 20, 21, 22, 23, 24 and the Global Constraints/
  Execution-notes sections; spec D13-D27 in full; the registry's layer
  table (lines 24-38) and its deferred-details layer-8 paragraph (lines
  137-190) in full; `feature-gap-audit.md` in full; `claim-validation-
  2026-08-18.md`'s claim #2 and #7 entries; `.github/workflows/ci.yml`'s
  sample-07 CI block; `tools/package_samples.sh` in full (219 lines);
  `MANUAL_VERIFICATION.md` in full (202 lines); `README.md`'s file-tree and
  Roadmap sections; `src/rx_rhi_vk/src/device.cpp`'s `setPresentMode`/
  `recreateSwapchain` (declarations verified in `device.h` lines 27-38 and
  103-118; definitions verified in `device.cpp` at lines 461 and 468
  respectively — both files read/grepped directly this session); the
  `src/` and `tools/` directory listings confirming no Stage 1/2 code and
  no `fetch_assets.sh`/`regen_references.sh` exist yet; `src/rx_platform/
  include/rx_platform/window.h` in full (31 lines — no keyboard API);
  a repo-wide grep for `SDL_GetKeyboardState`/`SDL_SCANCODE` across
  `samples/` (zero hits) and a read of `samples/06_materials/main.cpp`'s
  orbit-camera code (confirmed time-driven, not input-driven) — both
  ground the new keyboard-movement Conflicts entry. `git tag -l` and
  `gh release view v0.3.0-phase3` (checked for release-notes structural
  precedent feeding row 32: no prior release has published an A/B
  performance-numbers section — v0.4.0-phase4 would be the first).
- **Inferred / not independently verified (flagged inline at each row):**
  every "Library support" cell for a subsystem whose implementing task
  (10-23) has not yet landed is necessarily UNVERIFIED-by-code — grounded
  only in the plan/spec text, which is the best available source before
  those tasks are dispatched. This is expected and correctly scoped for a
  primary-gate document (the gate runs BEFORE implementation), not a
  shortfall in this matrix's rigor.
- **Fetched externally this session (real quotes, not paraphrase from
  memory):** `github.com/bkaradzic/bgfx` `examples/05-instancing/
  instancing.cpp` (rows 1, 22 — instanced-vs-per-cube A/B structure,
  `bgfx::getStats()->numDraw` HUD counter, overflow warnings) and
  `github.com/google/filament` `samples/gltf_viewer.cpp` (rows 8, 18, 24,
  25 — ImGui `ViewerGui` Stats panel, `PlotLinesSeries` frame-timing
  history, `camutils::Manipulator` camera delegation, `--batch`
  `AutomationEngine` headless multi-frame automation). Two further
  fetches — Filament's `Materials.md.html` doc and its GitHub release
  notes (searching for "automatic instancing" specifically) and Godot's
  debugger-overview/MultiMesh-fish docs (searching for a Monitors-style
  stats overlay and a published instancing-vs-individual benchmark) —
  returned pages that genuinely loaded but did not contain the specific
  claims sought; nothing from those four fetches is cited as precedent
  anywhere in this matrix, recorded here rather than silently omitted.
- **Dead links / version ambiguities:** none encountered — all in-repo
  citations resolved to real, currently-existing files and line ranges at
  the time of this session; all external fetches resolved to real, live
  pages (no 404s), though two of the six returned no on-topic content
  (noted above) rather than the sought claim.
- **Scope note:** this ticket is downstream of nearly every other Phase 4
  ticket (Tasks 10-23), so most of its "Library support" verification is
  necessarily a dependency check ("does the producing task exist yet, and
  what does its OWN task text promise") rather than independent code
  verification — that is the correct posture for a gate document whose job
  is to sharpen acceptance criteria before those upstream tasks are even
  dispatched, not to audit already-delivered code.
