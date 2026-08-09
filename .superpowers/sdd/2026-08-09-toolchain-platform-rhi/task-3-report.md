# Task 3 report: Dependency cache module, proven with spdlog

## Status: DONE

## What was implemented

- `cmake/DepCache.cmake` — `rx_dep_cache_key()` and `rx_add_cached_dependency()`,
  copied verbatim from the brief. Cache key is
  `sha256(NAME|TAG|RX_TARGET_TRIPLE|<zig version>)[0:16]`, so it varies by
  dependency pin, target triple, and zig toolchain version, as required. On
  MISS it clones the dep's repo into `${CMAKE_BINARY_DIR}/_deps-src/<name>`,
  fetches+checks out the pin, configures+builds+installs it with the *same*
  toolchain file/build type as the outer project into
  `.deps-cache/<name>-<hash>/`, then drops a `.rx-built` marker. On HIT
  (marker present) it does nothing but log and export
  `<NAME>_CACHE_DIR`/extend `CMAKE_PREFIX_PATH` — no `execute_process` calls
  that could touch a compiler are reached on the HIT path at all.
- `third_party/CMakeLists.txt` — pins spdlog at `v1.17.0`, calls
  `rx_add_cached_dependency` with
  `-DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF`,
  then `find_package(spdlog REQUIRED PATHS "${spdlog_CACHE_DIR}" NO_DEFAULT_PATH)`.
  One addition beyond the brief's literal text (see Deviation below):
  `set_target_properties(spdlog::spdlog PROPERTIES IMPORTED_GLOBAL TRUE)`.
- `tools/dep_cache_smoketest/{CMakeLists.txt,main.cpp}` — exact executable
  from the brief; links `spdlog::spdlog`, calls `spdlog::info("hello from
  cached spdlog")`.
- `CMakeLists.txt` — added `add_subdirectory(third_party)` and
  `add_subdirectory(tools/dep_cache_smoketest)` after `enable_testing()`,
  before the existing `add_subdirectory(tools/toolchain_check)`.

## Deviation from the brief, and why

The brief's exact code, as written, does not configure successfully. Root
cause: `find_package(spdlog ...)` inside `third_party/CMakeLists.txt` creates
the `spdlog::spdlog` IMPORTED target, but CMake's imported-target visibility
is directory-scoped — the target is visible in the directory that created it
and its children, not in **sibling** `add_subdirectory()` calls. Since
`tools/dep_cache_smoketest` is a sibling of `third_party` (both are
`add_subdirectory()`'d directly from the root `CMakeLists.txt`), the
smoketest's `target_link_libraries(... spdlog::spdlog)` failed at generate
time with:

```
CMake Error at tools/dep_cache_smoketest/CMakeLists.txt:1 (add_executable):
  Target "dep_cache_smoketest" links to target "spdlog::spdlog" but the
  target was not found.
```

`find_package(... GLOBAL)` (the standard fix) only exists from CMake 3.24;
this project is pinned to CMake 3.22.1. The correct fix at 3.22 is to
explicitly promote the already-created imported target to global scope via
`set_target_properties(spdlog::spdlog PROPERTIES IMPORTED_GLOBAL TRUE)`,
which I added immediately after the `find_package()` call in
`third_party/CMakeLists.txt`. Confirmed the target is a real `add_library(...
IMPORTED)` (not an `ALIAS`, which cannot have properties set on it) by
inspecting the installed `spdlogConfigTargets.cmake`. This is a one-line,
narrowly-scoped, production fix, not a redesign — everything else matches
the brief verbatim. Later tasks that add more cached dependencies (SDL3,
vk-bootstrap) consumed from other subdirectories will need the same
one-liner in their own `third_party/CMakeLists.txt` entries.

No other deviations. CMake args, cache-key formula, directory layout, and
smoketest source are all exactly as specified.

## Verification: MISS then HIT (commands and actual output)

All commands run from `/media/ywadi/second/renderer_x` on branch `main`.

### 1. Clean slate, first configure (expect MISS)

```
$ rm -rf build/linux-native .deps-cache
$ cmake --preset linux-native 2>&1 | tee /tmp/.../final_miss.log | grep -m1 "\[dep-cache\] MISS for spdlog"
-- [dep-cache] MISS for spdlog (key=spdlog-c3c3e6d45a2d5f37) - building once
```

Full log (`final_miss.log`) shows, after the MISS line: a nested `cmake -S
... -B .../_deps-build/spdlog ...` configure of spdlog itself (Clang 21.1.0
via the zig wrapper), then `ninja`-style build output compiling spdlog's 7
source files (`cfg.cpp`, `file_sinks.cpp`, `stdout_sinks.cpp`, `async.cpp`,
`color_sinks.cpp`, `bundled_fmtlib_format.cpp`, `spdlog.cpp`), linking
`libspdlog.a`, and installing the full include tree + `libspdlog.a` +
CMake package files into `.deps-cache/spdlog-c3c3e6d45a2d5f37/`. Top-level
configure then finds threads, and finishes with "Build files have been
written to: .../build/linux-native".

### 2. Build and run

```
$ cmake --build --preset linux-native
[1/4] Building CXX object tools/toolchain_check/CMakeFiles/toolchain_check.dir/main.cpp.o
[2/4] Linking CXX executable tools/toolchain_check/toolchain_check
[3/4] Building CXX object tools/dep_cache_smoketest/CMakeFiles/dep_cache_smoketest.dir/main.cpp.o
[4/4] Linking CXX executable tools/dep_cache_smoketest/dep_cache_smoketest

$ ./build/linux-native/tools/dep_cache_smoketest/dep_cache_smoketest
[2026-08-09 21:33:06.373] [info] hello from cached spdlog
```

Matches the brief's expected output exactly (`[...] [info] hello from
cached spdlog`). Note the build step itself never compiles any spdlog
source file — only `dep_cache_smoketest`'s own `main.cpp` — because spdlog
was already fully built and installed during the *configure* step above,
and the smoketest links the prebuilt `libspdlog.a` from `.deps-cache/`.

### 3. Force reconfigure without touching the pin (expect HIT, zero spdlog build)

```
$ rm -rf build/linux-native
$ cmake --preset linux-native 2>&1 | tee /tmp/.../final_hit.log | grep -m1 "\[dep-cache\] HIT for spdlog"
-- [dep-cache] HIT for spdlog (key=spdlog-c3c3e6d45a2d5f37) - reusing cached install, no compilation

$ grep -c "_deps-build/spdlog" /tmp/.../final_hit.log
0
```

Same cache key (`spdlog-c3c3e6d45a2d5f37`) as the MISS run, confirming the
`.deps-cache/` entry was reused rather than rebuilt under a new key. The
full `final_hit.log` (21 lines) contains no `_deps-build/spdlog`, no
`Building CXX object` for any spdlog source file, and no invocation of
spdlog's own `cmake -S/-B` configure or `--build ... --target install` —
i.e. zero compilation of the cached dependency, proven by absence in the
log rather than merely asserted. The only compiler-adjacent line in the
whole log is the *outer* project's own one-time compiler-identification
check, unrelated to spdlog.

Rebuilt and reran afterward to confirm the resulting build is still fully
functional post-HIT:

```
$ cmake --build --preset linux-native
[1/4] Building CXX object tools/toolchain_check/CMakeFiles/toolchain_check.dir/main.cpp.o
[2/4] Linking CXX executable tools/toolchain_check/toolchain_check
[3/4] Building CXX object tools/dep_cache_smoketest/CMakeFiles/dep_cache_smoketest.dir/main.cpp.o
[4/4] Linking CXX executable tools/dep_cache_smoketest/dep_cache_smoketest

$ ./build/linux-native/tools/dep_cache_smoketest/dep_cache_smoketest
[2026-08-09 21:33:18.381] [info] hello from cached spdlog
```

### 4. Extra robustness check (not required by the brief, done because the
   cache key's raison d'être is cross-preset reuse)

Since the wiring in root `CMakeLists.txt` is unconditional (not gated per
preset), and the design note explicitly says this same mechanism will be
reused by `windows-cross-zig` in later tasks, I also reconfigured/built the
`windows-cross-zig` preset to make sure adding `third_party` didn't quietly
break it:

```
$ cmake --preset windows-cross-zig
... [dep-cache] MISS for spdlog (key=spdlog-b2d778a38067f2e7) - building once ...
-- Build files have been written to: .../build/windows-cross-zig

$ cmake --build --preset windows-cross-zig
[1/2] Building CXX object tools/dep_cache_smoketest/CMakeFiles/dep_cache_smoketest.dir/main.cpp.obj
[2/2] Linking CXX executable tools/dep_cache_smoketest/dep_cache_smoketest.exe

$ wine build/windows-cross-zig/tools/dep_cache_smoketest/dep_cache_smoketest.exe
[2026-08-09 21:32:35.138] [info] hello from cached spdlog
```

Confirms: (a) the cache key genuinely differs by target triple —
`spdlog-b2d778a38067f2e7` (windows) vs `spdlog-c3c3e6d45a2d5f37` (linux) for
the identical `(spdlog, v1.17.0)` pin and zig version, living side by side
under `.deps-cache/` — and (b) spdlog cross-compiles and links correctly
under the zig windows toolchain and actually runs correctly under Wine.

## Files changed

- `cmake/DepCache.cmake` (new)
- `third_party/CMakeLists.txt` (new)
- `tools/dep_cache_smoketest/CMakeLists.txt` (new)
- `tools/dep_cache_smoketest/main.cpp` (new)
- `CMakeLists.txt` (modified — two `add_subdirectory()` lines added)

`.deps-cache/` and `build/` are already covered by the repo's `.gitignore`
(added in Task 1), so no generated/cached artifacts were staged.

## Self-review

- **Completeness**: all 4 required files present, root `CMakeLists.txt`
  wired exactly as specified (plus the one necessary fix). Both MISS and HIT
  paths exercised and independently verified via log inspection, not just
  exit codes.
- **Quality — cache key**: confirmed by direct observation that the key
  string includes `RX_TARGET_TRIPLE` (linux vs windows keys differ for the
  same pin) and the zig version (read live via `zig version` inside the
  key function, not hardcoded). `NAME` and `TAG` are also both present in
  the key inputs, matching "per (name, pin, target triple, zig version)"
  from the constraints.
- **Quality — MISS/HIT behavior**: MISS path performs real git
  clone/fetch/checkout + nested configure/build/install, writes the
  `.rx-built` marker only on success (a failed build leaves no marker, so a
  half-built cache can't be mistaken for a HIT). HIT path is a pure
  `if(NOT EXISTS marker)` short-circuit — verified via full-log grep that no
  `_deps-build/spdlog` or spdlog compile lines appear on the HIT reconfigure.
- **Discipline**: no scope creep. Did not add ctest registration for the
  smoketest (brief's Step 3 doesn't ask for one), did not generalize the
  `IMPORTED_GLOBAL` fix into `DepCache.cmake` (it's specific to how
  `find_package(spdlog)` exposes its target, not something the generic
  cache function should assume for every future dependency), did not touch
  Tasks 1/2 toolchain files beyond reading them for `RX_TARGET_TRIPLE`.
- **Testing**: every claim above is backed by an actual command transcript
  captured during this session (log files under the scratchpad dir), not
  inferred from source reading.

## Concerns

None blocking. One thing worth flagging for later tasks/reviewers: the
`IMPORTED_GLOBAL TRUE` promotion has to be repeated by hand in every future
`third_party/CMakeLists.txt` entry that other sibling subdirectories will
link against directly (SDL3 in Task 5, vk-bootstrap in Task 7) — it is not
automated by `DepCache.cmake` itself, by design, since the exact imported
target name varies per upstream package and `DepCache.cmake` intentionally
stays name-agnostic.
