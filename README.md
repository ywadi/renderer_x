# RendererX

A from-scratch Vulkan 1.3 renderer for custom game engines. RendererX establishes a baseline of dynamic rendering and synchronization2 support, targets Steam Deck as the hardware floor, and cross-compiles from Linux to Windows via a vendored zig toolchain. Shaders are authored in Slang. Phase 3 status: Phase 1's foundation layers, Phase 2's runtime Slang compilation and reflection, bindless resource management, render graph, material system, and six samples (triangle, shader hot-reload, bindless mesh, texture streaming, multipass rendering, materials) are complete.

[![CI](https://github.com/ywadi/renderer_x/actions/workflows/ci.yml/badge.svg)](https://github.com/ywadi/renderer_x/actions/workflows/ci.yml)

## Requirements

- **Linux host** (for building, including cross-compilation to Windows)
- **CMake** 3.21 or later
- **Ninja** (build generator)
- **Vulkan 1.3 driver** (on the machine running compiled binaries)
- **zig** 0.16.0 (vendored per-project at `toolchain/zig/zig`, installed via the one-time setup below — not required in PATH; the CMake toolchain locates it automatically)

## Quick Start

### One-time setup

Before the first build, install the vendored zig toolchain:

```sh
mkdir -p toolchain
curl -L -o /tmp/zig.tar.xz https://ziglang.org/download/0.16.0/zig-x86_64-linux-0.16.0.tar.xz
tar -xf /tmp/zig.tar.xz -C toolchain
mv toolchain/zig-x86_64-linux-0.16.0 toolchain/zig
rm /tmp/zig.tar.xz
```

CI verifies this tarball against a pinned SHA256 (see `.github/workflows/ci.yml`); locally you can do the same if desired.

### Build

Configure and build for Linux native (including Steam Deck):

```sh
cmake --preset linux-native
cmake --build --preset linux-native
```

Run the triangle sample in headless mode (automated correctness gate):

```sh
./build/linux-native/samples/01_triangle/sample_01_triangle
```

Run the triangle sample in interactive present-mode (opens a window):

```sh
./build/linux-native/samples/01_triangle/sample_01_triangle --present
```

For cross-compilation to Windows (from Linux):

```sh
cmake --preset windows-cross-zig
cmake --build --preset windows-cross-zig
```

The resulting Windows executable is statically linked — copy only `build/windows-cross-zig/samples/01_triangle/sample_01_triangle.exe` to a Windows machine and run it directly.

For detailed build and run instructions per platform, including Steam Deck and manual verification checklist, see [`samples/README.md`](samples/README.md).

## Project Layout

- **cmake/** — Build system configuration, toolchain files (linux-native, windows-cross-zig), dependency caching
- **src/rx_core/** — Foundation library: math (GLM), logging (spdlog), memory, containers, handles
- **src/rx_platform/** — Platform abstraction: window (SDL3), input, events, threads
- **src/rx_rhi_vk/** — Vulkan 1.3 RHI: device/queue/swapchain, command buffers, pipelines, synchronization
- **src/rx_shader/** — Runtime Slang-to-SPIR-V compilation and shader reflection (descriptor/push-constant layout)
- **src/rx_graph/** — Render graph: declarative passes, automatic barrier derivation, transient pooling, dynamic rendering execution
- **src/rx_material/** — Material system: Slang-module materials, pipeline caching, COM-lite public API, material hot-reload
- **src/rx_task/** — Job system: `rx::task::Scheduler`, an enkiTS-backed worker pool + main-thread post/pump queue
- **src/rx_asset/** — Asset pipeline: glTF import (`rx::asset::Registry`), pooled geometry (`GeometryPool`), KTX2/texture streaming (`TextureCache`)
- **src/rx_scene/** — Scene submission: no-ECS render-proxy layer (`rx::scene::Scene`), reversed-Z `Camera`, frustum/shadow-caster culling + instancing collapse (`DrawListBuilder`)
- **src/rx_shadow/** — Shadow quality bridge: fitted-ortho shadow frustum, slope-scaled bias, comparison-sampler PCF
- **src/rx_debug_ui/** — Dear ImGui debug overlay as a declarative render-graph pass
- **shaders/multipass/** — Shader sources for 05_multipass sample (Slang)
- **shaders/material/** — Shared material-system shaders (Slang)
- **shaders/shadow/** — Shared shadow-caster shader (Slang)
- **samples/01_triangle/** — Hardcoded white triangle via dynamic rendering, headless and present modes
- **samples/02_hotreload/** — Fullscreen triangle with a runtime-compiled (Slang, not offline slangc) fragment shader and a reflection-driven pipeline layout; live shader hot-reload in present mode
- **samples/03_bindless_mesh/** — Procedural meshes and textures driven entirely through one bindless descriptor table and a reflection-derived pipeline layout
- **samples/04_streaming/** — Texture streaming into a fixed bindless residency budget, exercising deferred eviction/re-registration safety while frames are in flight
- **samples/05_multipass/** — Multipass render graph (shadow map, forward shading, tonemap) demonstrating declarative passes and automatic barrier derivation
- **samples/06_materials/** — Material-system showcase using the public COM-lite API with hot-reload support and parameter overrides
- **samples/07_stress/** — Parallel-recording stress benchmark: a large procedural instanced field driven through a chunked render-graph pass, single-thread vs. default-worker-count A/B
- **samples/08_gltf_viewer/** — Real glTF asset import (async), StandardPBR/Unlit rendering via the material system, orbit camera
- **samples/09_scene/** — Phase 4 phase-exit sample: Registry → Scene → DrawListBuilder → render-graph fly-through, ImGui HUD (cull counters, layer/light-channel toggles, pool/memory stats), `--stress` scene-path A/B benchmark vs. 07_stress
- **tools/** — Utilities: dependency cache, Slang prebuilt fetch, build-budget checker, test-asset fetch/reference-regeneration, sample packaging
- **third_party/** — Vendored dependencies (volk, vk-bootstrap, VMA, SDL3, GLM, spdlog)

## Roadmap

**Phase 1 (complete):** Toolchain, platform abstraction, core library, RHI foundation, first triangle sample with both headless and present-mode rendering.

**Phase 2 (complete):** Runtime Slang compilation and shader reflection (`src/rx_shader/`); bindless descriptor management (`rx::rhi::BindlessTable`); reflection-driven pipeline layouts; three new samples (02_hotreload, 03_bindless_mesh, 04_streaming) plus expanded test coverage; CI + packaging for all four samples.

**Phase 3 (complete):** Render graph (`src/rx_graph/`, declarative passes, derived sync2 barriers, transient pooling, dynamic rendering); material system (`src/rx_material/`, Slang-module materials, lazy pipeline cache, COM-lite public API, hot reload); two new samples (05_multipass, 06_materials) with corresponding shader directories (`shaders/multipass/`, `shaders/material/`); expanded test coverage; CI + packaging for all six samples.

**Phase 4 (complete):** Job system (`src/rx_task/`) and a zero-per-frame-allocation chunked executor for parallel command recording; asset pipeline (`src/rx_asset/`, glTF import via fastgltf/meshoptimizer/Draco/MikkTSpace, pooled geometry, KTX2 textures); scene submission (`src/rx_scene/`, no-ECS render proxies, reversed-Z camera, frustum + shadow-caster culling with GPU instancing collapse); shadow quality bridge (`src/rx_shadow/`); an ImGui debug overlay (`src/rx_debug_ui/`); expanded platform input (mouse capture, gamepad, keyboard); three new samples (07_stress, 08_gltf_viewer, 09_scene — the phase-exit fly-through + stress-v2 benchmark) with corresponding shader directories; expanded test coverage; CI + packaging for all nine samples. Tag `v0.4.0-phase4`.

**Phase 5 and beyond:** Rendering techniques (shadows beyond the single-map baseline, post-processing, upscaling); LOD/meshlet geometry processing; public SDK/DLL surface; tooling (GPU markers, profiling, validation). See [`docs/superpowers/specs/`](docs/superpowers/specs/) for design documents.

## Testing

Run the full test suite (including headless triangle correctness gate):

```sh
ctest --preset linux-native --output-on-failure
```

The suite includes unit tests for rx_core, rx_platform, rx_rhi_vk, shader compilation, and the headless triangle sample. Interactive `--present` mode is tested manually; see [`MANUAL_VERIFICATION.md`](MANUAL_VERIFICATION.md).

### Windows-cross-zig: verify under Wine locally, not just "it builds"

Any change that touches a binary CI's `windows-cross-zig` job actually **runs**
(not merely builds) must be verified under Wine locally before it is
considered green — matching CI's own invocation exactly, not just a build
check:

```sh
cmake --preset windows-cross-zig && cmake --build --preset windows-cross-zig
xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample' --output-on-failure
```

This is **not** a device-free run: `.github/workflows/ci.yml`'s own comment on
this step documents that installing lavapipe on the runner gives Wine's
built-in Vulkan support (`winevulkan`) a real, software Vulkan implementation
to forward to — so every binary this filter does **not** exclude (notably
`rx_asset_gltf_gpu_tests.exe`, and any future `*_gpu_tests` binary this
filter does not add to its exclusion list) constructs a real `VkDevice` and
exercises real GPU-resource lifetime under Wine, exactly like it does on
`linux-native`. A prior local-verification gap covered only the binaries that
are genuinely device-free under Wine (`rx_task_tests.exe`,
`rx_asset_gltf_tests.exe`, `shader_spirv_test.exe`, `rx_core_tests.exe`,
`rx_platform_tests.exe`, `rx_shader_tests.exe`) and treated a clean
`windows-cross-zig` *build* as sufficient for the GPU-backed-but-not-excluded
binaries — which let a genuine, timing-sensitive lifetime defect in the async
import pipeline (rx_asset) reach CI-red on both `windows-cross-zig` (Wine)
and `linux-native` before it was ever exercised under Wine locally (CI run
32180630087; see `.superpowers/sdd/2026-08-11-phase4-scene-assets/
wine-segfault-fix-report.md`). Closing that gap: local `windows-cross-zig`
verification always includes the Wine run above, for every binary the CI
filter leaves in, not just a build check.
