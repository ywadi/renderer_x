# Task 7 report: CI matrix, real build-budget check, Wine test fix, sample artifacts

Branch: `main` (worked directly, as authorized)
Commits (oldest to newest):
- `3d61a97` Add GitHub Actions CI: linux-native + windows-cross-zig, real budget check, Wine CRLF fix
- `1b55ffd` Fix CI: linux-native job needs X11 dev headers to build SDL3 from source
- `6b28f91` Fix CI: linux-native needs libxtst-dev/libxkbcommon-dev for SDL3's X11 XTest check
- `b82f004` Fix CI: pin WINEARCH=win64 so Wine doesn't need wine32/i386 multiarch
- `2995475` Fix CI: windows-cross-zig needs a host Vulkan loader/ICD for Wine's winevulkan

Final green run: **https://github.com/ywadi/renderer_x/actions/runs/31344444614**
(`linux-native` in 2m32s, `windows-cross-zig` in 16m42s, both artifacts uploaded)

## What was built

- `tools/check_build_budget.sh <preset> [budget_seconds=60]` — touches
  `src/rx_core/src/log.cpp` (a leaf `.cpp` linked transitively into every
  test binary and the sample) immediately before timing
  `cmake --build --preset <preset>`, so it measures a real
  recompile-one-TU-plus-relink-the-dependent-chain, not a no-op. Configures
  and builds once, untimed, first if the binary dir doesn't exist yet.
  Fails if the timed build exceeds the budget. Verified locally on both
  presets (2-3s each) and confirmed in CI logs (2s linux-native, 3s
  windows-cross-zig — both well under the 60s default).
- `src/rx_core/tests/log_test.cpp` — the captured-spdlog-output assertion
  compared against a literal `"hello 42\n"`, which only matches on Linux
  (Windows spdlog terminates the same formatted line with `"\r\n"`).
  Strips trailing `\r`/`\n` before comparing against `"hello 42"` instead.
  Reproduced the original failure and confirmed the fix under
  `wine build/windows-cross-zig/src/rx_core/rx_core_tests.exe` (wine-11.0
  installed locally) before touching the workflow at all.
- `.github/workflows/ci.yml` — two jobs, both `ubuntu-latest`:
  - **linux-native**: apt-installs `ninja-build mesa-vulkan-drivers
    vulkan-tools libvulkan-dev xvfb` plus (added after the first real CI
    run — see Deviations) the X11 development packages SDL3's own build
    needs (`libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev
    libxfixes-dev libxss-dev libxtst-dev libxkbcommon-dev`); installs zig
    0.16.0 from a checksummed download; caches `third_party/slang-prebuilt`
    and `.deps-cache` (per-preset key, both keyed on
    `hashFiles('third_party/CMakeLists.txt')`); configures, builds, runs
    `xvfb-run -a ctest --preset linux-native --output-on-failure`, runs the
    budget check, uploads `sample_01_triangle` as
    `sample_01_triangle-linux-native`.
  - **windows-cross-zig**: apt-installs `ninja-build wine xvfb` plus (added
    after the first real CI run of this job — see Deviations)
    `mesa-vulkan-drivers vulkan-tools libvulkan-dev`; pins `WINEARCH=win64`
    at the job level; installs the same checksummed zig; same cache
    strategy (own per-preset key); configures, builds, runs
    `wine .../toolchain_check.exe` as a smoke check, runs
    `xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|sample'
    --output-on-failure` (GPU-heavy tests excluded, documented inline —
    see Deviations), runs the budget check, uploads
    `sample_01_triangle.exe` as `sample_01_triangle-windows-cross-zig`.
  - zig 0.16.0 sha256 (`70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00`
    for `zig-x86_64-linux-0.16.0.tar.xz`) taken directly from
    `https://ziglang.org/download/index.json`'s own `"0.16.0" ->
    "x86_64-linux" -> "shasum"` field, then independently re-verified:
    downloaded the tarball, checked that sum against it directly with
    `sha256sum -c`, extracted it, and confirmed the extracted `zig` binary
    is byte-for-byte identical (`sha256sum`) to this repo's own
    already-in-use `toolchain/zig/zig` (gitignored, fetched once by a
    developer) — not just trusted from the index, verified twice over.
  - `.deps-cache` cached per-preset (`deps-cache-linux-native-<hash>` /
    `deps-cache-windows-cross-zig-<hash>`), not shared: `DepCache.cmake`'s
    own cache-key formula already varies by target triple, but
    `actions/cache` treats an identical *key* across two concurrent jobs as
    one entry — only the first job to finish saves it, starving whichever
    job loses that race of ever getting its own dependencies cached. A
    shared `slang-prebuilt` cache key is fine (byte-identical host-side
    tooling regardless of target).

## Real CI iteration — every failure diagnosed from actual job logs, not guessed

The first real push (`3d61a97`) was red on both jobs, as the brief
anticipated could happen. Four more root causes surfaced across the
following pushes, each diagnosed from that run's own log and fixed before
moving on:

1. **linux-native Configure failure**: SDL3 (built from source via the
   dep-cache) hard-fails its own CMake configure with "SDL could not find
   X11 or Wayland development libraries" — this development machine never
   surfaced it because it already has a full desktop environment's dev
   packages installed; the bare `ubuntu-latest` runner does not. Fixed with
   `libx11-dev`/`libxext-dev`/etc. (`1b55ffd`).
2. **linux-native Configure failure, second layer**: fixing the top-level
   X11 check revealed a second, more specific fatal check one level deeper
   in SDL3's own `CheckX11` macro ("Couldn't find dependency package for
   XTEST") — CMake configure stops at the first fatal error, so this was
   invisible until the first was fixed. Cross-checked SDL3's own
   `cmake/sdlchecks.cmake` (release-3.4.14, the pinned tag) directly for
   every remaining `SDL_missing_dependency()` call it could still reach,
   rather than fixing one check at a time again. Fixed with `libxtst-dev`
   + `libxkbcommon-dev` (`6b28f91`).
3. **windows-cross-zig Test failure**: `rx_platform_tests`' very first,
   unconditional `Window::create()` call (no skip guard — see
   `src/rx_platform/tests/window_test.cpp`) failed outright under Wine
   with `SDL_CreateWindow failed: Installed Vulkan doesn't implement the
   VK_KHR_surface extension`. The job's own log also showed "it looks like
   wine32 is missing" — first attributed the failure to that (a brand-new
   Wine prefix defaulting to `WINEARCH=win32`, which needs `wine32`/i386
   multiarch this runner's `wine` package didn't have) and pinned
   `WINEARCH=win64` (`b82f004`).
4. **windows-cross-zig Test failure, same symptom, real root cause**: the
   `WINEARCH=win64` fix did **not** resolve it — the next run reproduced
   the identical failure. Investigated further:
   `src/rx_platform/src/window.cpp`'s `Window::create()` unconditionally
   requests `SDL_WINDOW_VULKAN`, and Wine's own built-in Vulkan support
   (`winevulkan`/`vulkan-1.dll`) is a passthrough to whatever real Vulkan
   loader/ICD exists on the *host* — this job's runner had none installed
   at all (only `linux-native`'s job did), so `winevulkan` had nothing to
   forward to. Fixed by installing the same
   `mesa-vulkan-drivers`/`vulkan-tools`/`libvulkan-dev` (lavapipe) packages
   `linux-native` already uses, giving `winevulkan` a real, if software,
   implementation to pass through to (`2995475`). This run went fully
   green.

The `it looks like wine32 is missing` message, in hindsight, is printed by
Wine's own launcher script regardless of `WINEARCH` and regardless of
success/failure (confirmed: it appears identically in the run where
`toolchain_check.exe` and `rx_core_tests.exe` both passed cleanly) — it was
a red herring that happened to co-occur with the real bug, not the cause.
`2995475`'s commit message corrects the previous commit's now-disproven
"verified locally" claim: that local verification used this machine's own
`winehq-stable` Wine install, which already has a working Vulkan
ICD/loader present system-wide — the exact variable CI's bare runner
lacked, so the local test never isolated `WINEARCH` from Vulkan-ICD
presence. `WINEARCH=win64` is kept anyway as separately-correct hygiene for
a 64-bit-only cross-compilation target, with its comment corrected to not
overclaim what it fixed.

## Verification performed

- **Real device path (this machine, NVIDIA RTX 2080)**: `ctest --preset
  linux-native --output-on-failure` — 5/5 green.
- **Lavapipe-only path, simulating the CI runner's software-only GPU**:
  forced `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json` (excluding
  this machine's real GPU) and ran the whole suite under `xvfb-run` — all
  5 tests, including `rx_rhi_vk_tests` (91 real doctest assertions) and
  `sample_01_triangle_headless` (real triangle-readback pass, exit 0),
  passed against lavapipe alone. This is the direct evidence behind the
  workflow's own comment that the linux-native job's tests genuinely
  exercise lavapipe rather than hitting a skip guard.
- **Wine CRLF fix**: reproduced the original failure and confirmed the fix
  via `wine build/windows-cross-zig/src/rx_core/rx_core_tests.exe`
  (12/12 assertions passing) before ever touching the workflow.
- **Budget script**: ran on both presets locally (2s/3s) and confirmed via
  the actual CI logs of the final green run (2s linux-native, 3s
  windows-cross-zig).
- **zig checksum**: downloaded the real tarball, verified the pinned
  sha256 against it, extracted it, and confirmed the extracted `zig`
  binary is byte-identical to this repo's own in-use toolchain — not
  invented, not trusted blind from a single source.
- **Workflow YAML**: validated with `actionlint` (zero findings on every
  revision) in addition to a Python `yaml.safe_load` parse check.
- **Real CI, both jobs, final run
  ([31344444614](https://github.com/ywadi/renderer_x/actions/runs/31344444614))**:
  - `linux-native` (2m32s): Configure/Build/Test/Budget/Upload all green;
    `ctest` log shows all 5 tests individually `Passed` (not skipped) with
    a combined `100% tests passed... Total Test time (real) = 4.11 sec`;
    budget check logged `2s` against the `60s` budget.
  - `windows-cross-zig` (16m42s — a cold `.deps-cache`, since every prior
    attempt failed before its cache-save post-step could run):
    `toolchain_check.exe` printed `target=windows` under Wine;
    `ctest -E 'rx_rhi_vk|sample'` log shows all 3 remaining tests
    individually `Passed`, including `rx_platform_tests` (6.04s — real
    window/SDL/Vulkan-probe work, not an instant skip) with
    `100% tests passed... Total Test time (real) = 9.21 sec`; budget check
    logged `3s` against the `60s` budget; both `sample_01_triangle(.exe)`
    artifacts uploaded and confirmed present via `gh run view`.
  - `gh run view 31344444614 --exit-status` exits `0`.
- **Commit hygiene**: `git log -1 --format='%B'` inspected on every one of
  the 5 commits individually (grepped for `claude|anthropic|co-authored|
  generated by|ai assistant`) — clean on all. Author/committer identity on
  every commit is the user's own configured git identity, untouched.
  Working tree clean; local `HEAD` matches `origin/main`.

## Notes / deviations from the brief worth flagging forward

- **Package lists expanded beyond the brief's literal apt lists on both
  jobs**, each with an explanation in both the workflow's own inline
  comments and the commit messages above:
  - `linux-native`: 9 additional X11 dev packages beyond the brief's
    `mesa-vulkan-drivers vulkan-tools libvulkan-dev xvfb`, required for
    SDL3's own from-source build on a bare runner (this dev machine's
    already-installed desktop packages hid this gap during local-only
    validation).
  - `windows-cross-zig`: added `mesa-vulkan-drivers vulkan-tools
    libvulkan-dev` (identical to linux-native's triplet) beyond the
    brief's `ninja-build wine xvfb`, because Wine's own Vulkan passthrough
    needs a real host ICD even just to let `SDL_CreateWindow(...,
    SDL_WINDOW_VULKAN)` succeed for rx_platform's *non-GPU* tests — a
    dependency the brief's own "Wine in CI has no Vulkan" framing didn't
    anticipate, since it's about window creation succeeding at all, not
    about running the GPU-heavy pipeline.
- **`rx_rhi_vk_tests`/`sample_01_triangle_headless` are excluded from the
  windows-cross-zig ctest run via `-E`, per the brief's explicit sanctioned
  option** ("if lavapipe genuinely can't run it, say so explicitly... never
  a silent pass"). This is a deliberate `ctest -E` exclusion, documented
  inline in the workflow with the reasoning above — not a runtime skip
  guard, and not attempted at all, so there's no ambiguity about whether it
  ran. Now that lavapipe is installed on this job's runner too (for the
  window-creation fix above), it's *possible* Wine's `winevulkan` passthrough
  could handle more of the real device/swapchain/synchronization2 pipeline
  than assumed — but verifying that with confidence (a much bigger claim
  than "window creation succeeds") was out of this task's scope and not
  attempted; flagging forward in case a future task wants to investigate
  lifting this exclusion.
- **`WINEARCH=win64` is kept despite not being the actual fix** for the
  failure it was originally introduced to fix (see the iteration log
  above) — it remains correct, narrowly-scoped hygiene for a 64-bit-only
  cross-compilation target, and its comment in the workflow now says so
  accurately rather than overclaiming.
- **No concurrency group was added** to auto-cancel superseded runs on
  rapid pushes (a reasonable production hardening, but out of the brief's
  explicit scope) — three earlier, now-superseded runs from this
  iteration were manually cancelled/left to finish instead. Worth
  considering in a follow-up if this workflow sees frequent rapid pushes.

## Files touched

- Created: `.github/workflows/ci.yml`, `tools/check_build_budget.sh`
- Modified: `src/rx_core/tests/log_test.cpp` (CRLF-normalizing 2-line
  change to one assertion)
