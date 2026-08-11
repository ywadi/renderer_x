# Task 6 report: Present-mode control (spec seed 1)

## Fix round 1 (post-review)

Review (`task-6-review.md`): spec ✅, 1 Medium to fix, 1 Informational
(disclosure only). This section documents the fix; the rest of this report
is the original round-1 account, left intact below for the record.

**Medium — GPU test assertion nearly vacuous.** The original
`presentMode() ∈ {MAILBOX, IMMEDIATE, FIFO}` check after the `VsyncOff`
toggle would pass even for a selector hardwired to always return `FIFO` —
it only proved the result was *some* legal mode, never that it was the
*right* one for this surface. **Fix:** the test now independently queries
`vkGetPhysicalDeviceSurfacePresentModesKHR` for the fixture's real surface
(the same call `device.cpp`'s `selectPresentMode()` makes internally, but
re-derived from scratch in the test rather than trusted from the code under
test), computes the expected ladder outcome itself (MAILBOX if that query
reports it available, else IMMEDIATE, else FIFO), and asserts
`device->presentMode() == expected` after `setPresentMode(VsyncOff)` +
`recreateSwapchain()`. This now fails on any surface reporting MAILBOX or
IMMEDIATE available if the selector doesn't actually pick it — a hardwired
selector no longer passes. Driver-independence is preserved: the test makes
no assumption about *which* optional modes exist, only that the selector's
choice matches the documented preference order applied to whatever the real
availability list turns out to be. `VsyncOn` → exactly `FIFO` was already
asserted (`CHECK(device->presentMode() == VK_PRESENT_MODE_FIFO_KHR)`,
unconditional, no query needed — the Vulkan spec guarantees it); left
unchanged. The fallback-to-FIFO branch (surface supports neither optional
mode) remains inspection-verified only, per the original disclosure below —
no available driver here still forces it, so this fix does not change that.

**Informational — logging line-count wording (no action, per review).**
"one-line warning" (device.h/device.cpp/README) describes the single line
of runtime *log output* `RX_LOG_WARN`'s one call produces, not the source
code's physical line count (the call itself spans two source lines, a
concatenated string literal). Disclosed here per the review; wording left
as-is since it accurately describes the actual log line emitted at runtime.

### Verification (fix round 1)

- `cmake --build build/linux-native --target rx_rhi_vk_tests` — rebuilds
  clean after the test change.
- `xvfb-run -a ./build/linux-native/src/rx_rhi_vk/rx_rhi_vk_tests
  --test-case="*present-mode*"` — passes, **20/20 assertions** (was 18;
  the 2 new ones are the independently-queried expected-mode check
  replacing the old membership check). Observed: `VsyncOff` resolved to
  `IMMEDIATE` under the default system validation layer (this surface has
  no MAILBOX here) — matches the independently-computed expectation.
- `xvfb-run -a ./build/linux-native/src/rx_rhi_vk/rx_rhi_vk_tests` (full
  suite) — **35/35 test cases, 807/807 assertions pass** (was 805; +2 from
  the strengthened test).
- `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
  VK_LAYER_PATH=/home/ywadi/sponza/vvl xvfb-run -a ctest --preset
  linux-native -R rx_rhi_vk` — **1/1 pass**. Under this ICD/layer
  combination `VsyncOff` resolved to `MAILBOX` (this lavapipe build does
  support it) — again matches the independently-computed expectation,
  confirming the strengthened assertion discriminates correctly across two
  different driver/layer combinations, not just one.
- `xvfb-run -a ctest --preset linux-native` — **16/16 tests pass**, full
  suite unaffected.
- `cmake --build build/windows-cross-zig --target rx_rhi_vk_tests` — builds
  and links clean (`.exe` present); only the pre-existing, unrelated
  `_WIN32_WINNT` macro-redefinition warning.

## Scope

`src/rx_rhi_vk/device.{h,cpp}`, all six samples' `main.cpp` flag parsing,
`samples/README.md`. No changes outside this scope (texture wiring and the
log-sink task are separate agents' worktrees).

## What changed

**`rx::rhi::PresentMode`** (`device.h`): `enum class PresentMode { VsyncOn,
VsyncOff };`, plus a free `presentModeName(VkPresentModeKHR)` helper for
logging/HUD use, shared by `Device`'s own internal logging and all six
samples' startup log line.

**`Device::setPresentMode(PresentMode)`**: records the desired mode only —
does not touch the live swapchain. **`Device::presentMode()`**: returns the
actual `VkPresentModeKHR` currently in use (read back from vk-bootstrap after
the ladder resolves it), for logging/HUD.

**Ladder** (`selectPresentMode()`, `device.cpp`, anonymous namespace):
`VsyncOn` always resolves to `FIFO` with no query (the one mode the Vulkan
spec guarantees every surface supports). `VsyncOff` queries
`vkGetPhysicalDeviceSurfacePresentModesKHR` directly and prefers `MAILBOX`,
then `IMMEDIATE`, falling back to `FIFO` with a one-line `RX_LOG_WARN` only
when the surface supports neither. The already-verified-available result is
handed straight to `vkb::SwapchainBuilder::set_desired_present_mode()`,
bypassing vk-bootstrap's own (different, undocumented-here) fallback.

**Single recreation path, no second flow invented**: `recreateSwapchain()`
is the sole place the ladder actually executes for a live `Device` — it
already runs on every `NeedsRecreate` (resize, out-of-date, suboptimal) and
now also resolves `desiredPresentMode_` against the ladder every time.
`setPresentMode()` itself does nothing to the swapchain; a caller that wants
the new mode applied immediately (every sample's `--vsync` handling) calls
`recreateSwapchain(surface)` right after `setPresentMode()`, reusing the
exact function resize already drives.

**Creation-time default, explicit — behavioral change**: `Device::create()`
now calls `swapchainBuilder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)`
unconditionally, replacing vk-bootstrap's own implicit
MAILBOX-if-available-else-FIFO preference. Before this task, a sample
running against a MAILBOX-capable surface got MAILBOX with no code anywhere
having chosen it; every `Device` now gets FIFO unless something explicitly
opts out via `setPresentMode(VsyncOff)` + `recreateSwapchain()`. Documented
at the creation site and on `PresentMode`/`setPresentMode()`'s own comments.

**All six samples** (`01_triangle` .. `06_materials`): `--vsync on|off`
(default `on`), parsed in the same `for`-loop/`std::string_view` style as
`--validate`, consuming one extra `argv` slot for the value; an unrecognized
value logs an error and defaults to `on`. Forwarded only into `runPresent()`
(new second parameter, `rx::rhi::PresentMode`) — `runHeadless()`'s signature
is untouched, since its swapchain is built once and never presented, so
there is nothing for a present-mode choice to affect there; the flag still
parses in headless mode, it just has no effect (documented at the flag-parse
site and in each file's own header comment). Inside `runPresent()`, right
after `Device::create()` succeeds and before any per-swapchain-image
resource (`FrameSync`, image views) is built: if `--vsync off`,
`setPresentMode()` + `recreateSwapchain(surface)` once; either way, one
`RX_LOG_INFO("--present: present mode in use: {}", ...)` line using the new
`presentModeName()` helper. The default (`on`) case skips the extra
recreate call entirely — `Device::create()`'s own explicit-FIFO default
already matches it, so no wasted double-swapchain-build at startup.

**`samples/README.md`**: full `--vsync` explanation (behavioral change,
ladder, headless no-op) written once under `01_triangle`, mirroring how
`--validate` is documented; the other five samples get a one-line pointer
back to it, matching the existing `--validate` cross-reference convention.
Each sample's "Expected output" section now notes the startup present-mode
log line. One CLI example (`--present --vsync off`) added to the "Building
and running" section.

## Tests

**GPU test** (`device_test.cpp`, new `TEST_CASE`): same fixture and the same
acquire/record/submit/present shape as the existing round-trip test directly
above it, including its fixed `COLOR_ATTACHMENT_OUTPUT` barrier `srcStage`
(read before writing this test, per the brief) — copied, not
reinvented, since the brief required following that exact acquire-semaphore
chaining. Adds: `Device::create()` → `CHECK(presentMode() ==
VK_PRESENT_MODE_FIFO_KHR)`; `setPresentMode(VsyncOff)` +
`REQUIRE(recreateSwapchain(surface))`; `CHECK` the result is one of
`{MAILBOX, IMMEDIATE, FIFO}` (the ladder's *contract*, not a specific
optional mode — driver-dependent); then one full acquire/submit/present
cycle against the recreated swapchain; `CHECK_FALSE(hasValidationErrors())`.

**Sample flag parsing**: no existing "device-free arg-parse test" harness
exists anywhere under `samples/` (checked — every sample's only registered
`ctest` case runs the real binary headless with `--validate`; there is no
separate unit-test binary for CLI parsing to extend). Per the task's own
conditional wording ("where the samples have parse tests"), none were added;
covered instead by direct manual runs of the built binaries (below) plus the
existing `ctest` headless registrations, which already exercise `main()`'s
parsing loop end to end and now implicitly confirm `--vsync` is a no-op
there (unchanged headless behavior/output).

**Manual verification** (`xvfb-run`, hidden window):
- `sample_01_triangle --present --validate` (no `--vsync`): logs
  `present mode in use: FIFO` once from `Device::create()`, no
  `recreateSwapchain` log line (confirms no redundant recreate on the
  default path), sample's own line echoes `FIFO`.
- `sample_01_triangle --present --validate --vsync off`: `Device::create()`
  logs `FIFO`, then `Device::recreateSwapchain` logs `IMMEDIATE
  (PresentMode::VsyncOff)` (this dev machine's Mesa/lavapipe surface has no
  MAILBOX here), sample's own line echoes `IMMEDIATE`; ran 5s under Xvfb, no
  crash.
- `sample_06_materials --present --validate --vsync off`: same pattern,
  confirmed on a more complex sample (allocator/uploader/material-system
  wiring all present) — no crash, no validation errors, present-mode log
  line correct.
- `sample_01_triangle --validate --vsync off` (headless, no `--present`):
  flag parses, ignored, unchanged `triangle readback PASSED` output.
- `sample_01_triangle --vsync bogus`: logs
  `--vsync expects 'on' or 'off', got 'bogus' -- defaulting to on`,
  continues normally.

## Verification evidence

- `cmake --preset linux-native && cmake --build build/linux-native` — full
  project, zero errors, zero new warnings.
- `cmake --preset windows-cross-zig && cmake --build build/windows-cross-zig
  --target rx_rhi_vk_tests sample_01_triangle sample_02_hotreload
  sample_03_bindless_mesh sample_04_streaming sample_05_multipass
  sample_06_materials` — all link successfully (`.exe` outputs present);
  only pre-existing, unrelated `_WIN32_WINNT` macro-redefinition warnings.
- `xvfb-run -a ctest --preset linux-native` — **16/16 tests pass** (full
  suite: `rx_rhi_vk_tests`, all six `sample_NN_*_headless` gates, everything
  else untouched by this task).
- `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
  VK_LAYER_PATH=/home/ywadi/sponza/vvl xvfb-run -a ctest --preset
  linux-native -R rx_rhi_vk` — **1/1 pass** under the newer validation layer
  + lavapipe ICD. Under this combination the ladder resolved `VsyncOff` to
  `MAILBOX` (this lavapipe build does support it, contrary to the brief's
  own hedge that only FIFO might be available under Xvfb/lavapipe) — the
  test asserts the ladder's contract (`{MAILBOX, IMMEDIATE, FIFO}`), not a
  specific mode, so this is a pass either way.
- `xvfb-run -a ./build/linux-native/src/rx_rhi_vk/rx_rhi_vk_tests` (default
  system layer) — **35/35 test cases, 805/805 assertions pass**, zero
  validation errors (the two pre-existing, independently-documented
  `VK_KHR_portability_enumeration` false positives are unaffected by this
  task and still filtered by `Context::hasValidationErrors()`).

## Notes / concerns

- `Device::recreateSwapchain()` still rebuilds the swapchain from scratch
  (destroy-then-build, no `oldSwapchain` hand-off) — pre-existing behavior,
  unchanged; out of this task's scope (present-mode selection only).
- The ladder's `VsyncOff` fallback-to-FIFO branch (surface supports neither
  MAILBOX nor IMMEDIATE) is exercised only by the warning-log code path in
  this task's testing — no available driver/surface combination on this
  dev machine actually lacks both modes, so that exact branch's *log
  output* was verified by inspection/reasoning (guarded, one-line,
  `RX_LOG_WARN`) rather than by an environment that forces it. The ladder
  logic itself (three sequential `if`s, no other exit) is simple enough
  that this is a reasonable risk to accept, but it is disclosed rather than
  silently assumed correct.
