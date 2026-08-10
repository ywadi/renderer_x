# Task 1 Report: Slang runtime linking + ShaderCompiler (rx_shader)

## Summary

Implemented the full Task 1 scope: reworked `tools/fetch_slang.cmake` for
version-keyed markers and a target-side Windows archive fetch, empirically
proved the Windows link smoke test (the plan's risk gate) both links AND
runs correctly under Wine, built the `rx_shader` module (`Compiler` +
`CompileResult` exactly per the brief's interface, including the opaque
retained `IComponentType` for Task 2), wrote real tests exercising the
actual Slang compiler (no mocks), and wired runtime Slang-library placement
(RPATH `$ORIGIN` + build-time copy) proven by `rx_shader_tests` running
straight from the build tree. Both presets configure, build, and test
clean. One real, previously-unknown toolchain defect was found and fixed
along the way (see "Findings" below) — not a plan-level blocker, but
load-bearing for anyone else linking a versioned `.so` through this
project's zig toolchain.

## Windows link smoke test (risk gate) — PASSED, with a bonus result

Per the brief, this was done first, before writing any of the real
Compiler implementation. A trivial TU (`src/rx_shader/tests/link_smoketest.cpp`)
calling `slang::createGlobalSession`, built via the `windows-cross-zig`
preset, links cleanly against the MSVC-built `slang-compiler.lib` through
zig/LLD — no linker errors. Empirically verified twice: once as a
standalone `zig c++` invocation in isolation (before touching the real
CMake files), and again as the actual `rx_shader_link_smoketest` target
inside the real build.

Beyond what the gate required (link success only), I also copied the
required DLLs next to the built `.exe` and ran it under Wine 11.0 (present
on this machine) — it executes correctly and returns 0. I went one step
further and ran the *entire* `rx_shader_tests.exe` (real Slang
compilation, not just `createGlobalSession`) under Wine too: all 7 test
cases / 40 assertions pass identically to the Linux run. This is directly
relevant to Task 8's open question ("wine-run the non-GPU test set... if
[the Slang DLLs] don't [load under wine], exclude... investigate") — on
this machine, with this Slang version, they do load and work under Wine,
including the on-demand `slang-glslang`/`slang-glsl-module` plugins
exercised by the bad-source-diagnostics test path. This is one data point,
not exhaustive CI-environment coverage, so Task 8 should still verify on
the actual CI runner rather than take this as settled.

## Findings / discrepancies vs. the research doc

1. **zig cc rejects a SONAME-versioned `.so` path as a link input (new
   finding, not in the research doc).** Slang's shipped
   `slangTargets-release.cmake` points `slang::slang`'s
   `IMPORTED_LOCATION_RELEASE` directly at
   `lib/libslang-compiler.so.0.2026.14.1`. Linking this via this project's
   `zig-cxx-linux` wrapper failed with `unrecognized file extension` — a
   real limitation in zig 0.16.0's driver-level file-type sniffing (it
   doesn't recognize a SONAME-style multi-dot version suffix as a valid
   direct link input, unlike gcc/clang's drivers, and unlike LLD itself,
   which has no such restriction). Confirmed directly with a minimal
   repro (`zig c++ trivial.cpp -o out lib/libslang-compiler.so.0.2026.14.1`
   fails; the same command with the plain `libslang-compiler.so` symlink
   succeeds). Fixed in `src/rx_shader/CMakeLists.txt` by retargeting
   `IMPORTED_LOCATION_RELEASE` at that plain-named symlink (same inode,
   same embedded `DT_SONAME`, so the consuming binary's actual runtime
   dependency — and therefore the runtime-lib copy step — is completely
   unaffected; only the path fed to the compiler driver at link time
   changes). This did not affect the standalone Linux API-validation
   smoke test earlier in this task because that used plain `g++`, not the
   project's zig wrapper — worth flagging since it means "compiles with
   g++" is not sufficient evidence for "compiles in this project."
2. **`findCapability` uses lowercase `spirv_1_3`, not the uppercase
   `SPIRV_1_3` also present in `slangc -h capability`'s output.** Both
   exist; `slangc -h capability` explicitly documents `spirv_1_{0..5}`
   (lowercase) as "minimum supported SPIR-V version," which is the
   documented semantic this task needs (matches R:A5's "capability
   spirv_1_3" exactly) — used that one. The uppercase `SPIRV_1_x` family
   appears to be a different capability-atom namespace; not investigated
   further since it wasn't needed.
3. Everything else in R:A1/A2/A4/A5/A6/D2 matched the real shipped
   `slang.h`/archives exactly as documented — API signatures, archive
   layouts (including the Windows `cmake/` vs Linux `lib/cmake/slang/`
   split), threading rules, and the COM-lite cross-ABI linking claim all
   verified directly against the real v2026.14.1 archives (both Linux and
   a freshly-downloaded Windows one) before writing any code against them.

## Implementation

### 1. `tools/fetch_slang.cmake` (reworked)

- Marker is now `.rx-fetched-<version>`; the old unversioned
  `.rx-fetched` is never consulted. A missing/stale versioned marker wipes
  the whole platform directory (`file(REMOVE_RECURSE)`) before re-fetching
  — closes the Phase 1 deferred finding (a version bump previously left a
  stale marker in place and silently kept old binaries).
- Host-side `slangc` fetch is unchanged in spirit (still guards on
  `CMAKE_HOST_SYSTEM_NAME`, still Linux-only today) but now goes through a
  shared `_rx_fetch_slang_platform()` function.
- New target-side fetch: when `CMAKE_SYSTEM_NAME STREQUAL "Windows"`
  (true under the `windows-cross-zig` toolchain even though the fetch
  itself still runs on this Linux host), additionally fetches
  `slang-2026.14.1-windows-x86_64.tar.gz` into
  `third_party/slang-prebuilt/windows-x86_64/`. On native builds, target
  == host, so no second download.
- Exposes `RX_SLANG_TARGET_ROOT` and `slang_ROOT` (both cache vars, the
  latter consulted automatically by `find_package(slang CONFIG)` via
  CMP0074) so `src/rx_shader/CMakeLists.txt` needs no per-platform
  `PATHS`/`NO_DEFAULT_PATH` branching — verified CMake's Config-mode search
  always tries both the `<prefix>/(cmake|CMake)/` (Windows layout) and
  `<prefix>/(lib/*|share)/cmake/<name>*/` (Linux layout) forms regardless
  of host OS.

### 2. `src/rx_shader` (new module)

- `include/rx_shader/compiler.h` / `src/compiler.cpp`: `Compiler::create()`
  lazily creates one process-lifetime `slang::IGlobalSession` (mutex-guarded
  static, never explicitly torn down) shared by every `Compiler` instance
  in the process, then creates a per-instance `ISession` targeting SPIR-V
  with an explicit `spirv_1_3` capability floor (graceful fallback +
  logged warning if a future Slang version ever renames that capability).
  `compileFromSource`/`compileFromFile` share one pipeline: load module →
  find each entry point by name → `createCompositeComponentType` → `link`
  → `getLayout` (for per-entry-point stage) → `getEntryPointCode` per
  entry point. Diagnostics from every step are concatenated and always
  captured in `CompileResult::diagnostics`, logged via `RX_LOG_WARN` on
  success-with-diagnostics or `RX_LOG_ERROR` on failure — never swallowed.
  The linked `IComponentType` is retained in `CompileResult::linkedProgram`
  on success (null on failure) for Task 2.
- `CMakeLists.txt`: `find_package(slang CONFIG REQUIRED)` (no manual
  `PATHS`), promotes `slang::slang` to `IMPORTED_GLOBAL` (needed by sibling
  directories the same way spdlog/Vulkan-Headers/SDL3/vk-bootstrap already
  are), the zig-cc `IMPORTED_LOCATION_RELEASE` workaround from Finding #1,
  a `rx_shader_deploy_runtime_libs()` function (glob-based library
  discovery so a future version bump doesn't require editing filenames,
  `BUILD_WITH_INSTALL_RPATH`/`INSTALL_RPATH="$ORIGIN"` + POST_BUILD copy),
  applied to both `rx_shader_link_smoketest` and `rx_shader_tests`.
- Tests (`tests/compiler_test.cpp`, real execution, no mocks): known-good
  vertex+fragment module → 2 SPIR-V blobs, magic `0x07230203`, correct
  `VK_SHADER_STAGE_VERTEX_BIT`/`VK_SHADER_STAGE_FRAGMENT_BIT`, retained
  `linkedProgram`; deliberately-broken source → `ok=false` + diagnostics
  containing the literal offending source line; two compiles through the
  same `Compiler` (session reuse) with a generous timing sanity bound;
  two independent `Compiler::create()` calls (process-wide global-session
  reuse) with the same bound; `compileFromFile` round-trip via a temp
  file; `compileFromFile` on a nonexistent path fails cleanly. 7 test
  cases, 40 assertions.
- `CMakeLists.txt` (root): added `add_subdirectory(src/rx_shader)`.

### 3. Runtime lib placement — proof

`rx_shader_tests`' `RUNPATH` is verified (via `readelf -d`) to be exactly
`$ORIGIN` (not an absolute path into `third_party/`), and `ldd` resolves
`libslang-compiler.so.0.2026.14.1` to the copy sitting next to the binary
in the build tree. `rx_shader_tests` runs from that build-tree location
under `ctest` — this is the actual proof the mechanism works, not just
that it's configured. Same mechanism verified on Windows: the four
required DLLs (`slang-compiler`, `slang-glslang`, `slang-glsl-module`,
`slang-rt` — `slang-llvm`/`gfx` correctly excluded) land next to the
`.exe` files, and `rx_shader_tests.exe` runs correctly under Wine straight
from that location.

## Verification

- **linux-native, full clean build from scratch:** 12.2s wall, zero
  warnings under `-Wall -Wextra` (checked manually against the real
  compile-database flags for both new source files; the project itself
  enables no warning flags anywhere, so this was an extra check, not a
  build requirement). `ctest --preset linux-native`: **6/6 tests pass**
  (`shader_spirv_test`, `rx_core_tests`, `rx_platform_tests`,
  `rx_rhi_vk_tests`, `rx_shader_tests`, `sample_01_triangle_headless`).
- **windows-cross-zig, full clean configure+build from scratch:** 14.7s
  wall, zero errors, all 47 build targets including
  `rx_shader_link_smoketest.exe` and `rx_shader_tests.exe`.
  `ctest --preset windows-cross-zig -E 'rx_rhi_vk|sample'` (same exclusion
  CI already uses, since Wine here has no real Vulkan device): **4/4
  pass**, including `rx_shader_tests`. Additionally ran
  `rx_shader_tests.exe` and `rx_shader_link_smoketest.exe` directly under
  Wine outside ctest — both pass/exit 0.
- Phase 1's `slangc`-based offline SPIR-V path (`shader_spirv_test`,
  `triangle.{vert,frag}.spv`) still works unmodified on both presets.
- Fetch rework verified live: the reworked script re-fetched the Linux
  archive fresh (old unversioned marker correctly ignored and removed)
  and fetched the Windows archive for the first time when
  `windows-cross-zig` was configured; both platform trees' version-keyed
  markers (`.rx-fetched-2026.14.1`) are the only markers present
  afterward.

## Deviations from brief / spec

None. No coordinator sign-off was needed — no Fixed Decision required
deviation. The zig-cc linker-driver workaround (Finding #1) is an
implementation detail needed to make Fixed Decision #1 ("link
`slang::slang`... via zig/LLD") actually work under this project's
specific toolchain; it doesn't change what gets linked or shipped, only
which on-disk path the compiler driver is handed.

## Concerns for the coordinator

1. **Pre-existing, unrelated to this task:**
   `.superpowers/sdd/2026-08-09-toolchain-platform-rhi/task-4-report.md`
   (tracked in git, part of this repo's history since commit `4c6ba3e`,
   "Track .superpowers/ SDD workspace in git instead of ignoring it')
   contains a "Full commit message" example block with literal AI
   attribution text (`Co-Authored-By: Claude...` / `Claude-Session:
   https://claude.ai/...`). I confirmed the *actual* commit
   `git log --format='%B'` shows for that content today (`b508684`, "Add
   rx_core...") carries no such text — the live git history is clean, so
   the attribution text only exists inside that one report file's
   markdown content, not in any commit's actual metadata. I did not touch
   that file (out of scope for this task, and it's someone else's task
   record), but since the repo's own policy is "no AI attribution... in
   this repo's history **or remote-visible content**" and that file is
   tracked/pushed, it's worth the coordinator's attention.
2. **CI cache-key overlap (not exercised by this task; flagging for Task
   8):** `.github/workflows/ci.yml` caches `third_party/slang-prebuilt`
   under the identical key
   (`slang-prebuilt-${{ hashFiles('tools/fetch_slang.cmake') }}`) in both
   the `linux-native` and `windows-cross-zig` jobs. Before this task, both
   jobs populated the same single `linux-x86_64/` subtree, so this was
   harmless. Now the `windows-cross-zig` job also populates a second
   `windows-x86_64/` subtree under the same cache path/key — whichever
   job's cache-save completes first "wins" that key, so the other job may
   not get a hit for its own platform's subtree on a cold cache and will
   just re-fetch it directly from GitHub (self-healing, not a correctness
   bug, just a minor CI time cost on some runs). Not fixed here since it's
   pure CI wiring outside this task's file list; Task 8 (which does own
   CI) should consider keying by target platform.
3. `rx_shader`'s Windows-target build was verified via `wine`-run tests on
   this development machine, not on the actual GitHub Actions Windows-cross
   runner; Task 8 should still re-verify there since Wine versions/configs
   can differ.

## Files created

- `src/rx_shader/include/rx_shader/compiler.h` (98 lines)
- `src/rx_shader/src/compiler.cpp` (332 lines)
- `src/rx_shader/CMakeLists.txt` (158 lines)
- `src/rx_shader/tests/compiler_test.cpp` (186 lines)
- `src/rx_shader/tests/doctest_main.cpp` (7 lines)
- `src/rx_shader/tests/link_smoketest.cpp` (21 lines)

## Files modified

- `tools/fetch_slang.cmake` (rewritten: 165 insertions, 63 deletions)
- `CMakeLists.txt` (+1 line: `add_subdirectory(src/rx_shader)`)

## Readiness for Task 2

`rx_shader`'s `CompileResult::linkedProgram` (a
`Slang::ComPtr<slang::IComponentType>`) is the exact lifetime-anchored
handle Task 2's `reflect()` needs to call `->getLayout(...)` on. `rx_shader`
already links `Vulkan::Headers` publicly (needed for `VkShaderStageFlagBits`
today; Task 2's `ShaderLayoutInfo`/`VkDescriptorType` mapping can use the
same include without new plumbing). Task 2's `reflection_test.cpp` can join
the existing `rx_shader_tests` binary via the existing `doctest_main.cpp`
(no changes needed there).
