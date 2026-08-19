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
acceptance criterion in the gate matrix for this ticket). **No sample yet
consumes this surface** — Task 24 (`09_fly_through`) is the first real
consumer (mouse-look + gamepad + WASD fly-through camera); the rows below
are the raw platform-level checks, independent of that sample, and the
sample itself will carry its own MANUAL_VERIFICATION section once it lands.

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
      drivability") — run on real Deck hardware (or via a real gamepad on
      desktop as a stand-in until Deck hardware is available) once Task 24
      lands a sample that actually consumes stick/trigger/button input;
      confirm the built-in controls (or an attached pad) drive the
      fly-through camera as expected, and check the log for the
      gyro/device-name diagnostic line this ticket adds
      (`rx_platform: gamepad connected id=... name="..." hasGyroSensor=...`)
      — SDL issue #9148 predicts a Steam Deck's own gyro reports
      `hasGyroSensor=false` at this project's pinned SDL3 version; this row
      confirms that diagnostic actually appears on real Deck hardware,
      not just in the virtual-joystick test.

**Last run:** not yet performed — this task (Task 20) implemented and
automated-tested the platform-level surface only; no sample exists yet to
drive an end-to-end human-observed session against. Fill in the checkboxes
above once Task 24's `09_fly_through` sample lands and this section can be
run for real (on desktop first, then Steam Deck hardware before any release
claiming Deck gamepad support).

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
