### Task 15: Clustered shading integration + frame-pipeline adoption

The lit path consumes cluster lists for point/spot (directional stays
direct), targeting hundreds-to-thousands of local lights; the scene path
adopts the charter frame-pipeline spine this stage needs (depth prepass
policy per the Task 1 ruling; shadows → cluster assignment → opaque lighting
order).

**Files:** `shaders/material/forward_entry.slang` (+lighting module),
sample scene-path pass graphs, `src/rx_scene`, tests.
**Acceptance sketch:**
- Clustered-vs-unclustered equivalence: an N-light scene renders within
  tolerance of a brute-force all-lights reference path (discrimination:
  clustering changes cost, never the image).
- Scaling numbers published: 100 / 1k / 5k synthetic lights, desktop
  driver-labeled (the "suddenly scales" claim measured, not asserted).
- Zero validation errors incl. sync validation on the new pass chain, both
  drivers.
**Steps:** equivalence harness first → integrate → measure/publish → both
presets + real driver → commit.

