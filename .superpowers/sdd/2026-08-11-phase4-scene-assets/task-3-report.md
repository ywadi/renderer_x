# Task 3 report: Tracy profiler client integration

## Hotfix (defect found post-closure by Task 4's implementer)

**Defect:** `src/rx_rhi_vk/src/tracy_gpu.cpp`'s `createGpuProfileContext()`
created its short-lived setup `VkCommandPool` with only
`VK_COMMAND_POOL_CREATE_TRANSIENT_BIT` — missing
`VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`. Task 4's implementer hit
this building a real `rx::graph::Executor` under `RX_TRACY=ON` with
validation (full diagnosis: `task-4-report.md`, "Concern" section).

**Root cause, traced directly against the vendored v0.14.0
`TracyVulkan.hpp`:** `VkCtx`'s constructor (both the plain and calibrated
overload) always does ONE begin/end/submit/waitIdle cycle on the setup
`cmd` first (`CreateQueryPool()`'s own reset). Then, **only when
`m_timeDomain` is still `VK_TIME_DOMAIN_DEVICE_EXT`** — i.e.
`FindAvailableTimeDomains()` did not find a host-comparable calibration
domain (`VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT` on Linux) among whatever
`vkGetPhysicalDeviceCalibrateableTimeDomainsEXT` reports for the selected
physical device — it re-begins the SAME `cmd` **two more times** for the
timestamp-write and re-reset steps. Re-beginning a command buffer that has
already reached the executable state, without an explicit
`vkResetCommandBuffer`/`vkResetCommandPool` in between, requires the
owning pool to carry `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`
(Vulkan spec, `VUID-vkBeginCommandBuffer-commandBuffer-00050`). Our pool
didn't have it.

**Fix, and why this flag and not "reset the pool per collect cycle":** the
setup pool in question is created, used, and destroyed entirely within one
`createGpuProfileContext()` call — it never touches the per-frame
`RX_GPU_COLLECT` path (that runs on a completely different, caller-owned
command buffer elsewhere). There is no "collect cycle" of this project's
own to insert a reset into: every extra `vkBeginCommandBuffer` happens
*inside* Tracy's own constructor, synchronously, with no seam this
project's code could hook a reset into even if desired. The pool's own
creation flag is the only lever available, and it is also the textbook-
correct one — it is exactly the contract Tracy's own manual states for
this buffer ("must be in the initial state and be able to be reset...
will rerecord and submit it to the queue multiple times"). Added
`VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` to the pool's `flags`
(OR'd with the existing `TRANSIENT_BIT`), with the reasoning above recorded
as a comment at the call site.

### Verification

**Reproduced first, then fixed, then reproduced-fixed** (not fix-then-hope):

1. Confirmed the failure is real and driver-dependent, not universal:
   forcing lavapipe alone (`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`)
   and running `rx_graph_gpu_tests --validate` against the **pre-fix**
   binary reproduced the exact VUID Task 4 reported
   (`VUID-vkBeginCommandBuffer-commandBuffer-00050`), 5/5 test cases
   failing. Running the identical binary with the DEFAULT device
   selection (NVIDIA discrete GPU visible and preferred by vk-bootstrap)
   showed 0 failures — confirming the bug is real but conditionally
   exercised, not something the pre-fix binary always trips.
2. Applied the fix, rebuilt.
3. **`rx_graph_gpu_tests` and `rx_material_gpu_tests`, lavapipe forced,
   default (system) validation layer:** 5/5 and 25/25 test cases pass
   (84/84 and 344/344 assertions), zero `VUID-vkBeginCommandBuffer`
   errors — only this codebase's own pre-existing, already-documented
   `VK_KHR_portability_enumeration`-layer-version false positives.
4. **Same two binaries, lavapipe forced, `VK_LAYER_PATH=/home/ywadi/sponza/vvl`
   (the "newer" layer build):** 5/5 and 25/25 pass, **zero** validation
   lines of any kind (that layer build doesn't even emit the
   portability-enumeration false positives round 1's report already
   catalogued) — clean under both validation layers, as required.
5. **Full suite, both dev presets, default device selection (standard
   `ctest`):** `linux-native` 16/16 (28.7s), `windows-cross-zig` 16/16
   (51.4s, Wine-executed) — no regression from the fix.

### Why did the original Task 3 verification (16/16 green) not catch this?

**Answer:** vk-bootstrap's `PhysicalDeviceSelector` prefers the discrete
GPU when more than one device is visible, and this dev machine has both
an NVIDIA proprietary driver and Mesa llvmpipe/lavapipe installed — every
`ctest` run in round 1 and fix round 1 used the *default* device
selection, which picked NVIDIA, never lavapipe. Traced directly against
`TracyVulkan.hpp`'s `FindAvailableTimeDomains()`: NVIDIA's driver reports
`VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT` among its calibrateable time
domains, so Tracy's `VkCtx` constructor takes the **single-begin**
branch (no re-record at all) — the bug is structurally unreachable on
that path. Lavapipe reports only `VK_TIME_DOMAIN_DEVICE_EXT` (confirmed
by forcing it and observing the VUID above), forcing the **multi-begin**
branch that the missing reset flag actually breaks. Round 1's own report
DID check lavapipe — but only for `VK_EXT_calibrated_timestamps`
*extension presence* (`vulkaninfo`'s extension list, confirmed present on
both drivers) — never for which *time domains* the extension reports once
enabled, which is the finer-grained, driver-specific fact that actually
decides which of Tracy's two constructor branches runs. Extension
presence and "which calibration domains it offers" are different facts;
checking only the former missed the latter.

**Verification-matrix gap, stated plainly:** a local `ctest` run on any
developer machine with a discrete GPU installed silently exercises only
that GPU's device-selection path — never lavapipe — for every test that
constructs a real `Device`/`Executor`, even though this project's own CI
target and documented reference driver *is* lavapipe. "Green suite
locally" and "green suite on the driver CI actually runs on" are not the
same claim unless a run explicitly forces the CI driver via
`VK_ICD_FILENAMES`. Task 3's own verification checklist never did this for
the GPU-context-creation path specifically (device/Executor-constructing
GPU tests) — it should have, every time a task touches Vulkan device or
context creation, not only when a task's own brief happens to mention
lavapipe. Recording this here for the Stage 0 audit: **any task whose
diff can affect physical-device selection, device/Executor construction,
or per-device Vulkan object creation should include an explicit
lavapipe-forced GPU-test run in its own verification, not just a plain
`ctest` pass**, exactly as this hotfix now does and as Task 4's own
verification already did unprompted.

---

## Fix round 1 (post-review)

Review (`task-3-review.md`, commit `460d7b6`): **Approved with findings** —
spec ✅, 1 Medium + 1 Low sharing one root cause, 1 Informational (unchanged,
still accepted as originally disclosed). This section documents the fix;
the rest of this report is the original round-1 account, left intact below
for the record — where the two disagree, this section is authoritative.

**Root cause (Medium + Low):** this project's vendored Tracy was built with
`TRACY_ON_DEMAND` left at Tracy's own default, OFF. Traced directly against
the vendored v0.14.0 source (not assumed):

- `ScopedZone::Name()` (`TracyScoped.hpp`, backing `RX_ZONE_DYNAMIC_NAME`)
  and the dynamic-name `VkCtxScope` constructor (`TracyVulkan.hpp`, backing
  `RX_GPU_ZONE_DYNAMIC`) both do a real `tracy_malloc()`/`AllocSourceLocation()`
  + `memcpy()` on every call, gated only by `if (!m_active) return;` — and
  with `TRACY_ON_DEMAND` off, `m_active` is unconditionally `is_active`,
  never checked against `GetProfiler().IsConnected()`.
- `VkCtx::Collect()` (`TracyVulkan.hpp`, backing `RX_GPU_COLLECT`) always
  runs its real `vkGetQueryPoolResults` readback path; its own
  `#ifdef TRACY_ON_DEMAND if (!GetProfiler().IsConnected()) { <cheap
  reset only>; return; }` early-out branch was compiled out entirely.

Both contradict D3's "passive (~no cost) until a profiler connects" and the
brief's "no allocations" cheap-macro constraint — for these two macros and
this one collect call specifically, not the plain `RX_ZONE`/`RX_ZONE_NAMED`/
`RX_FRAME_MARK`/`RX_PLOT` macros, which the review independently confirmed
were already lock-free-ring-buffer-only regardless of this flag.

**Fix:** `third_party/CMakeLists.txt`'s Tracy `CMAKE_ARGS` now passes
`-DTRACY_ON_DEMAND=ON` (alongside the existing `-DTRACY_ENABLE=ON
-DTRACY_STATIC=ON`) — a dependency-build-time flag, so it required a fresh
Tracy library build under a new dep-cache key (automatic; the dep-cache key
is a hash of `CMAKE_ARGS`, so this is not a hand-invalidation). Tracy's own
`set_option` macro applies `TRACY_ON_DEMAND` as a `PUBLIC` compile
definition on `Tracy::TracyClient`, so it propagates to this project's own
code the identical way `TRACY_ENABLE` already does — zero additional CMake
wiring on this project's side. Documented at both dynamic-name macro
declarations (`rx_core/profile.h`'s `RX_ZONE_DYNAMIC_NAME`, `rx_rhi_vk/tracy_gpu.h`'s
`RX_GPU_ZONE_DYNAMIC`/`RX_GPU_COLLECT`) and in `third_party/CMakeLists.txt`'s
own Tracy-vendoring comment, including the disclosed tradeoff: on-demand
means the client buffers nothing before a profiler connects, so **capture
history starts at connection time** — attaching mid-run shows zones from
that point forward only, never retroactively.

### Verification (fix round 1)

**1. Disconnected passivity — empirical, both by source citation and by
direct timing measurement.** Cited the exact early-out branches above (not
re-asserted from the macro name). For the empirical half, built two tiny
standalone probes in the scratchpad, linked against the project's own two
already-built Tracy variants (`.deps-cache/tracy-e3a816522c42b4b5` =
`TRACY_ENABLE` only, the exact pre-fix build; `.deps-cache/tracy-7fe6be05d35747f5`
= `TRACY_ENABLE`+`TRACY_ON_DEMAND`, the fix), using the project's own
zig-cxx-linux toolchain wrapper for ABI consistency, each running 2,000,000
iterations of `ZoneScopedN("probe_zone"); ZoneName(dynamicName.data(), dynamicName.size());`
with **no profiler ever connecting**:

```
without TRACY_ON_DEMAND (pre-fix):  ~49.0–50.1 ns/iteration  (3 runs, <2% variance)
with    TRACY_ON_DEMAND (post-fix): ~1.93 ns/iteration       (3 runs, <0.1% variance)
```

A ~25× reduction. A malloc-call-counting `LD_PRELOAD` interposer was also
tried first but turned out not to observe Tracy's real cost at all —
`tracy_malloc` (`TracyAlloc.hpp`) resolves to `rpmalloc()`, Tracy's own
bundled pool allocator, not libc `malloc()` (confirmed by reading the
header) — so it is reported for completeness (flat ~0 calls in both builds,
confirming rpmalloc's own page-growth isn't what scales with the loop) but
the timing numbers above are the decisive evidence.

Repeated the identical experiment through this project's **own**
`rx_core/profile.h` macros (`RX_ZONE_NAMED`/`RX_ZONE_DYNAMIC_NAME`) with a
third configuration — `TRACY_ENABLE` undefined entirely, the real
`RX_TRACY=OFF` behavior — as the absolute zero-cost baseline:

```
RX_TRACY=OFF (TRACY_ENABLE undefined):              ~0.00 ns/iteration (loop fully optimized away, 3 runs)
RX_TRACY=ON + TRACY_ON_DEMAND=ON, disconnected:      ~1.3–1.4 ns/iteration (3 runs)
```

~1.3 ns is a handful of CPU cycles (one atomic connection-id load plus a
branch) — negligible in absolute terms and against the true no-Tracy
baseline, and a ~36× reduction from the pre-fix raw-Tracy-macro measurement
above. This satisfies the "cite the exact branch AND show a timing delta
... being negligible" verification option in full, with the branch citation
and the timing delta pointing at the identical mechanism.

**2. Live capture re-done, connecting mid-run.** Ran
`sample_05_multipass --present` under `xvfb-run` with the on-demand-fixed
binaries, let it run **5 seconds with no profiler connected** (the passive
phase this fix restores), then connected `tracy-capture` mid-run
(8s capture window): 761 frames, 6,064 zones,
(`/tmp/.../scratchpad/sample05_capture_ondemand.tracy`). `tracy-csvexport`
confirms every zone this task placed still captures correctly
post-connection, structurally identical to the round-1 capture:

```
graph_pass         executor.cpp:826   2274 counts (~3 passes x 758 frames)
execute            executor.cpp:762    758 counts (once per frame)
Collect            TracyVulkan.hpp:256 758 counts (RX_GPU_COLLECT firing once/frame post-connection)
present            device.cpp:346      757 counts
acquireNextImage   device.cpp:325      758 counts
```

GPU zones (`-g -u`): `forward`/`shadow`/`tonemap` (the real, dynamic
per-pass names) — 757 each, with real captured GPU execution-time values —
confirming per-pass dynamic naming, CPU and GPU, still works correctly with
on-demand connection. Bonus confirmation of the disclosed tradeoff: the
first captured GPU zone's own "time from start of program" timestamp is
~14.2s into the process's life (it had been running ~5s passively plus
connection/setup time before capture began) — capture genuinely starts at
connection time, not process start, exactly as documented.

**3. Full suite, both presets, both configurations — all green.** Fresh
Tracy builds (new dep-cache keys, since `CMAKE_ARGS` changed) for both
`RX_TRACY=ON` presets:

```
linux-native      (RX_TRACY=ON, on-demand):  16/16 passed (26.7s)
windows-cross-zig (RX_TRACY=ON, on-demand):  16/16 passed (47.2s, Wine-executed)
```

`TracyTargets.cmake` for both fresh installs confirms
`INTERFACE_COMPILE_DEFINITIONS "TRACY_ENABLE;TRACY_ON_DEMAND"`; both
projects' own `compile_commands.json` show `TRACY_ON_DEMAND` on all 69
Tracy-linked compile commands.

Re-ran the `RX_TRACY=OFF` configuration fresh (new build tree): zero
`[dep-cache]` Tracy activity, 16/16 tests passed (25.0s), zero `tracy::`/
`___tracy_` symbols via `nm -C` across every binary/library, zero
`TRACY_ENABLE`/`TRACY_ON_DEMAND` in `compile_commands.json`, no
`TracyClient*` object anywhere in the build tree — identical clean result
to round 1, confirmed again after the fix.

**Zero validation errors, re-confirmed with the on-demand build:** re-ran
`sample_05_multipass --present --validate` under sync validation with the
new binaries — 19,900 `[vulkan validation]`-tagged lines, **all 19,900**
carrying this codebase's own pre-existing `"(known false positive: ...)"`
markers (the same three already-documented categories from round 1); zero
unrecognized validation lines; zero non-validation `[error]` lines. Same
clean result as round 1, unaffected by the on-demand change (expected — the
fix touches only Tracy's own client-side behavior, not any Vulkan call this
project makes).

**4. Cost disclosure, honest before/after (this is the part round 1's
report omitted and the review caught):**

| | Before (round 1, as shipped) | After (this fix) |
|---|---|---|
| `RX_ZONE`/`RX_ZONE_NAMED`/`RX_FRAME_MARK`/`RX_PLOT`, disconnected | ~free (lock-free ring-buffer write only) | unchanged — still ~free |
| `RX_ZONE_DYNAMIC_NAME`/`RX_GPU_ZONE_DYNAMIC`, disconnected | **real `malloc`+`memcpy` every call, unconditionally** (~49 ns/call measured) | **early-out before allocating** (~1.3–1.9 ns/call measured, ~25–36× reduction) |
| `RX_GPU_COLLECT`, disconnected | **real `vkGetQueryPoolResults` readback every frame, unconditionally** | **early-out to a cheap query-pool reset only** |
| Capture semantics | N/A (always "recording", nothing to distinguish) | **tradeoff, disclosed:** no pre-connection buffering — a profiler attaching mid-run sees zones from that moment forward only, never retroactively |

Round 1's report claimed D3's "passive (~no cost) until a profiler
connects" was satisfied without disclosing that two of the macros this same
task added were the exception. That gap is closed: the exception no longer
exists (both now early-out identically to the always-cheap macros when
disconnected), and the one real tradeoff the fix itself introduces
(connection-time capture start) is now stated explicitly rather than left
implicit.

---

**Scope note (coordinator mid-task correction):** `src/rx_task` is **excluded**
from this task entirely — the coordinator flagged it as concurrently owned by
a Task 2 fix round while this task was in flight. No file under `src/rx_task`
was read or edited. `rx::task::Scheduler::parallelFor`'s zone+plot placement,
named in the original dispatch, is **dropped** from this task's scope; it is
not implemented here and remains open for whichever task next touches
`rx_task`.

## 1. Vendoring

Tracy client, pinned **v0.14.0** (verified current stable tag via
`git ls-remote --tags` at vendoring time — matches
`[R:threading]`'s "≥ v0.11" citation and its "`VK_EXT_calibrated_timestamps`
added in v0.11.0" fact). **BSD-3** license (Tracy's own `LICENSE` file).
Client-only: exactly one compiled translation unit
(`public/TracyClient.cpp`) plus client-side headers. The profiler UI/server
(`profiler/`, `server/`, `capture/`, `python/`) is never built by this
project, never `add_subdirectory`'d, never linked.

Routed through the dep-cache (`rx_add_cached_dependency`, `third_party/CMakeLists.txt`)
like spdlog/Vulkan-Headers/SDL3/vk-bootstrap/enkiTS — **not** hand-vendored as
loose source like volk/VMA/stb: Tracy's own `CMakeLists.txt` already builds a
real `TracyClient` target and installs a clean CMake package
(`install(EXPORT TracyConfig ...)` → `TracyConfig.cmake`, exporting
`Tracy::TracyClient`) — verified directly by reading it before writing the
vendoring block. `CMAKE_ARGS -DTRACY_ENABLE=ON -DTRACY_STATIC=ON` — the
`TRACY_ENABLE=ON` is deliberate and load-bearing: `TracyClient.cpp` is itself
guarded by `#ifdef TRACY_ENABLE` top-to-bottom, so building the dependency
without it would produce a near-empty stub library that this project's own
`TRACY_ENABLE`-visible code (via rx_core's public compile definition) would
fail to link against.

Entirely guarded behind the new top-level `RX_TRACY` option (declared in
root `CMakeLists.txt`, before `third_party` is added): when OFF, the whole
Tracy `rx_add_cached_dependency` block never executes — Tracy is never
cloned, configured, or compiled, and `Tracy::TracyClient` never exists as a
target. This is a structural guarantee, not a convention.

## 2. `rx_core/profile.h`

CPU-zone macros, all no-op when `TRACY_ENABLE` is undefined (which itself is
only ever defined when `RX_TRACY` is ON, via `rx_core`'s own `CMakeLists.txt`,
`PUBLIC`):

- `RX_ZONE` — object-like, statement-style (`RX_ZONE;`), mirrors Tracy's own
  `ZoneScoped`. Function-name zone.
- `RX_ZONE_NAMED(nameLiteral)` — `ZoneScopedN`.
- `RX_ZONE_DYNAMIC_NAME(text, size)` — **added beyond the four macros named
  in the dispatch** (`RX_ZONE`/`RX_ZONE_NAMED`/`RX_FRAME_MARK`/`RX_PLOT`).
  Necessary because dispatch item 4 explicitly requires per-pass zones keyed
  by a render-graph pass's runtime `std::string` name, which `ZoneScopedN`
  cannot take directly (its `name` must have static storage duration).
  Wraps Tracy's own documented idiom: manual `tracy.tex`, "If you want to
  set zone name on a per-call basis, you may do so using the
  `ZoneName(text, size)` macro" — verified directly against the vendored
  v0.14.0 manual before writing this. Called with an `RX_ZONE_NAMED`
  already active in the same scope.
- `RX_FRAME_MARK` — `FrameMark`, statement-style.
- `RX_PLOT(nameLiteral, value)` — `TracyPlot`.

`#else` branch defines every macro as a bare no-op, matching Tracy's own
`#ifndef TRACY_ENABLE` branch bit-for-bit, without depending on any Tracy
header being reachable (it is not, when `RX_TRACY` is OFF — Tracy was never
fetched). No other rx_* public header includes a Tracy CPU header anywhere
in this codebase (verified by grep before finishing).

**TDD test:** `src/rx_core/tests/profile_test.cpp` — one TU using all five
macros unconditionally (no `#ifdef` of its own), added to `rx_core_tests`.
Compiled and run green in **both** verified configurations (RX_TRACY=ON and
RX_TRACY=OFF — see §6).

## 3. CMake wiring

- `RX_TRACY` option, root `CMakeLists.txt`, **default ON**.
- `CMakePresets.json`: `RX_TRACY: "ON"` explicit in both `linux-native` and
  `windows-cross-zig` cacheVariables.
- `src/rx_core/CMakeLists.txt`: `if(RX_TRACY)` → `target_compile_definitions(rx_core PUBLIC TRACY_ENABLE)`
  + `target_link_libraries(rx_core PUBLIC Tracy::TracyClient)`. Both PUBLIC
  because rx_core is a transitive dependency of every other library/sample —
  this is the single wiring point that makes the whole project agree on
  whether Tracy is compiled in.
- Trivially OFF-able: `-DRX_TRACY=OFF` (verified with a fresh, separate
  configure — §6).

## 4. Zone placements

| Site | File | Macro(s) |
|---|---|---|
| FrameSync acquire | `rx_rhi_vk/src/device.cpp`, `Device::acquireNextImage` | `RX_ZONE` |
| FrameSync present | `rx_rhi_vk/src/device.cpp`, `Device::present` | `RX_ZONE` |
| Executor::execute (whole) | `rx_graph/executor.cpp` | `RX_ZONE` |
| Executor per-pass (CPU) | `rx_graph/executor.cpp`, inside the pass loop | `RX_ZONE_NAMED("graph_pass")` + `RX_ZONE_DYNAMIC_NAME(pass.name().data(), pass.name().size())` |
| Executor per-pass (GPU) | `rx_graph/executor.cpp`, same loop | `RX_GPU_ZONE_DYNAMIC` (transient GPU zone, dynamic pass name) |
| MaterialSystem::loadMaterial | `rx_material/material_system.cpp` | `RX_ZONE` |
| MaterialSystem::getPipeline | `rx_material/material_system.cpp` | `RX_ZONE` |
| MaterialSystem::reloadChanged | `rx_material/material_system.cpp` | `RX_ZONE` |
| Uploader::uploadToBuffer | `rx_rhi_vk/src/upload.cpp` | `RX_ZONE` |
| Uploader::uploadToImage | `rx_rhi_vk/src/upload.cpp` | `RX_ZONE` |
| DescriptorArena::beginFrame | `rx_rhi_vk/src/descriptor_arena.cpp` | `RX_ZONE` |
| ParamArena::beginFrame | `rx_material/instance.cpp` | `RX_ZONE` |
| ~~rx_task Scheduler::parallelFor~~ | — | **excluded, see scope note above** |

**Note on "FrameSync acquire/present":** `rx::rhi::FrameSync` itself declares
no `acquire`/`present` method — it owns only sync primitives and command
buffers. The real acquire/present calls every sample's frame loop drives
FrameSync's fences/semaphores around are `Device::acquireNextImage`/
`Device::present` (`rx_rhi_vk/device.cpp`), which is where the zones landed.

**RX_FRAME_MARK, all six samples, both modes** (`samples/*/main.cpp`):
present-mode path is identical across all six (right after
`device->present(...)`'s status handling, before `frameSync->advanceFrame()`).
Headless-mode path differs per sample's own structure (some render exactly
one frame via a single `runOnce()`, others loop):

- `01_triangle`: one `RX_FRAME_MARK` after its single headless `runOnce()`.
- `02_hotreload`: inside the shared `renderAndReadback` lambda's per-`frame`
  loop (each iteration re-clears/redraws the same attachment).
- `03_bindless_mesh`: one `RX_FRAME_MARK` after its single headless render
  `runOnce()` (the readback-only `runOnce()` that follows is not a frame).
- `04_streaming`: inside its real `rx::rhi::FrameSync`-driven headless loop
  (the one sample whose headless mode genuinely cycles frames-in-flight).
- `05_multipass`, `06_materials`: inside the `for (frame...) { cmdCtx->runOnce(...) }`
  headless loop, each iteration being its own `Executor::execute()` call.

## 5. GPU context (`rx_rhi_vk/tracy_gpu.{h,cpp}`)

New, second Tracy-header seam (mirrors `profile.h`'s rule, scoped to
`<tracy/TracyVulkan.hpp>`): `GpuProfileContext` (= `TracyVkCtx` when
`TRACY_ENABLE`, else `void*`), plus three self-null-safe macros:
`RX_GPU_ZONE_DYNAMIC`, `RX_GPU_COLLECT`, `RX_GPU_CONTEXT_DESTROY`.

**Real bug caught and fixed during implementation:** Tracy's `VkCtxScope`
constructor (backing `TracyVkZoneTransient`) dereferences `ctx` whenever its
`active` parameter is true, with no null-guard of its own (verified directly
against the vendored v0.14.0 source). A naive `active=true` would
null-pointer-dereference-crash the instant `createGpuProfileContext()`
returns null. Fixed by computing `active` as `(ctx) != nullptr` inside the
macro itself, and by wrapping `RX_GPU_COLLECT`/`RX_GPU_CONTEXT_DESTROY` in
their own internal null checks — so every call site in `executor.cpp` is
safe unconditionally, with no caller-side null check or `#ifdef` needed.

**Calibrated vs plain, wired through vk-bootstrap:** `Device::create()`
(`device.cpp`) now calls `physResult.value().enable_extension_if_present(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)`
— vk-bootstrap's own "enable if present, else no-op" call — and exposes the
result via a new `Device::calibratedTimestampsEnabled()` accessor.
`Device` has zero knowledge of Tracy; `createGpuProfileContext()`
(`tracy_gpu.cpp`) is the sole consumer, choosing `TracyVkContextCalibrated`
or plain `TracyVkContext` based on that one accessor.

**Lavapipe empirically checked (dispatch item 5's explicit ask):** verified
directly via `vulkaninfo` on this dev machine, which exposes both an NVIDIA
RTX 2080 (proprietary driver) and Mesa's llvmpipe/lavapipe as Vulkan
devices — **both report `VK_EXT_calibrated_timestamps`** (`VK_EXT_calibrated_timestamps: extension revision 2` for each). `[R:threading]`'s "unverified"
flag on lavapipe support is resolved: **present**. One caveat, disclosed
honestly: because every Vulkan implementation available in this environment
supports the extension, the plain (uncalibrated) `TracyVkContext` fallback
branch in `tracy_gpu.cpp` compiles and follows the identical documented Tracy
idiom, but was not driver-exercised live in this task — there is no
available driver here that lacks the extension to force that branch.

**`TracyVkCollect` hook:** at the very end of `Executor::execute()`, after
`finalBarriers()` (so `cmd` is guaranteed outside a render-pass instance,
Tracy's documented requirement) — chosen over reaching into `FrameSync`
itself, which has (and should keep) zero knowledge of Tracy or of any
Executor's GPU context.

**`TracyVkZone` around per-pass recording:** implemented as
`RX_GPU_ZONE_DYNAMIC` (Tracy's *transient* GPU zone,
`TracyVkZoneTransient`), not plain `TracyVkZone`, for the identical reason
the CPU-side per-pass zone needed `RX_ZONE_DYNAMIC_NAME`: a pass's name is a
runtime `std::string`, and `TracyVkZone`'s own `name` parameter must be a
compile-time literal. Cited: Tracy manual, "Transient GPU zones" — "available
in OpenGL, Vulkan, Direct3D 11/12 and WebGPU macros."

## 6. Verification

### Both dev presets, RX_TRACY=ON (default)

```
linux-native:      16/16 tests passed  (26.6s)
windows-cross-zig: 16/16 tests passed  (47.4s, Wine-executed)
```

Configure logs show Tracy actually fetched/built with `TRACY_ENABLE: ON`
(cmake/options.cmake's own status lines) on both toolchains; windows-cross-zig
linked Tracy's `ws2_32`/`dbghelp`/`secur32` MinGW-family libs automatically
(Tracy's own CMakeLists handles this — no project-side code needed), with
only a benign duplicate `_WIN32_WINNT` macro-redefinition warning
(same value, harmless).

### RX_TRACY=OFF (extra required verification run)

Separate, fresh configure (`-DRX_TRACY=OFF`) at `build/linux-native-notracy`:

- Configure log: **zero** dep-cache activity for Tracy (the `if(RX_TRACY)`
  block never ran at all — no clone, no compile).
- Build: 105/105 targets built clean.
- `ctest`: **16/16 tests passed** (24.6s), including `rx_core_tests`
  (`profile_test.cpp` compiling/running as bare no-ops).
- **Zero Tracy symbols, verified two ways:**
  - `nm -C` across every executable and static library in the build tree:
    zero `tracy::`-namespaced or `___tracy_`-prefixed symbols anywhere (a
    control run against the same command on the RX_TRACY=ON build's
    `sample_05_multipass` found `___tracy_after_lock_lockable_ctx` and
    friends immediately, confirming the check itself is meaningful).
  - `grep -c "TRACY_ENABLE" compile_commands.json`: **0** occurrences in the
    OFF build vs **69** in the ON build.
  - No `TracyClient.cpp.o` object file anywhere in the OFF build tree.
  - (`strings` on the OFF binaries does surface the literal path
    `.../rx_rhi_vk/src/tracy_gpu.cpp` — that is DWARF debug-line info
    naming *this project's own* no-op stub source file, not Tracy library
    code; `nm` is the authoritative check and it is clean.)

### Live Tracy capture against `sample_05_multipass --present`

Built Tracy's own `tracy-capture` and `tracy-csvexport` CLI tools
**standalone, outside this project's build** (scratchpad only — never
vendored into the repo, consistent with "client-only, never the
profiler UI/server"), to connect a real capture and inspect it.

Ran `sample_05_multipass --present` under `xvfb-run` (headless X, this
sandbox has no real display), connected `tracy-capture` over the network
protocol Tracy's client broadcasts on:

**Run 1 (no `--validate`, ~8s capture):** 1858 frames, 14,862 zones,
46.04 MB trace (`/tmp/.../scratchpad/sample05_capture.tracy`).
`tracy-csvexport`'s zone-statistics view confirms every CPU zone this task
added, attributed to the exact file:line this task placed it at:

```
name,src_file,src_line,counts
graph_pass,      rx_graph/executor.cpp,   826, 5571   (~3 passes x 1857 frames)
execute,         rx_graph/executor.cpp,   762, 1857   (once per frame)
uploadToBuffer,  rx_rhi_vk/src/upload.cpp,128, 1863
Collect,         tracy/TracyVulkan.hpp,   256, 1857   (Tracy's own self-instrumented
                                                        Collect() -- confirms RX_GPU_COLLECT
                                                        fired exactly once/frame)
present,         rx_rhi_vk/src/device.cpp,346, 1856
acquireNextImage,rx_rhi_vk/src/device.cpp,325, 1857
```

`tracy-csvexport -g -u` (per-GPU-zone-event report) confirms the per-pass
**GPU** zones with their real, dynamic, render-graph-derived names —
`shadow`, `forward`, `tonemap` (sample_05's three declared passes) — each
~1856 times, with real captured GPU execution-time values (tens of
microseconds), attributed to `rx_graph/executor.cpp` (the
`RX_GPU_ZONE_DYNAMIC` call site). This is direct, positive proof the whole
GPU-context/calibrated-timestamps/transient-zone/collect pipeline is live
and correct end to end, not just compiling.

The run's own log confirms the calibrated path was taken:
```
Device::create: VK_EXT_calibrated_timestamps ENABLED on the selected physical device
rx_rhi_vk: Tracy GPU context created with calibrated timestamps (VK_EXT_calibrated_timestamps)
```

**Run 2 (`--present --validate`, ~6s capture, sync validation active):**
1783 frames, 14,262 zones, 35.15 MB trace
(`/tmp/.../scratchpad/sample05_capture_validated.tracy`). Log analysis:
24,020 `[vulkan validation]`-tagged lines total, and **all 24,020** carry
this codebase's own pre-existing `"(known false positive: ...)"` markers
(three categories, all already documented in `context.cpp` from earlier
phases — `VK_KHR_portability_enumeration` layer-version mismatch, SPIR-V
`SourceLanguage=Slang` mismatch, and the separate-sampler sync-validation
false positive). **Zero** validation lines outside that already-accepted
set; **zero** `RX_LOG_ERROR` lines of any kind. Zero-validation-error, with
sync validation active, with the full CPU+GPU Tracy instrumentation live.

## Files

- New: `src/rx_core/include/rx_core/profile.h`, `src/rx_core/tests/profile_test.cpp`,
  `src/rx_rhi_vk/include/rx_rhi_vk/tracy_gpu.h`, `src/rx_rhi_vk/src/tracy_gpu.cpp`.
- Modified (CMake): `CMakeLists.txt`, `CMakePresets.json`,
  `third_party/CMakeLists.txt`, `src/rx_core/CMakeLists.txt`,
  `src/rx_rhi_vk/CMakeLists.txt`.
- Modified (zones): `src/rx_rhi_vk/include/rx_rhi_vk/device.h`,
  `src/rx_rhi_vk/src/device.cpp`, `src/rx_graph/executor.cpp`,
  `src/rx_material/material_system.cpp`, `src/rx_material/instance.cpp`,
  `src/rx_rhi_vk/src/upload.cpp`, `src/rx_rhi_vk/src/descriptor_arena.cpp`,
  `samples/{01_triangle,02_hotreload,03_bindless_mesh,04_streaming,05_multipass,06_materials}/main.cpp`.
- Evidence (scratchpad, not committed): `sample05_capture.tracy`,
  `sample05_capture_validated.tracy`, `zone_summary.csv`,
  `gpu_zone_events.csv`, `sample05_present.log`,
  `sample05_present_validated.log`.

## Concerns

1. `rx_task`/`Scheduler::parallelFor` zone+plot: excluded per the
   coordinator's mid-task scope correction. Open for a future task.
2. `RX_ZONE_DYNAMIC_NAME` is a fifth macro beyond the four literally named
   in the resolutions — added because dispatch item 4 requires it; documented
   and cited in `profile.h`.
3. The plain (uncalibrated) `TracyVkContext` GPU-context fallback branch is
   reviewed and compiles but was not driver-exercised live in this task —
   no available Vulkan implementation in this sandbox lacks
   `VK_EXT_calibrated_timestamps` to force that branch.
