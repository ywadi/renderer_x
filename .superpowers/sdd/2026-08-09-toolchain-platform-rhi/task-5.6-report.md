# Task 5.6 (ad-hoc) Report: Stop requiring a Windows Vulkan loader lib that isn't needed

## The fix

`src/rx_platform/CMakeLists.txt` called `find_package(Vulkan REQUIRED)` but only ever
links `Vulkan::Headers` (never `Vulkan::Vulkan`, the loader import library). CMake's
bundled `FindVulkan.cmake` (this install: CMake 3.22) gates `Vulkan_FOUND` — and
therefore the existence of the `Vulkan::Headers` target at all — behind **both**
`Vulkan_INCLUDE_DIR` **and** `Vulkan_LIBRARY` (the loader) being found. There is no
headers-only bypass in this CMake version, and no Windows `vulkan-1.lib` anywhere on
this Linux host, so `find_package(Vulkan REQUIRED)` hard-failed configuring
`windows-cross-zig` even though nothing in the project links the loader at
compile/link time (Task 6+'s RHI layer loads Vulkan entry points dynamically at
runtime via volk).

Replaced it with a direct `FetchContent` of the official `KhronosGroup/Vulkan-Headers`
repository, added to **`third_party/CMakeLists.txt`** (not inline in
`rx_platform/CMakeLists.txt`) — placed right next to the existing GLM/doctest
`FetchContent_Declare`/`FetchContent_MakeAvailable` calls, which is where every other
plain-FetchContent (uncached, header-only/compiled-nothing) dependency in this project
already lives. This is a deliberate, scoped deviation from the brief's literal
phrasing ("...in src/rx_platform/CMakeLists.txt..."): the brief itself flags that
Task 6's `rx_rhi_vk` will also need `Vulkan::Headers`, so centralizing the fetch
where GLM/doctest already are (rather than duplicating it per-consumer) is the
established, consistent pattern for this exact category of dependency, and it also
means Task 6 gets `Vulkan::Headers` for free with zero additional CMake work.

`src/rx_platform/CMakeLists.txt` now just drops the `find_package(Vulkan REQUIRED)`
line (replaced with an explanatory comment) — the `target_link_libraries(... Vulkan::Headers ...)`
line is untouched and unchanged, since the target name is identical; only its
*source* changed (FetchContent'd INTERFACE target instead of a `find_package`-imported
one).

No `IMPORTED_GLOBAL` promotion was needed for the new target (unlike `spdlog::spdlog`
and `SDL3::SDL3-static`/`SDL3::Headers` earlier in the same file): those needed
promotion because CMake's directory-scoping rule for target visibility only applies
to **imported** targets, and `find_package()`/dependency-cache installs produce
imported targets. `FetchContent_MakeAvailable(Vulkan-Headers)` runs
`add_subdirectory()` on Vulkan-Headers' own `CMakeLists.txt`, which does a plain
`add_library(Vulkan-Headers INTERFACE)` + `add_library(Vulkan::Headers ALIAS
Vulkan-Headers)` — a regular (non-imported) target, which is global by default in
CMake. This is exactly the same situation as the pre-existing `glm::glm` and
`doctest::doctest` targets, which `rx_core`/`rx_platform_tests` (sibling directories)
already consume with no extra promotion — confirmed by inspecting
`src/rx_core/CMakeLists.txt`.

## Vulkan-Headers tag chosen, and why

**`vulkan-sdk-1.3.280.0`**, verified to actually exist via `git ls-remote --tags
https://github.com/KhronosGroup/Vulkan-Headers.git` before use (not guessed):

```
577baa05033cf1d9236b3d078ca4b3269ed87a2b	refs/tags/vulkan-sdk-1.3.280.0
```

Then cloned it directly and inspected `include/vulkan/vulkan_core.h`:

```
#define VK_HEADER_VERSION 280
#define VK_HEADER_VERSION_COMPLETE VK_MAKE_API_VERSION(0, 1, 3, VK_HEADER_VERSION)
```

i.e. `VK_HEADER_VERSION_COMPLETE` == **1.3.280** exactly — an exact match (not just
"compatible with") this machine's driver-reported `Vulkan Instance Version: 1.3.280`
from `vulkaninfo --summary`. Also confirmed the repo's root `CMakeLists.txt` creates
exactly the target shape needed: `add_library(Vulkan-Headers INTERFACE)` +
`add_library(Vulkan::Headers ALIAS Vulkan-Headers)`, header-only, no loader/library
target defined anywhere in it.

## SDL3-probing findings

Investigated for real rather than assuming, across the entire freshly-fetched SDL3
source tree (`build/linux-native/_deps-src/SDL3` and, separately, the
`windows-cross-zig` cross-build's copy):

```
grep -rn "find_package(Vulkan\|FindVulkan" build/linux-native/_deps-src/SDL3/
```

returned **zero** matches, in the whole tree — not just the top-level
`CMakeLists.txt`. SDL3's own Vulkan support macro (`CheckVulkan()` in
`cmake/sdlchecks.cmake`) only sets preprocessor defines
(`SDL_VIDEO_VULKAN`/`HAVE_VULKAN`/`SDL_VIDEO_RENDER_VULKAN`) gated on an `SDL_VULKAN`
option — no CMake package probing of any kind. SDL3's public `SDL3/SDL_vulkan.h`
header doesn't `#include <vulkan/vulkan.h>` either; it declares its own minimal
forward-compatible `VkInstance`/`VkSurfaceKHR` typedefs guarded by "don't define
`VkInstance` if it's already included" (confirmed by reading the header directly).
This matches the brief's hypothesis exactly: SDL3's Vulkan support is
runtime-`dlopen`/`LoadLibrary`-based (`SDL_Vulkan_LoadLibrary`), never a CMake-time
loader dependency. **No SDL3 fix was needed.**

## Verification

### `windows-cross-zig` — fully clean state (`rm -rf build/windows-cross-zig .deps-cache`)

Configure (`cmake --preset windows-cross-zig`) completed with a final:

```
-- Configuring done
-- Generating done
-- Build files have been written to: /media/ywadi/second/renderer_x/build/windows-cross-zig
```

with no errors anywhere in the log (checked via `grep -in "error\|fatal"`, zero
matches besides comment text). `spdlog` and `SDL3` both cache-MISS'd and rebuilt from
scratch under the Windows cross-toolchain; `Vulkan-Headers`, `doctest`, `glm` were all
freshly fetched into `build/windows-cross-zig/_deps/`:

```
$ find build/windows-cross-zig/_deps -maxdepth 1 -iname "*vulkan*"
build/windows-cross-zig/_deps/vulkan-headers-build
build/windows-cross-zig/_deps/vulkan-headers-src
build/windows-cross-zig/_deps/vulkan-headers-subbuild
```

Build (`cmake --build --preset windows-cross-zig`):

```
[1/19] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/math_test.cpp.obj
[2/19] Building CXX object tools/toolchain_check/CMakeFiles/toolchain_check.dir/main.cpp.obj
[3/19] Linking CXX executable tools/toolchain_check/toolchain_check.exe
[4/19] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/handle_test.cpp.obj
[5/19] Building CXX object src/rx_core/CMakeFiles/rx_core.dir/src/log.cpp.obj
[6/19] Building CXX object tools/dep_cache_smoketest/CMakeFiles/dep_cache_smoketest.dir/main.cpp.obj
[7/19] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/log_test.cpp.obj
[8/19] Linking CXX executable tools/dep_cache_smoketest/dep_cache_smoketest.exe
[9/19] Building CXX object src/rx_platform/CMakeFiles/rx_platform.dir/src/window.cpp.obj
[10/19] Building CXX object _deps/doctest-build/CMakeFiles/doctest_with_main.dir/doctest/doctest.cpp.obj
[11/19] Building CXX object _deps/glm-build/glm/CMakeFiles/glm.dir/detail/glm.cpp.obj
[12/19] Linking CXX static library _deps/doctest-build/libdoctest_with_main.a
[13/19] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/doctest_main.cpp.obj
[14/19] Building CXX object src/rx_platform/CMakeFiles/rx_platform_tests.dir/tests/window_test.cpp.obj
[15/19] Linking CXX static library _deps/glm-build/glm/libglm.a
[16/19] Linking CXX static library src/rx_core/librx_core.a
[17/19] Linking CXX static library src/rx_platform/librx_platform.a
[18/19] Linking CXX executable src/rx_core/rx_core_tests.exe
[19/19] Linking CXX executable src/rx_platform/rx_platform_tests.exe
```

19/19, no errors — `windows-cross-zig` configures and builds cleanly from a fully
clean state, satisfying the stated bar (configure+build clean; the brief does not
require `ctest` to pass for this preset, only for `linux-native`).

As a bonus check (wine is installed on this host), I also ran
`ctest --preset windows-cross-zig`: `rx_platform_tests` **passed** running the actual
`.exe` under Wine (proving the Vulkan-Headers-linked binary genuinely links and runs,
not just compiles). `rx_core_tests` failed on one pre-existing, unrelated assertion
(`log_test.cpp`'s captured-stdout string comparison) that is a Wine-stdout artifact,
not a Vulkan/rx_platform issue — rx_core was not touched by this task, and this is
outside the brief's stated verification bar for this preset. Flagging it as a
pre-existing observation, not something this task needed to or did fix.

### `linux-native` — fully clean state (`rm -rf build/linux-native .deps-cache`)

Configure:

```
-- Configuring done
-- Generating done
-- Build files have been written to: /media/ywadi/second/renderer_x/build/linux-native
```

`spdlog` and `SDL3` cache-MISS'd and rebuilt from scratch under the native Linux
toolchain too (both `.deps-cache` entries were wiped along with `build/linux-native`,
so this exercised SDL3's native build fresh as well, not just cache reuse from the
Windows pass). No errors (only harmless pre-existing `wayland.xml` XML-validity
warnings from SDL3's own Wayland protocol scanning, unrelated to this change).

Build (`cmake --build --preset linux-native`):

```
[1/19] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/handle_test.cpp.o
[2/19] Building CXX object tools/toolchain_check/CMakeFiles/toolchain_check.dir/main.cpp.o
[3/19] Linking CXX executable tools/toolchain_check/toolchain_check
[4/19] Building CXX object src/rx_core/CMakeFiles/rx_core.dir/src/log.cpp.o
[5/19] Building CXX object tools/dep_cache_smoketest/CMakeFiles/dep_cache_smoketest.dir/main.cpp.o
[6/19] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/math_test.cpp.o
[7/19] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/log_test.cpp.o
[8/19] Linking CXX executable tools/dep_cache_smoketest/dep_cache_smoketest
[9/19] Building CXX object src/rx_platform/CMakeFiles/rx_platform.dir/src/window.cpp.o
[10/19] Building CXX object _deps/glm-build/glm/CMakeFiles/glm.dir/detail/glm.cpp.o
[11/19] Linking CXX static library _deps/glm-build/glm/libglm.a
[12/19] Linking CXX static library src/rx_core/librx_core.a
[13/19] Linking CXX static library src/rx_platform/librx_platform.a
[14/19] Building CXX object _deps/doctest-build/CMakeFiles/doctest_with_main.dir/doctest/doctest.cpp.o
[15/19] Linking CXX static library _deps/doctest-build/libdoctest_with_main.a
[16/19] Building CXX object src/rx_platform/CMakeFiles/rx_platform_tests.dir/tests/window_test.cpp.o
[17/19] Building CXX object src/rx_core/CMakeFiles/rx_core_tests.dir/tests/doctest_main.cpp.o
[18/19] Linking CXX executable src/rx_platform/rx_platform_tests
[19/19] Linking CXX executable src/rx_core/rx_core_tests
```

Test (`ctest --preset linux-native`):

```
Test project /media/ywadi/second/renderer_x/build/linux-native
    Start 1: rx_core_tests
1/2 Test #1: rx_core_tests ....................   Passed    0.00 sec
    Start 2: rx_platform_tests
2/2 Test #2: rx_platform_tests ................   Passed    0.08 sec

100% tests passed, 0 tests failed out of 2

Total Test time (real) =   0.08 sec
```

Both tests pass. To confirm the real Vulkan-instance-extension test genuinely
exercised the live display/driver (not skipped), ran the test binary directly with
`--success` against this machine's real X11 session (`DISPLAY=:1`, confirmed a working
Vulkan 1.3.280 driver via `vulkaninfo --summary`):

```
$ DISPLAY=:1 build/linux-native/src/rx_platform/rx_platform_tests --success
[doctest] doctest version is "2.5.3"
[doctest] run with "--help" for options
===============================================================================
/media/ywadi/second/renderer_x/src/rx_platform/tests/window_test.cpp:9:
TEST CASE:  Window::create/destroy lifecycle succeeds under any video driver

/media/ywadi/second/renderer_x/src/rx_platform/tests/window_test.cpp:11: SUCCESS: REQUIRE( window.has_value() ) is correct!
  values: REQUIRE( true )

/media/ywadi/second/renderer_x/src/rx_platform/tests/window_test.cpp:12: SUCCESS: CHECK( window->sdlWindow() != nullptr ) is correct!
  values: CHECK( 1 != nullptr )

===============================================================================
/media/ywadi/second/renderer_x/src/rx_platform/tests/window_test.cpp:16:
TEST CASE:  Window reports Vulkan instance extensions when a real display backend is present

/media/ywadi/second/renderer_x/src/rx_platform/tests/window_test.cpp:27: SUCCESS: CHECK( extensions.size() > 0 ) is correct!
  values: CHECK( 2 >  0 )

===============================================================================
[doctest] test cases: 2 | 2 passed | 0 failed | 0 skipped
[doctest] assertions: 3 | 3 passed | 0 failed |
[doctest] Status: SUCCESS!
```

`CHECK( extensions.size() > 0 )` evaluated `2 > 0` — the test genuinely received 2
real Vulkan surface extensions from SDL3 querying the live X11 display backend (not
the "dummy driver reports none, skip" branch), confirming the header-source switch
did not change runtime behavior at all — `linux-native` never used `find_package`'s
loader lib in the first place, only `Vulkan::Headers`' types, which are unaffected.

## Files changed

- `third_party/CMakeLists.txt` — added the `Vulkan-Headers` `FetchContent_Declare`/`FetchContent_MakeAvailable` block (pinned to `vulkan-sdk-1.3.280.0`), next to the existing GLM/doctest fetches.
- `src/rx_platform/CMakeLists.txt` — removed `find_package(Vulkan REQUIRED)`, replaced with an explanatory comment; `Vulkan::Headers` linkage line unchanged.

Commit: `720d988` — "Fetch Vulkan-Headers directly instead of find_package(Vulkan)"

## Self-review

- **Completeness:** Both presets verified clean from a fully wiped state
  (`build/<preset>` + `.deps-cache` both removed before each). SDL3's own CMake was
  checked for real (whole-tree grep + reading the relevant macro/header), not
  assumed. `linux-native`'s real Vulkan-extension test was run directly and shown to
  genuinely query the live X11/Vulkan driver (2 extensions returned), not just
  compile or hit a skip branch.
- **Quality:** Vulkan-Headers tag verified to exist via `git ls-remote` before use,
  and its `vulkan_core.h`'s `VK_HEADER_VERSION_COMPLETE` cross-checked byte-for-byte
  against this machine's `vulkaninfo --summary` output (1.3.280 == 1.3.280, exact
  match, not just "1.3-compatible").
- **Discipline:** No unrelated files touched. Considered whether to add the
  `FetchContent` block inside `src/rx_platform/CMakeLists.txt` per the brief's literal
  wording vs. centralizing in `third_party/CMakeLists.txt`; chose the latter as the
  scoped, non-duplicative choice consistent with how every other plain-FetchContent
  header-only dependency (GLM, doctest) is already declared in this project, and
  because the brief's own "Heads-up" section anticipates Task 6 needing the same
  target — noted explicitly here rather than silently deviating.
- **Testing:** Both transcripts above are real, captured directly from this session's
  tool output, not fabricated or predicted.

## Concerns

1. **Deviation flagged above:** placed the `FetchContent` call in
   `third_party/CMakeLists.txt` rather than literally inside
   `src/rx_platform/CMakeLists.txt` as the brief's prose suggested. Judgment call
   made in favor of consistency with the existing dependency-declaration pattern and
   avoiding duplication once Task 6 needs the same target; flagging for visibility in
   case the intent was strictly per-consumer declarations.
2. **Pre-existing, unrelated, out-of-scope:** `rx_core_tests`' `log_test.cpp` fails
   one assertion when run under Wine as part of `ctest --preset windows-cross-zig`
   (a bonus check beyond the brief's stated bar for that preset, which only requires
   configure+build). This is a spdlog-captured-stdout comparison unrelated to Vulkan
   or rx_platform, not touched or caused by this task, and not gated by the brief's
   verification bar (which requires `ctest` cleanliness only for `linux-native`,
   where all tests pass). Left unfixed as out of scope; noting for whoever picks up
   CI (Task 14) since it will surface there too.
3. **Carried forward per the brief's own "Heads-up":** Task 6 will add `vk-bootstrap`
   via the dependency cache; its own upstream `CMakeLists.txt` may have a similar
   `find_package(Vulkan)` pattern for its *library* target (not just its
   tests/examples, which the plan already disables). Not investigated here since
   vk-bootstrap isn't added yet — explicitly flagged in the brief as something Task 6
   must check for itself, not assumed fine.
