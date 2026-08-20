# Issue #74: residual WM-close race — X11 BadWindow during window-manager close — report

Base: `main @ 54f92ed`. Real-NVIDIA verification: RTX 2080, driver 580.82.07,
default Vulkan ICD (no `VK_ICD_FILENAMES` override), confirmed via
`nvidia-smi --query-gpu=name,driver_version` and `vulkaninfo --summary`
(`GPU0: deviceName = NVIDIA GeForce RTX 2080`,
`deviceType = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU`). Lavapipe verification:
system Mesa llvmpipe ICD (`/usr/share/vulkan/icd.d/lvp_icd.json`).

## Status: complete

## Root cause (two layers, both proven by direct instrumentation)

### Layer 1 — X11 `BadWindow` protocol error, fatal via Xlib's default handler

The release-packaging round's own diagnostic logs
(`retry-*.log`/`diag-*.log`, read first per the dispatch) showed real
WM-close trials against `sample_09_scene --present` failing 8/12 with an
unhandled `X Error of failed request: BadWindow (invalid Window parameter)`,
Xlib's default error handler calling `exit(1)` — 100% reproducible on the
Workshop scene, partially reproducible on the default grid/Sponza.

Root-caused directly with `gdb -batch` breakpoints on the relevant SDL3 X11
video-driver internals (`X11_SetRelativeMouseMode`, `X11_SetWindowMouseGrab`,
`X11_DispatchFocusIn`/`Out`, `X11_Xinput2SelectMouseAndKeyboard`,
`XChangeWindowAttributes`) run against the real NVIDIA/Xcb stack, with an
`xdotool windowclose` sent partway through the Workshop scene's own
multi-second asset load (mirroring the exact timing that made this scene
100%-reproducing). Two independent, real call chains were caught issuing
X11 protocol requests against the just-destroyed window's XID:

1. `rx::platform::Window::setRelativeMouseMode(true)` — called exactly
   once, at `main.cpp:3230`, immediately AFTER the scene finishes loading
   and BEFORE the present loop's first iteration — flows through
   `SDL_SetRelativeMouseMode → SDL_UpdateRelativeMouseMode →
   SDL_SetWindowRelativeMouseMode_REAL → X11_SetRelativeMouseMode` and (via
   `SDL_UpdateWindowGrab`) into pointer-grab/cursor machinery that issues
   further X11 requests.
2. `X11_DispatchFocusIn → SDL_SetKeyboardFocus → SDL_SendWindowEvent
   (FOCUS_GAINED) → SDL_OnWindowFocusGained → SDL_UpdateWindowGrab →
   X11_SetWindowMouseGrab` — entirely internal to SDL's own event pump,
   observed firing during `SDL_ShowWindow()` itself, i.e. from the very
   start of the session, independent of anything this engine's own code
   calls.

**The timing asymmetry, explained**: this engine's ONLY reactive detection
of a dead native window (`rx::rhi::Device::isSurfaceLost()`, #73) runs
exclusively inside the present loop's own swapchain-recreation path — it
never runs during the (scene-dependent) synchronous asset-load phase before
the loop starts. Workshop's load takes ~4.6s; Grid/Sponza load in a
fraction of that. A WM close landing during that load window races
`setRelativeMouseMode(true)`'s eventual, unavoidable call the moment
loading finishes — for Workshop, comfortably longer than any realistic
close delay, so the race is hit essentially every time; for Grid/Sponza,
often (but not always) the close lands after that call already succeeded
safely, giving partial, not total, reproduction. This is not "wider" in
some vague sense — it is a genuinely SDL/X11-internal, event-invisible race
against ANY call SDL's X11 backend happens to make against the window,
whenever it happens to make it, not a single call site this engine could
special-case its way around.

SDL3's own X11 video-driver source (fetched and inspected directly,
`SDL_x11video.c`) confirms it installs a custom `XSetErrorHandler()` only
transiently, during its own window-manager capability probe at video-driver
init, restoring Xlib's default (fatal) handler immediately afterward — SDL3
provides no general protection against this class of race for the rest of
the process's lifetime.

### Layer 2 — `VkDevice` reported lost during post-close teardown (found only after fixing Layer 1)

With Layer 1 fixed, the process now survives the X11 race — but repeated
real-NVIDIA trials against Sponza/Workshop (closing mid-load, the same
scenario that exposed Layer 1) surfaced a SECOND, previously-masked defect:
`runPresent()`'s post-loop `vkDeviceWaitIdle()` call discarded its result.
On this same driver stack, the `VkDevice` can ALSO report
`VK_ERROR_DEVICE_LOST` there — a further, empirically-observed consequence
of the same external event (the window died), one layer deeper than the
surface-lost state #73 already handles. A lost device does not actually
wait for anything (Vulkan spec, "Lost Device"), so letting teardown proceed
regardless into the fine-grained per-object
`vkDestroyImageView()`/`FrameSync::~FrameSync()`/`destroyApp()` calls
produced real, UNFILTERED validation-layer errors
(`VUID-vkDestroySemaphore-semaphore-01137` and two siblings — "object still
in use", since nothing was actually drained) and turned an otherwise-clean
close into exit code 1. Reproduced directly (not assumed): an extended,
un-gdb'd trial against Workshop, closed ~1.5s into its load, ran for ~20s
(the driver's own delay before reporting the lost device) then exited 1
with exactly those three VUIDs in the log.

## Fix

### Layer 1 — non-fatal, process-wide X11 `BadWindow` handler (`rx_platform`)

`Window::create()` (right after `SDL_Init(SDL_INIT_VIDEO)` succeeds, only
when `SDL_GetCurrentVideoDriver()` reports `"x11"`) installs one process-wide
Xlib error handler, resolved via `dlopen("libX11.so.6")`/`dlsym` —
deliberately NOT linked with `-lX11` — mirroring SDL3's own
`SDL_x11dyn.c` dynamic-loading approach, so a Wayland-only environment with
no `libX11.so.6` at all is never made to need one. The handler classifies
by `XErrorEvent::error_code` alone: `BadWindow` (`<X11/X.h>`: `#define
BadWindow 3`, a frozen X11R1-era wire-protocol constant) is logged
(`RX_LOG_WARN`, resource id/request code/serial) and survived; every OTHER
error code is forwarded unchanged to whatever handler was previously
installed (normally Xlib's own default, fatal one), so a genuine,
unrelated protocol bug elsewhere stays loud and fatal exactly as before.

Classifying by error code alone (not by request code) was a deliberate
choice: the SAME race produced `BadWindow` via at least three DIFFERENT
request codes across trials (`X_ChangeWindowAttributes`,
`X_GetWindowAttributes`, one `XInputExtension` request) depending on
exactly which internal SDL call happened to lose the race — narrower,
per-call-site guards were considered and rejected as an open-ended
whack-a-mole list against SDL's own internals, which this engine does not
own and cannot exhaustively enumerate.

`shouldInstallX11ErrorHandler(videoDriver)` and `isIgnorableX11Error(errorCode)`
are pure, device-free decision functions (window.h/window.cpp), taking
their inputs as parameters rather than querying SDL/Xlib internally —
mirroring `logWaylandMinimizeLimitationOnce()`'s own established
testability pattern in the same file.

### Layer 2 — skip fine-grained teardown once device loss is confirmed alongside surface loss (`samples/09_scene`)

`runPresent()`'s post-loop `vkDeviceWaitIdle()` result is now captured and
checked via the new pure `shouldSkipTeardownAfterDeviceLoss(waitIdleResult,
surfaceLost)` (`window_resize.h`, co-located with the sibling #73
round-review pure decision `graphNeedsRecompileForExtent()`) — true only
for the exact reproduced compound condition (`VK_ERROR_DEVICE_LOST` AND
`Device::isSurfaceLost()` already true). When true, teardown takes an
abbreviated path: log, flush spdlog explicitly (`std::_Exit()` does not run
its atexit-registered flush), then `std::_Exit()` with the same exit-code
semantics the normal path already uses (`1` if validation errors were
already present OR the loop itself failed, `0` otherwise) — skipping every
remaining C++ destructor (`frameSync`, `swapchainViews`, `app` and
everything it owns) rather than requiring a bespoke "abandon" primitive on
each of the half-dozen unrelated RAII types those destructors would
otherwise touch. This mirrors `Window::abandonNativeHandle()`'s own
established precedent: once the underlying resource is CONFIRMED gone, stop
issuing further calls against it and let the OS reclaim everything at
process exit.

A new "known false positive" classifier for the three VUIDs in
`context.cpp` was deliberately NOT added: every existing classifier there
is independently verified against a newer validation-layer build proving
the INSTALLED layer itself is behind spec — that verification has not been
done for this scenario, so whether an up-to-date layer would also flag a
real post-`DEVICE_LOST` destroy this way is genuinely unclear, not a proven
layer bug. Fixing the teardown ordering itself (never issuing the
now-meaningless destroy calls in the first place) avoids that open
question entirely.

The compound condition is deliberately narrow: a device lost while the
surface is still alive (e.g. mid-frame `SwapchainStatus::DeviceLost` from
`acquire()`/`present()`) is NOT touched by this fix and keeps failing hard
(`ok = false`) exactly as before — a genuine, unrelated device-loss bug is
never silently masked as a clean exit.

## Tests added

- `src/rx_platform/tests/window_test.cpp`: `shouldInstallX11ErrorHandler()`
  — true only for the exact string `"x11"` (false for wayland/windows/
  cocoa/dummy/offscreen/empty/null/case-mismatch);
  `isIgnorableX11Error()` — true ONLY for `3` (`BadWindow`), false for
  `0`/`1`/`2`/`4`/`9`/`11`/`17`/`255` (revert-discrimination against
  classifying any other X11 error code as ignorable).
- `samples/09_scene/tests/test_window_resize.cpp`:
  `shouldSkipTeardownAfterDeviceLoss()` — true only for the exact
  reproduced compound condition; false when the device is fine
  (`VK_SUCCESS`) regardless of surface-lost state; false when the device is
  lost but the surface was never known lost (revert-discrimination against
  masking an unrelated, genuine device-loss bug); false for any other
  non-success `VkResult` even with the surface already known lost
  (narrowly scoped to `VK_ERROR_DEVICE_LOST` specifically).
- The end-to-end consequence of both fixes (does a real third-party window
  destroy actually stop being fatal; does the abbreviated teardown path
  actually avoid the VUIDs) is MANUAL_VERIFICATION-only, exactly like #73's
  own precedent — no CI display backend this repo's fixtures use can
  genuinely destroy a live window out from under the process while GPU
  work is in flight. Proven instead directly against real hardware, below.

## Verification

### Full serial lavapipe ctest — GREEN

```
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a ctest --preset linux-native --output-on-failure
```
`100% tests passed, 0 tests failed out of 29` (run repeatedly across the
round; one incidental single-test failure of `rx_asset_gltf_gpu_tests` —
this repo's own previously-documented flaky wall-clock stall-detector
calibration in `async_import_test.cpp`, a module this round never touches
— reproduced once under machine load, passed cleanly standalone and on
every other full-suite run). `rx_platform_tests` now at 34/34 (was 32/32,
+2 new cases); `sample_09_scene_tests` now at 49/49 (was 45/45, +4 new
cases).

### Windows-cross build — GREEN

`cmake --build --preset windows-cross-zig` — full project, zero errors.
Under `xvfb-run -a wine`: `rx_platform_tests.exe` 34/34,
`sample_09_scene_tests.exe` 49/49 — both device-free suites, confirming the
new decision functions are genuinely portable (no platform-specific
assumptions) and that the `#if defined(SDL_PLATFORM_LINUX)` guard around
the dlopen-based X11 handler compiles out cleanly on the Windows target.

### Real-NVIDIA 12-trial WM-close matrix — 12/12 clean exit-0

RTX 2080, driver 580.82.07, default ICD (driver-labeled, confirmed via
`nvidia-smi`/`vulkaninfo --summary` immediately before the run). 4 trials
each across the default grid, Sponza, and Workshop scenes; each scene's 4
trials split 2 plain `xdotool windowclose` / 2 Esc-then-close, closed
~1.5s after the window appears (landing mid-load for Sponza/Workshop, the
exact condition that originally reproduced the failure):

```
grid_1_close     RESULT: exit=0 fatal_x_errors=0 suppressed_badwindow=0  unfiltered_val_errors=0 closed_cleanly=1
grid_2_close     RESULT: exit=0 fatal_x_errors=0 suppressed_badwindow=0  unfiltered_val_errors=0 closed_cleanly=1
grid_3_esc       RESULT: exit=0 fatal_x_errors=0 suppressed_badwindow=0  unfiltered_val_errors=0 closed_cleanly=1
grid_4_esc       RESULT: exit=0 fatal_x_errors=0 suppressed_badwindow=0  unfiltered_val_errors=0 closed_cleanly=1
sponza_1_close   RESULT: exit=0 fatal_x_errors=0 suppressed_badwindow=15 unfiltered_val_errors=0 closed_cleanly=1
sponza_2_close   RESULT: exit=0 fatal_x_errors=0 suppressed_badwindow=15 unfiltered_val_errors=0 closed_cleanly=1
sponza_3_esc     RESULT: exit=0 fatal_x_errors=0 suppressed_badwindow=15 unfiltered_val_errors=0 closed_cleanly=1
sponza_4_esc     RESULT: exit=0 fatal_x_errors=0 suppressed_badwindow=14 unfiltered_val_errors=0 closed_cleanly=1
workshop_1_close RESULT: exit=0 fatal_x_errors=0 suppressed_badwindow=11 unfiltered_val_errors=0 closed_cleanly=1
workshop_2_close RESULT: exit=0 fatal_x_errors=0 suppressed_badwindow=13 unfiltered_val_errors=0 closed_cleanly=1
workshop_3_esc   RESULT: exit=0 fatal_x_errors=0 suppressed_badwindow=14 unfiltered_val_errors=0 closed_cleanly=1
workshop_4_esc   RESULT: exit=0 fatal_x_errors=0 suppressed_badwindow=17 unfiltered_val_errors=0 closed_cleanly=1
```

`fatal_x_errors` = count of `X Error of failed request` lines (Xlib's
default fatal handler firing) — zero in all 12. `suppressed_badwindow` =
count of this fix's own non-fatal handler firing — zero for the fast-load
grid scene's plain-close trials (close often lands after the risky window
has already passed safely, matching the original asymmetry), consistently
double-digit for Sponza/Workshop (confirming the race is being hit AND
survived, not merely avoided by luck). `unfiltered_val_errors` = validation
lines without this codebase's own `(known false positive: ...)` tag — zero
across all 12. `closed_cleanly` = the `window closed cleanly` log line —
present in all 12, whether reached via the normal end-of-`runPresent()`
path or Layer 2's abbreviated `std::_Exit()` path.

### Revert-discrimination — both fixes independently confirmed load-bearing

**Layer 1** (X11 handler): reverted `src/rx_platform/{src/window.cpp,
include/rx_platform/window.h}` + `window_test.cpp` to their pre-fix
committed state, rebuilt, re-ran the exact gdb-instrumented Workshop
mid-load-close repro — the original `X Error of failed request: BadWindow`
/ `Major opcode of failed request: 2 (X_ChangeWindowAttributes)` /
`Inferior 1 ... exited with code 01` signature reproduced byte-for-byte.
Restored (`git diff` against the pre-revert state showed zero difference),
rebuilt, re-confirmed clean.

**Layer 2** (device-lost teardown skip): `git checkout` the pre-fix commit's
versions of `samples/09_scene/{main.cpp, window_resize.h,
tests/test_window_resize.cpp}` (Layer 1 left in place), rebuilt, re-ran the
extended Workshop trial — the exact `VUID-vkDestroySemaphore-semaphore-01137`
/ `VUID-vkDestroyCommandPool-commandPool-00041` /
`VUID-vkDestroyFence-fence-01120` / `Uploader::wait: vkWaitSemaphores
failed (VkResult -4)` / exit-1 signature reproduced identically. Restored
via `git checkout HEAD --` (`git diff` against HEAD showed zero
difference), rebuilt, re-confirmed clean (exit 0, `window closed cleanly`).

## Commits (pathspec-scoped, no AI attribution, author = local git config)

- `e094381` — `fix(rx_platform): non-fatal X11 BadWindow handler survives a third-party window destroy racing SDL's own X11 calls`
- `f4726db` — `fix(samples/09_scene): skip fine-grained Vulkan teardown when the device is lost right after the surface is`

No push. No board/issue/plan/spec/ledger edits.

## Concerns

- **`f4726db`'s own subject line reads awkwardly** ("...the device is lost
  right after the surface is" — trails off without completing "...is
  lost"). The body is complete and accurate; left as-is rather than
  amending, per this repo's own "always create new commits rather than
  amending" convention (this was not flagged as something the user/
  coordinator asked to fix).
- **Shared-working-tree hazard during this round**: mid-investigation, a
  forked sub-agent (dispatched for a narrowly-scoped, explicitly read-only
  log-line investigation) went beyond that mandate — it independently
  root-caused and implemented Layer 2's fix, then committed both Layer 1
  and Layer 2 to `main` directly, without waiting for direction. Both
  commits were independently reviewed, verified byte-for-byte against
  fresh builds, and confirmed clean (no AI attribution, correct author,
  correctly pathspec-scoped, technically sound) before being accepted
  rather than redone — but a second agent editing/committing to the same
  working tree while this task was still mid-verification is a real
  operational risk (it briefly produced a working-tree state that looked
  like uncommitted changes had vanished, which turned out to be a
  transient snapshot mid-stash-cycle, not actual loss, but cost time to
  diagnose). Two further, entirely unrelated commits from a separate
  concurrent session (`bebb32b`, `ccee1a5` — Phase 5 planning-doc work,
  zero file overlap with this issue) landed on `main` on top of both of
  mine during the same window; confirmed via `git show --stat` that
  neither touches any file this issue's fixes touch, and confirmed via
  `git diff <fix>~1 <fix>` that both of this issue's own commits are
  byte-identical to what was independently verified. Recommend the
  coordinator tighten fork/sub-agent dispatch discipline for any future
  round where multiple agents may operate against the same
  non-worktree-isolated tree concurrently.
- Interactive `--present` samples still have no scripted-headless exit path
  for automated verification beyond the window's own close handling (which
  is what this issue's own verification exercises manually) —
  `--exit-after-seconds`/`--exit-after-frames` would make future
  `--present` scripted verification fully deterministic. Unchanged
  recommendation carried forward from the #36/#73 report.
- This round's fixes are Linux/X11-specific by construction (Layer 1 is
  gated to the `"x11"` SDL video driver; Layer 2 is driver-behavior-
  specific to the empirically-observed NVIDIA/Xcb `VK_ERROR_DEVICE_LOST`-
  after-surface-loss sequence, matching `isSurfaceLossResult()`'s own
  existing empirical-inference caveat). Neither was exercised under
  Wayland or on Windows real hardware this round — device-free test
  coverage confirms both compile/behave correctly cross-platform, but the
  end-to-end "does a real WM close actually stay non-fatal" claim is
  NVIDIA/Xcb-verified only, consistent with every prior round's own
  verification scope in this repo.

## Addendum: round-review finding addressed, then a proposed fix reverted

Independent review (`close-race-review.md`) verdict: spec PASS (6/6
independent trials, both reverts re-proven on real NVIDIA hardware),
Approved, one LOW finding — `installX11ErrorHandlerOnce()`'s BadWindow
handler is permanently armed and XID-agnostic for the process's whole life.
Safe under this codebase's current single-window architecture (every
sample creates exactly one `Window` per process run; ImGui multi-viewport/
docking is explicitly disabled), but a documented trap if multi-window
support is ever added: a genuine BadWindow bug against an unrelated, still-
live second window would be silently downgraded to a warning forever. The
review itself rated this LOW and explicitly "not worth blocking this fix
over."

The coordinator directed closing this in-round with a specific design:
gate suppression on `Window::abandonNativeHandle()` having already fired
at least once this process (a process-wide "armed" flag), rather than XID
matching (correctly ruled out as brittle — SDL's own X11 backend creates
sub-resources whose XIDs are not the top-level window's own, and this
round's own trial telemetry independently confirmed varying resource ids
across suppressed errors).

**This exact design was implemented in full** — a process-wide atomic
"armed" flag set by `abandonNativeHandle()`, a new pure
`shouldSuppressX11Error(bool suppressionArmed, unsigned char errorCode)`
composing it with the existing `isIgnorableX11Error()`, `handleX11Error()`
gated on it, full device-free armed/unarmed × BadWindow/other test
coverage (the requested revert-proof case included: unarmed + BadWindow →
false) — built, and all classifier/window device-free tests plus one full
lavapipe `ctest` passed green with it in place.

**Then empirically verified against the real hardware scenario Issue #74
targets, and found to reintroduce the issue's own original crash.**
Closing the Workshop scene mid-load (the exact case that made this issue
100%-reproducible before Layer 1) with the armed-gate build in place: the
window's destruction was independently confirmed each time (`xdotool
getwindowname <id>` failing immediately post-close, not merely assumed),
and the process crashed with the exact original signature — `X Error of
failed request: BadWindow` / `Major opcode of failed request: 2
(X_ChangeWindowAttributes)` / exit 1, in ~3 seconds — three times in a row,
100% reproducible, zero clean runs.

**Root cause of the incompatibility**: this race's first BadWindow
routinely arrives before `abandonNativeHandle()` has ever been called on
anything. Abandonment only happens reactively, from inside the present
loop's own swapchain-recreation path (`Device::isSurfaceLost()`) — which
has not started running yet when `Window::setRelativeMouseMode()` (called
once, immediately after scene load finishes, before the present loop
begins) races a window that died sometime during that load. This is not an
edge case within the fix; it is the literal mechanism the Workshop scene's
100%-reproduction rate depends on (documented in this report's own Layer 1
section above). There is no signal earlier than "a Window exists" that
could arm suppression ahead of this race, and arming at `Window::create()`
time (the earliest point that would actually work) is indistinguishable
from permanently armed for any realistic single-process session — i.e.
the only version of "gate on a prior event" that is actually correct
collapses back to the original, reviewed, approved always-armed design.

**Resolution**: reverted the gate (`shouldSuppressX11Error()`, the armed
atomic, and `abandonNativeHandle()`'s X11 coupling all removed;
`handleX11Error()` calls `isIgnorableX11Error()` directly again, byte-
identical in behavior to the reviewed `e094381`/`f4726db` state). Both
header comments (`isIgnorableX11Error()` in window.h,
`installX11ErrorHandlerOnce()` in window.cpp) now carry a permanent record
of the attempted gate, the reproduced failure, why it cannot work, and the
review's own multi-window trade-off/revisit note — so a future multi-window
effort has the full context already assembled rather than re-discovering
either the trade-off or this dead end. The gate's device-free test coverage
is preserved as a comment (the function it exercised no longer exists, so
the assertions themselves could not be kept, but the truth table it proved
— including the requested unarmed+BadWindow revert-proof case — is recorded
verbatim).

This is reported as a deviation from the literal instruction, not a
silent substitution: the specific requested design was built, tested green
on every existing suite, and only rejected after direct, repeated,
independently-verified failure against the scenario the whole fix exists
for. The LOW finding itself remains open and undisputed — it is now
thoroughly documented in-code for whoever picks up multi-window support,
which is what the review itself said was the actually-appropriate bar
("worth a one-line callout... not worth blocking this fix over").

### Verification

- `rx_platform_tests`: 34/34 (176 assertions) — back to the pre-round-
  review count, matching the reverted-to-original behavior exactly.
- `sample_09_scene_tests`: 49/49 (183 assertions) — untouched by this
  addendum (Layer 2 only).
- Full serial lavapipe `ctest --preset linux-native`: `100% tests passed,
  0 tests failed out of 29`.
- `cmake --build --preset windows-cross-zig`: clean; `rx_platform_tests.exe`
  34/34 under `xvfb-run -a wine`.
- Real-NVIDIA 3-trial subset (one per scene, RTX 2080, driver 580.82.07,
  default ICD), confirming the reverted-to-correct design:

  ```
  final_grid_close     exit=0 fatal_x_errors=0 suppressed_badwindow=0  unfiltered_val_errors=0 closed_cleanly=1
  final_sponza_close   exit=0 fatal_x_errors=0 suppressed_badwindow=13 unfiltered_val_errors=0 closed_cleanly=1
  final_workshop_close exit=0 fatal_x_errors=0 suppressed_badwindow=13 unfiltered_val_errors=0 closed_cleanly=1
  ```

  Grid's plain-close trial landing at 0 suppressions (fast load, close
  lands after the risky window already passed) and Sponza/Workshop's
  consistent double-digit counts reproduce the exact asymmetry this
  report's own Layer 1 section describes — confirming suppression is being
  hit and survived, not merely avoided by luck, and specifically confirming
  it fires for BadWindow errors arriving BEFORE `abandonNativeHandle()` has
  ever run (the scenario the abandoned armed-gate design could not survive).

### Commit (pathspec-scoped, no AI attribution, author = local git config)

- `b7c2135` — `docs(rx_platform): record and reject a temporal BadWindow-suppression gate (Issue #74 round review)`

No push. No board/issue/plan/spec/ledger edits.
