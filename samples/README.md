# Samples

Runnable demos that exercise `rx_rhi_vk` (and, beneath it, `rx_core`/
`rx_platform`) end to end against a real Vulkan driver. Each sample gets its
own subdirectory and its own `CMakeLists.txt`; all of them are built as part
of the normal `cmake --build --preset <preset>` flow (see the root
`CMakeLists.txt`'s `add_subdirectory(samples/...)` lines) and don't need any
separate build step.

## Downloading a prebuilt sample bundle (no build required)

Every green CI run (`.github/workflows/ci.yml`) uploads one workflow
artifact per platform — `rendererx-samples-linux-x86_64.zip` from the
`linux-native` job, `rendererx-samples-windows-x86_64.zip` from the
`windows-cross-zig` job — built by `tools/package_samples.sh`. Download
either from the run's "Artifacts" section on GitHub, unzip it, and each
sample is immediately runnable from its own subdirectory, no `cmake`/build
step involved:

```
01_triangle/
  sample_01_triangle[.exe]         # + triangle.vert.spv, triangle.frag.spv
02_hotreload/
  sample_02_hotreload[.exe]        # + hotreload.slang, the Slang runtime
                                    #   libs below, and LICENSE
03_bindless_mesh/
  sample_03_bindless_mesh[.exe]    # + texture.png, the Slang runtime libs
                                    #   below, and LICENSE
04_streaming/
  sample_04_streaming[.exe]        # + the Slang runtime libs below, and
                                    #   LICENSE (no other external asset)
05_multipass/
  sample_05_multipass[.exe]        # + shaders/multipass/*.slang (6 files),
                                    #   the Slang runtime libs below, and
                                    #   LICENSE
06_materials/
  sample_06_materials[.exe]        # + materials/checker.slang,
                                    #   materials/rim.slang, +
                                    #   material_shaders/{material,
                                    #   forward_entry}.slang (rx_material's
                                    #   own shared files) +
                                    #   material_shaders/{brdf,
                                    #   energy_compensation_off,
                                    #   energy_compensation_on}.slang
                                    #   (MaterialSystem::create() requires
                                    #   these unconditionally -- see this
                                    #   sample's own Redistribution section
                                    #   below), the Slang runtime libs
                                    #   below, and LICENSE
07_stress/
  sample_07_stress[.exe]           # + shaders/stress/*.slang (4 files), the
                                    #   Slang runtime libs below, and LICENSE
08_gltf_viewer/
  sample_08_gltf_viewer[.exe]      # + material_shaders/{material,
                                    #   forward_entry,standard_pbr,unlit,
                                    #   brdf,energy_compensation_off,
                                    #   energy_compensation_on}.slang,
                                    #   tonemap.{vert,frag}.slang,
                                    #   ibl_shaders/{equirect_to_cubemap,
                                    #   irradiance_convolve,prefilter_specular,
                                    #   dfg_lut,skybox}.slang,
                                    #   environments/gate_test_env.hdr,
                                    #   references/{loading_state,
                                    #   loaded_scene}.png, a pre-staged
                                    #   assets/DamagedHelmet/glTF/ (+ its own
                                    #   LICENSE.txt), the Slang runtime libs
                                    #   below, and LICENSE
09_scene/
  sample_09_scene[.exe]            # + material_shaders/{material,
                                    #   forward_entry,standard_pbr,unlit,
                                    #   brdf,energy_compensation_off,
                                    #   energy_compensation_on}.slang,
                                    #   shadow_shaders/shadow_caster.vert.slang,
                                    #   ibl_shaders/{equirect_to_cubemap,
                                    #   irradiance_convolve,prefilter_specular,
                                    #   dfg_lut}.slang (no skybox.slang --
                                    #   this sample renders no skybox pass),
                                    #   environments/gate_test_env.hdr,
                                    #   tonemap.{vert,frag}.slang,
                                    #   references/grid_scene.png, a
                                    #   pre-staged assets/DamagedHelmet/glTF/
                                    #   (+ its own LICENSE.txt), the Slang
                                    #   runtime libs below, and LICENSE
```

`01_triangle` is the one exception to "needs the Slang runtime libs": its
shaders are precompiled offline by `slangc` at build time, so it ships only
its two `.spv` files and nothing Slang-related at all [D2] — see its own
"Redistribution" section below. The other eight do real in-process Slang
compilation at startup, so each of their directories additionally carries
`libslang-compiler.so*`/`slang-compiler.dll` plus the `slang-glslang`/
`slang-glsl-module`/`slang-rt` plugins it dlopens on demand, and the Slang
`LICENSE` file (Apache-2.0 WITH LLVM-exception — kept for attribution when
redistributing those libraries). `cd` into any one subdirectory and run the
binary exactly as described in that sample's own section below — every
subdirectory in the zip is independently self-contained, so copying just
one elsewhere works too.

## 01_triangle

`samples/01_triangle/main.cpp` — renders one hardcoded white triangle on a
black background via a Vulkan 1.3 dynamic-rendering pipeline (no vertex
buffers: the vertex shader generates its 3 NDC positions from
`SV_VertexID`). Two modes, selected by a command-line flag, sharing the
exact same pipeline-construction code and draw call:

- **Headless (default, no flags)** — the correctness gate this repo's CI and
  `ctest` run. Builds the full stack (window, instance, surface, device,
  real swapchain — created and queried, but deliberately never written to),
  renders into a dedicated **offscreen** 256x256 image, reads the result
  back to the host, and asserts the center pixel is white and a corner pixel
  is black. Exits 0 on pass, 1 on failure. Validation is OFF by default (see
  `--validate` below) — an end-user running this sample as-is needs no
  Vulkan SDK / validation layers installed. `ctest` registers this case as
  `sample_01_triangle_headless` with `--validate` passed explicitly, so the
  automated gate additionally fails if the Vulkan validation layer reported
  anything beyond this project's two documented, narrowly-matched
  false-positive guards (see `context.cpp`).
- **`--validate`** — a developer/CI flag, not needed for a normal run: turns
  on the Vulkan validation layers and the debug messenger that reports
  through them. Requires the Vulkan SDK (or an equivalent
  `VK_LAYER_KHRONOS_validation` install) to actually be present on the
  machine — without it, `Context::create()` simply builds an unvalidated
  instance (vk-bootstrap's `request_validation_layers()` is only called when
  this flag is set), so nothing breaks, but no validation checking happens
  either. Combine with headless (the default) or `--present`. This is what
  every `ctest` registration below passes.
- **`--present`** — opens a real, visible, resizable window and renders the
  same triangle into the actual swapchain images every frame, via the
  canonical frames-in-flight present loop (`rx::rhi::FrameSync`:
  `src/rx_rhi_vk/include/rx_rhi_vk/frame_sync.h`). This is the first and
  only place in this codebase that writes to a swapchain image — and only
  ever the image `vkAcquireNextImageKHR` actually returned. Runs until the
  window is closed (or the process receives `SIGINT`/`SIGTERM`, which SDL
  translates into a clean shutdown); survives being resized at any point,
  any number of times. Add `--validate` to also confirm zero validation
  errors across those resizes (what `MANUAL_VERIFICATION.md`'s pre-release
  checklist does). Not part of `ctest` — it's interactive by nature. See
  `MANUAL_VERIFICATION.md` at the repo root for the per-platform manual
  check this mode gets before a release.
- **`--vsync on|off`** (default `on`) — present-mode control
  [`src/rx_rhi_vk/include/rx_rhi_vk/device.h`'s `Device::setPresentMode()`].
  **Behavioral change from earlier phases:** `Device::create()` used to
  hand swapchain creation to vk-bootstrap with no present mode requested at
  all, which meant an *implicit* MAILBOX-if-the-surface-supports-it,
  else-FIFO preference — nobody in this codebase had actually chosen that,
  it just fell out of vk-bootstrap's own default. Every `Device` now
  requests an *explicit* default of FIFO (`on`, the default here) instead,
  so a plain `--present` run with no `--vsync` flag behaves the same on
  every machine regardless of what the surface happens to support. Pass
  `--vsync off` to opt into the uncapped-framerate ladder instead: MAILBOX
  if the surface supports it, else IMMEDIATE, else FIFO again (with a
  one-line warning logged, since that silently keeps vsync effectively on
  despite asking for it off) — whichever this machine's driver/surface
  combination actually supports. The present mode actually settled on is
  logged once at startup in `--present` mode (see below); only `--present`
  mode has anything to apply it to — a plain headless run parses `--vsync`
  like any other flag but has no visible swapchain for it to affect, so it
  is silently ignored there.
- **`--fullscreen`** — enters borderless-desktop fullscreen at startup
  (`rx::platform::Window::setFullscreen()`: `SDL_SetWindowFullscreen(window,
  true)` with no exclusive display mode set — SDL3's own documented meaning
  of "borderless fullscreen desktop", not `VK_EXT_full_screen_exclusive`,
  which this codebase does not use). In this sample (and 02-07), it's
  applied immediately after the window is created but before the first
  swapchain is built, so it costs no extra recreation; 08_gltf_viewer's
  window/device setup is bundled into one helper, so it applies this the
  same way `--vsync off` already does there — one explicit
  `recreateSwapchain()` call right after. There is no runtime hotkey to
  toggle it back off in this sample — close the window (or resize/
  un-fullscreen via the OS's own window controls) to exit fullscreen. Only
  `--present` mode has a window for this to affect; a headless run parses
  it like any other flag but ignores it, same as `--vsync` above. Related:
  minimizing the window while `--present` is running (any sample) is also
  handled — see "Zero-extent/minimize handling" below.

### Zero-extent/minimize handling

Every sample's `--present` mode tolerates the window being minimized (or
resized down to 0×0, which amounts to the same thing at the Vulkan level):
`rx::rhi::Device::recreateSwapchain()` queries the surface's real extent
before rebuilding anything, and — instead of handing a 0×0 size to
`vkb::SwapchainBuilder` (a validation error, `VUID-VkSwapchainCreateInfoKHR-
imageExtent-01689`) — enters a suspended-present state: no swapchain is
built, and `acquireNextImage()`/`present()` return `SwapchainStatus::
Suspended` without issuing the underlying Vulkan calls at all. Every
sample's present loop checks for this status and skips rendering that frame
(polling for a real extent to resume on) instead of crashing. This is
driven entirely by the queried surface extent, not by SDL's minimize
event/flag — on Wayland, SDL3 cannot reliably detect a real,
compositor-driven minimize via either of those (see
`rx::platform::logWaylandMinimizeLimitationOnce()`'s own comment), so the
extent query is what actually keeps this safe there too.

### Expected output

**Headless mode** prints one of these lines to the log and exits accordingly
— there is no visible window (it's created hidden):

```
[info] triangle readback PASSED
```

**`--present` mode** logs the present mode actually in use once at startup
(FIFO unless `--vsync off` resolved to something else — see above):

```
[info] --present: present mode in use: FIFO
```

It then opens an 800x600 window titled `rx_triangle_sample
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

### Redistribution

Unlike every other sample here, 01_triangle's shaders (`triangle.vert.slang`/
`triangle.frag.slang`) are compiled to SPIR-V **offline**, by `slangc`, at
build time (`CMakeLists.txt`'s `add_custom_command`s) — this binary never
calls into Slang at runtime, so it needs **no Slang runtime libraries at
all** [D2]. It does still need the two resulting `.spv` files on disk
somewhere it can find them at run time: `CMakeLists.txt` deploys
`triangle.vert.spv`/`triangle.frag.spv` next to the binary at build time
(the same pattern 02_hotreload uses for `hotreload.slang`), and `main.cpp`'s
`resolveSpvPath()` looks for them next to the executable (via
`SDL_GetBasePath()`) before falling back to the compile-time build-tree
path. A redistributed copy of just this sample's three files (the binary +
the two `.spv` files) runs identically outside the build tree — no Slang
DLLs/`.so`s, no LICENSE file, nothing else needed.

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
  rendered, not just that compilation succeeds. Validation is off by
  default (see 01_triangle's `--validate` section above for what that flag
  does and why); `ctest` registers this case as
  `sample_02_hotreload_headless` with `--validate` passed explicitly.
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
- **`--vsync on|off`** (default `on`) — same present-mode control as
  01_triangle's `--vsync` section above; only `--present` mode has a
  swapchain for it to affect.
- **`--fullscreen`** — same borderless-desktop fullscreen toggle as
  01_triangle's `--fullscreen` section above; only `--present` mode has a
  window for it to affect. Minimize handling during `--present` is the same
  extent-query-driven guard described in 01_triangle's "Zero-extent/minimize
  handling" section.

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

**`--present` mode** logs the present mode actually in use once at startup
(same `[info] --present: present mode in use: ...` line as 01_triangle
above), then opens an 800x600 window titled `rx_hotreload_sample
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
  Validation is off by default (see 01_triangle's `--validate` section
  above); `ctest` registers this case as `sample_03_bindless_mesh_headless`
  with `--validate` passed explicitly.
- **`--present`** — opens a real window showing the same 5 objects (2
  cubes, 2 spheres, 1 plane) with the camera continuously orbiting the
  scene. Survives window resizes (the depth buffer is recreated alongside
  the swapchain). Not part of `ctest` — see `MANUAL_VERIFICATION.md`.
- **`--vsync on|off`** (default `on`) — same present-mode control as
  01_triangle's `--vsync` section above; only `--present` mode has a
  swapchain for it to affect.
- **`--fullscreen`** — same borderless-desktop fullscreen toggle as
  01_triangle's `--fullscreen` section above; only `--present` mode has a
  window for it to affect. Minimize handling during `--present` is the same
  extent-query-driven guard described in 01_triangle's "Zero-extent/minimize
  handling" section.

### Expected output

**Headless mode**:

```
[info] 5 distinct texture colors found among 5 probes
[info] bindless mesh headless gate PASSED
```

**`--present` mode** logs the present mode actually in use once at startup
(same `[info] --present: present mode in use: ...` line as 01_triangle
above), then opens a 900x700 window titled `rx_bindless_mesh_sample
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

## 04_streaming

`samples/04_streaming/main.cpp` — the sample most likely to reveal real
synchronization bugs: 24 procedurally generated textures (flat HSV-wheel
hues, computed at runtime — not loaded from any file) compete for a
resident budget of only 8 bindless slots. Every few frames (headless) or
roughly once a second (`--present`) the next logical texture streams in
and the oldest resident texture is evicted. `rx::rhi::BindlessTable`'s
free list is LIFO, so releasing the victim's handle and then registering
the incoming texture deterministically reuses that exact same physical
descriptor slot — which means BOTH halves of the swap need protecting,
not just the destruction: rewriting a descriptor slot via
`registerSampledImage()` is only legal (per Vulkan's update-after-bind
rules) when that slot is not dynamically used by any still-pending
command buffer, so this sample defers the incoming texture's
registration itself — not merely the victim's `rx::rhi::Texture2D`
destruction — into the same `rx::rhi::DeletionQueue`-retired callback,
tagged with the current frame number. That callback only runs once that
frame's fence is confirmed signaled, which (given this codebase's
single-queue, submission-ordered execution) also guarantees every
earlier frame that might still have been dynamically sampling the old
descriptor contents in that slot has completed too — so the incoming
texture's grid cell only becomes resident (and drawable) 2 frames after
the eviction decision, not immediately. Getting that timing exactly
right for BOTH the destroy and the rewrite is this sample's whole point
— see `main.cpp`'s header comment ("FRAME-LAG SAFETY ARGUMENT" and
"DESCRIPTOR REWRITE SAFETY") for the full argument, and
`rx_rhi_vk/bindless.h`'s RELEASE-SAFETY CONTRACT for the underlying spec
rule.

24 fixed grid cells (one per logical texture, not one per physical slot)
sit in a 6x4 grid under a static orthographic camera; a cell is drawn only
while its texture is currently resident — a non-resident cell is skipped
entirely in the draw loop, which is exactly what
`VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` (set on `BindlessTable`'s
bindings) makes valid: a bound descriptor set is allowed to contain slots
no draw call this frame ever indexes. The shader's set 0 only uses 2 of
`BindlessTable`'s 3 fixed bindings (images + samplers — no storage
buffers; this sample's per-object transform is a push-constant `mvp`
instead, since the grid and camera never move), which
`PipelineLayoutBuilder`'s external-set-0 substitution explicitly supports
("a shader is free to use a strict subset of the three slots").

- **Headless (default, no flags)** — the `ctest` correctness gate. Runs 60
  real frames through a genuine 2-frames-in-flight offscreen loop (2
  dedicated offscreen images, not 1 — real CPU/GPU overlap between
  adjacent frames is exercised on purpose, not serialized away), with
  deferred readback probes at every grid cell's screen position on every
  frame. Asserts every one of the 24 logical textures was actually
  observed resident on screen at some point (not just "registered" —
  the probe reads real rendered pixels). Validation is off by default (see
  01_triangle's `--validate` section above); `ctest` registers this case as
  `sample_04_streaming_headless` with `--validate` passed explicitly, which
  adds the zero-validation-errors assertion that would catch a
  premature-destroy bug.
- **`--present`** — opens a real window showing the same 24-cell grid,
  static camera, streaming continuing indefinitely at roughly 1
  texture/second. Not part of `ctest` — see `MANUAL_VERIFICATION.md`.
- **`--vsync on|off`** (default `on`) — same present-mode control as
  01_triangle's `--vsync` section above; only `--present` mode has a
  swapchain for it to affect.
- **`--fullscreen`** — same borderless-desktop fullscreen toggle as
  01_triangle's `--fullscreen` section above; only `--present` mode has a
  window for it to affect. Minimize handling during `--present` is the same
  extent-query-driven guard described in 01_triangle's "Zero-extent/minimize
  handling" section.

### Expected output

**Headless mode**:

```
[info] sample_04_streaming: 24 / 24 logical textures observed resident at some point
[info] streaming headless gate PASSED
```

**`--present` mode** logs the present mode actually in use once at startup
(same `[info] --present: present mode in use: ...` line as 01_triangle
above), then opens a 900x700 window titled `rx_streaming_sample
(--present)` showing up to 8 flat-colored squares arranged across a 6x4
grid against a dark background, smoothly cycling through the HSV color
wheel as textures stream in and evict roughly once a second — cells light
up left-to-right, top-to-bottom as their logical texture becomes resident,
and go dark again once evicted. Closing the window exits with status 0.

### Redistribution

Same mechanism as 02_hotreload/03_bindless_mesh: this sample compiles its
shader at runtime (`rx_shader_deploy_runtime_libs()` ships the Slang
runtime libs next to the binary). Unlike 03_bindless_mesh, there is no
external asset file at all — every texture is procedurally generated in
code — so a redistributed copy of just this sample's binary plus the Slang
runtime libraries runs identically outside the build tree.

## 05_multipass

`samples/05_multipass/main.cpp` — the Phase 3 acceptance test made visible:
a real, if small, multipass pipeline (shadow map → forward Lambert+shadow
shading → Reinhard tonemap) driven entirely by `rx_graph`'s
`RenderGraph`/`Executor`, not hand-rolled `vkCmdBeginRendering`/barrier
calls. Every layout transition and sync2 barrier between the three passes
is derived by `RenderGraph::compile()` and applied by `Executor::execute()`
— this sample never calls `vkCmdPipelineBarrier2` or
`rx::rhi::transitionImage()` itself.

**Scene**: a ground plane (XZ, `y=0`) with one cube and one sphere sitting
on it, one fixed-elevation directional light. Objects only ever translate
(never rotate), which lets the shader skip a normal-matrix transform
entirely — object-space normals equal world-space normals under a
translation-only model matrix.

**Graph shape**: `shadow` (writes `"shadowmap"`: Absolute 1024x1024
D32_SFLOAT, depth-only — no fragment shader stage at all, since nothing
needs to be written but depth) → `forward` (reads `"shadowmap"` as a
texture input; writes `"hdr"`: SwapchainRelative R16G16B16A16_SFLOAT, and
its own `"depth"`: SwapchainRelative D32_SFLOAT) → `tonemap` (reads
`"hdr"`; writes the backbuffer). Shadow sampling is a manual single-tap
depth comparison in the fragment shader (no `VK_COMPARE_OP`-enabled
`VkSampler`) against a plain bindless `Texture2D`+`SamplerState`, exactly
the convention every other sample in this codebase uses for texture reads.

**Camera**: a fixed, top-down *orthographic* camera (not perspective) —
deliberate, so the headless gate's probe pixels below are analytically
derived from the same view/projection math the sample uses at render time,
mirroring `04_streaming`'s own `cellProbePixel()` derivation.

- **Headless (default, no flags)** — the `ctest` correctness gate. Fixed
  camera/light, 3 frames through the graph, readback, and three assertions:
  a ground-plane pixel inside the cube's analytically-derived shadow
  footprint is darker than an unshadowed ground pixel by more than 2x; the
  unshadowed pixel is non-trivially bright; every readback byte is in
  range (genuinely tonemapped). Validation is off by default (see
  01_triangle's `--validate` section above); `ctest` registers this case
  as `sample_05_multipass_headless` with `--validate` passed explicitly.
- **`--present`** — opens a real window showing the same scene, with the
  light continuously orbiting in azimuth (the camera itself never moves).
  Not part of `ctest` — see `MANUAL_VERIFICATION.md`.
- **`--vsync on|off`** (default `on`) — same present-mode control as
  01_triangle's `--vsync` section above; only `--present` mode has a
  swapchain for it to affect.
- **`--fullscreen`** — same borderless-desktop fullscreen toggle as
  01_triangle's `--fullscreen` section above; only `--present` mode has a
  window for it to affect. Minimize handling during `--present` is the same
  extent-query-driven guard described in 01_triangle's "Zero-extent/minimize
  handling" section.

### Expected output

**Headless mode**:

```
[info] shadow probe world=(0.0,2.0) pixel=(128,167) channels=(...) brightness_sum=...
[info] lit probe world=(-4.0,-4.0) pixel=(49,49) channels=(...) brightness_sum=...
[info] multipass headless gate PASSED
```

**`--present` mode** logs the present mode actually in use once at startup
(same `[info] --present: present mode in use: ...` line as 01_triangle
above), then opens a 900x700 window titled `rx_multipass_sample
(--present)` showing a reddish cube and a bluish sphere on a grayish floor,
lit from a fixed-elevation directional light whose azimuth continuously
orbits — the cube's shadow visibly sweeps across the floor as the light
turns. Closing the window exits with status 0.

### Redistribution

Same mechanism as 02_hotreload/03_bindless_mesh/04_streaming: this sample
compiles its shaders at runtime, so it needs the Slang runtime libs
deployed next to it (`rx_shader_deploy_runtime_libs()`). Unlike any earlier
sample, it ships **six** on-disk shader sources instead of one
(`scene_types.slang`, `shadow.vert.slang`, `lit.vert.slang`,
`lit.frag.slang`, `tonemap.vert.slang`, `tonemap.frag.slang`) — one file
per shader stage, plus `scene_types.slang` (the single source of truth for
the `ObjectTransform` bindless-row layout both `shadow.vert.slang` and
`lit.vert.slang` read), concatenated in the right order at compile time so
`rx::shader::reflect()` sees one linked program per pass; see
`scene_types.slang`'s own header comment for why this is done via textual
concatenation rather than a real Slang `import`/`__include`. A
redistributed copy of just this sample's build-output directory (binary +
those six `.slang` files + the Slang runtime libraries) runs identically
outside the build tree.

## 06_materials

`samples/06_materials/main.cpp` — the Phase 3 showcase for `rx_material`'s
**public** COM-lite API (`rx_api.h`): every material creation, instance
creation, per-instance parameter override, texture creation, and hot-reload
call in this sample goes through `IRxMaterialSystem`/`IRxMaterial`/
`IRxMaterialInstance`/`IRxTexture` — never an internal `rx::material::` type,
except inside one clearly marked bridge section in `main.cpp` (system
creation, and the draw-time `bindInstance()`/`getPipeline()`/
`pipelineLayout()` calls, which have no public equivalent in Phase 3 by
design — draw submission itself is not part of the ABI surface this phase).

**Scene**: 4 procedural objects (2 cubes, 2 spheres — generators adapted from
03_bindless_mesh's own position+uv convention, extended with a per-vertex
normal like 05_multipass's) drawn through a single `rx_graph` forward pass.
Two materials, `materials/checker.slang` (a procedural UV checkerboard times
a `tint` parameter) and `materials/rim.slang` (a view-dependent rim-light
term times a `rimColor` parameter), each instanced TWICE with different
parameter values — cube A/B get `checker` (warm orange / cool teal), sphere
A/B get `rim` (magenta / golden yellow). `checker.slang` also takes a
bindless texture-index parameter, bound to a real, procedurally generated
checker texture created through the public `IRxMaterialSystem::
createTexture2D()` and `IRxMaterialInstance::setTexture()` — exercising that
public path end to end (see that file's own header comment for why its
content never actually reaches the rendered pixels yet — Phase 3 materials
cannot sample a bindless texture from inside their own shading function).

**No camera transform in the shared material pipeline**: `shaders/material/
forward_entry.slang`'s shared vertex stage has no model/view/projection
transform at all — clip position and world position are the literal same
vertex attribute. This sample resolves that gap by pre-transforming every
vertex's position into clip space and every vertex's normal into view space
**on the CPU**, using its own camera matrices, before uploading — possible
without precision loss specifically because the camera is *orthographic*
(its projection matrix's `w` is always exactly `1.0`, matching
`forward_entry.slang`'s own hardcoded assumption). See `main.cpp`'s own
header comment for the full mechanism and the exact analytic pixel
derivations this makes possible.

- **Headless (default, no flags)** — the `ctest` correctness gate. Fixed
  orthographic camera, 3 frames through the graph, readback, and 4 analytic
  pixel assertions (one per object, channel-order-exact, accounting for
  this project's known SRGB-swapchain-encoding caveat — see
  `samples/04_streaming`'s own finding). Validation is off by default (see
  01_triangle's `--validate` section above); `ctest` registers this case as
  `sample_06_materials_headless` with `--validate` passed explicitly.
- **`--present`** — opens a real window showing the same 4 objects with the
  camera continuously orbiting (azimuth-only), polling `materials/
  checker.slang` and `materials/rim.slang` for on-disk changes roughly once
  a second and calling the public `IRxMaterialSystem::reloadChanged()` —
  02_hotreload's own keep-last-good UX, applied here to materials. Not part
  of `ctest` — see `MANUAL_VERIFICATION.md`.
- **`--vsync on|off`** (default `on`) — same present-mode control as
  01_triangle's `--vsync` section above; only `--present` mode has a
  swapchain for it to affect.
- **`--fullscreen`** — same borderless-desktop fullscreen toggle as
  01_triangle's `--fullscreen` section above; only `--present` mode has a
  window for it to affect. Minimize handling during `--present` is the same
  extent-query-driven guard described in 01_triangle's "Zero-extent/minimize
  handling" section.

### Expected output

**Headless mode**:

```
[info] object 0 (checker) world=(-2.0,1.3,0.0) pixel=(50,77) channels=(...) expected_linear=(255,140,38) matched=true
[info] object 1 (checker) world=(2.0,1.3,0.0) pixel=(205,77) channels=(...) expected_linear=(38,217,230) matched=true
[info] object 2 (rim) world=(-2.0,-1.3,0.0) pixel=(50,178) channels=(...) expected_linear=(152,50,139) matched=true
[info] object 3 (rim) world=(2.0,-1.3,0.0) pixel=(205,178) channels=(...) expected_linear=(152,139,43) matched=true
[info] materials headless gate PASSED
```

**`--present` mode** logs the present mode actually in use once at startup
(same `[info] --present: present mode in use: ...` line as 01_triangle
above), then opens a 900x700 window titled `rx_materials_sample
(--present)` showing an orange checkerboard cube (top-left), a teal
checkerboard cube (top-right), a magenta rim-lit sphere (bottom-left), and a
golden-yellow rim-lit sphere (bottom-right), with the camera orbiting
continuously around the vertical axis. Editing and saving either
`checker.slang` or `rim.slang` (the copies deployed next to the running
binary, not the ones in the source tree) changes that material's rendering
within about a second, with a console log line confirming the reload
(`hot-reload of '...' succeeded`); a syntactically broken edit keeps the
last-good material rendering instead of crashing. Closing the window exits
with status 0 and logs:

```
[info] --present: window closed cleanly
```

### Editing a material live

With `--present` running, open the deployed `materials/checker.slang` or
`materials/rim.slang` (next to the running binary) in any editor and change
its `evaluate()` body — e.g. checker.slang's cell count:

```hlsl
const float kCellsPerAxis = 6.0; // was 3.0 -- a finer checkerboard
```

Save the file — the affected cubes/spheres update within about a second.
This sample's hot-reload support covers edits that change a material's
*math*, matching `test_material_system.cpp`'s own reload-fixture convention
— NOT edits that add/remove/retype a `gParams` field (that changes the
parameter block's byte layout out from under this sample's already-created
`IRxMaterialInstance` objects); `recordDraws()` in `main.cpp` catches that
case defensively (logs it, skips that one object's draw for the frame)
rather than letting it crash the session.

### Redistribution

Same Slang-runtime-lib mechanism as every in-process-compiling sample, plus
a ledger item this sample resolves for `rx_material` itself: the two shared
files every material composes against (`shaders/material/material.slang`,
`shaders/material/forward_entry.slang`) used to be locatable only via
`RX_MATERIAL_SHADER_DIR`, a compile-time absolute path anchored at this
repository's own checkout — meaningless for a redistributed binary.
`MaterialSystem::create()` (`src/rx_material/include/rx_material/
material_system.h`) now takes an optional `sharedShaderDir` parameter;
`RX_MATERIAL_SHADER_DIR` stays the default for every other caller
(`rx_material`'s own unit/GPU tests), unchanged. This sample resolves a real
runtime directory (`SDL_GetBasePath()`-relative, exactly like
02_hotreload's own `resolveShaderPath()`) and passes it explicitly — so a
redistributed copy of this sample's build-output directory (binary +
`materials/` + `material_shaders/` + the Slang runtime libraries) runs
identically outside the build tree, with no reference to this repository's
checkout at all. Verified directly: this sample's packaged `.zip` output,
unzipped to a directory outside the build tree entirely, passes its own
headless gate unmodified on both `linux-native` and (via Wine)
`windows-cross-zig`.

**[Phase 5 Task 12, #48 fix]** `MaterialSystem::create()` has required
`brdf.slang`/`energy_compensation_{off,on}.slang` (Task 7/8's BRDF module +
its link-time permutation axis) in `sharedShaderDir` **unconditionally**
since Task 8 landed — this sample's own `checker.slang`/`rim.slang` never
reference them, but `create()` fails outright without them present
regardless. `tools/package_samples.sh` never staged the three files, so
every packaged zip built between Task 8 and this fix silently shipped a
redistributed `06_materials` whose `MaterialSystem::create()` failed before
ever rendering a frame — never caught because CI's own packaging step and
ctest run both only ever exercise the *build tree* (where CMake's own
deploy step already had them). Found and fixed by Task 12's own
standalone-unzipped-copy verification (see `08_gltf_viewer`'s own
Redistribution section below for the sibling gap that verification pass
actually set out to check).

## 07_stress

`samples/07_stress/main.cpp` — Phase 4 Stage 0's own exit sample and this
codebase's parallel-recording benchmark: `--draws N` (default 30000)
procedural instanced objects (4 mesh/pipeline-state variants, never real
GPU instancing — each is its own `vkCmdDrawIndexed` call, deliberately, so
CPU recording cost scales with draw count), laid out on a non-overlapping
XZ grid under a fixed orthographic top-down camera (no orbit — both the
static camera and the once-computed, never-re-uploaded instance data are
deliberate: recomputing either per frame would conflate that cost with the
one thing this sample measures), drawn through a CHUNKED forward pass
(`Pass::setExecuteChunked()`) + the same `shaders/multipass/tonemap.*`
shared shaders every other multi-pass sample reuses.

**`--threads N`** is the sample's own measurement instrument (not an
engine-wide switch — `docs/threading.md`): it overrides this sample's own
`Scheduler`'s worker count, which the executor's chunk-count derivation
self-scales to. `--threads 1` collapses the forward pass to one chunk
(the serial baseline); the default (no `--threads`) uses
`hardware_concurrency() - 1` workers (the parallel side of the same A/B
comparison). This is the mechanism `MANUAL_VERIFICATION.md`'s/CI's own
single-thread-vs-default-worker-count stress numbers come from.

- **Headless (default, no flags)** — the `ctest` correctness gate
  (`sample_07_stress_headless`, `--draws 16` — small on purpose, see this
  sample's own CMakeLists.txt comment): fixed 3 frames, exact
  draws-submitted/chunk-count/pool-allocation-budget counters, plus four
  dominance-style analytic pixel probes (one per mesh/pipeline variant),
  channel-order-exact against the real backbuffer format.
- **`--draws N`** (default 30000) — instance count; the same flag CI's own
  stress-numbers steps pass at 30000.
- **`--threads N`** — worker-count override, described above.
- **`--present`** — opens a real window rendering the same instanced
  field continuously, logging a live `stress: fps=... cpu_record_ms=...
  draws=...` line. Not part of `ctest` — see `MANUAL_VERIFICATION.md`.
- **`--vsync on|off`** / **`--fullscreen`** — same present-mode controls as
  every other sample above.

### Expected output

**Headless mode**:

```
[info] stress: frame=0 threads=1 cpu_record_ms=... draws=16 chunkCount=1 poolAllocations=...
[info] sample_07_stress: variant 0 probe world=(...) pixel=(...) ...
[info] stress headless gate PASSED
```

**`--present` mode** opens a window showing the full instanced field from
directly above, logging `stress: fps=... cpu_record_ms=... draws=...`
roughly once a second; closing it exits with status 0.

### Redistribution

Slang runtime libs + LICENSE, plus `shaders/stress/*.slang` (4 on-disk
sources: `instanced.{vert,frag}.slang`, `tonemap.{vert,frag}.slang`) —
no other external asset, since every mesh/instance transform is procedural
and uploaded by this sample itself, never loaded from disk.
`tools/package_samples.sh` stages exactly these; unaffected by the Task 12
packaging fixes above (this sample never links `MaterialSystem`/
`standard_pbr.slang` at all, so it was never exposed to the
`brdf.slang`/`energy_compensation_*` gap those fixes closed).

## 08_gltf_viewer

`samples/08_gltf_viewer/main.cpp` — the **Phase 5 Stage 1 demonstrator**
(originally Phase 4 Stage 1's async-import showcase; promoted at Stage 1's
own exit, Task 12/#48, to also exercise every Stage 1 engine facility): a
real glTF asset (DamagedHelmet by default, `--scene <path>` override),
imported **asynchronously** (Task 15's own async-import pipeline — this is
the sample built specifically to demonstrate it, with a rendered loading
state visible while the import runs) and rendered through D22's shipped
material library (`shaders/material/standard_pbr.slang` / `unlit.slang`,
now built on Task 7/#43's ported Filament BRDF module and Task 8/#44's
KHR_materials_ior/specular-consuming rework) driven entirely via D26.1's
bindless per-draw addressing (`SV_VulkanInstanceID` indexing a real bindless
`StructuredBuffer<RxDrawData>` this sample builds and uploads itself — never
a per-draw push constant; `samples/07_stress`'s own `gPush.instanceIndex` is
the named anti-pattern this does not repeat). An equirect environment is
baked (Task 9/#45's compute IBL chain: equirect→cubemap, irradiance,
prefiltered specular, multiscatter DFG LUT) and bound (Task 10/#46's
`rx::scene::Scene::setEnvironment()`) by default, replacing the old
Phase-4-era flat-ambient term with real image-based lighting plus a skybox
background pass — `--no-env` reproduces the old zero-indirect-light render
for comparison. A mouse-drag orbit camera (left-click-drag; reads SDL mouse
state directly via `SDL_GetMouseState()` — sample-local, not a new
`rx_platform` input surface), `--exposure` (a real EV100 value fed directly
into `rx::scene::Camera::setExposure(float)`, Task 4/#40's physical-units
API — higher EV100 *darkens* the image, the physical-camera convention,
e.g. `--exposure 5` is noticeably darker than the default; exposure
pre-multiplies the scene's own light/ambient/environment intensities before
shading runs, never a post-tonemap or post-shading multiplier — the shared
`shaders/multipass/tonemap.{vert,frag}.slang` shaders this sample reuses
verbatim are byte-for-byte untouched either way), and an ImGui HUD (`--present`
mode; Task 12/#48, built on the engine's own `rx_debug_ui::Overlay` facility
— the same one `09_scene`'s HUD consumes, not a sample-local reimplementation)
reporting the bound environment (path, physical intensity, prefiltered mip
count) and the live exposure state (aperture/shutter/ISO, EV100, resulting
pre-exposure multiplier) round out the interactive half.

**D28**: each glTF material's `alphaMode`/`doubleSided` become
`MaterialSystem`'s own fixed-function pipeline-state axis
(`MaterialFixedFunctionState`) — blend/depth-write/cull-mode fields on the
`VkPipeline` itself, not a specialization constant. Two glTF materials that
happen to share `standard_pbr.slang`'s own bytes but differ in
`alphaMode`/`doubleSided` are two independent `loadMaterial()` calls,
yielding two independently-cached `VkPipeline`s.

**No `MaterialSystem::bindInstance()`**: that method is the documented
pre-D26.1 legacy path (it always pushes `MaterialSystem`'s own default
identity draw-data row — the push constant itself carries no exposure
field at all since Task 4/#40's pre-exposure migration) — this sample
drives its own real per-scene draw-data buffer, with `--exposure` baked
into its own lightColor/ambientColor before upload, by hand
(`recordSceneDraws()` in `main.cpp`): resolve the pipeline
(`getPipeline()`, pre-resolved once per material at load time per D27, so
the very first real draw never stalls on a cold Slang-to-`VkPipeline`
compile), push a real `MaterialGlobalsPush`, bind set 1 against a
hand-built (but pipeline-layout-**compatible**, per the Vulkan spec's own
"identically defined" rule) descriptor-set layout.

**D17 headless correctness gate**: captures a loading-state frame (before
the async import can possibly have completed) and a loaded-scene frame
(once it reaches a terminal state), compares each against its own committed
256×256 lavapipe-rendered reference PNG (`references/loading_state.png` /
`loaded_scene.png`) via a tolerance gate (±4/255 per channel, <0.5%
failing-pixel budget) — enforced as a hard PASS/FAIL only when the running
device actually is lavapipe (`isLavapipeDevice()`); any other driver's own
divergence is logged as INFO only. `tools/regen_references.sh` (documented,
never auto-run) is the only sanctioned way to update the two committed
PNGs.

- **Headless (default, no flags)** — the `ctest` correctness gate
  (`sample_08_gltf_viewer_headless`, run with `--validate`): imports the
  default scene asynchronously, captures + gates both frames as described
  above.
- **`--quit-during-load`** — starts the async import, lets it run for a
  short bounded window (long enough for ≥1 real GPU resource — a texture or
  geometry upload — to plausibly have landed), cancels it, and tears down
  immediately with zero unfiltered validation errors — registered as
  `sample_08_gltf_viewer_quit_during_load`. The standing lesson this whole
  stage's review history keeps finding rollback bugs in: abandon/teardown
  paths get real-GPU-resource tests, never a mock.
- **`--present`** — opens a real window; left-click-drag orbits the camera.
  Not part of `ctest` — see `MANUAL_VERIFICATION.md`.
- **`--scene <path>`** — imports a different glTF/GLB file instead of
  DamagedHelmet.
- **`--exposure <n>`** — a real EV100 value fed into
  `Camera::setExposure(n)`, pre-multiplied onto the scene's light/ambient
  intensities before shading (Filament-style pre-exposure, not a
  post-tonemap multiplier). Higher EV100 *darkens* the image (the
  physical-camera convention: more EV100 means less exposure) — e.g.
  `--exposure 5` renders noticeably darker than the default. `0` — the
  default, and the only value that is a SENTINEL rather than a real
  EV100 — is neutral (no override applied at all, `exposure() == 1.0`).
- **`--env <path.hdr|.exr>`** — bakes and binds an equirectangular
  environment (Task 9's compute IBL chain + Task 10's runtime binding);
  container format is detected by magic number, not this flag's own file
  extension, so a Radiance `.hdr` or an OpenEXR `.exr` both work
  unmodified. Empty (the default) resolves to the committed
  `environments/gate_test_env.hdr` fixture.
- **`--no-env`** — explicitly binds no environment at all (not even the
  default fixture) — reproduces the pre-Task-10 zero-indirect-light render
  for comparison.
- **`--env-intensity <n>`** (default `1.0`) — a physical-units multiplier on
  the bound environment's own intensity, applied before `--exposure`'s own
  pre-exposure multiply.
- **`--bench-frames <n>`** — headless-only: after the scene loads and the
  environment bakes, times `n` repeated offscreen render+readback
  iterations and logs a `sample08: perf frame_bench ...` line (average/min/
  max wall-clock milliseconds per iteration — CPU-record + GPU-submit-and-
  wait time, not vsync-paced present-mode timing). `0` (the default)
  disables it entirely.
- **`--vsync on|off`** (default `on`) — same present-mode control as
  01_triangle's `--vsync` section above.
- **`--fullscreen`** — same borderless-desktop fullscreen toggle as
  01_triangle's `--fullscreen` section above. Minimize handling during
  `--present` is the same extent-query-driven guard described in
  01_triangle's "Zero-extent/minimize handling" section.

### Expected output

**Headless mode**:

```
[info] sample_08_gltf_viewer: D17 loading_state gate: failingPixels=0/65536 (0.0000%) pass=true
[info] sample_08_gltf_viewer: D17 loaded_scene gate: failingPixels=0/65536 (0.0000%) pass=true
[info] sample_08_gltf_viewer: headless gate PASSED
```

**`--present` mode** opens a 1280×720 window showing a distinct dark-navy
loading screen (never pure black) while DamagedHelmet imports
asynchronously, then transitions to the rendered helmet — a dark,
gunmetal-plated combat helmet with a gold-tinted visor, lit by a single
fixed key light plus real image-based lighting from the bound environment
(a procedural sky/ground gradient by default) and its own skybox
background. An ImGui HUD in the top-left reports the bound environment and
current exposure state (see this section's own `--exposure`/`--env`
descriptions above). Left-click-dragging orbits the camera around the
helmet. Closing the window exits with status 0 and logs:

```
[info] sample_08_gltf_viewer: window closed cleanly
```

### Redistribution

Same Slang-runtime-lib + `sharedShaderDir` mechanism 06_materials
establishes, extended with a second shared pair
(`material_shaders/standard_pbr.slang` / `unlit.slang`, D22's shipped
library) alongside the same `material.slang` / `forward_entry.slang`, plus
the shared tonemap shaders (`tonemap.{vert,frag}.slang`, deployed flat next
to the binary, same convention as 05_multipass/07_stress's own copies), the
two committed `references/` PNGs (so a redistributed binary's own
`--validate` headless gate is self-contained too), and — new at Task 12/#48,
Stage 1's exit — `ibl_shaders/` (rx_ibl's five bake-chain/skybox compute
and graphics shaders, `rx::ibl::bakeEnvironment()`'s own explicit
`shaderDir` lookup) plus `environments/gate_test_env.hdr` (the committed
default-environment fixture, `resolveDefaultEnvironmentPath()`'s own
packaged-first lookup). Both were a real, closed packaging gap before Task
12: without them, a redistributed zip's `--env` default resolution
silently degraded to "no environment bound" even though the Slang libs and
DamagedHelmet asset were both already staged correctly — verified fixed by
unzipping a freshly packaged `.zip` outside the build tree and confirming
`--validate` logs a real `sample08: perf ibl_bake ...` line (the bake
actually running, not silently skipped) before the headless gate passes.
**Unlike every other
sample in this list**, this one genuinely redistributes third-party
content: `tools/fetch_assets.sh`'s own DamagedHelmet download is pre-staged
into `assets/DamagedHelmet/glTF/` next to the binary (this sample's own
`resolveDefaultScenePath()` looks there first, falling back to this
repository's own `assets/fetched/` copy only for an unpackaged build-tree
run), together with that asset's own dual CC-BY-4.0/CC-BY-NC-4.0 attribution
notice (`assets/DamagedHelmet/LICENSE.txt` — see `tools/fetch_assets.sh`'s
own header comment for the verified license finding) **and the full legal
text of both licenses** (`assets/DamagedHelmet/CC-BY-4.0.txt` /
`CC-BY-NC-4.0.txt`, vendored verbatim from creativecommons.org into
`samples/08_gltf_viewer/licenses/` and copied in by `tools/
package_samples.sh` — the notice is a human-readable summary, not a
substitute for the actual grant). Verified directly: this sample's
packaged `.zip` output, unzipped to a directory outside the build tree
entirely, passes its own headless gate unmodified, and both license texts
extract byte-identical to their vendored source.

## 09_scene

`samples/09_scene/main.cpp` — the Phase 4 phase-exit sample: the first
real production consumer of `rx::scene::Scene`/`DrawListBuilder`/
`rx_debug_ui::Overlay`/the Task 20 platform input surface outside their
own test suites. Default scene: a 4x4 instanced DamagedHelmet grid (one
real glTF import, 16 `Scene::createRenderable()` calls, genuine GPU-side
instancing — `instanceCount > 1` in a single `vkCmdDrawIndexed`, the first
consumer of that path in this codebase). Frustum + shadow-caster culling
and instancing collapse run for real every frame; a fitted-ortho shadow
map (`rx_shadow`) lights the grid. Since Task 10 (#46), the default
environment fixture is baked and bound the same way `08_gltf_viewer`'s
own default is (real IBL, no skybox pass here — `08` is the one that
renders a skybox).

**Fly-through camera** (`--present`): WASD + relative-mouse-mode look +
gamepad (left stick move, right stick look, triggers = speed multiplier) —
the Task 20 input surface's own first real consumer. **HUD** (ImGui, via
`rx_debug_ui::Overlay`): frame time/FPS, cull counters
(candidates/culled/visible/collapse ratio), a vsync checkbox, per-row
layer-mask visibility toggles + a light-channel demo toggle, `GeometryPool`
pool stats, and the D24 per-category memory report — the established HUD
`08_gltf_viewer`'s own environment/exposure readout (Task 12, #48) now
follows the same `rx_debug_ui::Overlay` pattern.

- **Headless (default, no flags)** — the `ctest` correctness gate
  (`sample_09_scene_headless`): exact cull-counter assertions (matrix row
  24) against the deterministic layer-mask split described in this file's
  own header comment, plus a D17 tolerance gate against the committed
  `references/grid_scene.png`, plus a shadow-on-vs-forced-off
  discrimination re-proof.
- **`--stress` / `--stress-draws N`** (`sample_09_scene_stress_headless`,
  64 draws) — STRESS-V2: a self-contained, Registry-free procedural
  instanced field (4 Unlit material variants) through the same
  Scene/DrawListBuilder/chunked-executor path, directly A/B-comparable to
  `07_stress`'s own `cpu_record_ms` numbers (same metric, same
  methodology) — CI's own stress-numbers steps run this at 30000 draws,
  single-thread and default-worker-count.
- **`--scene sponza`** — present-mode-only override to the fetched Sponza
  asset (`tools/fetch_assets.sh --sponza`; never CI-fetched).
- **`--threads N`** / **`--vsync on|off`** / **`--fullscreen`** — same
  conventions as `07_stress`/every earlier sample.
- **`--present`** — opens a real window; Esc toggles mouse-capture
  release/recapture, F11 toggles fullscreen. Not part of `ctest` — see
  `MANUAL_VERIFICATION.md`.

### Expected output

**Headless mode**:

```
[info] sample_09_scene: D17 grid_scene gate: failingPixels=0/65536 (0.0000%) pass=true
[info] sample_09_scene: headless gate PASSED
```

**`--present` mode** opens a window showing the helmet grid (or Sponza,
under `--scene sponza`) under real IBL + shadowing, with the HUD panel
described above in the top-left. Closing the window exits with status 0
and logs `sample_09_scene: window closed cleanly`.

### Redistribution

`material_shaders/` (`material.slang`/`forward_entry.slang`/
`standard_pbr.slang`/`unlit.slang`/`brdf.slang`/
`energy_compensation_{off,on}.slang` — the last three closed by Task 12,
#48, the same gap `06_materials`'/`08_gltf_viewer`'s own Redistribution
sections above describe) + `shadow_shaders/shadow_caster.vert.slang` +
`ibl_shaders/`+`environments/gate_test_env.hdr` (also closed by Task 12 —
this sample's own environment binding silently degraded to zero indirect
lighting in every redistributed copy before that fix) + the shared
tonemap shaders + the committed `references/grid_scene.png` + a pre-staged
DamagedHelmet asset (same dual-license attribution as `08_gltf_viewer`'s
own copy) + Slang runtime libs + LICENSE. Sponza/Workshop are ALSO staged
temporarily (owner directive, pre-go-live only — see
`tools/package_samples.sh`'s own header comment; Workshop is
skip-if-absent, Sponza is not). Verified directly (Task 12, #48): this
sample's packaged `.zip` output, unzipped outside the build tree, passes
its own headless gate unmodified and bakes its environment from the
unzipped copy's own path (not a dev-tree fallback) on both `linux-native`
and, independently re-verified by this round's review, `windows-cross-zig`
under a live Wine execution.

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
# Headless, end-user default -- validation off, no Vulkan SDK required.
# ctest (see below) runs this same headless mode but adds --validate:
./build/linux-native/samples/01_triangle/sample_01_triangle

# Interactive present-mode window:
./build/linux-native/samples/01_triangle/sample_01_triangle --present

# Same, with vsync off -- MAILBOX/IMMEDIATE if the surface supports either,
# else FIFO again (see the --vsync section above):
./build/linux-native/samples/01_triangle/sample_01_triangle --present --vsync off
```

```sh
# Headless, end-user default -- validation off, no Vulkan SDK required.
# ctest (see below) runs this same headless mode but adds --validate:
./build/linux-native/samples/02_hotreload/sample_02_hotreload

# Interactive present-mode window with live shader editing -- edit
# build/linux-native/samples/02_hotreload/hotreload.slang while this runs:
./build/linux-native/samples/02_hotreload/sample_02_hotreload --present
```

```sh
# Headless, end-user default -- validation off, no Vulkan SDK required.
# ctest (see below) runs this same headless mode but adds --validate:
./build/linux-native/samples/03_bindless_mesh/sample_03_bindless_mesh

# Interactive present-mode window with an orbiting camera:
./build/linux-native/samples/03_bindless_mesh/sample_03_bindless_mesh --present
```

```sh
# Headless, end-user default -- validation off, no Vulkan SDK required.
# ctest (see below) runs this same headless mode but adds --validate:
./build/linux-native/samples/04_streaming/sample_04_streaming

# Interactive present-mode window, grid streaming ~1 texture/second:
./build/linux-native/samples/04_streaming/sample_04_streaming --present
```

```sh
# Headless, end-user default -- validation off, no Vulkan SDK required.
# ctest (see below) runs this same headless mode but adds --validate:
./build/linux-native/samples/05_multipass/sample_05_multipass

# Interactive present-mode window, light orbiting in azimuth:
./build/linux-native/samples/05_multipass/sample_05_multipass --present
```

```sh
# Headless, end-user default -- validation off, no Vulkan SDK required.
# ctest (see below) runs this same headless mode but adds --validate:
./build/linux-native/samples/06_materials/sample_06_materials

# Interactive present-mode window, camera orbiting, material hot-reload --
# edit build/linux-native/samples/06_materials/materials/checker.slang or
# rim.slang while this runs:
./build/linux-native/samples/06_materials/sample_06_materials --present
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
`build/windows-cross-zig/samples/02_hotreload/sample_02_hotreload.exe`,
`build/windows-cross-zig/samples/03_bindless_mesh/sample_03_bindless_mesh.exe`,
`build/windows-cross-zig/samples/04_streaming/sample_04_streaming.exe`,
`build/windows-cross-zig/samples/05_multipass/sample_05_multipass.exe`, and
`build/windows-cross-zig/samples/06_materials/sample_06_materials.exe`.
01_triangle's binary is statically linked and needs no Slang DLLs (its
shaders are precompiled offline by `slangc` at build time — see its own
"Redistribution" section above) — but it does still need the two `.spv`
files deployed next to it (`triangle.vert.spv`/`triangle.frag.spv`). Copy
`sample_01_triangle.exe` plus those two files (not the whole build-output
directory's `CMakeFiles`/`.pdb`/`.cmake` bookkeeping) to the Windows machine
(or run it directly under Wine on Linux) and run it from a
terminal/`cmd.exe`/PowerShell:

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

04_streaming is the same shape as 02_hotreload for redistribution purposes
(real in-process Slang compilation, so it needs the same 4 Slang DLLs
deployed next to it) but, unlike 02_hotreload/03_bindless_mesh, has no
other external asset at all -- every texture is procedurally generated in
code. Copy the entire `build/windows-cross-zig/samples/04_streaming/`
directory (minus the `CMakeFiles`/`.pdb`/`.cmake` build bookkeeping) to run
it elsewhere:

```
sample_04_streaming.exe            REM headless correctness gate
sample_04_streaming.exe --present  REM interactive present-mode window, cycling grid
```

05_multipass is the same shape as 02_hotreload for redistribution purposes
(real in-process Slang compilation, so it needs the same 4 Slang DLLs
deployed next to it), plus its own six on-disk shader sources
(`scene_types.slang`, `shadow.vert.slang`, `lit.vert.slang`,
`lit.frag.slang`, `tonemap.vert.slang`, `tonemap.frag.slang` — see this
sample's own README section above for why six files, not one). Copy the
entire `build/windows-cross-zig/samples/05_multipass/` directory (minus the
`CMakeFiles`/`.pdb`/`.cmake` build bookkeeping) to run it elsewhere:

```
sample_05_multipass.exe            REM headless correctness gate
sample_05_multipass.exe --present  REM interactive present-mode window, light orbiting
```

06_materials is the same shape as 02_hotreload for redistribution purposes
(real in-process Slang compilation, so it needs the same 4 Slang DLLs
deployed next to it), plus its own `materials/` subdirectory
(`checker.slang`, `rim.slang`) and a sibling `material_shaders/`
subdirectory carrying `rx_material`'s two shared files (`material.slang`,
`forward_entry.slang` — see this sample's own README section above for why
two separate subdirectories rather than one flat layout). Copy the entire
`build/windows-cross-zig/samples/06_materials/` directory (minus the
`CMakeFiles`/`.pdb`/`.cmake` build bookkeeping) to run it elsewhere:

```
sample_06_materials.exe            REM headless correctness gate
sample_06_materials.exe --present  REM interactive present-mode window, camera orbiting
```

See `MANUAL_VERIFICATION.md` at the repo root for the actual per-platform
run checklist (what to record, what "pass" looks like on real hardware).

### Running the automated test suite

```sh
ctest --preset linux-native --output-on-failure
```

This runs `sample_01_triangle_headless`, `sample_02_hotreload_headless`,
`sample_03_bindless_mesh_headless`, `sample_04_streaming_headless`,
`sample_05_multipass_headless`, `sample_06_materials_headless`,
`sample_07_stress_headless`, `sample_08_gltf_viewer_headless`, and
`sample_08_gltf_viewer_quit_during_load` alongside every other project test
(`rx_core_tests`, `rx_platform_tests`, `rx_shader_tests`, `rx_rhi_vk_tests`,
`rx_asset_tests`, `rx_asset_gltf_tests`, `rx_asset_gltf_gpu_tests`,
`rx_graph_tests`, `rx_graph_gpu_tests`, `rx_material_tests`,
`rx_material_gpu_tests`, `shader_spirv_test`) — `--present` mode is
intentionally excluded from `ctest` (it blocks on a real window and user/OS
interaction) and is exercised manually instead.

Each of the `*_headless`/`*_quit_during_load` `ctest` cases passes
`--validate` (see each
sample's own section above) — a developer/CI flag that turns on the Vulkan
validation layers, requiring the Vulkan SDK (or an equivalent
`VK_LAYER_KHRONOS_validation` install) to be present on the machine running
`ctest`. This is what keeps the zero-validation-error bar enforced in CI
and local development. It is deliberately **not** the default for the
plain sample binaries: an end user who downloads a released build (see
"Downloading a prebuilt sample bundle" above) and runs it directly gets
validation OFF, so a machine with no Vulkan SDK installed (the normal case
for an end user) never has the loader attempt a validation-layer manifest
lookup at all — headless mode just runs and exits 0/1 on its own
correctness assertions, with no Vulkan-loader diagnostics in the mix.
