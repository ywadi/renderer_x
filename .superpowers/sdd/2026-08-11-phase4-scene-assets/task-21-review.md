# Task 21 review — `rx_debug_ui` ImGui overlay module (card #16, spec D20)

Reviewer: independent review session (not the implementer). Commit under
review: `bbd1df1` (local, `main`, 1 commit ahead of `origin/main`). Base:
`7fe9306`. Authority order followed: `gate/rulings-2026-08-18.md` §#16 >
spec (D20/D24/D25) > `gate/matrix-issue16-imgui-overlay.md` > ticket #16,
per the dispatch instructions.

Inputs read in full: `task-21-brief.md`, `task-21-report.md`,
`review-7fe9306..bbd1df1.diff`, `gate/rulings-2026-08-18.md`,
`gate/matrix-issue16-imgui-overlay.md`.

## Verdict 1 — Spec compliance: **PASS**

Every BINDING gate-ruling #16 criterion was independently verified against
either the real, pinned vendored source, the real in-repo `rx_graph`
source, or a live rebuild/rerun — not merely re-read from the implementer's
report.

| Criterion | Verification method | Result |
|---|---|---|
| Vendored tag exactly `v1.92.9b`, plain (not `-docking`) | `git -C _deps/imgui-src describe --tags`, `git log -1`, `imgui.h`'s `IMGUI_VERSION`/`IMGUI_VERSION_NUM` defines, grep for `DockingEnable`/`ImGuiDockNode` (none found; `imgui_demo.cpp` absent from the vendored source list) | Exact match |
| MIT license recorded | `LICENSE.txt` at the pinned tag read directly; recorded in `third_party/CMakeLists.txt`'s own comment — matches this repo's established convention (no dedicated NOTICE file exists for any vendored dep; license text/tag is recorded in the vendoring CMakeLists comment throughout `third_party/CMakeLists.txt`) | Confirmed |
| Nested `PipelineInfoMain.PipelineRenderingCreateInfo` filled (not the flat pre-2025-09-26 field) | Read `overlay.cpp` directly; a stale flat-field write would not compile against the real fetched header (confirmed by a clean build) | Confirmed |
| Descriptor pool: two typed entries (SAMPLED_IMAGE=16≥8, SAMPLER=2≥2), `FREE_DESCRIPTOR_SET_BIT` | Read `overlay.cpp`'s pool-creation code; exercised live by the GPU test (font atlas + one extra `ImGui_ImplVulkan_AddTexture`-registered texture), zero validation errors observed on a real rerun | Confirmed |
| Font-upload ruling: forced creation at init, at-most-once `vkQueueWaitIdle` across N frames, hook seam not bypassable | Traced the vendored backend's own source: exactly one live `vkQueueWaitIdle` call site in `imgui_impl_vulkan.cpp` (`UpdateTexture`, line 923; the other hit at line 1960 is a dead, commented-out line); confirmed `IMGUI_IMPL_VULKAN_USE_LOADER` is active in that TU (`#if defined(VK_NO_PROTOTYPES) && !defined(VOLK_H_)` — that TU never includes volk.h, so this holds regardless of `overlay.cpp`'s own includes); confirmed `ImGui_ImplVulkan_LoadFunctions()` unconditionally re-resolves the whole function table on every call (no "already loaded" short-circuit), so a hook installed before a later `Overlay::create()` call genuinely takes effect. Reran the assertion myself twice: once unmodified (count stayed 1 across 5 steady-state frames) and once with the exact regression the report describes reintroduced (`ImGui::Text("frame %d", frame)`), which tripped the count to 6 — reproducing the implementer's own documented discrimination trail exactly. D24 blind-spot note present in `memory_report.h`. | Confirmed, and independently re-proven to discriminate |
| `ImGui_ImplSDL3_SetGamepadMode(Manual, nullptr, 0)` immediately after platform-backend init | Read `overlay.cpp`: called directly after `ImGui_ImplSDL3_InitForVulkan`, before `LoadFunctions`/`Init` | Confirmed |
| Every SDL event → `ImGui_ImplSDL3_ProcessEvent` first, then platform input | Read `window.cpp`: `preDispatch(event)` runs unconditionally before the method's own `switch` on every drained event; `Overlay::processEvent()` wraps `ImGui_ImplSDL3_ProcessEvent` | Confirmed |
| Pass declaration: real `addPass().addColorOutput(target, LOAD_OP_LOAD).setSideEffect().setExecute(...)` chain; LOAD-not-CLEAR proven, discriminates against CLEAR | Read `overlay.cpp`'s `addPass()`: literal chain match. Read `render_graph.cpp`/`executor.cpp` directly: `resource.attachment = decl.attachment` (last writer wins) and `attachmentEverWritten[physIdx] ? LOAD : CLEAR` are real, confirmed lines. Reran the revert-discrimination myself: inverted the two `addPass()` call sites in the smoke test (overlay declared first) — both pixel assertions failed exactly as the report describes (`inside` no longer white, matches pattern instead) | Confirmed, independently re-proven to discriminate |
| Configure-time CMake link-boundary check fails configure on a real violation | Read `DependencyBoundaryCheck.cmake` and the root `CMakeLists.txt` loop (9 named core targets, `rx_debug_ui` correctly excluded). Reran the revert-discrimination myself: added `target_link_libraries(rx_core PRIVATE imgui)`, reconfigured — got the exact `FATAL_ERROR` with chain `rx_core -> imgui`; reverted (byte-identical, `diff`-confirmed), reconfigured clean | Confirmed, independently re-proven to discriminate |
| Zero unfiltered validation errors, incl. teardown | Ran `rx_debug_ui_gpu_tests --validate` under `xvfb-run -a` myself: 4/4 cases, 48/48 assertions, `SUCCESS`, only the pre-existing "known false positive" instance-creation warnings appear (filtered elsewhere in the shared harness, not this task's own code) | Confirmed |
| `ci.yml` Wine-exclusion edit matches established precedent, touches ONLY the exclusion list | `git diff 7fe9306..bbd1df1 -- .github/workflows/ci.yml`: the only functional change is `rx_debug_ui_gpu` appended to the existing `-E` regex (same style as `rx_rhi_vk\|rx_graph_gpu\|rx_material_gpu`), plus two explanatory comment additions. Nothing else in the file touched. | Confirmed |
| D5 main-thread one-liner | `overlay.h`'s header comment + `docs/threading.md`'s new registry row, both present and consistent with `RX_ASSERT_MAIN_THREAD` guards actually present on all four public entry points (`create`/`processEvent`/`beginFrame`/`addPass`) | Confirmed |
| `MANUAL_VERIFICATION.md` rows honest | Read the new section: correctly states "not yet performed," no sample consumes the overlay yet, routes exactly the rows the matrix itself says must stay manual (real-hardware gamepad ownership, "camera stops moving," Deck legibility) | Confirmed |

Two documented, disclosed (non-silent) deviations were checked against the
authority hierarchy and found acceptable:
- Purpose-built minimal window instead of the brief's literal "forced demo
  window" — explicitly the gate matrix's own stated preference (row 11:
  "more robust against a future ImGui version reflowing the demo window's
  contents"); matrix outranks ticket text per the stated hierarchy.
- Row 2's "invalid `MinImageCount`" criterion reinterpreted as validating
  `colorFormat` instead — the class's minimal `create(Device&, Window&,
  format)` surface (the brief's own interface sketch) genuinely has no
  caller path to construct that specific invalid config; validating the one
  real caller-tunable input is the honest reading, not a scope-narrowing.

Commit hygiene, independently verified (not just re-read from the report):
- Author/committer: `Yousef Wadi <ywadi85@gmail.com>` (matches the repo's
  git config; matches the user).
- Commit message and full body: grepped for
  `claude|anthropic|co-authored|ai-generated|fable|opus|sonnet` — zero
  hits.
- `git show bbd1df1 --stat` lists exactly the 16 files the report claims as
  "mine only"; `.superpowers/sdd/.../progress.md` is correctly excluded
  from the commit (confirmed it is NOT in the commit's file list) and is
  still the only modified file in the current working tree, left untouched
  by this review.
- `git branch -vv`: `main [origin/main: ahead 1]` — the commit exists only
  locally, nothing pushed.

## Verdict 2 — Code quality: **Approved**

No blocking (Critical/Major) findings. Two Nit-level observations, neither
worth a required fix:

- **Nit** — `test_overlay_gpu.cpp`'s `renderAndReadBack()` takes an unused
  `rx::rhi::Device&` parameter suppressed with `(void)device;`. Harmless
  (test-only code, kept for signature symmetry with the sibling
  `test_execute_gpu.cpp` helper), but the parameter could simply be
  dropped.
- **Nit** — `Overlay`'s documented "at most one live instance per process"
  contract is prose-only, not runtime-enforced (e.g. no static guard flag
  tripped on a second concurrent `create()`). Consistent with this
  codebase's existing convention for single-instance-by-convention classes
  elsewhere, not a new gap this task introduced, but a second `Overlay`
  instantiated by a future caller would silently corrupt shared ImGui
  global state rather than fail loudly.

Strengths worth recording: every version-specific/breaking-change claim in
the gate matrix and the report was checked directly against the real
fetched `v1.92.9b` source during this review (not re-trusted from the
report's citations) and matched exactly; the two chosen revert-discrimination
tests were independently reproduced byte-for-byte, including tripping the
exact same regression counts (6, and the LOAD/CLEAR pixel-check failures)
the implementer's own development history describes; failure-path cleanup
in `Overlay::create()` is complete and correctly ordered at every early-return
branch; the CMake dependency-boundary mechanism is a genuinely new,
well-reasoned, well-documented pattern with a real (not vacuous) failure
mode.

## Empirical work performed this review

- Configured and built `linux-native` from the real path
  (`/media/ywadi/second/renderer_x`, never via the `/home/ywadi/d2/renderer_x`
  alias) — clean configure, `ninja: no work to do` (targets already built
  from the implementer's own session, confirmed current).
- `xvfb-run -a build/linux-native/src/rx_debug_ui/rx_debug_ui_gpu_tests --validate`:
  4/4 cases, 48/48 assertions, zero validation errors — matches the report
  exactly.
- `xvfb-run -a ctest --preset linux-native --output-on-failure`: 24/24,
  ~147s — run twice (once before, once after all temporary revert edits),
  both green.
- Revert-discrimination #1 (dependency-boundary check): injected
  `target_link_libraries(rx_core PRIVATE imgui)`, reconfigured — got the
  exact documented `FATAL_ERROR` and chain; restored byte-identically
  (`diff`-confirmed), reconfigured clean.
- Revert-discrimination #2 (at-most-once `vkQueueWaitIdle`): reintroduced
  the exact `ImGui::Text("frame %d", frame)` regression the report
  describes catching during development; rebuilt and reran — count tripped
  from 1 to 6, exactly as documented; restored byte-identically, rebuilt
  clean, reran green.
- Bonus discrimination (LOAD-vs-CLEAR): independently inverted the two
  `addPass()` call sites in the render-smoke test; reran — both pixel
  assertions failed with the exact values the report's own discrimination
  log shows; restored byte-identically, rebuilt clean, reran green.
- Vacuousness sweep: read all four `rx_debug_ui_gpu_tests` cases and all
  three new `window_test.cpp` cases in full. None found vacuous; the
  `preDispatch`-ordering test uses a deliberately-wrong-default sentinel
  (`minimizedObservedInsideCallback = true`) that only flips to prove real
  ordering, a sound anti-vacuousness pattern. Two of the four GPU cases
  were independently confirmed discriminating via the revert tests above;
  the remaining two (invalid-colorFormat rejection, `WantCaptureMouse`
  reaction to a synthetic click) were inspected and are structurally
  sound (real `CHECK_FALSE`/`CHECK` against real, non-trivial state
  transitions), not independently defeated.
- Source-level verification (not report-trusted) of: the vendored backend's
  single `vkQueueWaitIdle` call site; `IMGUI_IMPL_VULKAN_USE_LOADER`'s
  activation condition; `ImGui_ImplVulkan_LoadFunctions()`'s unconditional
  per-call re-resolution; `AttachmentDesc`'s actual default
  (`SwapchainRelative`, 1.0×1.0); `render_graph.cpp`'s last-writer-wins
  attachment-shape and `executor.cpp`'s LOAD/CLEAR derivation.
- Working tree confirmed clean after all temporary edits: `git status
  --short` shows only the pre-existing, untouched
  `.superpowers/sdd/.../progress.md` modification.

## Not independently re-verified this review

- `windows-cross-zig` build/cross-compile and the Wine-hosted
  `rx_platform_tests` run (report claims 184/184 build steps, 11/11 ctest
  under Wine, plus a bonus direct-Wine GPU-test run) — not rebuilt this
  session; only the `ci.yml` diff for that job was checked directly.
- Real-hardware gamepad-ownership orthogonality and the "camera stops
  moving under HUD focus" manual row — correctly routed to
  `MANUAL_VERIFICATION.md` as not automatable; not something this review
  could exercise either.
- Whether `v1.92.9` (non-`b`) is truly API-identical to `v1.92.9b` — both
  the matrix and the implementer's report flag this as an assumed (not
  diffed) convention; irrelevant here since the exact `b` tag is what was
  actually pinned and verified.
