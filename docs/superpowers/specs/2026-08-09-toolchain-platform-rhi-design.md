# RendererX: Toolchain + Platform + RHI — Design

Status: Approved
Date: 2026-08-09

## Context: overall renderer scope

RendererX is a ground-up renderer for third-party game engines, shipped as a
single DLL. Fixed constraints for the whole project:

1. Vulkan is the only GPU backend (cross-platform via one API, no D3D12/Metal
   backends).
2. The build must be able to compile from any host machine to any target
   platform (true cross-compilation, not per-OS build machines).
3. Shaders are authored in Slang. The public API exposes engine-facing
   abstractions (`IMaterial` and friends) that support extension,
   inheritance, override, and overload.
4. Prefer ready-made libraries or ported implementations over writing
   subsystems from scratch.
5. SDL3 is the windowing/input/platform library.

The full system decomposes into these layers, bottom to top:

| # | Layer | Responsibility |
|---|-------|-----------------|
| 0 | Toolchain / Cross-Build | Compile from any host to any target |
| 1 | Platform Abstraction | Window, input, events, threads, file paths |
| 2 | Foundation / Core | Math, logging, memory, containers, job system |
| 3 | RHI — Vulkan Backend | Device/queue/swapchain, command buffers, pipelines, sync |
| 4 | Shader Compilation & Reflection | Slang → SPIR-V, reflection-driven layouts |
| 5 | Resource Management | Buffer/texture lifetime, staging, bindless tables |
| 6 | Render Graph | Declarative passes/resources, auto barriers, transient aliasing |
| 7 | Material / Shading Abstraction | `IMaterial`, `IShaderModule`, `ITexture`, `IMesh` |
| 8 | Scene Submission | Render items, transforms, cameras, lights; culling/LOD |
| 9 | Rendering Techniques | Shadows, lighting, post stack, upscaling |
| 10 | Asset Import (offline) | Mesh/texture import & baking tooling |
| 11 | Public SDK / DLL Surface | The interface game devs link against |
| 12 | Tools / Debug / Profiling | Validation, GPU markers, profiling |

This spec covers only layers 0–3. Each remaining layer gets its own spec later.

Two constraints were explicitly relaxed for now and should be revisited if
they become a problem: the C++ ABI-stability question for the DLL boundary
(layers 7/11), and shaders are shipped precompiled rather than requiring
runtime Slang compilation in the shipped DLL.

## Scope of this spec

Sub-project 1: **Toolchain + Platform Abstraction + RHI**. Goal: cross-compile
from a Linux host to Windows and Linux (Steam Deck included), with a working
Vulkan RHI that can render a hardcoded triangle through dynamic rendering.

## Target platforms

- Windows (native Vulkan drivers)
- Linux, including Steam Deck/SteamOS — Deck runs Mesa RADV on an RDNA2 iGPU,
  so it's covered by the Linux target, not a separate backend. Its feature
  ceiling (no mesh shaders, no hardware ray tracing) is a constraint on the
  RHI's *baseline* feature set, not a reason for special-casing it.
- macOS (via MoltenVK) — deferred, not part of this sub-project.

## Architecture

- **Build system:** CMake + Ninja. `zig cc`/`zig c++` is the compiler,
  selected via a CMake toolchain file per target triple: `linux-native`,
  `windows-cross-zig` (Linux host → Windows target). `steamdeck` is an alias
  of `linux-native` plus a Deck feature-safety check. `macos-cross` is
  deferred.
- **Dependencies:** vendored at a pinned commit/tag (submodule or
  `FetchContent`, no floating branches). A CMake dependency-cache module
  keys a build on `(dependency pin, target triple, zig version)`; a cache
  hit links pre-built static libs + headers as `IMPORTED` targets with zero
  compilation; a miss builds once and stores the result in `.deps-cache/`
  (reusable across machines/CI, not just local-disk). Slang is the
  exception: it is never built from source — official prebuilt release
  binaries (`libslang` + `slangc`) are fetched per platform and linked
  directly, since building a full compiler on every setup is both slow and
  unnecessary.
- **Platform layer:** SDL3 owns window/input/events/threads. `volk` owns
  Vulkan function loading (no static link against a per-platform loader).
- **RHI layer:** a thin, explicit Vulkan 1.3 wrapper. `vk-bootstrap` handles
  instance/device/swapchain bootstrap; VMA handles GPU memory. Baseline
  features are **dynamic rendering + synchronization2** — available on
  Windows, Linux, and Steam Deck. No mesh shaders or hardware ray tracing in
  the baseline; those become optional, capability-queried extensions later.
  Bindless-first descriptor design from day one, even though full
  descriptor/resource management is a later sub-project — retrofitting
  bindless after the fact is expensive to redo.

## Components

| Component | Responsibility | Depends on |
|---|---|---|
| `rx_core` | Math (GLM), logging (spdlog), basic handles/containers | none renderer-specific |
| `rx_platform` | Window/input/events/threads | SDL3, `rx_core` |
| `rx_rhi_vk` | Device/swapchain/pipeline/resource wrapper | volk, vk-bootstrap, VMA, `rx_platform`, `rx_core` |
| build scaffolding | Toolchain files per triple, dependency-cache module, Slang prebuilt-fetch script | — |

## Build flow

1. `cmake --preset linux-native` (or `windows-cross-zig`) configures with the
   matching toolchain file.
2. The dependency-cache module resolves each pinned dependency against
   `.deps-cache/<hash>/`: hit → link cached artifacts; miss → build once,
   then populate the cache. The Slang-fetch script downloads and checksums
   prebuilt binaries into `third_party/slang-prebuilt/<platform>/`.
3. `cmake --build` compiles only `rx_core`, `rx_platform`, `rx_rhi_vk`, and a
   smoke-test app against the cached dependency artifacts.
4. **Runtime smoke test** (the concrete "done" signal for this sub-project):
   open an SDL3 window → create instance/device/swapchain via vk-bootstrap →
   allocate a VMA-backed vertex buffer for a hardcoded triangle using a
   precompiled SPIR-V shader (Slang integration is layer 4, a later
   sub-project) → record the draw via dynamic rendering → present.

## Error handling

- Debug builds auto-enable Vulkan validation layers via vk-bootstrap's debug
  messenger, routed into `rx_core`'s logger.
- `rx_rhi_vk` translates `VkResult` failures into a small set of
  engine-level error codes at API boundaries (device-lost, out-of-memory,
  swapchain out-of-date/suboptimal → resize) rather than leaking raw
  `VkResult` through the wrapper.
- A dependency-cache build failure fails loudly, naming the exact dependency
  and target triple. It never silently falls back to rebuilding everything.

## Testing / success criteria

- CI matrix: `linux-native` and `windows-cross-zig`, each producing the
  triangle smoke-test binary. Deck-safe feature checks run as part of the
  `linux-native` job.
- Manual acceptance: the triangle renders on real Windows, Linux, and Steam
  Deck hardware.
- Build-time acceptance: the first checkout may take longer (one-time
  dependency bootstrap). Every subsequent build with unchanged dependency
  pins finishes in **under 1 minute** on the reference dev machine.

## Deferred to later sub-project specs (not dropped)

Everything below is still planned and will get its own design spec later —
it is out of scope for *this* spec only, not out of scope for the project:

- Slang runtime compilation/reflection (layer 4)
- Descriptor/resource management beyond the bindless-friendly baseline
  (layer 5)
- Render graph, materials, scene submission, techniques, asset import,
  public SDK surface, tooling (layers 6–12) — including, explicitly, these
  subsystems so they aren't lost before those layers get specced:
  - **Geometry processing** (layer 8, scene submission): meshlet
    generation, virtual geometry, LOD management, skeletal mesh skinning,
    morph targets. **meshoptimizer is the committed library** for
    import-time optimization (Phase 4), meshlet building, and LOD
    simplification (decided 2026-08-10; see also the Phase 4 seed notes).
    Asset decompression stays CPU-side (KTX2+zstd on worker threads) —
    GPU decompression was evaluated and dropped 2026-08-10 (D3D12-bound
    ecosystem; no expected win on the Deck floor).
  - **Lighting infrastructure & spatial queries** (layer 9, techniques):
    acceleration structures (BVH for ray tracing), clustered/deferred
    lighting grids, shadow cascades, global illumination probes.
  - **Hardware ray tracing** (layer 9, techniques; committed 2026-08-10):
    an OPTIONAL device capability, never baseline — Steam Deck (the
    hardware floor) exposes VK_KHR_ray_query/ray_tracing_pipeline via RADV
    but with minimal RT hardware, so every RT feature ships behind a
    startup capability query with a raster fallback (RT shadows → shadow
    maps, RT reflections → screen-space/cubemap). Optionality-with-
    fallback is the engine-wide principle for any feature above the
    Vulkan 1.3 baseline. Sequencing: requires Phase 4's scene layer
    (acceleration structures need real scene geometry); first deliverables
    are ray_query-based shadows/AO, not full RT pipelines. The existing
    foundations were chosen RT-compatible deliberately: bindless set 0
    (any-hit material access), Slang (RT shader stages), render graph
    (RT pass = compute-class pass + acceleration-structure resource type),
    VMA (AS allocation).
  - **Post-processing & image reconstruction** (layer 9, techniques): tone
    mapping, color grading, temporal anti-aliasing, hardware upscaling
    wrappers (DLSS, FSR, XeSS).
  - **Profiling & debug instrumentation** (layer 12, tooling): GPU markers
    (PIX/RenderDoc integration), debug line drawing, memory leak tracking,
    performance counters.
- macOS/MoltenVK support
- DLL ABI-stability strategy for the public interface
- **Task/mesh shaders** (committed 2026-08-10, geometry phase): optional
  fast path per the optionality principle — RADV exposes
  VK_EXT_mesh_shader on the Deck floor but RDNA2 task-stage throughput is
  weak and older GPUs lack it. The meshlet pipeline (meshoptimizer data)
  ships with a compute-culling + classic-vertex-pipeline baseline; mesh
  shaders are the capability-queried consumer of the same meshlet data.
- **GPU-driven pipelines / visibility buffers** (committed 2026-08-10):
  GPU-driven culling with indirect draw lists needs only core-1.3
  features (drawIndirectCount, device address, bindless) — no gating;
  sequenced with/after the geometry phase's meshlets and the HiZ
  occlusion milestone. Visibility-buffer shading (triangle-ID raster +
  deferred material resolve) is a techniques-phase decision AFTER
  meshlets exist; it requires a shade-from-ID path through the material
  system's specialization model — design work to scope in that phase's
  spec, not assumed.
- **Multi-language bindings** (committed 2026-08-10, SDK phase): the public
  API ships C-ABI-first — a single IDL source generates the C header, the
  C++ COM-lite header, and the DLL shim (bgfx precedent; kills header
  drift) — and each language binds the C header via its own native tool
  (Rust bindgen, Zig translate-c, C# P/Invoke generators, Python cffi,
  Lua FFI). SWIG is the recorded fallback if a broad scripting-language
  sweep is ever wanted directly from C++, but C-first is the strategy.
  The COM-lite surface discipline (PODs, no STL/exceptions, error codes)
  already satisfies every generator's input constraints by construction.
- **Main-loop ownership** (decided 2026-08-10, binds the SDK-phase spec):
  RendererX is library-model — the consuming game/engine owns `main()` and
  the frame loop and calls an explicit frame API (begin-frame → declare
  passes/submit → end-frame), never the reverse. No required init/frame
  callbacks, no engine-owned loop: the audience is custom-engine
  developers who own their own loops (bgfx/PhysX/FMOD precedent). An
  optional thin "runner" convenience (window + per-frame callback, e.g.
  over SDL3's callback mode) may ship later for quick starts, built ON the
  library API and never required by it.
