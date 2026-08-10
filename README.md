# RendererX

A from-scratch Vulkan 1.3 renderer for custom game engines. RendererX establishes a baseline of dynamic rendering and synchronization2 support, targets Steam Deck as the hardware floor, and cross-compiles from Linux to Windows via a vendored zig toolchain. Shaders are authored in Slang. Phase 2 status: Phase 1's foundation layers plus runtime Slang compilation and reflection, bindless resource management, and four samples (triangle, shader hot-reload, bindless mesh, texture streaming) are complete.

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
- **shaders/** — Shader source files (Slang)
- **samples/01_triangle/** — Hardcoded white triangle via dynamic rendering, headless and present modes
- **samples/02_hotreload/** — Fullscreen triangle with a runtime-compiled (Slang, not offline slangc) fragment shader and a reflection-driven pipeline layout; live shader hot-reload in present mode
- **samples/03_bindless_mesh/** — Procedural meshes and textures driven entirely through one bindless descriptor table and a reflection-derived pipeline layout
- **samples/04_streaming/** — Texture streaming into a fixed bindless residency budget, exercising deferred eviction/re-registration safety while frames are in flight
- **tools/** — Utilities: dependency cache, Slang prebuilt fetch, build-budget checker
- **third_party/** — Vendored dependencies (volk, vk-bootstrap, VMA, SDL3, GLM, spdlog)

## Roadmap

**Phase 1 (complete):** Toolchain, platform abstraction, core library, RHI foundation, first triangle sample with both headless and present-mode rendering.

**Phase 2 (complete):** Runtime Slang compilation and shader reflection (`src/rx_shader/`); bindless descriptor management (`rx::rhi::BindlessTable`); reflection-driven pipeline layouts; three new samples (02_hotreload, 03_bindless_mesh, 04_streaming) plus expanded test coverage; CI + packaging for all four samples.

**Phase 3 (in progress) and beyond:** Render graph (declarative passes, automatic barriers); material system (IMaterial, IShaderModule, ITexture, IMesh); asset pipeline (mesh/texture import); scene submission (cameras, lights, culling); rendering techniques (shadows, post-processing, upscaling); tooling (GPU markers, profiling, validation). See [`docs/superpowers/specs/`](docs/superpowers/specs/) for design documents.

## Testing

Run the full test suite (including headless triangle correctness gate):

```sh
ctest --preset linux-native --output-on-failure
```

The suite includes unit tests for rx_core, rx_platform, rx_rhi_vk, shader compilation, and the headless triangle sample. Interactive `--present` mode is tested manually; see [`MANUAL_VERIFICATION.md`](MANUAL_VERIFICATION.md).
