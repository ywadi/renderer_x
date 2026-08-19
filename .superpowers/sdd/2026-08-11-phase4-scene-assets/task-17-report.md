# Task 17 report — Window edge-state hardening (FG7, card #25)

Base commit: `ad9a5e6`. Implementer: this session. Requirements read in
order per the brief: plan Task 17 body + Gate hardening block, matrix
(`gate/matrix-issue25-window-hardening.md`), rulings §#25 (+ Errata — none
apply to #25), `gh issue view 25`.

## Commits

Two commits landed (see "Concerns" for how they landed):

- `bf98974` — `feat(samples): wire --fullscreen and suspended-present
  handling into every --present sample` (mine, clean: 10 files — the 8
  samples' `main.cpp`, `samples/README.md`, `MANUAL_VERIFICATION.md`).
- `ed5239a` — landed under a **different agent's** commit (a coordinator
  docs/registry commit, `docs: registry — techniques-phase charter...`)
  due to a concurrent `git add`/`git commit` race in this shared (non-
  worktree) working tree. It correctly and completely contains all 9 of
  my core files (`src/rx_platform/include/rx_platform/window.h`,
  `src/rx_platform/src/window.cpp`, `src/rx_platform/tests/window_test.cpp`,
  `src/rx_rhi_vk/include/rx_rhi_vk/device.h`, `src/rx_rhi_vk/src/device.cpp`,
  `src/rx_rhi_vk/tests/device_test.cpp`,
  `src/rx_rhi_vk/tests/frame_sync_test.cpp`,
  `src/rx_rhi_vk/tests/window_state_test.cpp`,
  `src/rx_rhi_vk/CMakeLists.txt` — verified byte-for-byte via
  `git diff HEAD -- <those 9 paths>` returning empty) but mixed with the
  registry doc and another agent's texture-fixture files. I did not
  invoke that commit myself; see "Concerns".

Both commits' author/committer identity is the local git config
(`Yousef Wadi <ywadi85@gmail.com>`), verified via `git show -s --format='%H
%an <%ae>'` on both SHAs. No AI attribution anywhere.

## Design (binding correction applied)

Per rulings §#25 (matrix conflict 1): the suspended-present guard is
**extent-query-driven**, not event-driven.

- `Device::recreateSwapchain(VkSurfaceKHR surface, std::optional<VkExtent2D>
  extentOverride = std::nullopt)` — after destroying the existing
  swapchain (unchanged), queries the real surface extent via
  `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` (or uses `extentOverride`
  when supplied — the DI seam matrix row 6 asked for). On
  `width==0 || height==0`: skips `vkb::SwapchainBuilder::build()`
  entirely, sets `suspended_=true`, returns `true` (success — "no work to
  do yet" is not a failure). On a nonzero extent: clears `suspended_` and
  runs the normal build path unchanged (same present-mode ladder as
  before).
- `SwapchainStatus` gains `Suspended`. `acquireNextImage()`/`present()`
  short-circuit to `Suspended` (or `AcquireResult{Suspended,0}`) **without
  calling the real Vulkan function** while `suspended_` is true — verified
  by two new test-only counters, `acquireCallCount()`/`presentCallCount()`,
  that only increment on the branch that actually issues the real call.
- `Device::isSuspended()` is a public accessor — incidentally also
  satisfies matrix row 11's "no host-facing 'present suspended' signal"
  gap (ruled N/A/folded into FG5, but the accessor exists as a side effect
  of the design and costs nothing extra).
- Wayland: `currentExtent` there is the `(0xFFFFFFFF, 0xFFFFFFFF)`
  sentinel, never a real `(0,0)` — my `width==0||height==0` check
  correctly never fires for that sentinel, so Wayland surfaces fall
  through to the unchanged normal build path exactly as before this task.
- `rx::platform::Window` gains `setFullscreen(bool)`/`isFullscreen()`
  (borderless-desktop: `SDL_SetWindowFullscreen` with no
  `SDL_SetWindowFullscreenMode()` call, then `SDL_SyncWindow()` before
  returning) and device-free-testable event-observed state
  (`minimizedEventObserved()`, `lastPixelSizeEvent()` — fed only by
  `pumpEvents()`'s handling of `MINIMIZED`/`RESTORED`/`PIXEL_SIZE_CHANGED`,
  documented as optimization/logging only, never the suspend gate).
- `rx::platform::logWaylandMinimizeLimitationOnce(const char* platformName)`
  — a free function taking the platform name as a parameter (testability
  seam) rather than calling `SDL_GetPlatform()` internally; called once
  from `Window::create()`. One-shot per process (an atomic
  compare-exchange, consumed only by a genuine `"Wayland"` match).

## Per-criterion proof (matrix rows)

**Row 1 (event payload discipline).** `Window::pumpEvents()`'s switch
reads `data1`/`data2` only inside the `PIXEL_SIZE_CHANGED` case;
`MINIMIZED`/`RESTORED` never touch `lastPixelSizeEvent_`. Device-free test
`window_test.cpp:49` proves this via `SDL_PushEvent` (see "Revert
evidence" — confirmed load-bearing).

**Row 2 (skip-acquire-entirely, distinguishable status, resume).**
`recreateSwapchain`'s zero-extent branch never calls
`vkb::SwapchainBuilder::build()`; `SwapchainStatus::Suspended` is the
distinguishable status; resume is the next call observing a nonzero
extent. GPU test `window_state_test.cpp:63` ("extentOverride seam...")
proves given `{0,0}` → `isSuspended()==true`, `swapchain()==VK_NULL_HANDLE`,
`swapchainImages().empty()`; the next no-override call (real 64×64 hidden
window) resumes: `isSuspended()==false`, real swapchain rebuilt, zero
validation errors. Full pass output:
```
window_state_test.cpp:80: SUCCESS: CHECK( device->isSuspended() )
window_state_test.cpp:81: SUCCESS: CHECK( device->swapchain() == nullptr )
window_state_test.cpp:82: SUCCESS: CHECK( device->swapchainImages().empty() )
[info] Device::recreateSwapchain: surface extent is 64x64 -- resuming presentation [Phase 4 Task 17]
window_state_test.cpp:94: SUCCESS: REQUIRE( device->recreateSwapchain(fixture->surface) )
window_state_test.cpp:96: SUCCESS: CHECK( device->swapchain() != nullptr )
window_state_test.cpp:97: SUCCESS: CHECK( device->swapchainImages().size() > 0 )
window_state_test.cpp:99: SUCCESS: CHECK_FALSE( fixture->context.hasValidationErrors() )
[doctest] test cases: 1 | 1 passed | 0 failed | 67 skipped
```

**Row 3 (Wayland one-shot log).** `logWaylandMinimizeLimitationOnce`
fires exactly once for a mocked `"Wayland"` string, never for `"Windows"`/
`"Linux"`/`nullptr`, via `LogForwardSink` capture (same mechanism
`rx_core/tests/log_test.cpp` already uses). Confirmed load-bearing (see
below).

**Row 4 (borderless-fullscreen toggle).** `setFullscreen()` is exactly
`SDL_SetWindowFullscreen(window_, fullscreen)` with no prior
`SDL_SetWindowFullscreenMode()` call, then `SDL_SyncWindow()`.
Device-free test `window_test.cpp` asserts
`SDL_GetWindowFullscreenMode()==nullptr` (borderless, not exclusive) after
`setFullscreen(true)`. GPU test `window_state_test.cpp`'s double-toggle
additionally drove a **real** windowed→fullscreen→windowed cycle on this
dev machine's actual desktop and observed a genuine resize:
`swapchainExtent()` matched `3840x1080` (this machine's real screen
resolution) after entering fullscreen, and `64x64` again on return —
zero validation errors across both cycles. (One informational finding:
`SDL_SyncWindow()` occasionally logged `"failed: "` with an empty
`SDL_GetError()` message on the fullscreen→windowed leg — a benign SDL
quirk; my design already treats that failure as log-only, never fatal,
matching the header comment's documented rationale, and the toggle still
completed correctly in every run.)

**Row 5 (single recreation call site).** No `recreateSwapchainForFullscreen`
or equivalent exists anywhere — every fullscreen application (all 8
samples) and the GPU test itself call the exact same
`Device::recreateSwapchain(surface)` a resize's `NeedsRecreate` branch
already uses.

**Row 6 (two-tier test design + DI seam).** Tier 1:
`window_test.cpp`'s `SDL_PushEvent`-driven state-machine tests
(device-free). Tier 2: `window_state_test.cpp`'s `extentOverride`-seam GPU
test. The true end-to-end "real OS minimize measured by the guard" case
is MANUAL_VERIFICATION-only, exactly as the matrix concluded is the only
option (no CI driver this repo uses can be made to report a genuinely
zero-sized surface).

**Row 7 (call-count present-skip, never wall-clock).**
`window_state_test.cpp`'s call-count test drives 60 suspended iterations
of `acquireNextImage()`/`present()` and asserts
`acquireCallCount()`/`presentCallCount()` stay at their pre-loop baseline
(0). Confirmed load-bearing (see below) — with the short-circuit
disabled, this doesn't just fail an assertion, it **crashes** (see
"Revert evidence").

**Row 8 (hidden-window CI extent — resolved empirically).** Observed
directly, not assumed: a 64×64 **hidden** window's real queried
`vkGetPhysicalDeviceSurfaceCapabilitiesKHR::currentExtent` on this dev
machine (NVIDIA proprietary driver, X11/Xorg session) is exactly `64x64`,
never zero — confirmed by the `"surface extent is 64x64 -- resuming
presentation"` log line above and by the two regression `CHECK`s added
(next paragraph) never tripping across the whole suite. I did not add a
temporary debug line and remove it (the brief's literal suggestion);
instead the guard's own **permanent** `RX_LOG_INFO` lines on every
suspend/resume transition already surface the observed extent whenever it
matters, in every build — a disclosed deviation choosing a durable
diagnostic over a one-shot temporary one. CI's own backend (Xvfb+lavapipe
per `.github/workflows/ci.yml`, confirmed by grep) was not independently
re-run by me (no CI access from this environment) — this is a real,
disclosed gap; the local finding is the evidence available to me, and the
mechanism (a real surface-capabilities query) is platform-independent by
construction.

**Row 8 regression CHECK.** Added to both existing fixtures: a
`CHECK_FALSE(device->isSuspended())` immediately after acquire in
`frame_sync_test.cpp`'s 3-iteration loop (plus after the loop) and after
`Device::create()` in `device_test.cpp`'s create test. Both pass; the full
suite (`rx_platform_tests` + `rx_rhi_vk_tests`, unmodified pre-existing
tests included) is unaffected — 68/68 test cases, 1990/1990 assertions,
0 failed (`rx_rhi_vk_tests`); 6/6, 33/33 (`rx_platform_tests`).

**Row 9 (windowed↔fullscreen GPU test, ≥2 cycles, cumulative zero
validation errors).** `window_state_test.cpp`'s double-toggle test runs 2
full cycles, asserts `recreateSwapchain` success + `isSuspended()==false`
+ (when a real resize is observed) extent parity after every transition,
and checks `hasValidationErrors()` once at the very end (catches a
second-cycle-only leak, not just per-transition).

**Row 13 (D25 — uploads not gated on suspension).**
`window_state_test.cpp`'s dedicated test builds a real `Allocator`+
`Uploader` against a **suspended** `Device`, uploads 256 bytes, and
`flush()`+`wait()`s the ticket — `REQUIRE` success, a generous 2s
wall-clock sanity bound (not the primary claim; matches Task 11's own
"timer around the call, not frame counters" convention), and
`isSuspended()` still true afterward (upload activity did not
side-effect-resume presentation). Observed: 220µs, zero validation
errors.

**Row 16 (MANUAL_VERIFICATION.md rows).** Added to the existing
01_triangle Linux/Windows/Steam-Deck subsections: unchecked minimize and
fullscreen-toggle rows, each honestly marked "not yet performed" — the
Linux subsection explicitly notes these are NOT covered by the pre-existing
2026-08-10 PASS entry (which predates this feature).

## Two-tier tests — what exists, file by file

- `src/rx_platform/tests/window_test.cpp` (device-free): event-state
  machine (MINIMIZED/RESTORED/PIXEL_SIZE_CHANGED via `SDL_PushEvent`),
  cross-window-ID isolation, `logWaylandMinimizeLimitationOnce` capture,
  SDL-level fullscreen toggle (`isFullscreen()`/`SDL_GetWindowFullscreenMode`).
- `src/rx_rhi_vk/tests/window_state_test.cpp` (new file, GPU): zero-extent
  DI-seam guard, suspended call-count assertion, D25 upload-not-gated
  interaction, real windowed↔fullscreen double-toggle.
- `src/rx_rhi_vk/tests/frame_sync_test.cpp`,
  `src/rx_rhi_vk/tests/device_test.cpp`: row-8 regression `CHECK`s added,
  otherwise unmodified and still passing.

## Samples: `--fullscreen` + Suspended-status handling

Scope decision: applied `--fullscreen` to **all 8** samples that already
carry `--vsync` (mirrors Task 6's own "all six samples gain --vsync"
precedent, and the plan text's literal "samples gain --fullscreen where
present-mode flags already exist" — not narrowed to 08_gltf_viewer alone,
despite the brief's "Landed context" bullet only naming that one sample
by way of situational awareness).

More importantly, **every** sample's present loop now handles
`SwapchainStatus::Suspended` from `acquireNextImage()`. This was not
optional cosmetics: I traced that without it, a real OS-level minimize
(driving a genuine zero-extent resize through the *existing*
`NeedsRecreate` branch) would let `recreateSwapchain()` enter the
suspended state (0 swapchain images) while the *unmodified* branch went
on to rebuild per-swapchain-extent resources regardless —

- `03_bindless_mesh`'s depth buffer (`createDepthBuffer(...,
  device->swapchainExtent())`, an invalid 0×0 `VkImage` create at that
  extent), and
- `05_multipass`/`06_materials`'s `compileForExtent(device->
  swapchainExtent())` (re-runs `graph.compile()` with a 0×0 extent,
  which would bake an invalid 0×0 transient-image plan into the graph)

— both real, would-crash-or-validation-error paths I found by reading
each sample's actual resize code, not assumed. Both are now gated on
`device->isSuspended()`, in both the acquire- and present-side
`NeedsRecreate` branches, plus the new dedicated `Suspended` branch. `07_stress`
and `08_gltf_viewer` never re-run `graph.compile()` on resize (only
`executor->realize(graph)`, which rebuilds off the graph's own
already-compiled, startup-time dimensions, never the live swapchain
extent) — verified by reading `Executor::realize()`'s own implementation
(`src/rx_graph/executor.cpp:855`, derives extents from
`graph.compiled()`, not a live `Device` query) — so no extra guard was
needed around those two samples' `realize()` calls beyond the
rebuild-only-once-resumed shape every Suspended branch already uses.
`01_triangle`/`02_hotreload`/`04_streaming` have no extent-dependent
resource beyond the swapchain image views themselves (already safe at 0
count — a trivial empty loop).

The suspended branch pattern (uniform across all 8): a 16ms sleep (avoid
busy-spinning the CPU while there's no vsync/present call to throttle the
loop), then `recreateSwapchain(surface)` (re-queries the real extent
every iteration — cheap: no swapchain object is created/destroyed while
still zero, only a `vkDeviceWaitIdle` + one capabilities query), then
rebuild-and-realize only once `isSuspended()` reports false.

## Build & test evidence (both presets)

**linux-native**, full suite, current HEAD:
```
100% tests passed, 0 tests failed out of 22
Total Test time (real) = 226.67 sec
```
(22/22: `shader_spirv_test`, `rx_core_tests`, `rx_task_tests`,
`rx_platform_tests`, `rx_shader_tests`, `rx_rhi_vk_tests`,
`rx_asset_tests`, `rx_asset_gltf_tests`, `rx_asset_gltf_gpu_tests`,
`rx_graph_tests`, `rx_graph_gpu_tests`, `rx_material_gpu_tests`,
`rx_material_tests`, all 8 `sample_0N_*_headless`,
`sample_08_gltf_viewer_quit_during_load`.) An earlier full run (before a
concurrent agent's in-progress texture-slot-bug fix in `rx_asset`/
`rx_material`/sample 08 finished landing) showed 3 unrelated failures
(`rx_asset_gltf_gpu_tests`, `rx_material_gpu_tests`,
`sample_08_gltf_viewer_headless`) — their own log content (`DIAG` lines,
`D17 loaded_scene gate ... pass=false`, texture-binding validation
errors) was unambiguously about material/texture correctness, not
window/swapchain state, and none of my changed code paths appeared
anywhere in that failure output (grepped for
`recreateSwapchain|suspend|fullscreen|window_state|isSuspended` — zero
matches). The re-run above, after that work settled, confirms 100% green.

**windows-cross-zig**: full build, 67/67 targets, zero errors (all 8
sample `.exe`s + `rx_platform_tests.exe`/`rx_rhi_vk_tests.exe` link
cleanly). Wine execution, matching CI's own exclusion convention
(`.github/workflows/ci.yml`'s `-E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|
sample'` — GPU/sample binaries excluded, no real Vulkan under Wine):
```
xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample' --output-on-failure
100% tests passed, 0 tests failed out of 10
Total Test time (real) = 95.82 sec
```
`rx_platform_tests` (containing every new device-free Window test in this
task) is in that set and passed under real Wine in 1.80s.
`rx_rhi_vk_tests` (containing `window_state_test.cpp`) is excluded from
Wine by the project's own standing convention (needs real Vulkan) —
verified only under `linux-native`, per that same convention.

## Revert evidence (load-bearing, scratch in-place revert/restore — not a
separate worktree; each cycle: disable → rebuild → run failing test →
restore → rebuild → confirm green again; `grep -rn "REVERT-TEST"`
confirms zero leftover markers in the final tree)

1. **Zero-extent guard** (`if (queriedExtent.width==0||height==0)` →
   `if (false && ...)`): the extentOverride-seam test's 5 assertions FAIL
   (`isSuspended()` stays false, a real swapchain gets built instead —
   the override is only consulted for the guard decision, so the fake
   `{0,0}` never reaches the real build call, meaning this specific
   revert doesn't crash but does meaningfully fail the test).
2. **Suspended short-circuit in acquireNextImage/present**
   (`if (suspended_)` → `if (false && suspended_)`): the call-count
   test **crashes with SIGSEGV** after real Vulkan validation errors
   (`VUID-vkAcquireNextImageKHR-swapchain-parameter`, "Invalid
   VkSwapchainKHR Object 0x0") — doctest itself reports `test case
   CRASHED`. This is the exact live crash/validation-failure path FG7
   exists to close.
3. **Window event-state switch** (guarded with `if (false)`): the
   device-free state-machine test fails 6 assertions (minimize/size
   updates never happen).
4. **`Window::setFullscreen`** (short-circuited to `return true;` before
   ever calling SDL): the double-toggle GPU test's `isFullscreen()`
   assertions fail (2 assertions) even though the honest fallback MESSAGE
   still fires correctly for the no-real-resize case.
5. **`logWaylandMinimizeLimitationOnce`** (short-circuited to `return;`
   unconditionally): 3 assertions fail (callCount stays 0 for the
   `"Wayland"` case).

After every individual revert+restore cycle, the full
`rx_platform_tests`/`rx_rhi_vk_tests` binaries were rebuilt and re-run
green (68/68, 1990/1990 and 6/6, 33/33) before moving to the next.

## Deviations (disclosed)

1. **Wayland log trigger timing.** Matrix row 3's proposed wording was
   "on first swapchain-recreation-with-nonzero-extent-after-a-suspend".
   I instead fire it once at `Window::create()` time (parameterized by
   platform name for testability). Rationale: the substantive content
   (diagnosing the Wayland minimize-detection gap) and the one-shot,
   device-free-testable shape are preserved; tying it to a live
   suspend/resume cycle would make it untestable without a real 0-extent
   surface (the same unreachable-headlessly problem row 6 already
   documents) and would mean a host running entirely windowed/maximized
   session never sees the diagnostic at all.
2. **Row 1's device-free test uses a delta/baseline comparison, not an
   absolute-`{0,0}`-after-any-pump assertion**, for the "MINIMIZED/
   RESTORED never move the size" claim (after the very first, zero-events
   -processed check, which IS asserted as exactly `{0,0}`). Found and
   fixed during this task: on this dev machine's real X11 desktop,
   `SDL_CreateWindow()` genuinely enqueues real, asynchronously-arriving
   `PIXEL_SIZE_CHANGED` events (observed non-deterministic timing across
   pumpEvents() calls) — an absolute-zero assertion after any pump was
   flaky on a real desktop (not bare Xvfb). The delta approach is
   arguably a MORE faithful test of the actual claim (never a smaller
   assertion), documented inline with the reproduction.
3. **Uploads-not-gated test's wall-clock check is secondary**, per
   design (not primary evidence) — matches Task 11's own established
   convention.
4. **8 samples get `--fullscreen`, not just 08_gltf_viewer** — see
   "Samples" section above for the reasoning (Task 6 precedent; literal
   plan-text scope).
5. `07_stress` has no dedicated `## 07_stress` section in
   `samples/README.md` at all (a pre-existing gap, confirmed via full-file
   grep — not introduced by this task); I did not add one (out of scope
   for a window-hardening ticket), so `07_stress` did not get a
   `--fullscreen` README row.

## Self-review

- Grepped the full diff for `REVERT-TEST`, `TEMP DIAG`, `TODO`, `FIXME`,
  `XXX` — clean.
- `RX_ASSERT_MAIN_THREAD` added to `Device::acquireNextImage`/`present`/
  `recreateSwapchain` (previously undocumented-only main-thread-only
  surface, now enforced — matches the brief's D5-convention note; not
  added to `setPresentMode` since that method wasn't touched this task).
- Verified `Device`'s move-assignment/constructor correctly carries
  `suspended_`/`acquireCallCount_`/`presentCallCount_` (both the "move
  from" and "reset other" halves).
- Confirmed `git diff` of every file I claim as mine contains only my own
  changes (no `DIAG` content, no unrelated hunks) before staging.

## Concerns for the coordinator

1. **Commit split by a concurrent-agent git race, not by design.** My
   9 core `rx_rhi_vk`/`rx_platform` files landed inside `ed5239a`, a
   commit authored by a different agent (coordinator docs/registry work),
   because both of us staged+committed in the same shared working tree at
   nearly the same moment — `git commit` commits whatever is in the index
   at call time regardless of who staged it. I did not author that commit
   invocation. Content integrity is verified intact (`git diff HEAD --
   <my 9 paths>` is empty), but the commit message/authorship context for
   that portion of my work is not mine, and it's bundled with an unrelated
   docs change and another agent's texture-fixture files. I chose NOT to
   rewrite/amend shared history to fix this (explicitly against the git
   safety protocol I'm bound by, and risky now that other agents may have
   already built on top of `ed5239a`) — flagging for the coordinator to
   decide whether this needs any follow-up (e.g., a note in the ledger).
2. **CI's own Xvfb+lavapipe backend was not independently exercised by
   me** for the row-8 hidden-window-extent empirical claim (no CI access
   from this environment) — my evidence is from this dev machine's real
   X11/NVIDIA session. The mechanism is platform-independent by
   construction (a real Vulkan surface-capabilities query), but this is
   disclosed rather than asserted as CI-verified.
3. **The fullscreen double-toggle GPU test's resize-parity assertions are
   environment-conditional** (only checked when a real resize is
   observed; otherwise an honest MESSAGE + the always-real
   zero-validation-errors claim). On this dev machine (real X11 desktop)
   it exercised the real path every time I ran it. Whether CI's bare
   Xvfb (no window manager, per the workflow file) exercises the real
   path or the honest-fallback path is unconfirmed by me.
4. **True OS-level minimize under a live `--present` session has not been
   run by a human** — MANUAL_VERIFICATION.md rows are honestly unchecked,
   per the matrix's own row 16 conclusion that this is not reproducible
   headlessly on any driver this repo's fixtures use.
