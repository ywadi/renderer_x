### Task 7: sample_04_streaming

**Files:** `samples/04_streaming/**`, README update, root wiring.

24 procedurally-generated textures, a resident budget of 8 bindless slots; each second (or every N frames headlessly) the next texture streams in through Uploader and the oldest resident is evicted: `BindlessTable::release` + DeletionQueue-retired destruction keyed on the frame fence — the eviction-while-in-flight safety is the entire point [R:D1 sample 3]. Grid of quads each drawing its texture if resident (partially-bound: non-resident slots must not be sampled — draw skips them; document why PARTIALLY_BOUND makes the set valid anyway).
Headless gate: run 60 frames, assert the full rotation happened (every texture was resident at some point — track via readback probes at grid positions on selected frames), zero validation errors (this catches premature-destroy bugs), exit codes.

**Verify:** headless gate in ctest; present mode verified; both presets; commit clean.

---

