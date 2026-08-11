# Task 6 review: Present-mode control (spec seed 1)

Commit reviewed: `85a70ed` (base `01a71e9`), worktree branch
`worktree-agent-a10267bc5dd531e03`. Attribution check: author/committer =
Yousef Wadi `<ywadi85@gmail.com>` on both fields; commit message has no
AI-attribution language. Clean.

## Spec compliance

| # | Requirement (brief / seed-1 / dispatch resolutions) | Verdict | Evidence |
|---|---|---|---|
| 1 | `PresentMode` enum (`VsyncOn`/`VsyncOff`) + `setPresentMode()` applied via the EXISTING recreate machinery — no second recreation flow | ✅ | `device.h`: `enum class PresentMode`; `setPresentMode()` only writes `desiredPresentMode_`. `recreateSwapchain()` is the sole call site of the new `selectPresentMode()` ladder for a live `Device`; `create()` inlines only the VsyncOn/FIFO half (no query needed). No second recreate path exists anywhere in the diff. |
| 2 | `presentMode()` returns the ACTUAL resolved mode, not the requested enum | ✅ | `actualPresentMode_` assigned from `vkbSwapchain.present_mode` (vk-bootstrap's own readback) in both `create()` and `recreateSwapchain()`, never derived from the `PresentMode` enum. Independently reproduced: under the 1.4.357 layer + lavapipe, requesting `VsyncOff` resolved to `MAILBOX`; under the default system layer it resolved to `IMMEDIATE` — two different concrete outcomes for the same request, confirming this is a real readback, not a hardcoded mapping. |
| 3 | `VsyncOff` ladder MAILBOX→IMMEDIATE→FIFO+warn, real surface queries | ✅ | `selectPresentMode()` calls `vkGetPhysicalDeviceSurfacePresentModesKHR` directly (two-call idiom, correct), checks `MAILBOX` then `IMMEDIATE`, single `RX_LOG_WARN` + `FIFO` only if neither is supported. Code-inspected line by line; no bug found. Not exercised as a forced branch in test/CI (disclosed), but functionally correct by inspection. |
| 4 | Explicit FIFO creation default replacing implicit vkb preference, documented as behavioral change | ✅ | Confirmed against vendored `vk-bootstrap` source in the worktree's own build tree (`_deps-src/vk-bootstrap/src/VkBootstrap.cpp:2257-2260`): the undisturbed default is literally `add_desired_present_modes` → `{MAILBOX, FIFO}`. `Device::create()` now calls `set_desired_present_mode(FIFO)` before `.build()`, which (per `SwapchainBuilder::set_desired_present_mode`'s `insert(begin(), …)` + the `if (desired_present_modes.empty())` guard around `add_desired_present_modes`) makes FIFO the *only* candidate handed to vk-bootstrap's own selection — not merely a preference nudge. Documented at the call site, in `device.h` comments, and in `samples/README.md` as an explicit "Behavioral change from earlier phases" callout. |
| 5 | `--vsync` in all six samples, parsed like `--validate`; mode logged at startup; headless unaffected | ✅ | All six `main.cpp` diffs are structurally identical: same `for`-loop `std::string_view` branch shape as `--validate`, default `on`, unrecognized value → `RX_LOG_ERROR` + default-to-on (reproduced live). Forwarded only into `runPresent()`; `runHeadless()` signature/behavior untouched — reproduced live (`--vsync off` with no `--present` still logs `triangle readback PASSED`, unchanged). Flag-order independence reproduced (`--vsync off --validate --present` behaves identically to `--present --validate --vsync off`). |
| 6 | GPU test using the corrected `srcStage=COLOR_ATTACHMENT_OUTPUT` acquire-chaining (release-weekend fix) | ✅ | New `device_test.cpp` test's barrier uses `vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, …)` — byte-for-byte the same stage pair as the pre-existing round-trip test directly above it (diffed the two `TEST_CASE`s against each other directly). |
| 7 | Zero validation errors, both layers | ✅ | Reproduced myself, not just re-read: (a) default system layer, `rx_rhi_vk_tests` binary directly — new test passes, `CHECK_FALSE(hasValidationErrors())` holds; (b) `VK_ICD_FILENAMES=.../lvp_icd.json VK_LAYER_PATH=/home/ywadi/sponza/vvl xvfb-run -a ctest --preset linux-native -R rx_rhi_vk` → **1/1 pass**, ladder resolved `VsyncOff` to `MAILBOX` under this combination (matches the report's own account); full `ctest` suite reproduced at **16/16**. |
| 8 | Samples' flag parity, headless ignore semantics, README rows | ✅ | All six samples carry the flag, the toggle-at-startup block, and the one-line summary log in identical positions; `samples/README.md` gets a full explanation under `01_triangle` plus accurate one-line cross-references from the other five, matching the established `--validate` convention. |

**Spec verdict: ✅ compliant.** Every dispatch-resolution item is implemented as described and independently reproduced against the actual worktree (not just re-read from the report).

## Deep-scrutiny findings

### 1. Recreation correctness on toggle — no widened exposure

The `--vsync off` toggle path (`setPresentMode()` + `recreateSwapchain(surface)`) runs in every sample **before `FrameSync::create()`** — i.e., before any image has ever been acquired from that `Device`. `recreateSwapchain()` itself is untouched by this task except for the present-mode query/apply inserted between "clear `swapchainImages_`" and "build the new swapchain" — the existing `vkDeviceWaitIdle()` → destroy-old → build-new sequence is unchanged. Net result: the toggle path is *strictly safer* than the resize path it reuses (zero frames in flight at toggle time, vs. resize which can happen mid-loop) — it does not widen the pre-existing destroy-then-build (no `oldSwapchain`) exposure, which remains exactly as disclosed and out of scope. No runtime vsync toggle exists mid-session in any sample (only a startup CLI flag), so the "toggling mid-frame" risk the brief asked about has no code path to actually occur in this diff.

### 2. GPU test's ladder-outcome assertion is weak (Medium)

`CHECK((actualAfterVsyncOff == MAILBOX || == IMMEDIATE || == FIFO))` is nearly vacuous as a regression check on the ladder's *preference order*: FIFO is a member of the accepted set and was already the pre-toggle value, so a hypothetical broken `selectPresentMode()` that ignored `desired` entirely and always returned FIFO would still pass this assertion in CI. The test never independently queries this surface's supported present modes (available to it via `device->physicalDevice()` + `fixture->surface`, both already exposed) to compute the *specific* expected outcome and assert equality — which would have closed the gap while still respecting the dispatch resolution's explicit instruction not to assert a hardcoded optional mode. This is disclosed by the implementer, and the dispatch resolution's literal wording ("asserting the LADDER's contract not specific optional modes") is satisfied — but the stronger, still-portable design was available and not taken.
Mitigating evidence (manual, not CI-captured): independently reproduced two *different* concrete outcomes for the same `VsyncOff` request across two driver/layer combinations (IMMEDIATE under the default layer, MAILBOX under 1.4.357+lavapipe), which does demonstrate the ladder discriminates by real availability rather than being a no-op — but that proof lives outside the automated suite.

### 3. Startup logging is 2-3 lines total, not literally one (Informational)

The sample's own contribution is one line (`--present: present mode in use: {}`), matching the resolution's intent, but `Device::create()` and (on `--vsync off`) `Device::recreateSwapchain()` each add their own present-mode log line underneath it — reproduced live, up to 3 present-mode-related lines per `--vsync off` run. Not a compliance gap (the "one line" language binds the sample's log statement, which is singular), and the extra Device-internal diagnostics are genuinely useful, not misleading. Noting only because the brief asked for explicit scrutiny of this point.

### No findings on

- **Behavioral-change diligence**: no Phase 3 sample, test, or doc claim was found anywhere in the repo that depended on the old implicit MAILBOX-preference (`grep`'d all tracked `.md` files); `MANUAL_VERIFICATION.md` was correctly left untouched — it tracks per-sample resize/close human checks, not CLI flags, and neither `--validate` nor `--vsync` has ever had a row there.
- **FIFO-fallback+warn branch**: code-inspected per the brief's instruction; correct (sequential `MAILBOX`→`IMMEDIATE` checks, single guarded `RX_LOG_WARN`, `FIFO` fallback).
- **Move semantics**: `desiredPresentMode_`/`actualPresentMode_` correctly added to `Device`'s move-assignment operator and reset on the moved-from instance.
- **Cross-platform build**: reran the windows-cross-zig target build in the worktree myself (`ninja: no work to do` — already current against this exact commit) and confirmed all six `.exe`s plus `rx_rhi_vk_tests.exe` exist and are freshly built.

## Verification performed independently (not just re-read from the report)

- `xvfb-run -a ctest --preset linux-native` in the worktree → **16/16 pass**.
- `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json VK_LAYER_PATH=/home/ywadi/sponza/vvl xvfb-run -a ctest --preset linux-native -R rx_rhi_vk --output-on-failure` → **1/1 pass**; verbose rerun of the binary directly under the same env confirms the new test's ladder resolved `VsyncOff` → `MAILBOX`, zero validation errors, 18/18 assertions in that test case.
- Confirmed vk-bootstrap's real default present-mode preference list directly in the vendored source (`_deps-src/vk-bootstrap/src/VkBootstrap.cpp`), validating the report's "behavioral change" claim against ground truth rather than trusting the prose.
- Live runs of `sample_01_triangle`: `--present --validate --vsync off` (FIFO→IMMEDIATE, correct log sequence, only the two pre-existing documented validation false positives), `--present --validate --vsync bogus` (correct error + default-to-on), `--validate --vsync off` headless (flag parsed, ignored, unchanged PASS), and flag-order independence (`--vsync off --validate --present` vs. the canonical order) — all matched expectations.
- `cmake --build build/windows-cross-zig --target rx_rhi_vk_tests sample_01_triangle sample_02_hotreload sample_03_bindless_mesh sample_04_streaming sample_05_multipass sample_06_materials` → no-op (already current), all seven binaries present.

## Quality verdict

**Approved with 1 Medium, 1 Informational.**

- Medium: GPU test's ladder-outcome assertion (`presentMode() ∈ {MAILBOX, IMMEDIATE, FIFO}`) can't distinguish a correctly-discriminating ladder from a broken one that always falls through to FIFO — recommend a follow-up that has the test independently query supported modes via `device->physicalDevice()` and assert against the specific computed expectation.
- Informational: startup present-mode logging is 2-3 lines across `Device`+sample, not literally one line — intentional and harmless, noted for completeness only.

Neither finding blocks merge; both are narrow, already substantially disclosed by the implementer, and do not indicate an actual functional defect in the shipped ladder/recreation logic, which was independently verified correct by direct code inspection and live reproduction.

## Fix round 1 re-review

Fix commit `b5b2042` (same worktree branch, base `85a70ed`). Scope check:
`git diff 85a70ed..b5b2042 --stat` touches exactly two files —
`src/rx_rhi_vk/tests/device_test.cpp` and this task's own report — no
scope creep. Attribution clean (author/committer = Yousef Wadi, no
AI-attribution language).

**Medium — closed, verified.** The test now independently calls
`vkGetPhysicalDeviceSurfacePresentModesKHR` on `device->physicalDevice()` +
`fixture->surface` itself, computes `expectedAfterVsyncOff` (MAILBOX, else
IMMEDIATE, else FIFO) from that real, freshly-queried list, and asserts
`device->presentMode() == expectedAfterVsyncOff` — replacing the old
membership check. This clears the discrimination bar stated in the
original finding: a selector hardwired to always return FIFO would now
fail on any surface reporting MAILBOX or IMMEDIATE available, since the
independently-computed expectation would be that mode while the hardwired
selector's actual output would stay FIFO. `VsyncOn` → exactly FIFO
(unconditional, no query, per spec guarantee) was already asserted and is
untouched by this fix.

Independently reproduced myself in the worktree, not just re-read from the
updated report:
- Default system validation layer, `--test-case="*present-mode*"` →
  **20/20 assertions pass** (was 18 pre-fix); log shows
  `Device::recreateSwapchain: present mode in use: IMMEDIATE
  (PresentMode::VsyncOff)` — the test's own independently-queried
  expectation matched, on a surface offering IMMEDIATE but not MAILBOX.
- Same layer, full suite: `./rx_rhi_vk_tests` → **35/35 test cases,
  807/807 assertions pass** (was 805).
- `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
  VK_LAYER_PATH=/home/ywadi/sponza/vvl` (the 1.4.357 layer) +
  `--test-case="*present-mode*"` → **20/20 pass**; log shows
  `Device::recreateSwapchain: present mode in use: MAILBOX
  (PresentMode::VsyncOff)` — a *different* concrete outcome, again matched
  by the test's own independent query, on a surface that this time offers
  MAILBOX. This is the actual discrimination proof: two different real
  availability lists, two different expected outcomes, both correctly
  matched by the strengthened assertion — not the same "any legal mode"
  pass twice.
- Same 1.4.357 layer via `ctest --preset linux-native -R rx_rhi_vk` →
  **1/1 pass**; full `ctest --preset linux-native` (default layer) →
  **16/16 pass**.

The implementer's claim of "verification against two different real
availability lists (IMMEDIATE and MAILBOX)" is confirmed accurate — I
reproduced both outcomes myself, on the same two layer/ICD combinations
named in the report.

**Informational — no action, disclosure accepted.** The report clarifies
"one-line warning" describes the single line of runtime log *output*
(`RX_LOG_WARN`'s one call), not source line count. Consistent with the
original finding's own framing (it was noted as non-blocking); no change
needed.

### Re-review verdict

**All findings addressed — Task 6 closed.**
