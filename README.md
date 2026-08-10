# RendererX

A from-scratch Vulkan 1.3 renderer for custom game engines. RendererX establishes a baseline of dynamic rendering and synchronization2 support, targets Steam Deck as the hardware floor, and cross-compiles from Linux to Windows via a vendored zig toolchain. Shaders are authored in Slang. Phase 1 status: foundation layers complete (toolchain → platform → RHI → first triangle sample).

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
- **shaders/** — Shader source files (Slang)
- **samples/01_triangle/** — Hardcoded white triangle via dynamic rendering, headless and present modes
- **tools/** — Utilities: dependency cache, Slang prebuilt fetch, build-budget checker
- **third_party/** — Vendored dependencies (volk, vk-bootstrap, VMA, SDL3, GLM, spdlog)

## Roadmap

**Phase 1 (complete):** Toolchain, platform abstraction, core library, RHI foundation, first triangle sample with both headless and present-mode rendering.

**Phase 2 (in progress):** Runtime Slang compilation and shader reflection; bindless descriptor design; additional samples and test coverage.

**Phase 3 and beyond:** Render graph (declarative passes, automatic barriers); material system (IMaterial, IShaderModule, ITexture, IMesh); asset pipeline (mesh/texture import); scene submission (cameras, lights, culling); rendering techniques (shadows, post-processing, upscaling); tooling (GPU markers, profiling, validation). See [`docs/superpowers/specs/`](docs/superpowers/specs/) for design documents.

## Testing

Run the full test suite (including headless triangle correctness gate):

```sh
ctest --preset linux-native --output-on-failure
```

The suite includes unit tests for rx_core, rx_platform, rx_rhi_vk, shader compilation, and the headless triangle sample. Interactive `--present` mode is tested manually; see [`MANUAL_VERIFICATION.md`](MANUAL_VERIFICATION.md).
