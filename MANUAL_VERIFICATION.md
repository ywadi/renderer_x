# Manual verification checklist

Automated `ctest` only covers what can run unattended (see
`samples/README.md`): the headless triangle correctness gate, not the
interactive `--present` window. Before every release, `--present` gets a
real, human-observed run on each target platform — this file is that
checklist, plus the record of the most recent run on each platform.

The sample binary (`sample_01_triangle` on Linux, `sample_01_triangle.exe`
on Windows) is **statically linked** — there are no companion `.dll`/`.so`
files to ship. Deploying to another machine of the same OS only requires
copying that one binary; nothing else from the build tree is needed.

## What "pass" means, every platform

- The window opens and shows a solid white, upward-pointing triangle
  centered in the lower-middle of an otherwise solid black window (see
  `samples/README.md`'s "Expected output" section for the exact shape/
  placement).
- Resizing the window (drag an edge/corner, or maximize/restore) keeps the
  triangle centered and correctly proportioned at the new size, with no
  visible corruption, stretching, or a frozen/stale frame — at any point,
  any number of times.
- Closing the window (clicking its close button, or `Alt+F4`/equivalent)
  exits the process promptly, with no crash, hang, or leftover process.
- On Linux, run with `--validate` (see `samples/README.md`) so the Vulkan
  validation layers are actually active, and confirm `VK_LAYER_KHRONOS_
  validation` output during the run contains no `[error]`-level line beyond
  this codebase's two documented, narrowly-matched false-positive guards
  (`context.cpp`'s `isKnownPortabilityEnumerationLayerBug` /
  `isKnownUnrecognizedSlangSourceLanguageBug`) — run with
  `RX_LOG` output visible (the default; nothing needs enabling) and grep for
  `[error]` if in doubt. `--validate` requires the Vulkan SDK (or an
  equivalent `VK_LAYER_KHRONOS_validation` install) on the machine; without
  it validation is silently off, which is the normal end-user default (see
  `samples/README.md`) but not what this pre-release check wants. Windows
  builds don't carry the validation layer in this project's toolchain, so
  this check is Linux-only.

## Linux (native, `linux-native` preset)

- [x] Build: `cmake --preset linux-native && cmake --build --preset linux-native`
- [x] Run: `./build/linux-native/samples/01_triangle/sample_01_triangle --present`
- [x] Triangle renders correctly (screenshot-verified, see below)
- [x] Survives repeated resizes (scripted: 5 sequential resizes + a 15-step
      randomized resize soak, sizes from 300x200 to 1200x900) with zero
      unexpected validation errors
- [x] Closes cleanly (verified via `SIGTERM`, which SDL3 translates into a
      graceful `SDL_EVENT_QUIT` the present loop already handles — see
      `samples/01_triangle/main.cpp`; exit code 0 both times, log line
      `--present: window closed cleanly` present)
- [x] Headless mode still exits 0: `./build/linux-native/samples/01_triangle/sample_01_triangle`
      → `triangle readback PASSED`

**Last run:** 2026-08-10, this repository's development machine.
- Distro: Pop!_OS 22.04 LTS (Ubuntu 22.04/jammy base), kernel 6.16.3
- GPU: NVIDIA GeForce RTX 2080 (discrete; `vkb::PhysicalDeviceSelector`
  picked this over the machine's secondary `llvmpipe` software adapter)
- Driver: NVIDIA proprietary 580.82.07 (Vulkan 1.4.312 reported)
- Result: **PASS** — no unexpected validation output on any run.

**[Phase 4 Task 17, FG7] Zero-extent/minimize + fullscreen rows (added
2026-08-18, gate ruling #25 row 16) — NOT covered by the 2026-08-10 PASS
run above, which predates this feature. Own checklist, own "Last run"
below.**
- [ ] Minimize the window (taskbar/dock, or the OS shortcut) and restore it
      at least twice; the process does not crash/hang, and `--validate`
      shows zero unexpected `[error]` lines across the whole minimize/
      restore sequence.
- [ ] Toggle borderless fullscreen on/off (`--fullscreen` at startup; there
      is no in-sample runtime hotkey — use the OS's own fullscreen/restore
      window controls to toggle back) at least twice; the window fills the
      display with no borders in fullscreen, returns to its prior windowed
      size/position on toggle-off, and `--validate` shows zero unexpected
      `[error]` lines.

**Last run:** not yet performed on real hardware — the automated GPU test
suite (`rx_rhi_vk_tests`'s `window_state_test.cpp`, this task's own report)
exercises the zero-extent guard via a dependency-injection seam (no real
display can be made to report a genuinely zero-sized surface headlessly —
see that report's own disclosure) and the real windowed↔fullscreen toggle
against this development machine's real desktop (not a minimize, which
this same desktop session cannot script safely). A TRUE OS-level minimize
under a live, real `--present` session has not yet been performed by a
human. Fill in the checkboxes and record hardware/driver/result above the
first time this is actually run.

## Windows (binary cross-compiled via `windows-cross-zig`, run natively)

- [ ] Build on Linux: `cmake --preset windows-cross-zig && cmake --build --preset windows-cross-zig`
- [ ] Copy `build/windows-cross-zig/samples/01_triangle/sample_01_triangle.exe`
      to the Windows machine (this one file only — statically linked)
- [ ] Run: double-click, or from a terminal: `sample_01_triangle.exe --present`
- [ ] Triangle renders correctly
- [ ] Survives repeated resizes (drag-resize the window a handful of times,
      including at least one maximize/restore) with no visual corruption
- [ ] Closes cleanly via the window's close button, no crash/hang
- [ ] Headless mode still exits 0: `sample_01_triangle.exe` (check
      `echo %ERRORLEVEL%` after it returns) → `triangle readback PASSED`
- [ ] [Phase 4 Task 17, FG7] Minimize the window (taskbar) and restore it at
      least twice; no crash/hang.
- [ ] [Phase 4 Task 17, FG7] Toggle borderless fullscreen on/off
      (`--fullscreen` at startup) at least twice via the OS's own window
      controls; fills the display with no borders, returns cleanly to
      windowed size on toggle-off.

**Last run:** not yet performed on real Windows hardware — this repository
was developed and verified on Linux only. `windows-cross-zig` is confirmed
to **configure and build** cleanly (see the Phase 1 completion ledger and
this task's report), and the resulting `.exe` has been exercised under Wine
for the non-GPU test suites elsewhere in this project's history, but a real
Windows machine has not yet run this sample's `--present` mode. Fill in the
checkboxes, distro-equivalent (Windows build/edition), GPU, and driver
version above the first time this is actually run on Windows, before the
next release that claims Windows support.

## Steam Deck (Desktop Mode, `linux-native` preset)

SteamOS's Desktop Mode is Linux with a real display server (KDE Plasma) and
a real Vulkan driver (AMD Mesa RADV) — there is no Deck-specific build
variant; it uses the exact same `linux-native` preset and binary as any
other Linux machine.

- [ ] Build directly on the Deck in Desktop Mode (open Konsole from the
      taskbar): `cmake --preset linux-native && cmake --build --preset linux-native`
      — or build on another Linux machine and copy just the
      `sample_01_triangle` binary over (statically linked, same as Windows)
- [ ] Run: `./sample_01_triangle --present` from Konsole
- [ ] Triangle renders correctly
- [ ] Survives repeated resizes (including snapping/tiling via KDE's window
      controls) with no visual corruption
- [ ] Closes cleanly via the window's close button, no crash/hang
- [ ] Headless mode still exits 0: `./sample_01_triangle`
- [ ] [Phase 4 Task 17, FG7] Minimize the window and restore it at least
      twice (Gamescope/Desktop Mode taskbar); no crash/hang.
- [ ] [Phase 4 Task 17, FG7] Toggle borderless fullscreen on/off
      (`--fullscreen` at startup) at least twice via KDE's own window
      controls; fills the display with no borders, returns cleanly to
      windowed size on toggle-off.

**Last run:** not yet performed on real Steam Deck hardware — this
repository was developed and verified on a Linux desktop only. The
`linux-native` preset (same one the Deck would use) is confirmed green on
that desktop machine's own AMD/NVIDIA-equivalent Vulkan/validation stack;
nothing in this codebase depends on desktop-only APIs (SDL3 + Vulkan 1.3
dynamic rendering/synchronization2 both run under SteamOS's RADV driver),
but that has not yet been confirmed on an actual Deck. Fill in the
checkboxes and record the SteamOS version/RADV (Mesa) driver version above
the first time this is actually run on a Deck, before the next release that
claims Steam Deck support.

## rx_platform input surface (Phase 4 Task 20, gate ruling #14)

`rx::platform::Window`'s new input surface (relative mouse mode + mouse
deltas, cursor show/hide/confine, gamepad hot-plug + stick/trigger/button
polling with deadzones, minimal keyboard) is exercised automatically —
including gamepad hot-plug/axis/button paths, which SDL3's
`SDL_AttachVirtualJoystick` makes CI-testable without real hardware (see
`src/rx_platform/tests/window_test.cpp`) — but a few things are genuinely
NOT automatable headlessly, matching this file's own established
"screenshot/human-observed, not scriptable" carve-out (see e.g. row 3's own
acceptance criterion in the gate matrix for this ticket). **`09_scene`
(Task 24) is now the first real consumer** (mouse-look + gamepad + WASD
fly-through camera — see this file's own `## 09_scene` section below for
its end-to-end checklist); the rows below remain the raw platform-level
checks, independent of that sample.

**[Issue #33 correction, superseding the paragraph above as originally
written]** Task 24's original `09_scene` wiring only went as far as
*consuming* `consumeMouseDelta()` each frame — it never actually called
`setRelativeMouseMode()`, so the facility below was still functionally
unconsumed: the OS cursor was never hidden/locked, and the reported defect
(cursor escapes the window/hits screen edges during `--present`
fly-through, making control impractical) was exactly that gap. `09_scene`
now calls `setRelativeMouseMode()` for real (captured by default entering
`--present` fly-through, Esc toggles release/recapture, click-to-recapture
on the viewport — see `samples/09_scene/mouse_capture.h` and this file's own
`## 09_scene` section below for the new checklist rows) — this facility is
genuinely consumed end to end as of Issue #33, not merely partially as
Task 24 first left it.

- [ ] Relative mouse mode: call `setRelativeMouseMode(true)` in a real
      (non-hidden) window session — cursor visibly disappears and stays
      centered/locked to the window regardless of how far the mouse is
      moved; `setRelativeMouseMode(false)` visibly restores the normal
      cursor at a sane position.
- [ ] Cursor `setCursorVisible(false)`/`(true)` — cursor visibly
      disappears/reappears over the window.
- [ ] Cursor `setCursorConfined(true)`/`(false)` — cursor is visibly
      unable to leave the window's bounds while confined, and free to
      leave once un-confined.
- [ ] Focus-loss/regain: alt-tab away from the window while
      `setRelativeMouseMode(true)` is active, then alt-tab back — no
      crash, and mouse-look resumes correctly on refocus (this is the
      real-OS analogue of `window_test.cpp`'s synthetic
      `SDL_PushEvent`-driven FOCUS_LOST/FOCUS_GAINED test).
- [ ] Keyboard: `isKeyDown(SDL_SCANCODE_W)` (etc.) reflects a REAL physical
      key press/release — device-free tests can only prove the bounds-check
      and at-rest (`false`) behavior (SDL provides no public API to inject
      synthetic keyboard STATE, only events, which don't move
      `SDL_GetKeyboardState()`'s own array — see `window_test.cpp`'s
      comment on this).
- [ ] Gamepad, REAL hardware hot-plug: plug in a physical gamepad while the
      app is running — connects and becomes `poll()`-active; unplug —
      disconnects cleanly, no crash/hang. (Automated coverage uses
      `SDL_AttachVirtualJoystick` instead; this row is the real-USB-event
      analogue.)
- [ ] Steam Deck: the ticket's own stated acceptance bar ("Steam Deck pad
      drivability") — run `09_scene --present` on real Deck hardware (or
      via a real gamepad on desktop as a stand-in until Deck hardware is
      available); confirm the built-in controls (or an attached pad) drive
      the fly-through camera as expected, and check the log for the
      gyro/device-name diagnostic line this ticket adds
      (`rx_platform: gamepad connected id=... name="..." hasGyroSensor=...`)
      — SDL issue #9148 predicts a Steam Deck's own gyro reports
      `hasGyroSensor=false` at this project's pinned SDL3 version; this row
      confirms that diagnostic actually appears on real Deck hardware,
      not just in the virtual-joystick test.

**Last run:** not yet performed on real hardware — this task (Task 20)
implemented and automated-tested the platform-level surface; `09_scene`
(Issue #33) now actually calls `setRelativeMouseMode()` and drives it end
to end (see this file's own `## 09_scene` section), but a real
human-observed session (desktop mouse/keyboard/gamepad, then Steam Deck)
has not yet been performed. Issue #33's own fix DID confirm, via log
inspection (not visual observation — genuinely not automatable headlessly,
see this file's own posture above), that `SDL_SetWindowRelativeMouseMode(true)`
succeeds with no failure warning logged against a real windowed session on
a real NVIDIA GPU/driver (RTX 2080, `DRIVER_ID_NVIDIA_PROPRIETARY`) — the
code path executes and is granted by a real driver, which is as far as a
non-interactive session can verify; whether the cursor visibly
disappears/stays locked as a human would observe it is still an open row
below. Fill in the checkboxes above the first time this is actually run,
before any release claiming Deck gamepad support.

## rx_debug_ui overlay (Phase 4 Stage 2 Task 21, spec D20, gate ruling #16)

`rx::debug_ui::Overlay` (font upload, descriptor pool, render-graph pass,
event dispatch, LOAD-not-CLEAR pattern-preservation, at-most-once
`vkQueueWaitIdle`) is exercised automatically end-to-end against an
offscreen readback target (`src/rx_debug_ui/tests/test_overlay_gpu.cpp`,
`rx_debug_ui_gpu_tests`) under lavapipe + validation, both CI presets
(GPU test excluded on windows-cross-zig/Wine — no real Vulkan device
there, same posture as `rx_rhi_vk_tests`/`rx_graph_gpu_tests`). **`09_scene`
(Task 24) is now the first real HUD consumer** (see this file's own
`## 09_scene` section below); the rows below are genuinely not automatable
headlessly and are routed here per the gate matrix's own text (row 6: "a
headless test cannot exercise the 'camera stops moving' half of this rule
directly, so that half is a MANUAL_VERIFICATION row").

- [ ] Visual HUD sanity in a REAL windowed session (not the offscreen
      readback target the automated test uses): the overlay renders
      crisp, correctly-composited text/widgets over live rendered content,
      with no flicker, tearing, or misplaced geometry across several
      seconds of continuous frames.
- [ ] Gamepad ownership orthogonality: with a REAL physical gamepad
      connected, confirm ImGui's own SDL3 backend never itself opens/
      closes it (`ImGui_ImplSDL3_SetGamepadMode(Manual, nullptr, 0)`,
      called immediately after `ImGui_ImplSDL3_InitForVulkan()` —
      `overlay.cpp`) — Task 20/#14's own gamepad hot-plug log line
      (`rx_platform: gamepad connected id=...`) should fire exactly once
      per physical connect, with no second, ImGui-driven open/close
      racing it (the hazard gate ruling #16 row 7 names).
- [ ] "Camera stops moving while the HUD has focus" (gate matrix row 6's
      second half): with a sample driving both a fly-through camera
      (Task 20's mouse-look) and this overlay, click/drag inside an open
      HUD panel — the camera must NOT respond to that mouse motion while
      `ImGui::GetIO().WantCaptureMouse` is true (the automated GPU test
      proves `WantCaptureMouse` itself flips correctly; it cannot drive a
      real camera to observe the consuming half of the contract). [Issue
      #33] `09_scene` now has a RELEASED mouse-capture state (Esc) as a
      prerequisite for this row to even be exercisable with a visible,
      free cursor — see this file's own `## 09_scene` section below for the
      capture-specific rows this composes with.
- [ ] Steam Deck: confirm the HUD renders and is legible/usable at Deck's
      actual display resolution and, if the HUD ever grows touch-target
      sizing considerations, that mouse/keyboard-driven toggles remain
      operable via Deck's trackpads (gamepad HUD navigation is explicitly
      NOT implemented this phase, per gate ruling #16 row 7 — Manual
      gamepad mode means the HUD is not gamepad-navigable at all yet).

**Last run:** not yet performed on real hardware — this task (Task 21)
implemented and automated-tested the module against an offscreen readback
target; `09_scene` (Task 24) now wires this overlay into a live windowed
present loop (see this file's own `## 09_scene` section), but a real
human-observed session has not yet been performed. Fill in the checkboxes
above the first time this is actually run.

## 05_multipass (`--present` mode)

`sample_05_multipass` needs the Slang runtime libraries (+ its six on-disk
shader sources) deployed next to it — see `samples/README.md`'s own
"Redistribution" section for the full manifest; it is not statically linked
like 01_triangle.

### What "pass" means, every platform

- The window opens and shows a reddish cube and a bluish sphere on a
  grayish floor, lit from a fixed-elevation directional light whose azimuth
  continuously orbits — the cube's shadow visibly sweeps across the floor
  as the light turns (see `samples/README.md`'s "Expected output" section).
- Closing the window exits promptly, with no crash/hang, and logs
  `--present: window closed cleanly`.
- On Linux, run with `--validate` and confirm no `[error]`-level validation
  output beyond this codebase's two documented false-positive guards (see
  the "What 'pass' means" section above for the exact mechanism).

### Linux (native, `linux-native` preset)

- [ ] Build: `cmake --preset linux-native && cmake --build --preset linux-native`
- [ ] Run: `./build/linux-native/samples/05_multipass/sample_05_multipass --present --validate`
- [ ] Shadow/light/cube/sphere render correctly and the shadow sweeps as
      the light orbits, with zero unexpected validation errors
- [ ] Closes cleanly; headless mode still exits 0

**Last run:** not yet performed as a real, human-observed run on this or
any platform (this checklist row is new as of Task 8; sample 05 itself
predates it). Functionally exercised end to end during this project's own
development, though not as a substitute for the human-observed check this
file otherwise requires: `sample_05_multipass_headless`'s own analytic
pixel assertions pass under `ctest` on every CI run (`.github/workflows/
ci.yml`), which is the automated half of this sample's correctness story.
Fill in the checkboxes and hardware/driver details above the first time
`--present` is actually watched running on real hardware.

## 06_materials (`--present` mode)

`sample_06_materials` needs the Slang runtime libraries + its own
`materials/`/`material_shaders/` subdirectories deployed next to it — see
`samples/README.md`'s own "Redistribution" section for the full manifest.

### What "pass" means, every platform

- The window opens and shows an orange checkerboard cube (top-left), a teal
  checkerboard cube (top-right), a magenta rim-lit sphere (bottom-left),
  and a golden-yellow rim-lit sphere (bottom-right), with the camera
  orbiting continuously (see `samples/README.md`'s "Expected output"
  section).
- Editing and saving the deployed `materials/checker.slang` or
  `materials/rim.slang` (next to the running binary) changes that
  material's rendering within about a second, with a console log line
  confirming the reload; a syntactically broken edit keeps the last-good
  material rendering rather than crashing the window.
- Closing the window exits promptly, with no crash/hang, and logs
  `--present: window closed cleanly`.
- On Linux, run with `--validate` and confirm no `[error]`-level validation
  output beyond this codebase's two documented false-positive guards.

### Linux (native, `linux-native` preset)

- [ ] Build: `cmake --preset linux-native && cmake --build --preset linux-native`
- [ ] Run: `./build/linux-native/samples/06_materials/sample_06_materials --present --validate`
- [ ] All 4 objects render correctly and the camera orbits smoothly, with
      zero unexpected validation errors
- [ ] Editing `materials/checker.slang`/`materials/rim.slang` (the deployed
      copies) live-updates the running window within ~1s, and a
      syntactically broken edit keeps the last-good material instead of
      crashing
- [ ] Closes cleanly; headless mode still exits 0

**Last run:** not yet performed as a real, human-observed run on real
display hardware. Functionally verified during this task's own development
via an offscreen X server (Xvfb, no human watching a real display): the
window opens, all 4 objects render at their analytically expected colors
(cross-checked against the same headless-gate math), the camera orbits,
editing the deployed `checker.slang`/`rim.slang` while running triggers a
logged `hot-reload of '...' succeeded` and visibly changes that material
(confirmed across 3 consecutive edit/reload cycles touching both files),
and the process exits cleanly (`--present: window closed cleanly`, exit
code 0) on `SIGTERM` on both the `linux-native` build and the
`windows-cross-zig` build run under Wine, from the packaged (unzipped,
outside the build tree) layout in both cases. This is real functional
verification, not a placeholder — but it is not the human-observed-on-real-
hardware check this file otherwise tracks. Fill in the checkboxes and
hardware/driver details above the first time `--present` is actually
watched running on a real display.

## 07_stress (`--present` mode)

`sample_07_stress` needs the Slang runtime libraries + its own four
on-disk shader sources deployed next to it — see `samples/README.md`'s own
"Redistribution" section for the full manifest; it is not statically
linked like 01_triangle. [Phase 4 Stage 2 Task 24 — this section was
missing; added while touching this file per that task's own binding
scope.]

### What "pass" means, every platform

- The window opens and shows a large, non-overlapping grid of red/green/
  blue/magenta cubes and spheres (the sample's 4 mesh/pipeline-state
  variants), viewed from directly above through a fixed, non-orbiting
  camera — the field never moves or animates (a deliberate experimental-
  design choice, see `samples/07_stress/main.cpp`'s own header comment).
- `--draws N` (default 30000) controls the field's own instance count;
  `--threads N` overrides the sample's own `Scheduler` worker count (a
  measurement instrument, not an engine-wide switch — `--threads 1`
  collapses the forward pass to genuinely serial recording, the A/B
  baseline the default multi-worker count is compared against).
- Closing the window exits promptly, with no crash/hang, and logs
  `--present: window closed cleanly`.
- On Linux, run with `--validate` and confirm no `[error]`-level validation
  output beyond this codebase's documented false-positive guards (see the
  "What 'pass' means" section at the top of this file for the exact
  mechanism).

### Linux (native, `linux-native` preset)

- [ ] Build: `cmake --preset linux-native && cmake --build --preset linux-native`
- [ ] Run: `./build/linux-native/samples/07_stress/sample_07_stress --present --validate`
- [ ] The full instanced field renders correctly (all 4 variant colors
      visible, no z-fighting/corruption), with zero unexpected validation
      errors
- [ ] `--threads 1` vs the default worker count both run without error
      (visually identical output — `--threads` affects CPU recording cost
      only, never the rendered result)
- [ ] Closes cleanly; headless mode still exits 0

**Last run:** not yet performed as a real, human-observed run on real
display hardware. Functionally verified during this and later tasks' own
development via an offscreen X server (Xvfb) against lavapipe:
`sample_07_stress_headless`'s own analytic pixel-dominance probes pass
under `ctest` on every CI run (`.github/workflows/ci.yml`), and the
`--present` path has been exercised under Xvfb (window opens, renders,
`SIGTERM` → `SDL_EVENT_QUIT` → clean exit) as part of Task 24's own
end-to-end verification of the render-graph/executor path 09_scene shares
with this sample. This is real functional verification, not a
placeholder — but it is not the human-observed-on-real-hardware check this
file otherwise tracks. Fill in the checkboxes and hardware/driver details
above the first time `--present` is actually watched running on a real
display.

## 08_gltf_viewer (`--present` mode)

`sample_08_gltf_viewer` needs the Slang runtime libraries + its own
`material_shaders/`/`references/` subdirectories + (for the default scene)
a pre-staged `assets/DamagedHelmet/glTF/` deployed next to it — see
`samples/README.md`'s own "Redistribution" section for the full manifest.

### What "pass" means, every platform

- The window opens showing a distinct dark-navy "loading" screen (never
  pure black) while the default DamagedHelmet asset imports
  asynchronously, then transitions to the rendered helmet once the import
  completes (no stall, no frozen window during the load).
- Left-click-dragging orbits the camera around the helmet smoothly; no
  jump/snap on mouse-down, no drift after release.
- `--scene <path/to/other.gltf>` loads a different glTF asset instead of
  DamagedHelmet; `--exposure <n>` visibly brightens (positive) or darkens
  (negative) the rendered scene, pre-tonemap.
- Closing the window exits promptly, with no crash/hang, and logs
  `sample_08_gltf_viewer: window closed cleanly`.
- On Linux, run with `--validate` and confirm no `[error]`-level validation
  output beyond this codebase's documented false-positive guards.

### Linux (native, `linux-native` preset)

- [ ] Build: `cmake --preset linux-native && cmake --build --preset linux-native`
- [ ] Fetch the default asset once: `tools/fetch_assets.sh`
- [ ] Run: `./build/linux-native/samples/08_gltf_viewer/sample_08_gltf_viewer --present --validate`
- [ ] Loading screen shows briefly, then the helmet renders; mouse-drag
      orbit feels smooth and centered on the helmet
- [ ] `--scene`/`--exposure` both visibly change the render as described
      above
- [ ] Closes cleanly; headless mode (`--validate`, no `--present`) still
      exits 0 with `headless gate PASSED`

**Last run:** not yet performed as a real, human-observed run on real
display hardware. Functionally verified during this task's own development
via an offscreen X server (Xvfb) against lavapipe (forced via
`VK_ICD_FILENAMES`, the same driver CI's own headless gate runs against):
the loading-state clear renders, the async import completes and the
helmet's own forward-shaded render replaces it, `--quit-during-load`
cancels mid-import (after >=1 real texture upload had already landed) and
tears down with zero unfiltered validation errors, and a real
SDL-delivered quit (`SIGINT` under Xvfb, which SDL3 translates into a
normal `SDL_EVENT_QUIT`) exits the present loop cleanly — logging
`sample_08_gltf_viewer: window closed cleanly` with zero
`VUID-vkDestroyDevice-*` "child object not destroyed" errors (a real
teardown-ordering bug this task's own development hit, root-caused, and
fixed: `FrameSync`'s owned command pool/fences/semaphores were being torn
down AFTER the `VkDevice` that owned them). This is real functional
verification, not a placeholder — but it is not the human-observed-on-real-
hardware, real-mouse-drag check this file otherwise tracks. Fill in the
checkboxes and hardware/driver details above the first time `--present` is
actually watched (and dragged) on a real display.

## 09_scene (`--present` mode, the Phase 4 phase-exit sample)

`sample_09_scene` needs the Slang runtime libraries + its own
`material_shaders/`/`shadow_shaders/`/`references/` subdirectories + a
pre-staged `assets/DamagedHelmet/glTF/` deployed next to it — see
`samples/README.md`'s own "Redistribution" section for the full manifest.

### What "pass" means, every platform

- The window opens showing the default DamagedHelmet grid (4 rows x 4
  columns; the fixed startup camera frames the two default-visible rows —
  8 helmets — plus a directional-light shadow on the ground) rendered
  through the full Registry → Scene → DrawListBuilder → render-graph path.
  `--scene sponza` (after `tools/fetch_assets.sh --sponza`) loads Sponza
  instead; without that fetch it fails loudly with a named "run
  `tools/fetch_assets.sh --sponza` first" error rather than hanging or
  crashing.
- WASD + mouse-look (relative mouse mode) + gamepad (left stick move,
  right stick look) fly the camera through the scene smoothly, with no
  jump/snap and no drift.
- [Issue #33] Mouse capture: CAPTURED by default on entering `--present`
  (cursor hidden, locked to the window, drives look immediately — no extra
  click needed). Esc toggles RELEASED (cursor visibly reappears at a sane
  position, HUD is fully clickable, camera stops responding to mouse motion
  claimed by an open HUD panel) and back to CAPTURED. While RELEASED,
  left-clicking the viewport (i.e. NOT an open HUD panel) recaptures.
  Gamepad look/move work identically in both capture states.
- The ImGui HUD shows FPS/frame-ms (60-frame rolling average), cull
  counters (visible/culled/recordsIn/drawsSubmitted + the instancing-
  collapse-ratio percentage), a vsync checkbox, TWO VISIBLY DISTINCT mask
  controls — one row of layer-mask checkboxes (toggling a row's checkbox
  hides/shows that whole row of helmets outright) and a SEPARATE light-
  channel checkbox (toggling it only changes whether row 0 is lit/casts a
  shadow — the helmets in row 0 never disappear when this is toggled,
  unlike the layer checkboxes) — pool stats, and the #27 memory report.
- The vsync checkbox visibly changes tearing/frame-pacing behavior and logs
  the same "present mode in use" line the `--vsync` CLI flag produces.
- `--stress` (optionally with `--stress-draws N`/`--threads N`) replaces
  the scene with a large Registry-free instanced field (30000 instances by
  default) through the same scene path, for the stress-v2 A/B comparison
  against `07_stress` (see this task's own report for the published
  numbers).
- Closing the window exits promptly, with no crash/hang, and logs
  `sample_09_scene: window closed cleanly`.
- On Linux, run with `--validate` and confirm no `[error]`-level validation
  output beyond this codebase's documented false-positive guards.

### Linux (native, `linux-native` preset)

- [ ] Build: `cmake --preset linux-native && cmake --build --preset linux-native`
- [ ] Fetch the default asset once: `tools/fetch_assets.sh`
- [ ] Run: `./build/linux-native/samples/09_scene/sample_09_scene --present --validate`
- [ ] Default helmet grid renders correctly; fly-through (WASD + mouse-look)
      feels smooth and correctly oriented
- [ ] Mouse capture [Issue #33]: on launch, the OS cursor is hidden and
      mouse-look works immediately (captured by default) — the cursor never
      escapes the window/hits screen edges during continuous mouse-look
      (the originally reported defect)
- [ ] Esc releases capture: OS cursor visibly reappears at a sane position;
      moving the mouse over the (non-HUD) viewport no longer spins the
      camera while an open HUD panel is hovered (`WantCaptureMouse` true);
      a second Esc recaptures (cursor hides again, look resumes)
- [ ] While released, left-clicking the (non-HUD) viewport recaptures
      (cursor hides, mouse-look resumes) without needing Esc again
- [ ] While released, the HUD (checkboxes, vsync toggle, layer-mask rows,
      etc. — see the HUD row below) is fully clickable/usable with the
      visible cursor, and the "Mouse: RELEASED ..." / "Mouse: CAPTURED ..."
      status line in the HUD itself reflects the current state accurately
- [ ] Alt-tab away and back while captured: no crash; mouse-look resumes
      correctly on refocus (re-arm composes with the capture toggle — see
      this file's own `## rx_platform input surface` section above)
- [ ] HUD shows FPS/cull-counters/vsync/layer-mask row toggles/light-channel
      toggle/pool stats/memory report; layer-mask toggles hide/show whole
      rows, the light-channel toggle only changes row 0's lighting/shadow
      (never its visibility) — confirms the two controls are genuinely
      independent, not one shared control
- [ ] vsync checkbox visibly changes present behavior and logs the same
      line the `--vsync` CLI flag does
- [ ] `--scene sponza` (after `tools/fetch_assets.sh --sponza`) loads and
      renders Sponza; without the fetch it fails loudly with a named error
- [ ] `--stress` renders the large instanced field smoothly
- [ ] Closes cleanly; headless mode (`--validate`, no `--present`) still
      exits 0 with `headless gate PASSED`

**Last run:** not yet performed as a real, human-observed run on real
display hardware. Functionally verified during this task's own development
via an offscreen X server (Xvfb) against lavapipe (forced via
`VK_ICD_FILENAMES`, the same driver CI's own headless gates run against):
the default helmet grid renders through the full scene path (shadow pass +
chunked forward pass + tonemap + HUD overlay pass, all real render-graph
passes), the headless gate's own EXACT counter assertions pass
(imported=16, visible=8, culled=8 — by layer mask, deterministic —
recordsIn=8, drawsSubmitted=1, collapse ratio 87.5%), the D17 tolerance-
pixel gate passes at 0 failing pixels against the committed
`references/grid_scene.png`, the HUD overlay pass produces real non-empty
ImGui draw data (a separate, non-pixel-gated headless frame proves this,
since FPS/frame-ms text is inherently non-deterministic run-to-run), the
`--stress` path (Registry-free, 30000 instances by default) runs cleanly
under `--present` with `--threads 2`, and a real `SIGTERM` (translated by
SDL3 into `SDL_EVENT_QUIT`) exits the present loop cleanly with
`window closed cleanly` logged and zero unfiltered validation errors, both
for the default grid and the `--stress` path. `--scene sponza` was
implemented per the same fetch-and-fail-loudly contract sample 08's own
Sponza-adjacent code follows, but was NOT exercised against a real fetched
Sponza asset in this environment (Sponza is present-mode-only, never
CI-fetched, and fetching the ~53 MB asset was outside this task's
available session time) — this is a disclosed scope gap, not a claimed
pass. This is real functional verification, not a placeholder — but it is
not the human-observed-on-real-hardware, real-mouse-and-gamepad-drive check
this file otherwise tracks. Fill in the checkboxes and hardware/driver
details above the first time `--present` is actually watched (and driven)
on a real display.

**[Issue #33 addendum]** The mouse-capture fix itself was additionally run
`--present --validate` for ~15s against a REAL windowed session (a real X
display, not Xvfb) on a REAL discrete NVIDIA GPU with the DEFAULT (unforced)
ICD loader — `vulkaninfo` confirms `GPU0: NVIDIA GeForce RTX 2080`,
`driverID = DRIVER_ID_NVIDIA_PROPRIETARY`, `driverInfo = 580.82.07`, and
`vkb::PhysicalDeviceSelector`'s own default discrete-GPU preference selects
it ahead of the also-installed lavapipe ICD: zero `[error]`-level log lines,
zero `Validation Error` lines without this codebase's own documented
false-positive guard prefix, and a clean `SIGTERM` exit logging
`window closed cleanly`. Critically, `SDL_SetWindowRelativeMouseMode(true)`
logged NO failure warning during this real run — `Window::setRelativeMouseMode()`
only logs on failure, so its absence here is positive evidence the real
driver GRANTED relative mode, not just that the call was reached. What this
does NOT prove, and remains genuinely open below: whether the OS cursor
*visibly* stays hidden/locked, whether Esc/click-to-recapture *feel* right,
and Steam Deck gamepad drivability — none of that is observable from a log
in a non-interactive session; a human must still watch and drive this the
first time the checkboxes above are filled in.

## Steam Deck (09_scene, `linux-native` preset)

Same posture as every other sample's own Steam Deck subsection (see
01_triangle's own, above): SteamOS Desktop Mode uses the identical
`linux-native` preset and binary, no Deck-specific build variant.

- [ ] Build directly on the Deck in Desktop Mode, or copy the packaged
      `09_scene/` directory over (see `tools/package_samples.sh`)
- [ ] Run: `./sample_09_scene --present` from Konsole (or Gamescope)
- [ ] Fly-through camera is drivable via the Deck's own built-in gamepad
      controls (left stick move, right stick look) — the ticket's own
      stated "Steam Deck pad drivability" acceptance bar
- [ ] HUD renders legibly at the Deck's own display resolution; touch/
      trackpad-driven toggle interaction remains usable (gamepad HUD
      navigation is explicitly NOT implemented this phase — Manual gamepad
      mode, gate ruling #16 row 7 — so HUD toggles need mouse/trackpad,
      not the pad's own face buttons)
- [ ] Check the log for the gyro/device-name diagnostic line
      (`rx_platform: gamepad connected id=... name="..." hasGyroSensor=...`)
      — SDL issue #9148 predicts `hasGyroSensor=false` on a real Deck at
      this project's pinned SDL3 version
- [ ] Closes cleanly via the window's close button, no crash/hang

**Last run:** not yet performed on real Steam Deck hardware — this
repository was developed and verified on a Linux desktop only; nothing in
this sample depends on desktop-only APIs (SDL3 + Vulkan 1.3 dynamic
rendering/synchronization2 both run under SteamOS's RADV driver, and this
sample uses no capability beyond what every earlier Phase 4 sample already
exercises there), but that has not yet been confirmed on an actual Deck.
Fill in the checkboxes and record the SteamOS version/RADV (Mesa) driver
version above the first time this is actually run on a Deck, before the
next release that claims Steam Deck support for this sample.
