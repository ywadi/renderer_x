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
