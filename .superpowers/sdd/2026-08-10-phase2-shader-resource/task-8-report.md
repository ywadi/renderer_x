# Task 8 report: CI + packaging for Phase 2 samples

Status: **complete**. Both CI jobs REAL green on GitHub. Commits `e051006..ca8f4be`.

Final green run: https://github.com/ywadi/renderer_x/actions/runs/31364667891
(`linux-native` 3m15s, `windows-cross-zig` 4m44s, both build-budget checks 2-3s
against a 60s budget).

## What was done

### 1. CI workflow (`.github/workflows/ci.yml`)

- **Slang prebuilt cache key fixed to per-job** (`slang-prebuilt-linux-native-...`
  / `slang-prebuilt-windows-cross-zig-...` instead of one shared key). This was
  a tracked item from Task 1's review: since Task 1, `windows-cross-zig`
  fetches TWO platform subtrees (`linux-x86_64/` for host-side `slangc`,
  `windows-x86_64/` for target runtime libs) under the one previously-shared
  cache path/key, so whichever job's save "won" that key forever starved the
  other job of a hit for the subtree only it populates. Per-job keys fix this.
- **`--output-junit` added to both jobs' `ctest` invocations**, uploaded as its
  own artifact (`ctest-results-linux-native` / `ctest-results-windows-cross-zig`,
  `if: always()` so it uploads even on a failed run). Addresses Phase 1's
  deferred-minor suggestion to consider `-V`/`--output-junit` for
  self-verifying logs: each `<testcase>` carries its full captured stdout,
  so e.g. every sample's own `"... headless gate PASSED"` line is durably
  present in a structured, greppable artifact, not just the raw console log.
  Console output itself is unchanged/still visible — ctest's default per-test
  `"... Passed"` line was already unconditional before this task.
- **Packaging step added to both jobs**: `tools/package_samples.sh <preset>
  <slang-platform-dir> <output-zip>`, uploaded as `rendererx-samples-linux-x86_64`
  / `rendererx-samples-windows-x86_64` workflow artifacts.
- Comment updated on the `windows-cross-zig` job's Wine test step to document
  (rather than silently rely on) the fact that `rx_shader_tests` already runs
  under Wine there and passes — Slang's Windows DLLs do load under Wine
  (Task 1 verified this locally; this task's own green run re-verifies it on
  the actual GitHub Actions runner, per the brief's "investigate, don't
  assume the dev machine's result transfers" instruction).

### 2. Packaging (`tools/package_samples.sh`, new)

Builds a per-platform zip with one subdirectory per sample, containing exactly
what a redistributed copy needs [R:D2], nothing else (no `CMakeFiles/`,
`cmake_install.cmake`, `CTestTestfile.cmake`, `.pdb`):

- `01_triangle/`: binary + `triangle.vert.spv` + `triangle.frag.spv`. **No
  Slang runtime libraries** — its shaders are precompiled offline by `slangc`
  at build time, so it never calls into Slang at runtime.
- `02_hotreload/`, `03_bindless_mesh/`, `04_streaming/`: binary + Slang
  runtime libs (`libslang-compiler.so*`/`slang-compiler.dll` +
  `slang-glslang`/`slang-glsl-module`/`slang-rt`) + the Slang `LICENSE` +
  each sample's own on-disk asset (`hotreload.slang` for 02, `texture.png`
  for 03; 04 has none). `slang-llvm`/`gfx` never appear anywhere.

Every expected file is existence-checked before copying (fails loudly, not
silently, if the build-output layout ever changes shape).

### 3. Real redistribution bug found and fixed: `sample_01_triangle`

While implementing the packaging requirement, found that
`sample_01_triangle` read its precompiled SPIR-V via a **hardcoded absolute
build-tree path** (`RX_TRIANGLE_VERT_SPV`/`RX_TRIANGLE_FRAG_SPV`, pointing at
`build/<preset>/shaders/...` on whichever machine built it) — a redistributed
copy run on any other machine would always fail at
`"failed to read compiled triangle shader SPIR-V"`. Fixed:

- `samples/01_triangle/CMakeLists.txt`: added a `POST_BUILD` copy step
  deploying the two `.spv` files next to the binary (same pattern
  `02_hotreload` already uses for `hotreload.slang`).
- `samples/01_triangle/main.cpp`: added `resolveSpvPath()` (same
  `SDL_GetBasePath()` mechanism `02_hotreload`/`03_bindless_mesh` already
  use), tried first, falling back to the old compile-time absolute path only
  for in-place build-tree runs.

Verified by copying just the binary + the two `.spv` files to an empty
directory and running it there — passed before this fix would have been
correct to add, and passes after.

### 4. Real pre-existing red main found and fixed: `upload_test.cpp`

`main` had been red for the last 3 pushes before this task started (Task 6's
and Task 7's own CI runs both failed identically — confirmed by pulling their
actual job logs via `gh api .../logs`). Same two assertions, same line
numbers, every time — not something this task's own changes caused, but
squarely inside "a red run gets fixed in-task."

Root cause: `src/rx_rhi_vk/tests/upload_test.cpp` had two `TEST_CASE`s that
constructed their destination buffer via `Allocator::createDeviceLocalBuffer()`
with transfer-only usage bits, on the documented assumption (`buffer.h`'s own
comment) that such a buffer can never measure `directPathCapable()==true`
"no matter the hardware." That held on this project's own development
machine's local lavapipe and on a real discrete GPU, but is **false** on
GitHub Actions' `ubuntu-latest` lavapipe/llvmpipe build specifically: it
exposes no non-host-visible `DEVICE_LOCAL` memory type at all, so VMA's soft
"steer away from `DEVICE_LOCAL`" preference (used when `usage` carries no
real device-consuming bit) has nothing better to steer toward, and the
buffer measures `DEVICE_LOCAL+HOST_VISIBLE` anyway — engaging the direct
path instead of the intended staging path, failing both tests' staging-
specific assertions.

Fixed both to use `createHostVisibleBuffer()` instead — its
`directPathCapable()` is a hardcoded `false` by construction, not a
measurement, which is the exact deterministic pattern a third test case in
the same file already modeled for this reason. Corrected the now-falsified
"no matter the hardware" doc comment on `createDeviceLocalBuffer()` in
`buffer.h` to describe the real, narrower guarantee. Verified clean on both
presets locally and confirmed the fix on the real GitHub Actions runner
(the same run this task ends on).

### 5. `samples/README.md` finalized

- New top-level "Downloading a prebuilt sample bundle" section describing the
  two CI zip artifacts and their exact per-sample layout.
- New "Redistribution" subsection for `01_triangle` (previously only
  02/03/04 had one), documenting the no-Slang-libs / two-`.spv`-files
  distinction.
- Corrected the Windows copy instructions for `01_triangle`, which
  previously (incorrectly, before the bug fix above) said "copy just that
  one `.exe`."

## Verify

- `ctest --preset linux-native --output-on-failure`: **9/9 passed** locally,
  both before and after every change in this task.
- Both presets rebuild cleanly (`cmake --build --preset ...`) after every
  change, including the `upload_test.cpp`/`buffer.h` fix.
- GitHub Actions run
  [31364667891](https://github.com/ywadi/renderer_x/actions/runs/31364667891):
  **both jobs green**. `linux-native`: Configure/Build/Test (9/9,
  xvfb+lavapipe)/Build budget (2s/60s)/Package/Upload — all green.
  `windows-cross-zig`: Configure/Build/Wine toolchain smoke check/Test under
  Wine (`rx_core_tests`, `rx_platform_tests`, `rx_shader_tests`,
  `shader_spirv_test` — `rx_shader_tests` confirmed passing under Wine on the
  real runner)/Build budget (3s/60s)/Package/Upload — all green.
- **Artifact spot-check** (the binding's own required proof): downloaded the
  actual `rendererx-samples-linux-x86_64` artifact from the green run above
  via `gh run download`, unzipped it to a scratch directory, and ran
  `sample_01_triangle` (headless) and `sample_02_hotreload` (headless) directly
  from that unzipped layout, with no build tree involved:
  - `sample_01_triangle` → `[info] triangle readback PASSED`, exit 0.
  - `sample_02_hotreload` → RUNPATH confirmed `$ORIGIN`, real in-process Slang
    compilation succeeded against the bundled `libslang-compiler.so` etc.,
    `[info] hotreload headless gate PASSED`, exit 0.
  - Confirmed via `find`: no `slang-llvm`/`gfx` artifact anywhere in either
    platform's zip; `LICENSE` present in every Slang-linking sample directory;
    `01_triangle/` carries no Slang libraries at all.
- Also downloaded and inspected `rendererx-samples-windows-x86_64.zip`:
  correct per-sample layout, all 4 DLLs + LICENSE present for 02/03/04, no
  `.pdb`/build bookkeeping leaked in.
- `git log --format='%B' e051006^..ca8f4be` grepped for
  `co-authored-by|claude|anthropic|generated with|ai-generated`: **zero
  matches** — both commits clean.

## Concerns for the coordinator

1. The `buffer.h`/`upload_test.cpp` fix, while directly necessitated by a
   real CI failure this task needed to clear, touches files outside this
   task's nominal file list (`.github/workflows/ci.yml` +
   `samples/README.md`). Flagged explicitly in both this report and the SDD
   ledger for Task 9's final whole-branch review to double check nothing
   else in the suite carries the same "no matter the hardware" assumption
   about VMA memory-type steering (my own grep across `rx_rhi_vk/tests/*.cpp`
   found no other instance, but a second pass may want to re-confirm).
2. Each sample directory inside a platform zip is fully self-contained (own
   copy of the ~34MB `libslang-compiler.so`/25MB `slang-compiler.dll` etc.),
   so the zips are ~136-165MB rather than a much smaller shared-lib layout
   would be. This is deliberate and matches the spec's own "per-sample
   directories... laid out exactly as a user would unzip-and-run them" and
   every sample's README "Redistribution" section's own claim that copying
   just that one sample's directory works standalone — flagging the size
   tradeoff explicitly in case the coordinator wants a shared-lib layout
   instead for the eventual `v0.2.0-phase2` GitHub Release in Task 9.
3. GitHub Actions' `ubuntu-latest` lavapipe/llvmpipe build exposing no
   non-host-visible `DEVICE_LOCAL` memory type (finding #4 above) is a
   genuinely interesting data point for the project's stated Steam Deck
   floor (also an integrated/unified-memory device, per the Phase 2 research
   file's B2) — worth keeping in mind for any future test that assumes a
   discrete-GPU-shaped memory layout.

## Files created

- `tools/package_samples.sh`

## Files modified

- `.github/workflows/ci.yml`
- `.gitignore`
- `samples/01_triangle/CMakeLists.txt`
- `samples/01_triangle/main.cpp`
- `samples/README.md`
- `src/rx_rhi_vk/include/rx_rhi_vk/buffer.h`
- `src/rx_rhi_vk/tests/upload_test.cpp`
