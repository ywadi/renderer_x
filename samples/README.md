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

## 02_hotreload

`samples/02_hotreload/main.cpp` — a fullscreen triangle whose fragment
shader is compiled **at runtime** by `rx_shader`'s `Compiler` (in-process
Slang → SPIR-V), not pre-baked offline by `slangc` like 01_triangle's
shaders. Unlike 01_triangle's hand-typed empty `VkPipelineLayoutCreateInfo`,
this sample's `VkPipelineLayout` always comes from `rx::shader::reflect()` +
`rx::rhi::PipelineLayoutBuilder::build()` — driven by whatever the compiled
shader actually declares (a single push-constant `float time`, no
descriptor bindings). Two modes, one shared compile→reflect→build→pipeline
path:

- **Headless (default, no flags)** — the `ctest` correctness gate. Compiles
  an embedded solid-white shader, renders it into a dedicated **offscreen**
  256x256 image (same discipline as 01_triangle: never an unacquired
  swapchain image), and reads it back. Then compiles a second embedded
  solid-black shader, swaps the pipeline, renders again, and asserts the
  readback actually changed — proving a reload really rebuilds what gets
  rendered, not just that compilation succeeds. Registered as the
  `sample_02_hotreload_headless` ctest case.
- **`--present`** — opens a real window and renders `hotreload.slang` (a
  file on disk, deployed next to the binary at build time — see
  `CMakeLists.txt`). Every ~250ms (~4Hz, a plain `std::filesystem::
  last_write_time` poll — no file-watcher dependency) it checks whether
  that file changed on disk; on a change it recompiles via
  `Compiler::compileFromFile`, and on success swaps in a freshly built
  pipeline. **On a compile failure, the last-good pipeline keeps
  rendering** — Slang's diagnostics are logged to the console (the exact
  error, with source context) and the window never goes blank or crashes
  because of a shader typo. Not part of `ctest` (interactive by nature) —
  see `MANUAL_VERIFICATION.md` for the per-platform manual check.

### Editing the shader live

With `--present` running, open the `hotreload.slang` sitting next to the
running binary (**not** `samples/02_hotreload/hotreload.slang` in the
source tree — that copy is only the template deployed at build time) in
any editor and change `fsMain`'s body, e.g.:

```hlsl
[shader("fragment")]
float4 fsMain(float4 fragCoord : SV_Position) : SV_Target
{
    uint cx = uint(fragCoord.x) / 32;
    uint cy = uint(fragCoord.y) / 32;
    float c = ((cx + cy) % 2 == 0) ? 1.0 : 0.0;
    return float4(c, c, 0.0, 1.0);   // yellow/black checkerboard
}
```

Save the file — the window updates within about 250ms, with a console log
line confirming the swap (`reload succeeded, pipeline swapped`). Introduce
a typo instead and the console logs Slang's diagnostics while the window
keeps showing whatever last compiled successfully; fix the typo and save
again to recover. `gPush.time` (seconds since the window opened) is
available to any edit that wants to animate; the shipped default uses it to
drive a moving diagonal stripe pattern.

Pipeline swap strategy: this sample's coordinator sign-off allows a plain
`vkDeviceWaitIdle()` immediately before destroying the old pipeline on
every reload, rather than a fence-keyed deferred-destruction queue (not yet
part of this codebase at the time this sample was written) — acceptable
because reloads are a rare, human-interactive event, not a per-frame
operation, so the stall is invisible in practice.

### Expected output

**Headless mode**:

```
[info] hotreload headless gate PASSED
```

**`--present` mode** opens an 800x600 window titled `rx_hotreload_sample
(--present)` showing an animated diagonal stripe pattern (orange/blue,
shifting continuously via the `time` push constant) covering the whole
window. Editing and saving `hotreload.slang` (see above) changes the
pattern within ~250ms. Closing the window exits with status 0 and logs:

```
[info] --present: window closed cleanly
```

### Redistribution

This is the sample that proves the Task 1 Slang-runtime-lib deployment
mechanism (`rx_shader_deploy_runtime_libs()`) end to end: linking
`rx_shader` pulls in `slang::slang`, and this binary does real in-process
compilation on every single run (both modes) — if the runtime libraries
(`libslang-compiler.so*`/`slang-compiler.dll` + the `slang-glslang`/
`slang-glsl-module`/`slang-rt` plugins) weren't copied next to the
executable at build time with a working RPATH, `Compiler::create()` would
fail immediately. A redistributed copy of just this sample's build-output
directory (the binary + `hotreload.slang` + those runtime libraries) runs
identically outside the build tree.

## 03_bindless_mesh

`samples/03_bindless_mesh/main.cpp` — the Phase 2 integration proof:
reflection-driven pipeline layouts, bindless resource management, and the
real upload path, all working together in one scene. Procedural geometry
(a cube, a UV sphere, and a plane — generated in code, no mesh importer)
textured with 4 procedurally generated images (checkerboards/gradients)
plus one real PNG decoded via `stb_image`, every texture uploaded through
`rx::rhi::Uploader` into one `rx::rhi::BindlessTable` (set 0). Just like
02_hotreload, the shader is compiled **at runtime** (`rx::shader::Compiler`)
so `rx::shader::reflect()` walks the actual shader driving this sample's
pipeline layout — there is no hand-typed `VkDescriptorSetLayoutBinding`
anywhere in this file for set 0.

**Set-0 substitution**: `rx::rhi::BindlessTable`'s real descriptor set
layout (sized from caller capacities, `VARIABLE_DESCRIPTOR_COUNT` on its
last binding) and a from-scratch layout `PipelineLayoutBuilder` would
otherwise build from the same reflected shape are NOT compatible Vulkan
pipeline layouts "by construction" — this sample is what exercises the fix
added this task: `PipelineLayoutBuilder::build()`'s new `externalSet0`
parameter, passed `bindlessTable.descriptorSetLayout()` directly, reuses
the table's real layout for set 0 instead of building a lookalike.

Per-draw push constants (12 bytes, well under the 128-byte budget):
`{ transformIndex, textureIndex, samplerIndex }` — all three uniform
across a draw call, never wrapped in `NonUniformResourceIndex()`. A depth
buffer (`rx::rhi::Texture2D`, `VK_FORMAT_D32_SFLOAT`, single mip level,
never uploaded to) enables correct depth testing across the 5 objects.

- **Headless (default, no flags)** — renders one frame offscreen at
  256x256 with the camera at a fixed, known pose, reads it back, and
  asserts at least 3 distinct texture colors landed on screen at known
  probe positions (in practice all 5 textures are distinct and probed).
  Registered as the `sample_03_bindless_mesh_headless` ctest case.
- **`--present`** — opens a real window showing the same 5 objects (2
  cubes, 2 spheres, 1 plane) with the camera continuously orbiting the
  scene. Survives window resizes (the depth buffer is recreated alongside
  the swapchain). Not part of `ctest` — see `MANUAL_VERIFICATION.md`.

### Expected output

**Headless mode**:

```
[info] 5 distinct texture colors found among 5 probes
[info] bindless mesh headless gate PASSED
```

**`--present` mode** opens a 900x700 window titled `rx_bindless_mesh_sample
(--present)` showing 5 objects in a row against a dark background: a
red/white checkerboard cube, a blue-to-green gradient cube, a yellow/blue
checkerboard sphere, a magenta-to-cyan gradient sphere, and a plane
textured with a real decoded PNG (an orange/teal concentric-ring pattern).
The camera orbits continuously; closing the window exits with status 0.

### Redistribution

Same mechanism as 02_hotreload: this sample compiles its shader at runtime
(`rx_shader_deploy_runtime_libs()` ships the Slang runtime libs next to the
binary), plus its own `texture.png` deployed next to the binary at build
time (`samples/03_bindless_mesh/CMakeLists.txt`) for the one real-PNG
texture. A redistributed copy of just this sample's build-output directory
(binary + `texture.png` + the Slang runtime libraries) runs identically
outside the build tree.

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

```sh
# Headless correctness gate (also runs via ctest, see below):
./build/linux-native/samples/02_hotreload/sample_02_hotreload

# Interactive present-mode window with live shader editing -- edit
# build/linux-native/samples/02_hotreload/hotreload.slang while this runs:
./build/linux-native/samples/02_hotreload/sample_02_hotreload --present
```

```sh
# Headless correctness gate (also runs via ctest, see below):
./build/linux-native/samples/03_bindless_mesh/sample_03_bindless_mesh

# Interactive present-mode window with an orbiting camera:
./build/linux-native/samples/03_bindless_mesh/sample_03_bindless_mesh --present
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

This produces `build/windows-cross-zig/samples/01_triangle/sample_01_triangle.exe`,
`build/windows-cross-zig/samples/02_hotreload/sample_02_hotreload.exe`, and
`build/windows-cross-zig/samples/03_bindless_mesh/sample_03_bindless_mesh.exe`.
01_triangle's binary is statically linked (no separate `.dll`s to ship) —
copy just that one `.exe` to the Windows machine (or run it directly under
Wine on Linux) and run it from a terminal/`cmd.exe`/PowerShell:

```
sample_01_triangle.exe            REM headless correctness gate
sample_01_triangle.exe --present  REM interactive present-mode window
```

02_hotreload is **not** self-contained the same way: it does real
in-process Slang compilation, so it needs `slang-compiler.dll` +
`slang-glslang.dll` + `slang-glsl-module.dll` + `slang-rt.dll` (all
deployed next to it at build time — see that sample's own README section
above) alongside the `.exe`, plus `hotreload.slang` for `--present` mode.
Copy the entire `build/windows-cross-zig/samples/02_hotreload/` directory
(minus the `CMakeFiles`/`.pdb`/`.cmake` build bookkeeping) to run it
elsewhere:

```
sample_02_hotreload.exe            REM headless correctness gate
sample_02_hotreload.exe --present  REM interactive present-mode window with live editing
```

03_bindless_mesh is the same shape as 02_hotreload (real in-process Slang
compilation, so it needs the same 4 Slang DLLs deployed next to it), plus
`texture.png` for its one real-PNG texture. Copy the entire
`build/windows-cross-zig/samples/03_bindless_mesh/` directory (minus the
`CMakeFiles`/`.pdb`/`.cmake` build bookkeeping) to run it elsewhere:

```
sample_03_bindless_mesh.exe            REM headless correctness gate
sample_03_bindless_mesh.exe --present  REM interactive present-mode window, orbiting camera
```

See `MANUAL_VERIFICATION.md` at the repo root for the actual per-platform
run checklist (what to record, what "pass" looks like on real hardware).

### Running the automated test suite

```sh
ctest --preset linux-native --output-on-failure
```

This runs `sample_01_triangle_headless`, `sample_02_hotreload_headless`, and
`sample_03_bindless_mesh_headless` alongside every other project test
(`rx_core_tests`, `rx_platform_tests`, `rx_shader_tests`, `rx_rhi_vk_tests`,
`shader_spirv_test`) — `--present` mode is intentionally excluded from
`ctest` (it blocks on a real window and user/OS interaction) and is
exercised manually instead.
