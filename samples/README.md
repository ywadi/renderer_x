# Samples

Runnable demos that exercise `rx_rhi_vk` (and, beneath it, `rx_core`/
`rx_platform`) end to end against a real Vulkan driver. Each sample gets its
own subdirectory and its own `CMakeLists.txt`; all of them are built as part
of the normal `cmake --build --preset <preset>` flow (see the root
`CMakeLists.txt`'s `add_subdirectory(samples/...)` lines) and don't need any
separate build step.

## 01_triangle

`samples/01_triangle/main.cpp` — renders one hardcoded white triangle on a
black background via a Vulkan 1.3 dynamic-rendering pipeline (no vertex
buffers: the vertex shader generates its 3 NDC positions from
`SV_VertexID`). Two modes, selected by a command-line flag, sharing the
exact same pipeline-construction code and draw call:

- **Headless (default, no flags)** — the correctness gate this repo's CI and
  `ctest` run. Builds the full stack (window, validated instance, surface,
  device, real swapchain — created and queried, but deliberately never
  written to), renders into a dedicated **offscreen** 256x256 image, reads
  the result back to the host, and asserts the center pixel is white and a
  corner pixel is black. Exits 0 on pass, 1 on failure, and also fails if the
  Vulkan validation layer reported anything beyond this project's two
  documented, narrowly-matched false-positive guards (see `context.cpp`).
  Registered as the `sample_01_triangle_headless` ctest case.
- **`--present`** — opens a real, visible, resizable window and renders the
  same triangle into the actual swapchain images every frame, via the
  canonical frames-in-flight present loop (`rx::rhi::FrameSync`:
  `src/rx_rhi_vk/include/rx_rhi_vk/frame_sync.h`). This is the first and
  only place in this codebase that writes to a swapchain image — and only
  ever the image `vkAcquireNextImageKHR` actually returned. Runs until the
  window is closed (or the process receives `SIGINT`/`SIGTERM`, which SDL
  translates into a clean shutdown); survives being resized at any point,
  any number of times, with zero validation errors. Not part of `ctest` —
  it's interactive by nature. See `MANUAL_VERIFICATION.md` at the repo root
  for the per-platform manual check this mode gets before a release.

### Expected output

**Headless mode** prints one of these lines to the log and exits accordingly
— there is no visible window (it's created hidden):

```
[info] triangle readback PASSED
```

**`--present` mode** opens an 800x600 window titled `rx_triangle_sample
(--present)` showing a solid white, upward-pointing triangle centered
horizontally in the lower-middle of an otherwise solid black window —
roughly:

```
┌──────────────────────────────┐
│                                │
│               ▲                │
│              ▲ ▲               │
│             ▲   ▲              │
│            ▲▲▲▲▲▲▲             │
│                                │
└──────────────────────────────┘
```

Resizing the window keeps the triangle centered and proportioned to the new
window size (viewport/scissor are dynamic state, recomputed from
`Device::swapchainExtent()` every frame — no stretching or stale content).
Closing the window exits the process with status 0 and logs:

```
[info] --present: window closed cleanly
```

## Building and running

Both sample modes and both build presets work identically on Linux and
Steam Deck (Desktop Mode is just Linux) — Windows only differs in how you
run the already-cross-compiled binary. There is no `samples`-specific
configure step; building the preset builds every sample.

### Linux (native) — including Steam Deck Desktop Mode

```sh
cmake --preset linux-native
cmake --build --preset linux-native
```

```sh
# Headless correctness gate (also runs via ctest, see below):
./build/linux-native/samples/01_triangle/sample_01_triangle

# Interactive present-mode window:
./build/linux-native/samples/01_triangle/sample_01_triangle --present
```

Steam Deck Desktop Mode: open Konsole (or any terminal) from the Desktop
Mode taskbar and run the exact same commands above — the Deck's SteamOS is
Linux with a Vulkan driver (AMD Mesa RADV), so `linux-native` is the correct
preset; there is no Deck-specific build variant. `--present`'s window
behaves like any other windowed X11/Wayland app under KDE Plasma (Desktop
Mode's desktop environment) — it can be moved, resized, and closed exactly
as described above.

### Windows (cross-compiled from Linux via zig)

```sh
cmake --preset windows-cross-zig
cmake --build --preset windows-cross-zig
```

This produces `build/windows-cross-zig/samples/01_triangle/sample_01_triangle.exe`.
The binary is statically linked (no separate `.dll`s to ship) — copy just
that one `.exe` to the Windows machine (or run it directly under Wine on
Linux) and run it from a terminal/`cmd.exe`/PowerShell:

```
sample_01_triangle.exe            REM headless correctness gate
sample_01_triangle.exe --present  REM interactive present-mode window
```

See `MANUAL_VERIFICATION.md` at the repo root for the actual per-platform
run checklist (what to record, what "pass" looks like on real hardware).

### Running the automated test suite

```sh
ctest --preset linux-native --output-on-failure
```

This runs `sample_01_triangle_headless` alongside every other project test
(`rx_core_tests`, `rx_platform_tests`, `rx_rhi_vk_tests`, `shader_spirv_test`)
— `--present` mode is intentionally excluded from `ctest` (it blocks on a
real window and user/OS interaction) and is exercised manually instead.
