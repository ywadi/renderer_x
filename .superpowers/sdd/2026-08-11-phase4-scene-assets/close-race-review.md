# Issue #74: residual WM-close race — independent review

Reviewed: `e094381` (`fix(rx_platform)`: non-fatal X11 BadWindow handler) and
`f4726db` (`fix(samples/09_scene)`: skip fine-grained teardown on confirmed
device-lost), on top of `main @ 54f92ed`. Two unrelated coordinator commits
(`bebb32b`, `ccee1a5`, Phase 5 planning docs) sit above them on `main` and
were confirmed zero-file-overlap (`git show --stat`) — not in scope, ignored.

Inputs: `gh issue view 74`, `close-race-residual-report.md`,
`review-54f92ed..f4726db.diff`. This review did not write the code under
review; it independently re-derives every claim in the report against the
actual diff, the actual source, and fresh hardware/build evidence gathered
in this round (real NVIDIA RTX 2080, driver 580.82.07, default ICD;
lavapipe `lvp_icd.json`; both confirmed via `nvidia-smi`/`vulkaninfo
--summary` before use).

## Verdict 1 — Spec compliance vs Issue #74's 12/12 tri-scene bar: **PASS**

Issue #74's fix bar: "12/12 clean-exit close trials across default grid,
Sponza, and Workshop on real NVIDIA (driver-labeled), plus the existing
suites." The report's own 12-trial matrix (4 each: grid/Sponza/Workshop,
mixed plain-close/Esc-then-close) shows 12/12 exit-0. This review
independently re-ran a 6-trial subset (2 per scene, plain `xdotool
windowclose` ~1.5s after window-appear, landing mid-load for Sponza/
Workshop) against the actual built binary and got 6/6 exit-0, telemetry
present and consistent:

```
grid_2_close     exit=0 fatal_x_errors=0 suppressed_badwindow=0  unfiltered_val_errors=0 closed_cleanly=1
grid_smoke_close exit=0 fatal_x_errors=0 suppressed_badwindow=0  unfiltered_val_errors=0 closed_cleanly=1
sponza_1_close   exit=0 fatal_x_errors=0 suppressed_badwindow=13 unfiltered_val_errors=0 closed_cleanly=1
sponza_2_close   exit=0 fatal_x_errors=0 suppressed_badwindow=13 unfiltered_val_errors=0 closed_cleanly=1
workshop_1_close exit=0 fatal_x_errors=0 suppressed_badwindow=11 unfiltered_val_errors=0 closed_cleanly=1
workshop_2_close exit=0 fatal_x_errors=0 suppressed_badwindow=13 unfiltered_val_errors=0 closed_cleanly=1
```

Grid's plain-close trials landing at 0 suppressed (fast load, close often
lands after the risky window has passed) and Sponza/Workshop's consistent
double-digit counts reproduce the exact asymmetry the report describes.
Additionally, this review confirmed Layer 2's `std::_Exit()` abbreviated
path fired in 4/6 trials (both Sponza, both Workshop; `grep -c "skipping
further Vulkan teardown"` == 1 in each of those four logs, 0 in both grid
logs) — direct evidence both layers of the fix are being exercised by real
hardware, not just Layer 1.

Existing suites: full serial lavapipe `ctest --preset linux-native`,
`100% tests passed, 0 tests failed out of 29` (run twice this round,
bracketing the revert-discrimination work below). `rx_platform_tests`
34/34 (176 assertions), `sample_09_scene_tests` 49/49 (183 assertions) —
both match the report's claimed counts exactly.

Not independently re-verified this round: the report's Windows-cross build
claim (`cmake --build --preset windows-cross-zig`, GREEN, 34/34 + 49/49
under `wine`). Out of this review's empirical checklist scope. Code read
confirms the `#if defined(SDL_PLATFORM_LINUX)` guard correctly compiles out
every X11-specific line, and `shouldInstallX11ErrorHandler()`/
`isIgnorableX11Error()`/`shouldSkipTeardownAfterDeviceLoss()` are plain,
platform-agnostic C++ with no Linux-specific dependency — so the claim is
plausible on inspection but taken on the report's word, not re-run.

## Verdict 2 — Code quality: **Approved**, one LOW finding

### Handler-scoping verdict (hardest-scrutiny item) — explicit answer

The X11 error handler installed by `e094381` is **permanently armed for
the rest of the process's life**, not armed/disarmed around a specific
window's teardown window. `installX11ErrorHandlerOnce()` runs once
(atomic `compare_exchange_strong` guard) inside `Window::create()`, and the
installed `handleX11Error()` stays the process's Xlib error handler
forever after — there is no corresponding "disarm" call anywhere in the
diff or the wider codebase (confirmed via `grep -rn XSetErrorHandler` —
the only call site is this one `installX11ErrorHandlerOnce()`). The report
and the code comments are explicit and honest about this being deliberate
("a third-party window destroy can happen at ANY point across the whole
session... this installs ONE process-wide non-fatal handler").

Assessed against each sub-question:

- **Suppression predicate tightness**: classifies by `error_code == 3`
  (`BadWindow`) ALONE — it does **not** check `event->resourceid` against
  the specific window that is expected to be dying. Verified directly:
  `src/rx_platform/src/window.cpp:60` (`isIgnorableX11Error(event->error_code)`)
  never reads `resourceid` for the decision, only for the log line. This
  means any BadWindow anywhere in the process, for any window, for the
  rest of the process's life, is now non-fatal.
- **Is that a real gap today?** Practically low-risk under this codebase's
  actual architecture: `rx_debug_ui/overlay.h:74` explicitly disables
  ImGui docking/multi-viewport ("vulkan backends only -- no docking/
  multi-viewport"), and every sample creates exactly one `Window` per
  process run (`grep -rn "Window::create"` across `samples/` — one
  headless + one `--present` call site per sample, never both live at
  once). With a single window and no secondary platform windows, there is
  currently no OTHER window whose BadWindow this handler could wrongly
  swallow.
- **Every suppression logged?** Yes — confirmed both by code read
  (`RX_LOG_WARN` in `handleX11Error()`, `window.cpp:61-68`, includes
  resource id/request code/minor code/serial) and empirically: this
  review's own trial logs show lines like `X11 BadWindow protocol error
  suppressed (resource=0x3a00031, request_code=2, minor_code=0,
  serial=1643)`, with the SAME resource id (`0x3a00031`) and request code
  (`2`, `X_ChangeWindowAttributes`) that this review's own Layer-1-reverted
  repro (below) caught Xlib's default handler treating as fatal — direct
  cross-validation that the log line correctly identifies the exact same
  request that used to kill the process.
- **Chained, not replaced?** Confirmed by code read:
  `g_previousX11ErrorHandler` stores whatever `XSetErrorHandler()` reports
  as previously installed, and every non-BadWindow error code is forwarded
  to it unchanged (`window.cpp:71-75`) — a genuine, unrelated X11 protocol
  bug elsewhere stays fatal exactly as before. Verified by code inspection
  only, not by empirically triggering a distinct real X11 error class in
  this round (see "not verifiable" below).
- **ABI mirror struct correctness**: `XErrorEventAbi` (`window.cpp:38-46`)
  was independently checked against the real `<X11/Xlib.h>` (`typedef
  struct { int type; Display *display; XID resourceid; unsigned long
  serial; unsigned char error_code; unsigned char request_code; unsigned
  char minor_code; } XErrorEvent;`) — field order and sizes match exactly.
  This was worth checking directly rather than trusting the comment: a
  transposed field (e.g. `resourceid`/`serial` swapped) would have silently
  misclassified errors while still "looking like it worked" in casual
  testing. It doesn't; it's correct, cross-validated by the same
  resource-id match noted above.

**Finding [LOW]**: the handler's permanence and XID-agnostic predicate are
a deliberate, honestly-documented, currently-safe tradeoff given this
engine's single-window/no-multi-viewport architecture — not a bug. Flagged
because the task specifically asked for this scrutiny: if multi-window
support (e.g. ImGui multi-viewport, a second present window, a tool window)
is ever added, this handler would silently downgrade a genuine BadWindow
bug against an UNRELATED window to a warning for the rest of that
process's life. Worth a one-line callout in `docs/threading.md` or
wherever multi-window support eventually gets designed, not worth blocking
this fix over.

### dlopen approach

Rationale is explicitly stated and correct: avoids a hard `-lX11` link
dependency so a Wayland-only environment with no `libX11.so.6` installed is
never forced to need one, mirroring SDL3's own `SDL_x11dyn.c` approach
(confirmed this describes real SDL3 behavior via the report's own direct
source inspection, consistent with SDL3's documented dynamic-loading
design). Failure paths are clean: `dlopen` failure and `dlsym` failure both
log a `RX_LOG_WARN` and return without installing anything — the process is
left exactly as fatal-on-BadWindow as it was before this fix, not worse.

### Thread safety

`XSetErrorHandler()` is called exactly once, inside `Window::create()`,
after `SDL_Init(SDL_INIT_VIDEO)` succeeds and before `SDL_CreateWindow()`
is called or a live `Window` is handed back to any caller — i.e. before
this engine's own code (or the returned window) can generate any further
X11 traffic. `Window`'s other methods (`pumpEvents()`,
`setRelativeMouseMode()`, etc.) all carry `RX_ASSERT_MAIN_THREAD` guards
per this codebase's own documented "D5, Phase 4" main-thread-only
convention (`docs/threading.md`), and every real call site in the
codebase calls `Window::create()` from `main()`, never from a worker
thread. No evidence of concurrent Xlib access at install time.

### Layer 2 — device-lost teardown skip

Confirmed by direct code read (`samples/09_scene/main.cpp:3700-3715`,
`window_resize.h:242-244`) that the skip is gated on the exact compound
condition — `waitIdleResult == VK_ERROR_DEVICE_LOST && surfaceLost` — never
on surface-lost alone. `shouldSkipTeardownAfterDeviceLoss()`'s own test
suite includes the correct revert-discrimination case
(`VK_ERROR_DEVICE_LOST` with `surfaceLost=false` → `false`, so a genuine
mid-frame device-loss-while-window-still-alive bug keeps failing hard via
the pre-existing `ok = false` path). The normal (non-skip) path is
byte-identical to the pre-fix code: `vkDeviceWaitIdle()`'s result is now
captured into a local instead of discarded, but when
`shouldSkipTeardownAfterDeviceLoss()` returns false, control falls through
to the exact same `vkDestroyImageView`/`frameSync.reset()`/`destroyApp()`/
validation-check/`!ok`-check/`"window closed cleanly"` sequence that
existed before this diff — traced line-by-line, confirmed identical.

## Revert-discrimination — both independently reproduced

**Layer 1**: `git checkout 54f92ed -- src/rx_platform/{src/window.cpp,
include/rx_platform/window.h,tests/window_test.cpp}`, rebuilt, ran one
Workshop mid-load close trial — reproduced on the first attempt:
`X Error of failed request: BadWindow (invalid Window parameter)` /
`Major opcode of failed request: 2 (X_ChangeWindowAttributes)` / exit 1 —
byte-for-byte the signature the report cites. Restored via `git checkout
HEAD --`, `git diff HEAD` showed zero difference, rebuilt, re-confirmed
clean (exit 0, `suppressed_badwindow=13`).

**Layer 2**: `git checkout 54f92ed -- samples/09_scene/{main.cpp,
window_resize.h,tests/test_window_resize.cpp}` (Layer 1 left in place),
rebuilt, ran one extended Workshop trial (~21.5s total, matching the
report's "~20s driver delay") — reproduced exactly:
`VUID-vkDestroySemaphore-semaphore-01137` (x2) /
`VUID-vkDestroyCommandPool-commandPool-00041` /
`VUID-vkDestroyFence-fence-01120` / `Uploader::wait: vkWaitSemaphores
failed (VkResult -4)` / exit 1. Restored via `git checkout HEAD --`,
`git diff HEAD` showed zero difference, rebuilt, re-confirmed clean (exit
0, `window closed cleanly`).

After both restorations, a final full lavapipe `ctest` run confirmed
`100% tests passed, 0 tests failed out of 29` — the working tree and build
are back to exactly the state under review.

## Commit hygiene: clean

- Author/committer on both commits: `Yousef Wadi <ywadi85@gmail.com>` —
  matches local `git config user.name`/`user.email` exactly.
- No AI attribution anywhere in either commit (`git show e094381 f4726db |
  grep -iE "claude|anthropic|co-authored|generated with|ai assistant"` —
  zero matches).
- Pathspec-scoped: `e094381` touches only `src/rx_platform/{include,src,
  tests}` (Layer 1's own files); `f4726db` touches only
  `samples/09_scene/{main.cpp,window_resize.h,tests/test_window_resize.cpp}`
  (Layer 2's own files) — zero cross-contamination between the two.
- Nothing pushed: `main...origin/main [ahead 4]` (the two fix commits plus
  the two unrelated, zero-overlap coordinator docs commits above them).
- The implementer-disclosed provenance concern (an overstepping sub-fork
  authored and committed both fixes directly) doesn't change this verdict:
  the code was reviewed on its own merits above, independent of who wrote
  it, per this review's own mandate.
- Confirmed cosmetic-only: `f4726db`'s subject line trails off ("...the
  device is lost right after the surface is") — the report already flags
  this; body text is complete and accurate; not worth a commit-message-only
  amend per this repo's own "new commit, not amend" convention.

## Not independently verifiable this round

- The "chained, not replaced" claim for non-BadWindow X11 errors was
  verified by code inspection only — this round did not construct a
  scenario that deliberately triggers a genuine, different X11 protocol
  error to confirm it still reaches the previous (fatal) handler at
  runtime. The code path is simple and directly inspectable
  (`window.cpp:71-75`), so this is a low-confidence-gap, not a
  contradicted claim.
- Windows-cross build (see Verdict 1) — report's claim taken as-is, not
  re-run.
- Wayland real-hardware behavior — both this review's trials and the
  report's own verification are X11/NVIDIA-only, consistent with this
  repo's established verification scope for X11-specific fixes.

## Restoration

All temporary reverts (`git checkout 54f92ed -- ...` for both layers) were
restored via `git checkout HEAD -- ...`; each restoration was confirmed
byte-identical via `git diff HEAD` showing no output before rebuilding.
Final `git status --short` shows only the pre-existing, untouched
`.superpowers/sdd/2026-08-11-phase4-scene-assets/progress.md` modification
that predates this review.
