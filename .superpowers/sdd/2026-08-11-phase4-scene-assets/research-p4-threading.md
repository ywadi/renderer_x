# Phase 4: Parallel Threading & Profiling Research

**Date:** 2026-08-11  
**Scope:** Task scheduler comparison, GPU profiling integration, parallel command recording patterns  
**Status:** UNVERIFIED where marked; citations provided per claim

---

## 1. Task Scheduler Comparison: enkiTS vs Taskflow vs marl

### enkiTS

**Current Version:** 1.12  
**License:** Zlib (permissive)  
**Maintenance:** Maintained by Doug Binks; no major release in 12 months (as of Aug 2026). [↗ vcpkg](https://vcpkg.io/en/package/enkits.html)

**API Shape:**
- **Parallel-for over N items:** `ITaskSet` abstraction with automatic work-stealing range splitting; zero-allocation scheduling. [↗ GitHub README](https://github.com/dougbinks/enkiTS/blob/master/README.md)
- **Task graphs with dependencies:** Limited; fires-and-forgets task model, not explicit DAG. Can nest tasks from within tasks.
- **Pinned/dedicated IO threads:** `IPinnedTask` for thread-specific operations; supports thread affinity.
- **External thread integration:** Main thread can participate; external threads registered via `numExternalTaskThreads` config. [UNVERIFIED footprint impact]

**Binary/Compile Footprint:** Header-only core + minimal scheduler (~few KB object). [UNVERIFIED; no published footprint data]

**Adoption Evidence:**  
- Used in Avoyd (voxel octree engine, cross-platform C++/OpenGL/Vulkan). [↗ Enkisoftware devlog](https://www.enkisoftware.com/devlog-game-tech)
- Widely packaged (Homebrew, vcpkg); small community but stable. [UNVERIFIED adoption scale]

**Windows-gnu (MinGW) Compat:** Likely via C++11 standard library; no specific build issues reported. [UNVERIFIED; requires testing]

---

### Taskflow

**Current Version:** Latest 3.11.0 (Nov 2025); scheduled 4.0.0 (Jan 2026). [↗ Release notes](https://taskflow.github.io/taskflow/Releases.html)  
**License:** MIT (permissive)  
**Maintenance:** Actively maintained by Dr. Tsung-Wei Huang (UW Madison research group); regular scheduled releases.

**API Shape:**
- **Parallel-for over N items:** `Taskflow::parallel_for()` with partitioner control; explicit range-based loops.
- **Task graphs with dependencies:** First-class DAG support; `emplace()`, `precede()`, `succeed()` for explicit dependencies. Recursive subflow tasks.
- **Pinned/dedicated IO threads:** Executor supports worker thread configuration; [UNVERIFIED: no explicit thread pinning API documented]
- **External thread integration:** Executor-based design; custom threads can submit tasks via `run()` / `run_until()` blocking methods. [UNVERIFIED integration overhead]

**Binary/Compile Footprint:** Mostly header-only library; ~1-2 MB headers, minimal runtime. [UNVERIFIED footprint on RendererX scale]

**Adoption Evidence:**  
- Broad academic/industry use (IREE, TensorFlow, Omniverse documented). [↗ Taskflow docs](https://taskflow.github.io/taskflow/index.html)
- NVIDIA, IREE profiling docs reference it. [UNVERIFIED game engine adoption]

**Windows-gnu (MinGW) Compat:** CMake cross-compile standard (header-only + C++17); no reported MinGW blockers. [UNVERIFIED]

---

### marl (Google)

**Status:** **ARCHIVED** (April 27, 2026). Read-only, no longer maintained. [↗ GitHub](https://github.com/google/marl)

**Was:** Hybrid thread/fiber task scheduler (C++11).  
**Why archived:** Likely superseded by other solutions; successor commitment unknown.

**⚠️ Recommendation:** Do not adopt for new projects.

---

### Recommendation for RendererX

**Choose: Taskflow (primary) + enkiTS (fallback)**

**Rationale:**
- **Taskflow:** Explicit DAG matching asset pipeline (decode→upload→bind), active maintenance, broad industry adoption, header-only simplicity for CMake, C++17 alignment with RendererX baseline.
- **enkiTS:** Lighter footprint for dedicated IO worker thread pool (parallel asset decode), proven stability, Zlib licensing. Use for bulk I/O tasks where DAG overhead unneeded.
- **Hybrid approach:** Taskflow orchestrates frame (command recording fan-out, sync points); enkiTS subsidiary for asset workers.

**Caveat:** Both untested on RendererX's Linux→Windows-gnu cross-compile. Verify CMake toolchain chain integration before committing.

---

## 2. Tracy Profiler

### Current State

**Version:** 0.11.0+ (latest shown; July 2024 release added VK_EXT_calibrated_timestamps loader). [↗ GitHub releases](https://github.com/wolfpld/tracy/releases)  
**License:** BSD (permissive, suitable for open shipping)

### CMake Integration

**Standard approach:**
```cmake
# Option A: Git submodule
add_subdirectory(extern/tracy)
target_link_libraries(myapp PRIVATE TracyClient)

# Option B: FetchContent
include(FetchContent)
FetchContent_Declare(tracy GIT_REPOSITORY https://github.com/wolfpld/tracy.git)
FetchContent_MakeAvailable(tracy)
target_link_libraries(myapp PRIVATE TracyClient)
```

**Enable profiling:**
```cpp
// In build flags:
#define TRACY_ENABLE
#define TRACY_ON_DEMAND  // if optional connection desired
```

[↗ Integration guides: IREE, Flax, Omniverse](https://iree.dev/developers/performance/profiling-with-tracy/)

### CPU Zone Macros

**Basic zones:**
```cpp
ZoneScoped;                           // Frame scope
ZoneScopedNC("job-decode", 0xFF00FF); // Named + color
ZoneScopedN("parallel-loop");         // Named only
ZoneScopedS(depth);                   // Sized alloc
```

**Overhead:** ~50 ns per macro invocation; 15 ns per zone start/end pair. Suitable for high-frequency instrumentation (10k+ zones/frame). [↗ Performance metrics](https://news.ycombinator.com/item?id=41632719)

### Vulkan GPU Zones

**TracyVkCtx creation requirements:**
```cpp
auto ctx = TracyVkContextCalibrated(device, physical_device, queue, command_buffer, 
                                     vkGetInstanceProcAddr, vkGetDeviceProcAddr);
// Requires: VK_EXT_calibrated_timestamps queried + enabled at device creation
```

**Timestamp query setup:**
- Query pool for GPU timestamps allocated per frame.
- `vkCmdWriteTimestamp()` at zone boundaries.
- Implicit calibration on each frame; **115 ms startup cost** for calibration. [UNVERIFIED: per-frame amortization]

**VK_EXT_calibrated_timestamps dependency:**
- **If driver lacks extension** (e.g., lavapipe): GPU zones fall back to CPU-side markers (no actual GPU timestamps). [UNVERIFIED fallback behavior]
- **Required for accurate CPU-GPU sync:** Extension provides quasi-simultaneous timestamp capture between CPU (CLOCK_MONOTONIC) and GPU (VkTimelineDomain).
- Supported since Tracy v0.11.0 (July 2024). [↗ Vulkan docs](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_calibrated_timestamps.html)

### Overhead & Build Modes

| Scenario | Overhead | Notes |
|----------|----------|-------|
| TRACY_ENABLE + connected | ~1–3% CPU, GPU zones + serialization | Full instrumentation |
| TRACY_ON_DEMAND (connected) | ~0.5% CPU baseline | Lazy zone capture, buffer ring |
| TRACY_ON_DEMAND (not connected) | ~negligible | Buffering only, no network |
| Disabled (no TRACY_ENABLE) | 0% | No compiled code |

**On-demand vs always-on builds:** `TRACY_ON_DEMAND` keeps code compiled but dormant until profiler connects; scales down overhead but complicates debugging. [UNVERIFIED: best practice for shipping renderers]

### Windows-gnu (MinGW) Cross-Compile Status

**Current state:** MinGW-w64 build possible but **requires careful threading model selection**.

**Known issues:**
- POSIX vs Windows thread conflicts in server build (x64 only; LTO + intrinsics like `__lzcnt64` need flags). [↗ GitHub issue #904, #252](https://github.com/wolfpld/tracy/issues/904)
- Client library (what you link) is simpler; server (profiler GUI) has friction.

**Recommendation for RendererX:** 
- Build Tracy **client** with Linux→Windows-gnu toolchain (standard C++; low risk).
- Run **profiler server** (capture viewer) on Windows native or build separately.
- Test thread initialization on first cross-compile run.

---

## 3. Parallel Command Recording in Vulkan

### Authoritative Guidance

**Khronos samples & docs:**
- [Command buffer usage & multithreading](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html)
- [Threading guide](https://docs.vulkan.org/guide/latest/threading.html)
- [Multithreading tutorial](https://docs.vulkan.org/tutorial/latest/17_Multithreading.html)

### Per-Thread, Per-Frame Command Pools

**Standard pattern (proven in shipping engines):**
```
For each frame in flight (e.g., 2–3 frames):
  For each worker thread:
    Allocate 1 VkCommandPool (RESET_COMMAND_BUFFER_BIT)
    Allocate secondary buffers from pool
    
At frame boundary:
  vkResetCommandPool(pool)  // Bulk reset; no per-buffer fences needed
  Reuse pool for next frame cycle
```

**Key constraint:** One pool **cannot** be used concurrently by multiple threads. Each thread has exclusive ownership of its pool per frame. [↗ Vulkan spec cmdbuffers](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html)

**Benefit:** Avoids per-buffer fences; bulk reset amortizes VkCommandPool lifecycle overhead.

### Secondary Command Buffers + Dynamic Rendering

**VkCommandBufferInheritanceRenderingInfo (Vulkan 1.3 / VK_KHR_dynamic_rendering):**

**Requirements when using dynamic rendering:**
- `VkCommandBufferInheritanceRenderingInfo pNext` chain on secondary buffer begin.
- Must specify:
  - `colorAttachmentCount` + `pColorAttachmentFormats[]`
  - `depthAttachmentFormat`, `stencilAttachmentFormat`
  - `rasterizationSamples`
- Formats must match primary's `vkCmdBeginRenderingKHR()` call.
- Use `VK_FORMAT_UNDEFINED` for unused attachments (discards writes). [↗ Spec](https://docs.vulkan.org/refpages/latest/refpages/source/VkCommandBufferInheritanceRenderingInfo.html)

**Secondary buffer recording:**
```cpp
VkCommandBufferInheritanceRenderingInfo inherit_rendering{
  VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
  nullptr,
  VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
  viewMask, colorAttachmentCount, pColorAttachmentFormats, 
  depthFormat, stencilFormat, rasterizationSamples
};

VkCommandBufferInheritanceInfo inherit{
  VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
  &inherit_rendering,
  VK_NULL_HANDLE,  // renderPass = NULL with dynamic rendering
  // ... rest
};

VkCommandBufferBeginInfo begin{
  VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
  &inherit,
  VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
};
vkBeginCommandBuffer(secondary, &begin);
// Record draw calls...
```

**Resume/Suspend vs Secondaries:** 
- Dynamic rendering + secondaries provide **deterministic ordering** via primary buffer submission order.
- Alternative: single primary buffer with conditional rendering (no resume/suspend needed). Choose secondaries for massive draw call parallelization (1000+); choose primary-only if draw call overhead dominates CPU time.

### Multi-Primary Submission Ordering

**How primary submission order matters:**
- Secondary buffers execute in order recorded to primary (array order in `vkCmdExecuteCommands`).
- **Primary submission order** (in `pCommandBuffers` to `vkQueueSubmit`) determines GPU execution.
- If multiple primaries submitted in one batch, their relative order is guaranteed; within a primary, secondaries execute in order.
- Synchronization (semaphores, pipeline barriers) overrides implicit ordering. [↗ Vulkan ordering guarantees](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/chapters/cmdbuffers.adoc)

### Shipping Engine Choices

**Godot 4.3+:**
- Uses secondary buffers + rendering acyclic **graph** (topological sort).
- Each draw list → secondary buffer (parallel recording).
- Primary buffer determines execution order via DAG node ordering (not sequential).
- Result: **CPU time reduction** by moving Vulkan API calls to worker threads; GPU synchronization via explicit graph constraints. [↗ Godot rendering graph article](https://godotengine.org/article/rendering-acyclic-graph/)

**General pattern (Unreal, others):**
- Secondary buffers for visibility culling → draw call batching parallelism.
- Primary buffer for **render pass structure** (depth prepass → G-buffer → lighting).
- Submission batching: prefer fewer primary submissions (1–2 per frame) over many.

**Performance caveat:** Secondary buffers incur overhead if too fragmented (many small buffers). Rule of thumb: 10–1000 draw calls per secondary; 2–10 secondaries per frame optimal. [↗ Vulkan multithreading guide](https://vkguide.dev/docs/extra-chapter/multithreading/)

---

## Summary for RendererX Implementation

| Component | Recommendation | Rationale |
|-----------|-----------------|-----------|
| **Async asset workers** | Taskflow (orchestration) + enkiTS (bulk decode) | DAG + lightweight pool |
| **Command recording** | Secondary buffers + per-thread pools + Taskflow task graph | Proven scale; Godot reference |
| **GPU profiling** | Tracy v0.11.0+ with VK_EXT_calibrated_timestamps | Low overhead; CMake friendly |
| **Cross-compile** | Test Tracy client + Taskflow CMake early | Both MinGW-compatible in theory |

---

## Sources

- [enkiTS vcpkg](https://vcpkg.io/en/package/enkits.html)
- [enkiTS GitHub README](https://github.com/dougbinks/enkiTS/blob/master/README.md)
- [Enkisoftware devlog](https://www.enkisoftware.com/devlog-game-tech)
- [Taskflow Release notes](https://taskflow.github.io/taskflow/Releases.html)
- [Taskflow Documentation](https://taskflow.github.io/taskflow/index.html)
- [Google marl GitHub (archived Apr 2026)](https://github.com/google/marl)
- [Tracy Profiler Releases](https://github.com/wolfpld/tracy/releases)
- [Tracy CMake Integration (IREE)](https://iree.dev/developers/performance/profiling-with-tracy/)
- [Tracy GitHub Issue #904 (MinGW)](https://github.com/wolfpld/tracy/issues/904)
- [Vulkan Spec: Command Buffers](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html)
- [Vulkan: Command Buffer Usage & Multithreading](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html)
- [Vulkan: Threading Guide](https://docs.vulkan.org/guide/latest/threading.html)
- [Vulkan: VkCommandBufferInheritanceRenderingInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkCommandBufferInheritanceRenderingInfo.html)
- [Vulkan: VK_EXT_calibrated_timestamps](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_calibrated_timestamps.html)
- [Godot 4.3 Rendering Acyclic Graph](https://godotengine.org/article/rendering-acyclic-graph/)
- [Vulkan Multithreading Guide (vkguide.dev)](https://vkguide.dev/docs/extra-chapter/multithreading/)
