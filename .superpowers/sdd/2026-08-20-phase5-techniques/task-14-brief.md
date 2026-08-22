### Task 14: Froxel grid + clustered light assignment (compute; Filament port)

The clustered Forward+ core: camera-frustum froxel grid + per-froxel light
lists built in compute, ported GLSL→Slang from Filament's published
froxelizer. The grid is explicitly designed as SHARED infrastructure — the
charter commits volumetrics (Task 30) to riding the SAME grid, so the grid's
layout/bindings are authored for two consumers from day one (a Task 1 spec
decision records the shared shape).

**Files:** `shaders/cluster/*.slang` (ported), `src/rx_scene` froxel/cluster
orchestration (graph compute passes), tests.
**Acceptance sketch:**
- Device-free froxel math: index↔slice round-trips, depth-slice
  distribution matches the ported reference's formula.
- GPU test: synthetic light sets → readback of per-froxel lists asserts
  EXACT membership for hand-computed cases (corner lights, spanning lights,
  behind-camera culls).
- Capacity+1 behavior loud and defined (max lights per froxel / total —
  content-scale rule); counters exact and CI-gateable.
**Steps:** failing math tests → GPU membership tests → port → both presets +
real driver → commit.

