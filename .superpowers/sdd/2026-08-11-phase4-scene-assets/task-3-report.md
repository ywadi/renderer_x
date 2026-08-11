# Task 3 report: Tracy profiler client integration

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
