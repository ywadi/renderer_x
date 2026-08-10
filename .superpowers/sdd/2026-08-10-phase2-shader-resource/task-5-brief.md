### Task 5: sample_02_hotreload (parallel-lane capable after Tasks 1-2)

**Files:** `samples/02_hotreload/main.cpp`, `CMakeLists.txt`, `shader/hotreload.slang` (installed next to binary), samples/README.md update. Root CMakeLists wiring.

Fullscreen triangle; fragment shader lives on disk next to the binary; poll mtime (~4Hz `stat`, no new deps [R:D1]); on change: `Compiler::compileFromFile` → on success build new pipeline (reflection-driven layout via Task 2) and swap (old pipeline retired via DeletionQueue if Task 4 is merged; otherwise `vkDeviceWaitIdle` swap is acceptable ONLY if Task 4 hasn't landed yet — coordinator will state which at dispatch); on failure keep last-good pipeline, log diagnostics, keep rendering.
Headless ctest mode: compile embedded source A → render 2 frames offscreen → readback color; then compile source B (different constant color) → render → readback differs as expected → exit 0. Present mode: window + live editing.
Ships Slang runtime libs per the Task 1 mechanism; this sample is the redistribution proof.

**Verify:** headless gate green in ctest; `--present` manually verified rendering + a live reload on this machine; both presets build; commit clean.

---

