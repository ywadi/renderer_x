# Phase 4 Seed Notes (pre-spec commitments)

**Status:** Input to the future Phase 4 spec — not a spec itself. Items
recorded here were committed to during Phase 3 and MUST be carried into the
Phase 4 spec/plan when it is authored (or explicitly re-deferred there with
rationale).

## Working scope sketch

Phase 4 = **Scene & Assets**: scene submission (cameras, transforms,
lights, draw-list building) + asset import (glTF 2.0 meshes/scenes, KTX2
textures — ready-made loaders per repo policy, candidates evaluated by a
research pass), exiting with a real imported scene (e.g. a Khronos sample
scene) rendered with shadows through the render graph, camera fly-through,
and visible culling statistics.

## Committed items

1. **Present-mode control (vsync on/off) and frame-rate cap/uncap.**
   - Current state (verified 2026-08-10): the swapchain never sets a
     present mode explicitly — it inherits vk-bootstrap's default
     preference (MAILBOX where available, FIFO fallback). Make the choice
     explicit in the swapchain path.
   - Phase 4 deliverable: `--vsync on|off` sample flag mapped to present
     modes (FIFO = vsync on; MAILBOX preferred / IMMEDIATE fallback =
     uncapped), including correct swapchain recreation on toggle.
   - Later phases (record in Phase 4 spec's deferred list, not dropped):
     FPS cap (CPU-side frame limiter, distinct from vsync; Steam Deck
     battery use case; cooperates with Gamescope's external pacing) and
     frame-time HUD belong to the profiling/instrumentation phase; public
     `setPresentMode`/frame-cap API belongs to the SDK/DLL phase.

2. **Frustum culling + shadow-caster culling** as part of scene
   submission (first real object-level culling; on-screen visibility
   counters in the fly-through sample). Occlusion (HiZ), light culling
   (tiled/clustered), and meshlet culling remain later-phase items.

3. **Default material library:** StandardPBR (glTF metallic-roughness
   model: base color, metallic/roughness, normal, occlusion, emissive,
   alpha modes) + Unlit, authored against the Phase 3 `IMaterialShader`
   interface with zero special treatment — the engine's own materials use
   the same extension mechanism customers do.

4. **Asset-driven texture path:** KTX2/Basis GPU-compressed textures
   through the existing Uploader/BindlessTable machinery; mip chains from
   the container (not blit-generated) where present.

5. **Layer/mask system in scene submission (user-requested 2026-08-10):**
   per-renderable layer bitmask; per-camera cull mask (a camera draws only
   matching layers); per-light channel mask (a light illuminates only
   matching objects AND only matching casters render into its shadow
   passes). Applied at draw-list building time — no render-graph changes
   required. Follow the established engine conventions (Unity layers /
   Unreal lighting channels / Godot cull masks) rather than inventing new
   semantics; pick mask widths and defaults in the Phase 4 spec.

6. **Input expansion in rx_platform (user-raised 2026-08-10):** the Phase 4
   fly-through camera requires relative mouse capture (raw deltas, cursor
   show/hide) and gamepad support (Steam Deck is the hardware floor — the
   fly-through must be drivable by pad, not only mouse). SDL3 provides
   both natively; the work is exposing them through rx_platform's existing
   event/input surface. Window-resize handling needs no Phase 4 work — the
   full chain (SDL3 events → swapchain recreation → graph re-realization)
   exists and is regression-tested as of Phase 3.

## Carried process notes

- Research pass before spec (library selection for glTF/KTX2 with
  citations), spec + plan authored by the coordinator, SDD execution with
  per-task review, exit via deployed samples + tagged release — same cycle
  as Phases 1-3.
