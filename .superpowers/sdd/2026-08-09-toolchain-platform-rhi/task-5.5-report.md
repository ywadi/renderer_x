# Task 5.5 Report: Fix windows-cross-zig missing RC compiler

## Status: DONE_WITH_CONCERNS

The RC-compiler bug that this ad-hoc task was scoped to fix is real, is now root-caused with
certainty (via direct CMake internals reading, not just log-reading), and is fixed and verified
by real, reproducible clean-state builds — including a real third-party CMake subbuild
(spdlog) and a second, larger one (SDL3) that itself calls `enable_language(RC)` directly.

Fixing it unblocked configure far enough to hit a **second, structurally unrelated,
pre-existing bug**: `rx_platform`'s `find_package(Vulkan REQUIRED)` cannot be satisfied when
cross-compiling to Windows, because no Windows-target Vulkan import library
(`vulkan-1.lib`/`.dll.a`) exists anywhere on this host (no Windows Vulkan SDK vendored or
installed) — confirmed directly: `Vulkan_LIBRARY-NOTFOUND` in `CMakeCache.txt`. This is not an
RC/resource-compiler problem, zig cross-compilation is not implicated at all, and a real fix
requires editing `src/rx_platform/CMakeLists.txt` (already-approved Task 5 code, outside this
task's authorized "purely additive to the toolchain file" scope) or `third_party/CMakeLists.txt`.
Per the brief's own escape valve ("if a proper fix would require... stop and report BLOCKED or
NEEDS_CONTEXT... rather than forcing a fragile workaround"), I did not hack around it (e.g. by
stuffing a bogus `Vulkan_LIBRARY` cache value into the toolchain file just to satisfy
`find_package_handle_standard_args`). See "Second bug found" below for full diagnosis and a
recommended fix path.

**Net result:** the actual RC-compiler fix is committed, correct, and proven — including proof
(via a throwaway, fully-reverted diagnostic run) that everything in the build graph *except*
`rx_platform` now builds cleanly end-to-end for `windows-cross-zig`. `windows-cross-zig` as a
whole still cannot configure cleanly today, solely because of the second, unrelated, out-of-scope
Vulkan bug. `linux-native` is reverified clean end-to-end (configure+build+test) after the fix.

---

## Root cause (verified, not assumed)

Reproduced the reported failure first, from a fully clean state:

```
rm -rf build/windows-cross-zig
cmake --preset windows-cross-zig
```

Real error text:

```
-- [dep-cache] MISS for spdlog (key=spdlog-b2d778a38067f2e7) - building once
-- The CXX compiler identification is Clang 21.1.0
-- Configuring incomplete, errors occurred!
See also ".../build/windows-cross-zig/_deps-build/spdlog/CMakeFiles/CMakeOutput.log".
CMake Error at /usr/share/cmake-3.22/Modules/CMakeDetermineRCCompiler.cmake:20 (message):
  Could not find compiler set in environment variable RC:

  zig-windres.
Call Stack (most recent call first):
  /usr/share/cmake-3.22/Modules/Platform/Windows-GNU.cmake:130 (enable_language)
  /usr/share/cmake-3.22/Modules/Platform/Windows-Clang.cmake:202 (__windows_compiler_gnu)
  /usr/share/cmake-3.22/Modules/Platform/Windows-Clang.cmake:212 (__windows_compiler_clang_base)
  /usr/share/cmake-3.22/Modules/Platform/Windows-Clang-CXX.cmake:3 (__windows_compiler_clang)
  /usr/share/cmake-3.22/Modules/CMakeCXXInformation.cmake:48 (include)
  CMakeLists.txt:13 (project)

CMake Error: CMAKE_RC_COMPILER not set, after EnableLanguage
CMake Error at cmake/DepCache.cmake:54 (message):
  [dep-cache] configure failed for spdlog - see output above
```

Confirmed the *mechanism*, not just the symptom, by reading the actual CMake 3.22 module
sources rather than guessing:

1. `CMakeDetermineCCompiler.cmake` (~line 165): because `CMAKE_C_COMPILER_ID` is `Clang`, CMake
   tries to infer a GNU cross-toolchain prefix from the compiler's **filename**, via regex
   `^(.+-)?(clang|g?cc)(-cl)?(...)?(-[^.]+)?(\.exe)?$`. The C compiler wrapper is named
   `zig-cc-windows`. That regex matches with group 1 (`_CMAKE_TOOLCHAIN_PREFIX`) = `"zig-"` and
   group 6 (suffix) = `"-windows"` — i.e. CMake mistakes `zig-cc-windows` for a real GNU
   cross-toolchain binary named `<prefix>-cc` with prefix `zig-`.
2. `Platform/Windows-GNU.cmake`'s `__windows_compiler_gnu` macro (line 126-128): when no
   `CMAKE_RC_COMPILER_INIT` has been set, it sets
   `CMAKE_RC_COMPILER_INIT = "${_CMAKE_TOOLCHAIN_PREFIX}windres"` — i.e. `"zig-windres"`, a
   binary that has never existed (zig has no such subcommand or binary).
3. This same macro *unconditionally* calls `enable_language(RC)` at the end (line 130) as part
   of enabling C/CXX for any GNU-ABI-like compiler once `CMAKE_SYSTEM_NAME=Windows` — regardless
   of whether any target in that CMake run actually compiles a `.rc` file. `CMakeDetermineRCCompiler.cmake`
   then `find_program()`s for `zig-windres`, fails, and aborts configure with `FATAL_ERROR`.
4. Because this happens on the *first* full C/CXX language-enable inside a Windows-GNU
   `project()` call, and `rx_add_cached_dependency` shells out to a completely separate `cmake`
   invocation per dependency (with its own fresh `project()`), the very first cached dependency
   processed (spdlog, alphabetically/declaration-order first in `third_party/CMakeLists.txt`)
   is where it always blows up — exactly matching the forensic finding from Task 5's review
   (`_deps-build/` containing only `spdlog`, no `build.ninja` at the top level).

This is precisely the brief's hypothesis, now confirmed against the actual CMake source, not just
plausible-sounding: **CMake's own Windows-GNU auto-detection resolves the RC compiler to a
nonexistent `zig-windres`, and nothing in the toolchain file ever overrides it.**

## The fix

Set `CMAKE_RC_COMPILER` explicitly in `cmake/toolchains/windows-cross-zig.cmake`, exactly the
way `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` already are, pointing at a new wrapper script
`cmake/zig-wrappers/zig-rc-windows` that calls zig's built-in resource compiler:

```
zig rc          Use Zig as a drop-in rc.exe
```

Per repo policy ("don't reinvent the wheel"), used this rather than hand-rolling anything.
Verified — did not assume — that it is genuinely a viable `CMAKE_RC_COMPILER`:

- `zig rc --help` confirms: *"Drop-in compatible with the Microsoft Resource Compiler"*, and
  supports the exact flags CMake's own default `CMAKE_RC_COMPILE_OBJECT` rule passes
  (`<CMAKE_RC_COMPILER> <DEFINES> <INCLUDES> <FLAGS> /fo <OBJECT> <SOURCE>`): `/fo <output>`,
  `-D`/`/d <name>[=value]`, `-I`/`/i <path>`.
- Directly test-invoked it outside CMake before wiring it in:
  `zig rc -DFOO=1 -I. /fo test2.res test2.rc` → exit 0, produced a real `MSVC .res` file
  (confirmed with `file(1)`).
- Confirmed *why* this specific command-line shape is what CMake will actually generate: since
  the wrapper's basename (`zig-rc-windows`) doesn't match CMake's `windres[^/]*$` pattern,
  `CMakeRCInformation.cmake` falls through to its default MS-rc.exe-style rule and `.res` output
  extension — which is exactly the resinator/`zig rc` dialect, not the windres dialect. (Setting
  `CMAKE_RC_COMPILER` explicitly, as a forced cache variable in the toolchain file, also means
  `CMakeDetermineRCCompiler.cmake`'s `if(NOT CMAKE_RC_COMPILER)` guard is never entered, so the
  broken `zig-windres` auto-detection path is never reached at all — this is why the fix works
  and is not merely papering over the symptom.)
- The wrapper also passes `/:target x86_64` explicitly (mirroring how `zig-cc-windows`/
  `zig-cxx-windows` hardcode `-target x86_64-windows-gnu`) rather than relying on `zig rc`'s
  default, since this project's `RX_TARGET_TRIPLE` is `x86_64-windows-gnu` and being explicit is
  more robust than depending on a tool default that could change.

`cmake/zig-wrappers/zig-rc-windows` (new file, mirrors the existing `zig-cc-windows`/
`zig-cxx-windows` pattern exactly):

```bash
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
exec "$REPO_ROOT/toolchain/zig/zig" rc /:target x86_64 "$@"
```

`cmake/toolchains/windows-cross-zig.cmake` (modified — added, nothing removed or reordered):

```cmake
set(CMAKE_RC_COMPILER  "${RX_REPO_ROOT}/cmake/zig-wrappers/zig-rc-windows"  CACHE FILEPATH "RC compiler" FORCE)
```

(plus an explanatory comment block placed immediately above it — see the diff in "Files
changed" below).

I also independently confirmed why this matters going forward, not just for spdlog: SDL3's own
`CMakeLists.txt` (fetched into `_deps-src/SDL3` by the dependency cache) calls
`enable_language(RC)` **directly** itself at line 2487 (unconditionally on `WIN32`, regardless of
static/shared), and globs `src/core/windows/*.rc` for `SHARED` builds only — so today, with
`SDL_SHARED=OFF`, no `.rc` is actually compiled by SDL3, but the language still has to be enabled
successfully, which it now is.

## Second bug found (out of scope for this task, not fixed)

Fixing the RC bug let configure proceed past spdlog and into SDL3, and then into `rx_core`
(clean) and `rx_platform`. It fails there, on a real, reproducible, and completely unrelated
error:

```
CMake Error at /usr/share/cmake-3.22/Modules/FindPackageHandleStandardArgs.cmake:230 (message):
  Could NOT find Vulkan (missing: Vulkan_LIBRARY)
Call Stack (most recent call first):
  /usr/share/cmake-3.22/Modules/FindPackageHandleStandardArgs.cmake:594 (_FPHSA_FAILURE_MESSAGE)
  /usr/share/cmake-3.22/Modules/FindVulkan.cmake:129 (find_package_handle_standard_args)
  src/rx_platform/CMakeLists.txt:1 (find_package)
```

Root cause, confirmed by reading `FindVulkan.cmake` and `CMakeCache.txt` directly:

- `src/rx_platform/CMakeLists.txt` does `find_package(Vulkan REQUIRED)` and links only
  `Vulkan::Headers` (a header-only interface target) — it never actually links the real Vulkan
  loader library.
- On this CMake version (3.22), `Vulkan::Headers` is only created `if(Vulkan_FOUND ...)`
  (`FindVulkan.cmake` line 142), and `Vulkan_FOUND` is gated on **both**
  `Vulkan_INCLUDE_DIR` **and** `Vulkan_LIBRARY` being resolved
  (`find_package_handle_standard_args(Vulkan DEFAULT_MSG Vulkan_LIBRARY Vulkan_INCLUDE_DIR)`,
  line 129-131) — there is no `COMPONENTS Headers`-only bypass available in this CMake version.
- For `WIN32` (true here, since the toolchain sets `CMAKE_SYSTEM_NAME=Windows`), `FindVulkan.cmake`
  does `find_library(Vulkan_LIBRARY NAMES vulkan-1 HINTS "$ENV{VULKAN_SDK}/Lib" ...)`. There is no
  `VULKAN_SDK` environment variable set, and no Windows-target Vulkan import library
  (`vulkan-1.lib`/`.dll.a`) exists anywhere on this Linux host — there's no Windows Vulkan SDK
  vendored in the repo or installed on the machine. Confirmed directly in
  `build/windows-cross-zig/CMakeCache.txt`:
  ```
  Vulkan_INCLUDE_DIR:PATH=/usr/include
  Vulkan_LIBRARY:FILEPATH=Vulkan_LIBRARY-NOTFOUND
  ```
  (`Vulkan_INCLUDE_DIR` incidentally resolved to the *host's* `/usr/include/vulkan` — harmless
  since Vulkan headers are pure, platform-agnostic C headers, but worth flagging as a latent
  cross-contamination smell: nothing in the toolchain file currently constrains `find_path` to
  avoid host system directories.)

**Why I did not fix this as part of Task 5.5:**

- It is not an RC-compiler / resource-compiler issue at all, and zig's cross-compilation
  behavior is not implicated — this would reproduce identically with any Windows-GNU cross
  toolchain on a host with no Windows Vulkan SDK.
- A real fix requires editing `src/rx_platform/CMakeLists.txt` (e.g. switching from
  `find_package(Vulkan REQUIRED)` to fetching `KhronosGroup/Vulkan-Headers` directly as a
  lightweight, header-only, per-repo-policy "ready-made" dependency that needs no import library
  on any platform) and/or `third_party/CMakeLists.txt`. Both are outside this task's stated file
  scope ("the fix must be purely additive to the Windows toolchain file").
- The project's own roadmap already has the correct long-term answer: **volk** (Task 6), a
  Vulkan meta-loader specifically designed so nothing needs to link against `vulkan-1.lib`/
  `libvulkan.so` at all — it's listed in this repo's own "ready-made libraries" policy alongside
  SDL3, vk-bootstrap, VMA, GLM, spdlog, doctest, and Slang. Fabricating a stub Windows import
  library right now (e.g. via `zig dlltool`/`zig lib` from a hand-written `.def` file) would be
  exactly the kind of hand-rolled workaround the repo's "don't reinvent the wheel" policy warns
  against, when a proper, already-planned solution (volk) exists.
- The brief's own guidance explicitly covers this situation: "If... a proper fix would require
  re-architecting the toolchain file setup, stop and report BLOCKED or NEEDS_CONTEXT... rather
  than forcing a fragile workaround." This is exactly that case, just discovered one layer
  deeper than the brief anticipated (only visible once the RC bug is actually fixed).

**Recommendation:** open a small follow-up ad-hoc task (e.g. Task 5.6, or fold into Task 6's
scope) to replace `rx_platform`'s `find_package(Vulkan REQUIRED)` with a direct
`FetchContent`/`rx_add_cached_dependency` of `Vulkan-Headers` (header-only, no library, works
identically cross-platform), removing the accidental dependency on a system-installed Vulkan SDK
entirely — this both fixes the Windows cross-compile gap and removes a latent Linux-side
assumption (that a system Vulkan loader happens to be installed) that Task 5 got away with by
accident.

## Fix verification

### Diagnostic proof the RC fix itself is complete (not partially masking the real bug)

Before accepting "blocked by a second bug" at face value, I temporarily commented out
`add_subdirectory(src/rx_platform)` in the top-level `CMakeLists.txt` (the only subdirectory that
touches Vulkan) to see how far the rest of the build graph gets with only the RC fix applied.
This change was **never committed** and was fully reverted immediately after (confirmed via
`git diff CMakeLists.txt` showing no diff before committing anything).

With `rx_platform` excluded, from a fully clean state (`build/windows-cross-zig` and
`.deps-cache` wiped):

```
$ cmake --preset windows-cross-zig
...
-- Installing: .../SDL3-56eca002d5d1e303/share/licenses/SDL3/LICENSE.txt
-- Configuring done
-- Generating done
-- Build files have been written to: /media/ywadi/second/renderer_x/build/windows-cross-zig

$ cmake --build --preset windows-cross-zig
[1/15] Building CXX object tools/toolchain_check/CMakeFiles/toolchain_check.dir/main.cpp.obj
[2/15] Linking CXX executable tools/toolchain_check/toolchain_check.exe
[3/15] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/handle_test.cpp.obj
[4/15] Building CXX object src/rx_core/CMakeFiles/rx_core.dir/src/log.cpp.obj
[5/15] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/math_test.cpp.obj
[6/15] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/log_test.cpp.obj
[7/15] Building CXX object tools/dep_cache_smoketest/CMakeFiles/dep_cache_smoketest.dir/main.cpp.obj
[8/15] Linking CXX executable tools/dep_cache_smoketest/dep_cache_smoketest.exe
[9/15] Building CXX object _deps/doctest-build/CMakeFiles/doctest_with_main.dir/doctest/doctest.cpp.obj
[10/15] Linking CXX static library _deps/doctest-build/libdoctest_with_main.a
[11/15] Building CXX object _deps/glm-build/glm/CMakeFiles/glm.dir/detail/glm.cpp.obj
[12/15] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/doctest_main.cpp.obj
[13/15] Linking CXX static library _deps/glm-build/glm/libglm.a
[14/15] Linking CXX static library src/rx_core/librx_core.a
[15/15] Linking CXX executable src/rx_core/rx_core_tests.exe
```

Every target that can exist without touching Vulkan — `toolchain_check.exe`,
`dep_cache_smoketest.exe`, `rx_core.a`, `rx_core_tests.exe` (statically linking spdlog and glm,
both cross-compiled) — builds cleanly cross-compiled to Windows. This confirms the RC fix is
complete and sufficient on its own merits; the only remaining blocker is the Vulkan-library gap
in `rx_platform`, isolated and confirmed unrelated to RC/zig cross-compilation.

### windows-cross-zig: real (non-diagnostic) clean-state transcript, current repo state

```
$ rm -rf build/windows-cross-zig .deps-cache
$ cmake --preset windows-cross-zig
Preset CMake variables:
  CMAKE_BUILD_TYPE="RelWithDebInfo"
  CMAKE_TOOLCHAIN_FILE:FILEPATH="/media/ywadi/second/renderer_x/cmake/toolchains/windows-cross-zig.cmake"

-- The C compiler identification is Clang 21.1.0
-- The CXX compiler identification is Clang 21.1.0
-- Detecting C compiler ABI info
-- Detecting C compiler ABI info - done
-- Check for working C compiler: /media/ywadi/second/renderer_x/cmake/zig-wrappers/zig-cc-windows - skipped
...
-- [dep-cache] MISS for spdlog (key=spdlog-b2d778a38067f2e7) - building once
   [... spdlog's own subbuild: configures cleanly, RC language enables cleanly, builds, installs ...]
-- Configuring done
-- Generating done
-- [dep-cache] MISS for SDL3 (key=SDL3-56eca002d5d1e303) - building once
   [... SDL3's own subbuild: configures cleanly (including its own `enable_language(RC)` call),
        builds all 268 targets, links libSDL3.a, installs into .deps-cache ...]
-- Configuring done
-- Generating done
-- Installing: /media/ywadi/second/renderer_x/.deps-cache/SDL3-56eca002d5d1e303/share/licenses/SDL3/LICENSE.txt
CMake Error at /usr/share/cmake-3.22/Modules/FindPackageHandleStandardArgs.cmake:230 (message):
  Could NOT find Vulkan (missing: Vulkan_LIBRARY)
Call Stack (most recent call first):
  /usr/share/cmake-3.22/Modules/FindPackageHandleStandardArgs.cmake:594 (_FPHSA_FAILURE_MESSAGE)
  /usr/share/cmake-3.22/Modules/FindVulkan.cmake:129 (find_package_handle_standard_args)
  src/rx_platform/CMakeLists.txt:1 (find_package)

-- Configuring incomplete, errors occurred!
```

Both spdlog and SDL3 — real, independent CMake subbuilds, cross-compiled to
`x86_64-windows-gnu`, one of which (SDL3) itself calls `enable_language(RC)` — complete
successfully and get installed into `.deps-cache/`. This is unambiguous proof the RC-compiler bug
is fixed. The `_deps-build/` forensic signature from the original bug report (only `spdlog`
present, no `build.ninja`) no longer occurs — configure now gets substantially further, all the
way to `rx_platform`, before hitting the second (unrelated) bug. `cmake --build` cannot be run at
all, because top-level configure never completes (`Configuring incomplete, errors occurred!` — no
`build.ninja` is ever generated at the top level), so I could not run a build/test pass for the
full preset; this is the direct, confirmed, sole consequence of the second bug, not the RC bug.

### linux-native: fully clean, unrelated to this fix, verified reverified

Per "must not break", re-verified from a fully clean state (`build/linux-native` and
`.deps-cache` both wiped) — this exercises the exact same `windows-cross-zig.cmake` diff's
sibling file (`linux-native.cmake`, untouched) plus the shared `DepCache.cmake`/
`third_party/CMakeLists.txt` machinery, so it's a real regression check, not "I didn't touch that
file":

```
$ rm -rf build/linux-native .deps-cache
$ cmake --preset linux-native
...
-- [dep-cache] MISS for spdlog (key=spdlog-c3c3e6d45a2d5f37) - building once
-- Configuring done
-- Generating done
-- [dep-cache] MISS for SDL3 (key=SDL3-216994ad42ea7d90) - building once
-- Configuring done
-- Generating done
-- Found Vulkan: /usr/lib/x86_64-linux-gnu/libvulkan.so
-- Configuring done
-- Generating done
-- Build files have been written to: /media/ywadi/second/renderer_x/build/linux-native

$ cmake --build --preset linux-native
[1/19] Building CXX object tools/dep_cache_smoketest/CMakeFiles/dep_cache_smoketest.dir/main.cpp.o
[2/19] Linking CXX executable tools/dep_cache_smoketest/dep_cache_smoketest
[3/19] Building CXX object tools/toolchain_check/CMakeFiles/toolchain_check.dir/main.cpp.o
[4/19] Linking CXX executable tools/toolchain_check/toolchain_check
[5/19] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/math_test.cpp.o
[6/19] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/log_test.cpp.o
[7/19] Building CXX object src/rx_core/CMakeFiles/rx_core.dir/src/log.cpp.o
[8/19] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/handle_test.cpp.o
[9/19] Building CXX object src/rx_platform/CMakeFiles/rx_platform.dir/src/window.cpp.o
[10/19] Building CXX object _deps/glm-build/glm/CMakeFiles/glm.dir/detail/glm.cpp.o
[11/19] Linking CXX static library _deps/glm-build/glm/libglm.a
[12/19] Linking CXX static library src/rx_core/librx_core.a
[13/19] Linking CXX static library src/rx_platform/librx_platform.a
[14/19] Building CXX object _deps/doctest-build/CMakeFiles/doctest_with_main.dir/doctest/doctest.cpp.o
[15/19] Linking CXX static library _deps/doctest-build/libdoctest_with_main.a
[16/19] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/doctest_main.cpp.o
[17/19] Linking CXX executable src/rx_core/rx_core_tests
[18/19] Building CXX object src/rx_platform/CMakeFiles/rx_platform_tests.dir/tests/window_test.cpp.o
[19/19] Linking CXX executable src/rx_platform/rx_platform_tests

$ ctest --preset linux-native
Test project /media/ywadi/second/renderer_x/build/linux-native
    Start 1: rx_core_tests
1/2 Test #1: rx_core_tests ....................   Passed    0.00 sec
    Start 2: rx_platform_tests
2/2 Test #2: rx_platform_tests ................   Passed    0.10 sec

100% tests passed, 0 tests failed out of 2

Total Test time (real) =   0.10 sec
```

`linux-native` configures, builds, and tests cleanly from a fully clean state after the fix,
identically to before it — the fix is purely additive to `windows-cross-zig.cmake` and does not
touch `linux-native.cmake` or any shared machinery's Linux-relevant behavior.

Full raw logs for every run above are preserved at:
- `/tmp/claude-1000/-media-ywadi-second-renderer-x/4b4c7bcd-4ad2-40e4-943f-716728851453/scratchpad/win-configure.log` (first real repro, post-fix)
- `/tmp/claude-1000/-media-ywadi-second-renderer-x/4b4c7bcd-4ad2-40e4-943f-716728851453/scratchpad/win-configure-final.log` (final real repro, post-fix, matches the excerpt above)
- `/tmp/claude-1000/-media-ywadi-second-renderer-x/4b4c7bcd-4ad2-40e4-943f-716728851453/scratchpad/win-configure-diag.log`, `win-build-diag.log` (throwaway `rx_platform`-excluded diagnostic, `CMakeLists.txt` reverted immediately after)
- `/tmp/claude-1000/-media-ywadi-second-renderer-x/4b4c7bcd-4ad2-40e4-943f-716728851453/scratchpad/linux-configure-final.log`, `linux-build-final.log`, `linux-ctest-final.log` (final linux-native clean pass)

## Files changed

- `cmake/toolchains/windows-cross-zig.cmake` (modified) — added `CMAKE_RC_COMPILER` cache
  setting + explanatory comment. Nothing removed, nothing reordered, `linux-native.cmake`
  untouched.
- `cmake/zig-wrappers/zig-rc-windows` (new, executable) — thin wrapper around `zig rc`, mirrors
  `zig-cc-windows`/`zig-cxx-windows` exactly.

No other files modified. (`CMakeLists.txt` was edited for a throwaway diagnostic and fully
reverted before any commit — confirmed via `git diff` showing zero changes to it.)

## Self-review

- **Completeness:** `windows-cross-zig` is verified from a totally clean state (`.deps-cache`
  wiped, per the brief's exact command) through the actual RC-compiler failure point and past it
  — proven via two independent real CMake subbuilds (spdlog, SDL3) succeeding. It is **not**
  verified end-to-end through a successful `cmake --build`, because a second, unrelated,
  pre-existing bug blocks configure before a `build.ninja` is ever generated. `linux-native` is
  verified fully clean end-to-end (configure+build+test), twice.
- **Quality:** root cause is genuinely understood — traced through the actual CMake 3.22 module
  source (`CMakeDetermineCCompiler.cmake`'s toolchain-prefix regex, `Windows-GNU.cmake`'s
  unconditional `enable_language(RC)`, `CMakeDetermineRCCompiler.cmake`'s `find_program` failure),
  not just pattern-matched against the brief's hypothesis. The fix's mechanism of action
  (bypassing the broken auto-detection entirely via a pre-set `CACHE FORCE` variable, not just
  supplying *a* working binary) is also verified, not assumed.
- **Discipline:** the fix touches exactly the two files the brief anticipated
  (`windows-cross-zig.cmake` + one new wrapper script). I did not touch
  `src/rx_platform/CMakeLists.txt` or `third_party/CMakeLists.txt` to work around the second bug,
  even though doing so was tempting and would have made the top-level status look better on
  paper — per the brief's own instruction to stop rather than force a fragile workaround. The one
  extra file edited during investigation (`CMakeLists.txt`, to isolate the second bug) was never
  committed and is confirmed fully reverted.
- **Testing:** both transcripts above are real, captured verbatim from actual command runs in
  this session (not reconstructed from memory), with full raw logs preserved on disk at the paths
  listed above.

## Concerns

1. **`windows-cross-zig` still cannot configure+build cleanly end-to-end today.** The RC bug —
   this task's actual subject — is fixed and proven. The remaining blocker (Vulkan library
   resolution for the Windows cross target) is real, pre-existing, and unrelated; seeing it only
   became possible after this fix. This is the most important thing to flag: the literal "fix
   requirement" bar in the brief (full clean configure+build success) is not met, for a reason
   outside this task's authorized scope.
2. Recommend a follow-up ad-hoc task (proposed: Task 5.6, or fold into Task 6's scope) to replace
   `rx_platform`'s `find_package(Vulkan REQUIRED)` with a direct fetch of `Vulkan-Headers`
   (header-only, no import library needed on any platform) — this fixes the Windows gap and also
   removes an accidental dependency on a system-installed Linux Vulkan SDK that Task 5 happened to
   get away with.
3. Minor, unrelated to either bug: `FindVulkan.cmake`'s `find_path(Vulkan_INCLUDE_DIR)` resolved
   to the *host's* `/usr/include` while cross-compiling to Windows (harmless today since Vulkan
   headers are portable C headers with no platform-conditional linking, but worth someone's
   attention if the toolchain file ever adds `CMAKE_FIND_ROOT_PATH`/sysroot constraints later —
   that could change which headers various `find_path` calls resolve to across both presets).
