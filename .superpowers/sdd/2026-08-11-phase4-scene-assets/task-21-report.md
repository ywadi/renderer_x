# Task 21 report — `rx_debug_ui` ImGui overlay module (card #16, spec D20)

Base commit: `7fe9306`. Implementer: this session. Requirements read in
order per the brief: `task-21-brief.md` (task text + BINDING gate block),
`gate/matrix-issue16-imgui-overlay.md`, `gate/rulings-2026-08-18.md` §#16,
`gh issue view 16`. Order of authority followed: rulings > spec > matrix >
ticket.

## Files changed (mine only)

- `third_party/CMakeLists.txt` — vendors Dear ImGui v1.92.9b (plain tag,
  source-only `FetchContent_Populate`, no upstream CMakeLists.txt exists)
  and defines a new, reusable `imgui` STATIC library target (core
  imgui.cpp/imgui_draw.cpp/imgui_tables.cpp/imgui_widgets.cpp — deliberately
  NOT imgui_demo.cpp — plus backends/imgui_impl_sdl3.cpp/imgui_impl_vulkan.cpp),
  `VK_NO_PROTOTYPES` PUBLIC, linking `Vulkan::Headers` PUBLIC + `SDL3::SDL3-static`
  PRIVATE.
- `cmake/DependencyBoundaryCheck.cmake` (new) — the configure-time
  link-boundary assertion mechanism (`rx_assert_target_excludes_dependency`),
  a genuinely new pattern in this repo (gate matrix row 12/"New gaps").
- `CMakeLists.txt` (root) — `add_subdirectory(src/rx_debug_ui)` + the
  9-core-target boundary-check loop right after it.
- `src/rx_debug_ui/` (new module) — `CMakeLists.txt`, `include/rx_debug_ui/overlay.h`,
  `overlay.cpp`, `tests/doctest_main_gpu.cpp`, `tests/test_overlay_gpu.cpp`.
- `src/rx_platform/include/rx_platform/window.h` /
  `src/rx_platform/src/window.cpp` — `pumpEvents()` gained an optional
  `const std::function<void(const SDL_Event&)>& preDispatch = nullptr`
  parameter, invoked once per drained event BEFORE this method's own
  handling — the seam `Overlay::processEvent()` hooks through. Generic
  (no ImGui type anywhere in the signature) — `rx_platform` stays
  ImGui-free.
- `src/rx_platform/tests/window_test.cpp` — three new device-free
  `TEST_CASE`s for the `preDispatch` seam (fires once per event, strictly
  BEFORE this class's own handling of that same event — proven directly,
  not inferred; default-nullptr regression guard).
- `src/rx_rhi_vk/include/rx_rhi_vk/memory_report.h` — doc-only note on the
  D24 accounting blind spot this module's font/HUD-texture allocations
  are.
- `docs/threading.md` — new `rx::debug_ui::Overlay` main-thread-only
  registry entry.
- `MANUAL_VERIFICATION.md` — new "rx_debug_ui overlay" section (visual HUD
  sanity, gamepad-ownership orthogonality with real hardware, the "camera
  stops moving" half of row 6, Deck legibility — none automatable
  headlessly, none has a consuming sample yet).
- `.github/workflows/ci.yml` — added `rx_debug_ui_gpu` to the
  windows-cross-zig job's ctest exclusion regex (no real Vulkan under
  Wine in CI, same posture as `rx_rhi_vk`/`rx_graph_gpu`/`rx_material_gpu`).

`.superpowers/sdd/2026-08-11-phase4-scene-assets/progress.md` shows
modified in `git status` but is **not mine** (the coordinator's own
dispatch-ledger entry, already present before this session started) —
excluded from my commit via explicit pathspec, matching Task 20's own
precedent.

## Design summary

- **Vendoring**: `v1.92.9b` pinned exactly (commit
  `f1cc2ae15e53a861a874c3034aae6798fde194ab`, verified directly via
  `git ls-remote --tags` and by inspecting the actual populated source
  tree's own `git describe --tags` after fetch — see "Vendored-version
  provenance" below). MIT (`LICENSE.txt` at the pinned tag). No upstream
  CMakeLists.txt exists (verified: the tag's own root file listing has
  none), so this follows the same "no CMakeLists → `FetchContent_Populate`
  source-only, `PARENT_SCOPE`-propagate `*_SOURCE_DIR`" convention as
  volk/VMA/stb/MikkTSpace — but unlike those four (each compiled straight
  into exactly one consumer), ImGui needs a real, reusable `imgui` library
  target since both `rx_debug_ui` and (Task 24) samples will link it.
- **`VK_NO_PROTOTYPES`, not `IMGUI_IMPL_VULKAN_USE_VOLK`**: verified
  directly against the fetched `imgui_impl_vulkan.cpp` that
  `IMGUI_IMPL_VULKAN_USE_VOLK` would make every Vulkan call inside it
  resolve through the SAME global volk function pointers every other TU
  in this project shares (`#if defined(VK_NO_PROTOTYPES) &&
  !defined(VOLK_H_)` — false once volk.h is included, so the backend's
  own indirection table never gets built). Choosing the OTHER documented
  path instead — `VK_NO_PROTOTYPES` (already this project's own PUBLIC
  define on `rx_rhi_vk`) + an explicit `ImGui_ImplVulkan_LoadFunctions()`
  call with a custom loader (`overlay.cpp`'s `imguiVulkanFunctionLoader`,
  which just calls `vkGetInstanceProcAddr` per symbol) — gives the
  backend its OWN TU-local static function-pointer table, entirely
  separate from the shared global one. This is what makes the
  at-most-once-`vkQueueWaitIdle` GPU test's interception seam
  (`rx::debug_ui::detail::setQueueWaitIdleHookForTests`) clean: it only
  ever affects ImGui's own internal resolution of that one symbol, never
  any other Vulkan call in the process (e.g.
  `rx::rhi::CommandContext::runOnce()`'s own, unrelated `vkQueueWaitIdle`
  call stays untouched by the hook).
- **Nested `PipelineInfoMain.PipelineRenderingCreateInfo`** (the
  2025-09-26 breaking change): filled exactly as the gate ruling
  specifies. The struct's `pColorAttachmentFormats` pointer is backed by
  process-lifetime static storage
  (`g_pipelineColorFormatStorage`), not a stack local — the vendored
  backend copies the WHOLE `InitInfo` by value into its own persistent
  state (`bd->VulkanInitInfo = *info;`), so the pointee must outlive the
  `Init()` call itself, not just its duration. Documented at both the
  storage variable's own definition and `colorFormat_`'s header comment.
- **Descriptor pool**: own pool (`InitInfo.DescriptorPool`, NOT the
  `DescriptorPoolSize` convenience path), two typed entries —
  `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` × 16 (2× the vendored backend's own
  stated per-atlas minimum of 8, headroom for HUD textures beyond the
  font atlas) and `VK_DESCRIPTOR_TYPE_SAMPLER` × 2 (exactly the stated
  minimum — the backend only ever creates linear+nearest, never more) —
  `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` set.
  `MinImageCount`/`ImageCount` both set to `rx::rhi::FrameSync::kFramesInFlight`
  (2) — sizes the backend's own vertex/index render-buffer rotation to
  this project's real frames-in-flight discipline, independent of
  whatever a real swapchain's own image count happens to be.
- **Font/texture upload — documented D25/D24 exception**: `create()`
  forces the font atlas's one-time upload synchronously, via a real
  one-shot `NewFrame → build a glyph-referencing UI → Render →
  RenderDrawData` cycle recorded through
  `rx::rhi::CommandContext::runOnce()`, BEFORE returning — the one
  unavoidable full-queue stall (the vendored backend's own internal
  `vkQueueWaitIdle`, `imgui_impl_vulkan.cpp`'s own "FIXME-OPT: Suboptimal!"
  comment) is a one-time startup cost, not a per-frame one. Fully
  documented (rationale, D25/D24 interaction, the at-most-once bound) in
  `overlay.h`'s own class comment and `memory_report.h`'s new scope note.
- **Gamepad ownership**: `ImGui_ImplSDL3_SetGamepadMode(Manual, nullptr, 0)`
  called immediately after `ImGui_ImplSDL3_InitForVulkan()`, before
  `ImGui_ImplVulkan_LoadFunctions()`/`Init()` — kills the AutoFirst
  default race against Task 20's own gamepad lifecycle (`rx::platform::Window`).
  Gamepad HUD navigation is not enabled this phase, as a direct
  consequence.
- **Event dispatch**: `Window::pumpEvents(preDispatch)`'s new parameter is
  the single seam; `Overlay::processEvent()` wraps
  `ImGui_ImplSDL3_ProcessEvent()`. `Window::pumpEvents()` itself gained no
  ImGui dependency (the parameter type is a plain
  `std::function<void(const SDL_Event&)>`).
- **Graph pass**: the REAL API chain —
  `graph.addPass("imgui_overlay").addColorOutput(targetName,
  desc).setSideEffect().setExecute(fn)`. LOAD-not-CLEAR is NOT a
  parameter anywhere in this class's own surface: verified directly
  against `render_graph.cpp` (step 1, "write-after-write chains... the
  later-declared pass depends on the immediately preceding one for the
  same name" — a REAL, enforced ordering, not a convention) and
  `executor.cpp:1101` (`attachmentEverWritten[physIdx] ?
  VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR`) that
  declaring this pass AFTER a caller's own scene pass against the same
  `targetName` is both necessary and sufficient for LOAD — the framework
  itself enforces the resulting ordering via a real `dependsOn` edge, not
  merely "if declared in the right order, it happens to work."
- **Core-libs-stay-ImGui-free**: a new, reusable CMake mechanism
  (`cmake/DependencyBoundaryCheck.cmake`) walks the transitive
  `LINK_LIBRARIES`/`INTERFACE_LINK_LIBRARIES` closure of `rx_core`,
  `rx_rhi_vk`, `rx_graph`, `rx_material`, `rx_platform`, `rx_task`,
  `rx_shader`, `rx_asset`, `rx_scene` at configure time and
  `FATAL_ERROR`s, naming the full dependency chain, on any match against
  `imgui`.

## Per-criterion proof (gate matrix rows)

| Row | Criterion | Evidence |
|---|---|---|
| 1 | Pin v1.92.9b, plain tag | `third_party/CMakeLists.txt`'s `RX_IMGUI_TAG`; fetched tree's own `git describe --tags` = `v1.92.9b` at commit `f1cc2ae15e5...`, verified this session (see below). |
| 2 | Init call shape + bool checks; invalid-config fails loudly | `overlay.cpp`: `InitForVulkan` before `LoadFunctions`/`Init`, every bool checked; `create()` validates `colorFormat != VK_FORMAT_UNDEFINED` and Device handles itself (the ONE real caller-tunable input this class's minimal surface exposes) since the vendored backend's own `IM_ASSERT`s compile to nothing under this project's NDEBUG-defined RelWithDebInfo builds. `TEST_CASE "Overlay::create() fails loudly ... on a deliberately-invalid colorFormat"` — PASSED, zero validation errors. |
| 3 | Nested `PipelineInfoMain.PipelineRenderingCreateInfo` | `overlay.cpp` fills exactly this shape; compiles against the real fetched header (a stale flat-field write would not compile at all) — build succeeds on both presets. |
| 4 | Descriptor pool: two typed entries + FREE_DESCRIPTOR_SET_BIT, exercised with font + 1 extra texture | `overlay.cpp`'s pool creation; `TEST_CASE "...LOAD-not-CLEAR... plus a second registered texture..."` registers a real dummy texture via `ImGui_ImplVulkan_AddTexture()` — PASSED, zero validation errors. |
| 5 | D25/D24 exception documented; at-most-once `vkQueueWaitIdle` across N frames | `overlay.h`/`memory_report.h` doc sections; `TEST_CASE "...vkQueueWaitIdle exactly once..."` — installs `detail::setQueueWaitIdleHookForTests`, asserts count == 1 after `create()` AND after 5 further steady-state frames — PASSED. Real discrimination trail during development (see below), not merely a passing assertion. |
| 6 | ProcessEvent first; WantCaptureMouse reacts | `Window::pumpEvents(preDispatch)` + `Overlay::processEvent()`; `TEST_CASE "...WantCaptureMouse..."` — 2-frame sequence (commit layout, process a synthetic click, recompute on next `beginFrame()`) — PASSED. The "camera stops moving" half is routed to `MANUAL_VERIFICATION.md`, per the matrix's own text. |
| 7 | Gamepad Manual mode | `overlay.cpp`'s explicit call, immediately after SDL3 backend init; verified by code inspection (no automated mechanism exists to prove "SDL never opens a gamepad" without a real device, matching the matrix's own framing) — real-hardware row added to `MANUAL_VERIFICATION.md`. |
| 8 | Frame lifecycle ordering | `beginFrame()`'s three-call sequence; `addPass()`'s execute callback's `Render()` + `RenderDrawData()` — every GPU test's own successful `Render()` output is itself proof this ordering is correct (a wrong order would produce empty/garbage draw data). |
| 9 | Real `rx_graph::Pass` API chain | `overlay.cpp`'s `addPass()` body — literal chain match. |
| 10 | Docking excluded | Plain tag pinned (row 1) — `-docking` variant never fetched; `imgui_demo.cpp` (docking's usual entry point in examples) not even vendored. |
| 11 | Readback methodology | `test_overlay_gpu.cpp`'s `renderAndReadBack()` — identical `createHostVisibleBuffer` + `vkCmdCopyImageToBuffer` + `invalidate()` + `mappedData()` pattern as `rx_graph/tests/test_execute_gpu.cpp:754-768`. |
| 12 | Configure-time link-boundary check | `cmake/DependencyBoundaryCheck.cmake` + root `CMakeLists.txt` loop over all 9 core targets — passes on the real tree; FATAL_ERRORs with the exact chain (`rx_core -> imgui`) when a violation is injected (see "Revert-discrimination evidence"). |
| 13 | D5 one-liner | `overlay.h`'s top-of-file thread-affinity comment + `docs/threading.md`'s new registry entry. |
| 14 | D24-D27 | D25 (row 5) + D24 (`memory_report.h`) — both documented, log-don't-drop. D26/D27: N/A-Phase-4, correctly untouched (ImGui submits no scene draws, never calls `MaterialSystem`). |

## Command tails

Configure (linux-native, imgui fetched fresh):
```
-- [dep-cache] HIT for ktx (key=ktx-6e7e780595aa2b62) - reusing cached install, no compilation
-- [fetch_slang] Slang 2026.14.1 prebuilt already present ... - skipping download
-- Configuring done
-- Generating done
-- Build files have been written to: .../build/linux-native
```
Fetched tree provenance:
```
$ git -C build/linux-native/_deps/imgui-src log -1 --format='%H %d'
f1cc2ae15e53a861a874c3034aae6798fde194ab  (grafted, HEAD, tag: v1.92.9b)
$ git -C build/linux-native/_deps/imgui-src describe --tags
v1.92.9b
```
Full `xvfb-run -a ctest --preset linux-native --output-on-failure` (final run):
```
100% tests passed, 0 tests failed out of 24
Total Test time (real) = 139.71 sec
```
(`rx_debug_ui_gpu_tests` at position 15, `1.76 sec`, zero validation
errors.)

`rx_debug_ui_gpu_tests --validate` standalone:
```
[doctest] test cases:  4 |  4 passed | 0 failed | 0 skipped
[doctest] assertions: 48 | 48 passed | 0 failed |
[doctest] Status: SUCCESS!
```

windows-cross-zig: full build succeeds (`184/184` build steps, `imgui`/
`rx_debug_ui`/`rx_debug_ui_gpu_tests.exe` all cross-compile cleanly, zero
warnings from any of this task's own files). `xvfb-run -a ctest --preset
windows-cross-zig -E '...rx_debug_ui_gpu...' --output-on-failure`:
```
100% tests passed, 0 tests failed out of 11
Total Test time (real) = 168.26 sec
```
(`rx_platform_tests` — including the three new `preDispatch` cases —
PASSED under Wine.) A throwaway `RX_TRACY=ON` cross-compile-only configure
(mirroring `ci.yml`'s own dedicated step) also builds `rx_debug_ui`/
`rx_debug_ui_gpu_tests.exe` cleanly. As a bonus (not required, not relied
upon): `rx_debug_ui_gpu_tests.exe` run directly under
`xvfb-run -a wine ...` on this dev machine's own fuller Wine/Vulkan
package set also passed all 4 cases for real — CI's own leaner Windows-job
package set is NOT expected to reproduce this, which is why the ctest
exclusion stands (matches `rx_rhi_vk`/`rx_graph_gpu`/`rx_material_gpu`'s
own documented posture in `ci.yml`).

## Revert-discrimination evidence

**Dependency-boundary check** (in-tree revert-and-restore, byte-identical
restore verified): temporarily added
`target_link_libraries(rx_core PRIVATE imgui)` to
`src/rx_core/CMakeLists.txt`, reconfigured —
```
CMake Error at cmake/DependencyBoundaryCheck.cmake:138 (message):
  [dependency-boundary-check] 'rx_core' transitively depends on something
  matching 'imgui' -- this violates the core-libraries-stay-ImGui-free hard
  boundary (spec D20, gate ruling #16).  Dependency chain: rx_core -> imgui
Call Stack (most recent call first):
  CMakeLists.txt:126 (rx_assert_target_excludes_dependency)
```
Reverted (`cp` from a pre-edit backup; `diff` confirmed byte-identical);
reconfigure succeeded cleanly again; full `ctest` suite re-run green
(24/24).

**LOAD-not-CLEAR** (in-tree revert-and-restore, byte-identical restore
verified): temporarily swapped the two `addPass()` call lines in the main
smoke `TEST_CASE` (declared `overlay->addPass()` BEFORE
`addPatternPass()`, inverting the write-after-write chain so the overlay
pass becomes the FIRST writer of `"bb"` and gets CLEARed, then the pattern
pass's own `vkCmdClearAttachments` overwrites the WHOLE render area on
top of it). Rebuilt, ran:
```
/.../test_overlay_gpu.cpp:378: ERROR: CHECK( approxEqual(inside, expectedWhite, 8) ) is NOT correct!
  values: CHECK( false )
/.../test_overlay_gpu.cpp:381: ERROR: CHECK_FALSE( approxEqual(inside, expectedPattern, 8) ) is NOT correct!
  values: CHECK_FALSE( true )
[doctest] test cases:  1 |  0 passed | 1 failed | 3 skipped
```
Reverted (byte-identical restore verified via `diff`); rebuilt; full
`ctest` suite re-run green (24/24).

**At-most-once `vkQueueWaitIdle`** (real discrimination trail during
development, not a separately staged probe): the FIRST version of the
steady-state loop used `ImGui::Text("frame %d", frame)` — observed
`waitIdleCount == 6` (one per newly-seen digit glyph across 5 frames,
plus `create()`'s own). A second attempt fixed the text but left the
steady-state window's title bar enabled — observed `== 2` (one extra from
the title's own never-before-seen glyphs, stable afterward, never 3+).
The final, correct version reuses ONLY glyphs `create()`'s own priming
frame already packed (`NoDecoration` + `TextUnformatted("rx_debug_ui")`)
— `== 1`, stable across all 5 steady-state frames. All three states are
documented inline in `test_overlay_gpu.cpp`'s own comment on the loop, so
a future reader does not have to rediscover this from scratch. This
IS the discrimination evidence for the assertion's real sensitivity — the
test genuinely caught two different real regressions (each an authentic
ImGui v1.92.x dynamic-font-atlas interaction, not a test bug) before
landing in its final form.

## Vendored-version provenance

- Repo: `github.com/ocornut/imgui`. Tag: `v1.92.9b` (plain, NOT
  `-docking`). Commit: `f1cc2ae15e53a861a874c3034aae6798fde194ab`.
  Verified twice: (a) `git ls-remote --tags
  https://github.com/ocornut/imgui.git` this session, confirming the tag
  resolves to this exact commit and that `v1.92.9b-docking` is a
  genuinely separate ref; (b) the actual `FetchContent`-populated source
  tree's own `git describe --tags`/`git log -1` after a real fetch,
  matching exactly.
- License: MIT (`LICENSE.txt` at the pinned tag, fetched and read this
  session — "Copyright (c) 2014-2026 Omar Cornut").
- API-surface claims (nested `PipelineInfoMain`, descriptor-pool minimums,
  `ImGui_ImplVulkan_LoadFunctions`'s loader-function contract, the
  `IMGUI_VULKAN_FUNC_MAP`/`IMGUI_IMPL_VULKAN_USE_LOADER` dispatch
  mechanism, `ImGui_ImplVulkan_UpdateTexture`'s internal
  `vkQueueWaitIdle` call site, `ImGui_ImplSDL3_GamepadMode`,
  `ImGui_ImplSDL3_ProcessEvent`) were all verified this session by
  fetching and reading the REAL `imgui_impl_vulkan.h`/`.cpp` (full 2000
  lines) and `imgui_impl_sdl3.h` at this exact pinned tag via `gh api
  repos/ocornut/imgui/contents/...?ref=v1.92.9b` — not carried over from
  the gate matrix's own citations unverified, and not assumed from
  general ImGui familiarity.

## Deviations from the brief/matrix (all reasoned, none silent)

1. **No device-free `rx_debug_ui_tests` binary.** This module's own logic
   is inherently GPU-bound (wrapping the vendored Vulkan/SDL3 backends);
   the one genuinely device-free seam this task added
   (`Window::pumpEvents()`'s `preDispatch` parameter) is tested where it
   lives, `rx_platform/tests/window_test.cpp`, not duplicated into an
   empty placeholder binary here. Matches the brief's own "TDD where
   device-free logic exists" — there is none beyond that one seam.
2. **Row 2's "invalid MinImageCount" criterion reinterpreted.**
   `MinImageCount`/`ImageCount` are NOT parameters of this class's own
   minimal `create(Device&, Window&, format)` surface (both hardcoded to
   `FrameSync::kFramesInFlight` internally) — there is no way for an
   external caller to construct that specific invalid config through this
   API at all. The device-free-adjacent test instead validates the ONE
   real caller-tunable input this surface exposes (`colorFormat`),
   which is the honest, applicable reading of "fail loudly on a
   deliberately-invalid config" given this class's actual (correctly
   minimal, per the brief) shape.
3. **`imgui_demo.cpp` not vendored.** The brief's Steps line mentions "a
   forced demo window"; the matrix's own row 11 explicitly prefers "a
   purpose-built minimal window... more robust against a future ImGui
   version reflowing the demo window's contents" over the literal demo
   window. Followed the matrix's own stated preference — smaller vendored
   surface, zero lost test coverage.
4. **`vkQueueWaitIdle` interception via `ImGui_ImplVulkan_LoadFunctions()`,
   not `IMGUI_IMPL_VULKAN_USE_VOLK`.** The matrix's row 3 quote mentions
   volk as "this project's Vulkan loader, relevant to the backend's own
   `IMGUI_IMPL_VULKAN_USE_VOLK` convenience define" without mandating
   that specific path. Verified directly (see Design summary) that the
   `VK_NO_PROTOTYPES`+`LoadFunctions()` path is both consistent with this
   project's existing `VK_NO_PROTOTYPES` convention (`rx_rhi_vk`'s own
   PUBLIC define) AND gives the at-most-once test a clean, non-global
   interception seam — a strictly better fit than the alternative, not a
   shortcut.

## Self-review

- Re-read `overlay.h`/`overlay.cpp` end to end against the gate matrix's
  own per-row citations (struct field names, function signatures,
  constants) — every one matches the REAL fetched source, not a
  paraphrase.
- Confirmed the WAW-dependency reasoning behind LOAD-not-CLEAR against
  `render_graph.cpp`'s real source (not assumed from the matrix's own
  prose) before writing the test, then confirmed it experimentally via
  the revert-discrimination swap above.
- Confirmed IM_ASSERT compiles to nothing in this project's actual build
  config (NDEBUG via RelWithDebInfo, both presets) before relying on that
  fact to justify `Overlay::create()`'s own validation — did not just
  assume it from the debug_checks.h precedent's own identical finding for
  bare `assert()`.
- Ran the full suite fresh on both presets from a genuine reconfigure (not
  just an incremental build) as the final check, after an unrelated
  build-cache hiccup (a stale `CTestTestfile.cmake` after a manual
  `rm -rf` of build artifacts mid-session) briefly hid
  `rx_debug_ui_gpu_tests` from `ctest -N` — reconfigured and reconfirmed
  24/24 before treating the suite as green. Also hit and resolved a
  pre-existing, unrelated `.deps-cache` key/`mikktspace` patch-idempotency
  environment issue (a mismatch between invoking cmake via the
  `/home/ywadi/d2/renderer_x` symlink vs. the real
  `/media/ywadi/second/renderer_x` path changes a downstream dep-cache
  key's literal string, forcing a spurious `mikktspace` FetchContent
  re-populate/re-patch that fails on an already-patched tree) — resolved
  by invoking consistently through the symlink path and clearing the
  stale `_deps/mikktspace-*` state once; not a defect in anything this
  task touched.
- No stray debug/diagnostic code left in the committed files (a temporary
  `MESSAGE("DIAG ...")` line used during the vkQueueWaitIdle
  investigation was removed before finalizing `test_overlay_gpu.cpp`).
