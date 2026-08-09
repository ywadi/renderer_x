# Task 5 Report: rx_platform (SDL3 window wrapper)

## Summary

Implemented `rx_platform`: a thin SDL3 window wrapper exposing `rx::platform::Window` with
create/destroy lifecycle, event pumping, and Vulkan surface/extension queries, exactly as
specified in the brief. SDL3 is pulled in through the Task-3 dependency-cache mechanism
(`rx_add_cached_dependency`), matching spdlog's pattern. Both required test cases pass on
this machine against the real X11 display and real Vulkan 1.3 driver — the Vulkan-extension
test genuinely retrieves real extensions (`VK_KHR_surface`, `VK_KHR_xlib_surface`), it does
not skip.

Getting there required three deviations beyond the brief's literal text, all forced by real
environment behavior discovered while implementing (details below):
1. A CMake toolchain fix so `find_library`/`find_package` can see Debian's multiarch library
   directory under the zig-cc toolchain (this was silently causing SDL3 to build *without*
   X11 support, and would have caused `find_package(Vulkan REQUIRED)` to fail outright).
2. Linking against `SDL3::SDL3-static` instead of the brief's `SDL3::SDL3`, because SDL3's
   own CMake config makes `SDL3::SDL3` an ALIAS, and ALIAS targets don't inherit
   `IMPORTED_GLOBAL` visibility from the target they point to.
3. Two missing pieces the brief's exact file listings omitted: `#include <SDL3/SDL_vulkan.h>`
   in `window.cpp` (SDL.h doesn't pull it in), and `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` in
   `window_test.cpp` (the brief's `rx_platform_tests` has only one test TU, unlike
   `rx_core_tests`'s dedicated `doctest_main.cpp`).

## What Was Implemented

### 1. `third_party/CMakeLists.txt` (modified)

Added SDL3 via `rx_add_cached_dependency`, exactly as the brief specifies:

```cmake
set(RX_SDL3_TAG "release-3.4.14")
rx_add_cached_dependency(
  NAME SDL3
  REPO https://github.com/libsdl-org/SDL.git
  TAG ${RX_SDL3_TAG}
  CMAKE_ARGS -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF
)
find_package(SDL3 REQUIRED PATHS "${SDL3_CACHE_DIR}" NO_DEFAULT_PATH)
```

Then, instead of the brief's literal one-liner
(`set_target_properties(SDL3::SDL3 PROPERTIES IMPORTED_GLOBAL TRUE)`), promoted the two real
imported targets that back it:

```cmake
set_target_properties(SDL3::SDL3-static PROPERTIES IMPORTED_GLOBAL TRUE)
set_target_properties(SDL3::Headers PROPERTIES IMPORTED_GLOBAL TRUE)
```

**Why the deviation:** the brief's known-gotcha note (based on spdlog's Task-3 experience)
assumed `SDL3::SDL3` would be a real imported target the way `spdlog::spdlog` is. It isn't.
SDL3's own `SDL3Config.cmake` creates `SDL3::SDL3` as an `ALIAS` to `SDL3::SDL3-static` (since
we build static-only). Calling `set_target_properties()` on an alias fails outright
(`set_target_properties can not be used on an ALIAS target`) — confirmed by trying the
brief's literal line first and seeing that exact CMake error. Promoting the real target
avoids that error, but then a *second* surprise appeared: promoting `SDL3::SDL3-static` to
`IMPORTED_GLOBAL` did **not** make the alias name `SDL3::SDL3` itself resolvable from
`src/rx_platform`'s directory scope — confirmed by a `configure` that failed with
`Target "rx_platform" links to target "SDL3::SDL3" but the target was not found`. ALIAS
targets are scoped to the directory they're created in independent of whether the target
they point to is global. So the consuming CMakeLists.txt links against `SDL3::SDL3-static`
directly (see below) rather than the alias.

### 2. `cmake/toolchains/linux-native.cmake` (modified)

Added:
```cmake
list(APPEND CMAKE_SYSTEM_LIBRARY_PATH "/usr/lib/${RX_TARGET_TRIPLE}")
```

**Why:** `find_package(Vulkan REQUIRED)` in `src/rx_platform/CMakeLists.txt` initially failed
with `Could NOT find Vulkan (missing: Vulkan_LIBRARY)` even though `/usr/lib/x86_64-linux-gnu/libvulkan.so`
exists on this machine and `Vulkan_INCLUDE_DIR` was found fine. Root cause: zig cc's `-v`
diagnostic output isn't in the format CMake's compiler-ABI-detection step expects from
gcc/clang, so `CMAKE_C_IMPLICIT_LINK_DIRECTORIES`/`CMAKE_LIBRARY_ARCHITECTURE` come back empty
for this toolchain (confirmed by inspecting the generated `CMakeCCompiler.cmake` —
`CMAKE_LIBRARY_ARCHITECTURE` is `""`). That silently drops the Debian/Ubuntu multiarch
directory from every `find_library()` search across the whole build, not just Vulkan.

This mattered more than just satisfying `find_package(Vulkan REQUIRED)` (which the brief only
needs for `Vulkan::Headers`, an include-only target): **SDL3's own CMake build hit the exact
same problem for its X11 detection.** Before this fix, SDL3 built successfully but *silently
without X11 support* (confirmed via `grep SDL_VIDEO_DRIVER_X11 SDL_build_config.h` showing
`/* #undef SDL_VIDEO_DRIVER_X11 */`, while `SDL_VIDEO_DRIVER_WAYLAND` was defined — Wayland
detection goes through pkg-config, which isn't affected, but X11 goes through CMake's
`FindX11.cmake`, which is). Since this machine has `DISPLAY=:1` (X11) and no
`WAYLAND_DISPLAY`, an X11-less SDL3 build would have meant `Window::create()` returning
`std::nullopt` (no usable video driver) or falling back to the dummy driver — which is exactly
the "silent skip" failure mode the task explicitly said would mean something is wrong. I
deleted the stale `.deps-cache/SDL3-*` entry and the whole `build/linux-native` directory,
applied this fix, and rebuilt from scratch; the rebuilt SDL3 has X11 support
(`nm libSDL3.a | grep X11_CreateWindow` after the fix — checked implicitly via the passing
test showing `VK_KHR_xlib_surface`, which only exists when the X11 backend is active).

This is a toolchain-level fix (not specific to Vulkan or SDL3) because it corrects a gap that
will otherwise recur for every future `find_library()`/`find_package()` call under this
toolchain — directly relevant to Task 6+ (`rx_rhi_vk`), which will also need
`find_package(Vulkan)` to fully resolve (including `Vulkan::Vulkan` for the loader, not just
headers).

### 3. `src/rx_platform/include/rx_platform/window.h` (created)

Implemented exactly per the brief: `Window` with move-only semantics, `create()` factory
returning `std::optional<Window>`, `sdlWindow()`, `pumpEvents()`,
`requiredVulkanInstanceExtensions()`, `createVulkanSurface()`.

### 4. `src/rx_platform/src/window.cpp` (created)

Implemented exactly per the brief's logic, with one addition:

```cpp
#include <rx_platform/window.h>
#include <rx_core/log.h>
// SDL3/SDL.h does not pull in the Vulkan-interop declarations
// (SDL_Vulkan_GetInstanceExtensions, SDL_Vulkan_CreateSurface); SDL3 keeps
// those in their own header.
#include <SDL3/SDL_vulkan.h>
```

**Why the deviation:** the brief's `window.h` includes `<SDL3/SDL.h>` and `<vulkan/vulkan.h>`,
and `window.cpp` includes only `<rx_platform/window.h>` and `<rx_core/log.h>`. Building
exactly that failed: `use of undeclared identifier 'SDL_Vulkan_GetInstanceExtensions'` /
`'SDL_Vulkan_CreateSurface'`. SDL3 declares those in a separate header,
`SDL3/SDL_vulkan.h`, which `SDL3/SDL.h` does not transitively include. Added the include to
`window.cpp` (the only place those calls are made); `window.h` doesn't need it since it only
declares types (`VkSurfaceKHR`, `VkInstance`) that come from `vulkan/vulkan.h`.

### 5. `src/rx_platform/tests/window_test.cpp` (created)

Implemented exactly per the brief's two `TEST_CASE`s, with one addition at the top:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

**Why the deviation:** without this, linking `rx_platform_tests` failed with pages of
`undefined symbol: doctest::...` errors (no doctest runtime implementation, no `main()`).
`rx_core_tests` gets this from a dedicated `tests/doctest_main.cpp` shared across several
test files; the brief's `rx_platform_tests` target has only `tests/window_test.cpp` as its
sole source, so it needs to both implement doctest's runtime and provide `main()` itself. A
separate `doctest_main.cpp` for a single-file test target seemed like unnecessary file-count
overhead, so the define was added directly to the one test file instead — functionally
identical outcome, no new file the brief didn't ask for.

### 6. `src/rx_platform/CMakeLists.txt` (created)

Implemented per the brief, with the `SDL3::SDL3` → `SDL3::SDL3-static` substitution described
above:

```cmake
find_package(Vulkan REQUIRED)

add_library(rx_platform STATIC
    src/window.cpp
)
target_include_directories(rx_platform PUBLIC include)
target_link_libraries(rx_platform PUBLIC SDL3::SDL3-static Vulkan::Headers rx_core)

add_executable(rx_platform_tests
    tests/window_test.cpp
)
target_link_libraries(rx_platform_tests PRIVATE rx_platform doctest::doctest)
add_test(NAME rx_platform_tests COMMAND rx_platform_tests)
```

### 7. `CMakeLists.txt` (root, modified)

Added `add_subdirectory(src/rx_platform)` after `src/rx_core`, exactly per the brief.

## Build and Test Results

### Clean configure (from scratch, after deleting `build/linux-native` and the stale
`.deps-cache/SDL3-*` entry to force SDL3 to rebuild with the toolchain fix)

```
$ cmake --preset linux-native
...
-- [dep-cache] MISS for SDL3 (key=SDL3-216994ad42ea7d90) - building once
... (SDL3 builds: 371 targets, including X11-detected video driver)
-- Found Vulkan: /usr/lib/x86_64-linux-gnu/libvulkan.so
-- Configuring done
-- Generating done
-- Build files have been written to: /media/ywadi/second/renderer_x/build/linux-native
```

### Build

```
$ cmake --build --preset linux-native --target rx_platform_tests
[1/2] Building CXX object src/rx_platform/CMakeFiles/rx_platform_tests.dir/tests/window_test.cpp.o
[2/2] Linking CXX executable src/rx_platform/rx_platform_tests
```

Clean build, no errors, no warnings.

### ctest

```
$ DISPLAY=:1 ctest --preset linux-native -R rx_platform_tests --output-on-failure
Test project /media/ywadi/second/renderer_x/build/linux-native
    Start 2: rx_platform_tests
1/1 Test #2: rx_platform_tests ................   Passed    0.06 sec

100% tests passed, 0 tests failed out of 1
```

### Direct verbose run confirming the Vulkan-extension test did NOT skip

```
$ cd build/linux-native && DISPLAY=:1 ./src/rx_platform/rx_platform_tests --success
[doctest] doctest version is "2.5.3"
===============================================================================
TEST CASE:  Window::create/destroy lifecycle succeeds under any video driver
/.../window_test.cpp:11: SUCCESS: REQUIRE( window.has_value() ) is correct!
/.../window_test.cpp:12: SUCCESS: CHECK( window->sdlWindow() != nullptr ) is correct!
===============================================================================
TEST CASE:  Window reports Vulkan instance extensions when a real display backend is present
/.../window_test.cpp:27: SUCCESS: CHECK( extensions.size() > 0 ) is correct!
  values: CHECK( 2 >  0 )
===============================================================================
[doctest] test cases: 2 | 2 passed | 0 failed | 0 skipped
[doctest] assertions: 3 | 3 passed | 0 failed |
[doctest] Status: SUCCESS!
```

`0 skipped` confirms neither `MESSAGE`-and-`return` skip branch fired; `extensions.size() == 2`
confirms real extensions were retrieved (verified in a separate ad hoc check to be
`VK_KHR_surface` and `VK_KHR_xlib_surface`, i.e. the real X11 Vulkan surface backend).

### Full suite regression check

```
$ cmake --build --preset linux-native && DISPLAY=:1 ctest --preset linux-native --output-on-failure
Test project /media/ywadi/second/renderer_x/build/linux-native
    Start 1: rx_core_tests
1/2 Test #1: rx_core_tests ....................   Passed    0.00 sec
    Start 2: rx_platform_tests
2/2 Test #2: rx_platform_tests ................   Passed    0.06 sec

100% tests passed, 0 tests failed out of 2
```

`rx_core_tests` (Task 4) still passes — no regression from the toolchain/dep-cache changes.

### Extra manual verification (not part of the committed test suite — ad hoc, done to gain
confidence in code paths the brief's two required tests don't exercise, then discarded)

Wrote a scratch program (in the session scratchpad, not committed) that: creates a `Window`,
retrieves real instance extensions, creates a real `VkInstance` via SDL's own
`SDL_Vulkan_LoadLibrary`/`GetVkGetInstanceProcAddr` (avoiding an unrelated zig/lld
`dlopen`-symbol-versioning issue that appears only when linking directly against
`libvulkan.so` from a hand-built command line, not from CMake-built targets), calls
`Window::createVulkanSurface(instance)`, and exercises move-assignment. Output:

```
extensions: 2
  VK_KHR_surface
  VK_KHR_xlib_surface
instance created
surface created: 0x10630220
moved.sdlWindow() == 0x105b2120 (orig now (nil))
```

This confirms: `createVulkanSurface()` returns a real, non-null `VkSurfaceKHR` against a real
instance and real window; move construction/assignment correctly transfers ownership and
nulls out the moved-from window (so its destructor is a safe no-op). A first pass at this
scratch script crashed on cleanup, but investigation under `gdb` (backtrace pointed at a call
through a null function pointer) showed the crash was in the *scratch script's* misuse of
`vkGetInstanceProcAddr(NULL, "vkDestroyInstance")` (querying a non-instance-independent
function with a null instance handle — undefined per the Vulkan spec) — not a bug in
`rx_platform`. This scratch code was deleted after verification; it is not part of the commit.

## Files Changed

**Created:**
- `src/rx_platform/CMakeLists.txt`
- `src/rx_platform/include/rx_platform/window.h`
- `src/rx_platform/src/window.cpp`
- `src/rx_platform/tests/window_test.cpp`

**Modified:**
- `third_party/CMakeLists.txt` (+22 lines: SDL3 dep-cache wiring + global-promotion fix)
- `cmake/toolchains/linux-native.cmake` (+9 lines: multiarch library search path fix)
- `CMakeLists.txt` (+1 line: `add_subdirectory(src/rx_platform)`)

## Deviations from the Brief (summary)

1. **`third_party/CMakeLists.txt`**: promote `SDL3::SDL3-static` and `SDL3::Headers` to
   `IMPORTED_GLOBAL`, not `SDL3::SDL3` (which is an ALIAS and rejects
   `set_target_properties()`).
2. **`cmake/toolchains/linux-native.cmake`**: added a multiarch library search path so
   `find_library` can see `/usr/lib/x86_64-linux-gnu` under zig cc. Not requested by the brief,
   but required for `find_package(Vulkan REQUIRED)` to succeed at all, and — more importantly
   — for SDL3's own build to detect X11 (without it, SDL3 built silently without X11 support,
   which would have made the Vulkan-extension test's "real display" premise false on this
   machine).
3. **`src/rx_platform/CMakeLists.txt`**: link `SDL3::SDL3-static` instead of `SDL3::SDL3`
   (consequence of #1 — the alias is not visible from this sibling directory even once the
   real target is global).
4. **`src/rx_platform/src/window.cpp`**: added `#include <SDL3/SDL_vulkan.h>` (missing from
   the brief's exact file; `SDL3/SDL.h` doesn't transitively include it).
5. **`src/rx_platform/tests/window_test.cpp`**: added
   `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` before including doctest.h (the brief's single
   test file has no separate main-providing translation unit).

All five were discovered by literally attempting the brief's exact text first and reacting to
the specific build/link errors that resulted, not guessed in advance.

## Self-Review

- **Completeness:** All brief-specified files created/modified; `rx_platform` and
  `rx_platform_tests` targets wired into the root build; both required test cases present and
  passing for real.
- **Quality — RAII/move semantics:** `Window`'s move constructor/assignment null out the
  moved-from `window_` pointer; the destructor and move-assignment both guard with
  `if (window_)` before `SDL_DestroyWindow`, so double-destroy and destroying a moved-from
  window are both safe. Verified explicitly via the ad hoc scratch check (moved-from
  `sdlWindow()` became `nullptr` after move).
- **Quality — error logging:** `SDL_Init`, `SDL_CreateWindow`, and
  `SDL_Vulkan_CreateSurface` failures are all logged via `RX_LOG_WARN` with `SDL_GetError()`
  interpolated, not swallowed silently.
- **Discipline — scope:** `rx_platform` owns only window/surface concerns (SDL3 init/window
  lifecycle, event pump, Vulkan surface/extension queries). No RHI, device, or swapchain logic
  leaked in — that's explicitly deferred to Task 7+ per the brief.
- **Testing:** Confirmed via direct `--success` run (not just ctest's terse pass/fail) that the
  Vulkan-extension test's `0 skipped` and `extensions.size() == 2` are real, not a masked skip.
  Also confirmed the full existing suite (`rx_core_tests`) still passes — no regression from
  the toolchain-level fix.
- **Commit message:** verified below to contain zero AI attribution.

## Concerns

- **`windows-cross-zig` preset is pre-existing broken, unrelated to this task.** While
  investigating whether adding SDL3 to `third_party/CMakeLists.txt` (unconditionally, for both
  presets) would break the Windows cross-compile preset, I found it already fails to configure
  from a clean `build/windows-cross-zig` directory *before* any of my changes
  (`CMake Error: CMAKE_RC_COMPILER not set` while building `spdlog`'s dep-cache subbuild for
  the windows-cross-zig toolchain) — confirmed via `git stash` back to the pre-Task-5 commit
  and reconfiguring clean. This is a latent issue from Task 2/3's windows toolchain setup, not
  something Task 5 introduced or is scoped to fix, but it will need attention before Task 14
  (CI matrix) if Windows builds are expected to work there.
- The multiarch `CMAKE_SYSTEM_LIBRARY_PATH` fix in the toolchain file is scoped to
  `linux-native.cmake` only; it does not attempt to address the same class of problem for the
  (already broken) `windows-cross-zig` toolchain, since that's out of scope here.

## Commit Message Confirmation

Commit message used (verified before running `git commit`, and reproduced here verbatim):

```
Add rx_platform: SDL3 window wrapper

- Add SDL3 (release-3.4.14) via rx_add_cached_dependency in third_party
- Promote SDL3::SDL3-static and SDL3::Headers to IMPORTED_GLOBAL (SDL3::SDL3
  is an ALIAS and rejects set_target_properties directly; aliases don't
  inherit global visibility from the target they point to)
- Fix linux-native toolchain's CMAKE_SYSTEM_LIBRARY_PATH so find_library
  can see the Debian multiarch lib directory under zig cc; this was
  silently causing SDL3 to build without X11 support and breaking
  find_package(Vulkan REQUIRED)
- Implement rx::platform::Window: SDL3 window lifecycle (move-only RAII),
  event pumping, and Vulkan surface/instance-extension queries via
  SDL_Vulkan_GetInstanceExtensions/SDL_Vulkan_CreateSurface
- Add rx_platform_tests covering window create/destroy and real Vulkan
  instance-extension retrieval against the live X11/Vulkan backend
```

Contains no "Co-Authored-By", no "Claude-Session", no AI attribution of any kind. Git
author/committer identity left untouched (whatever the local git config provides).
