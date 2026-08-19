### Task 21: `rx_debug_ui` — ImGui overlay module (D20)

**Files:** Vendor imgui v1.92.x (pinned, MIT recorded; core + sdl3 + vulkan backends only); create `src/rx_debug_ui/{CMakeLists.txt,include/rx_debug_ui/overlay.h,overlay.cpp}`.
**Interfaces:** `Overlay::create(Device&, Window&, format)` (own descriptor pool sized per [R:present]; font upload via existing Uploader; UseDynamicRendering with swapchain format); `beginFrame()` (SDL event feed already flowing through Window — overlay hooks the existing event dispatch), `addPass(RenderGraph&, targetName)` — declares a graph pass (side-effect, reads nothing) whose callback renders draw data; core libs stay ImGui-free (only samples + rx_debug_ui link it).
**Steps:** GPU smoke test (overlay pass renders; readback shows non-empty overlay region with a forced demo window; zero validation errors) → implement → both presets → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue16-imgui-overlay.md` as amended by
`gate/rulings-2026-08-18.md` §#16. Key deltas: pin **v1.92.9b**
(plain tag, NOT -docking — structural exclusion); init fills the
NESTED `PipelineInfoMain.PipelineRenderingCreateInfo` (2025-09-26
breaking change — the flat field no longer exists); descriptor pool =
two typed entries (SAMPLED_IMAGE ≥ 8 + SAMPLER ≥ 2,
FREE_DESCRIPTOR_SET_BIT; 2026-04-22 breaking change); FONT-UPLOAD
RULING: the backend's lazy internal upload (raw vkAllocateMemory +
`vkQueueWaitIdle` inside RenderDrawData — vendored, unmodifiable) is
a DOCUMENTED exception to D25/D24: force texture creation at init so
the stall is one-time, assert at-most-once QueueWaitIdle across an
N-frame run (guards against per-frame texture churn), note the D24
accounting blind spot in the memory report's docs;
`ImGui_ImplSDL3_SetGamepadMode(Manual, nullptr, 0)` immediately after
init (kills the AutoFirst race with Task 20's gamepad ownership;
gamepad HUD nav off this phase); every SDL event →
`ImGui_ImplSDL3_ProcessEvent` FIRST, then platform input; pass
declaration uses the REAL API chain — `addPass().addColorOutput(
target, LOAD_OP_LOAD).setSideEffect().setExecute(...)` — LOAD-not-
CLEAR is a named criterion with the pattern-visible-under-HUD test;
configure-time CMake link-boundary check (imgui absent from every
rx_* core target's transitive closure) lands here; main-thread
one-liner per convention.

