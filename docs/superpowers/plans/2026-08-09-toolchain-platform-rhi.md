# Toolchain + Platform + RHI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cross-compile RendererX from a Linux host to Windows and Linux (Steam Deck included) via zig, and stand up a Vulkan 1.3 RHI that renders a real triangle — verified by an automated, headless, pixel-level correctness check, not just "it builds."

**Architecture:** CMake+Ninja driven by `zig cc`/`zig c++` wrapper scripts per target triple. Compiled third-party dependencies (SDL3, spdlog, vk-bootstrap) are pinned, built exactly once, and cached by content hash via a custom `DepCache.cmake` module; header-only/single-file dependencies (GLM, VMA, volk, doctest) are fetched directly since there's nothing expensive to cache. Slang is never compiled — official prebuilt binaries are fetched per platform. Three static libraries (`rx_core`, `rx_platform`, `rx_rhi_vk`) build up to a triangle smoke-test app with an automated, headless, pixel-readback correctness gate plus a manual on-hardware verification pass.

**Tech Stack:** C++20, CMake 3.21+, Ninja, zig 0.16.0 (pinned, vendored at `toolchain/zig/`), Vulkan 1.3, SDL3, vk-bootstrap, VMA, volk, GLM, spdlog, doctest, Slang (prebuilt binaries only), GitHub Actions.

## Global Constraints

- Vulkan is the only GPU backend — no D3D12/Metal code paths.
- Baseline Vulkan features: **dynamic rendering + synchronization2** only. No mesh shaders, no hardware ray tracing (Steam Deck's RDNA2 iGPU can't do either) — these are optional, capability-queried extensions in later work, never assumed here.
- Target platforms for this plan: Windows and Linux (Steam Deck = `linux-native`, no special-casing). macOS support is deferred to a later spec, not dropped.
- Every compiled third-party dependency is pinned to an exact tag/commit, built once, and cached by `(name, pin, target triple, zig version)` — a cache hit must trigger zero compilation. Never rebuild an unchanged pin.
- Slang is consumed as prebuilt release binaries only — it is never compiled from source by this project.
- Once the dependency cache is warm, `cmake --build` for this project's own code must complete in under 1 minute on the reference dev machine (this machine).
- No AI attribution in any commit, anywhere, ever (per `/media/ywadi/second/renderer_x/CLAUDE.md`) — every commit step in this plan must produce a commit message with no such attribution, and must not alter the configured git author identity.
- No placeholder/TODO code. Every task's deliverable must be real and independently testable.

---

## File Structure

```
renderer_x/
├── CLAUDE.md                              # repo policy (already committed)
├── CMakeLists.txt                         # root build, grows one add_subdirectory per task
├── CMakePresets.json                      # linux-native + windows-cross-zig presets
├── cmake/
│   ├── zig-wrappers/
│   │   ├── zig-cc-linux / zig-cxx-linux    # native compiler wrappers
│   │   └── zig-cc-windows / zig-cxx-windows # cross compiler wrappers
│   ├── toolchains/
│   │   ├── linux-native.cmake
│   │   └── windows-cross-zig.cmake
│   └── DepCache.cmake                      # content-hashed dependency build cache
├── third_party/
│   └── CMakeLists.txt                      # single source of truth for every pin + fetch
├── tools/
│   ├── toolchain_check/                    # permanent toolchain sanity binary
│   └── fetch_slang.cmake                   # prebuilt Slang binary fetch script
├── shaders/
│   ├── triangle.vert.slang / triangle.frag.slang
│   └── CMakeLists.txt                      # invokes fetched slangc to produce .spv
├── src/
│   ├── rx_core/        (log.h/.cpp, handle.h, tests/)
│   ├── rx_platform/     (window.h/.cpp, tests/)
│   └── rx_rhi_vk/       (context, device+swapchain, buffer, command; tests/)
├── apps/
│   └── triangle_smoketest/main.cpp
├── .github/workflows/ci.yml
└── MANUAL_VERIFICATION.md
```

---

### Task 1: Native build scaffolding via zig

**Files:**
- Create: `cmake/zig-wrappers/zig-cc-linux`, `cmake/zig-wrappers/zig-cxx-linux`
- Create: `cmake/toolchains/linux-native.cmake`
- Create: `CMakeLists.txt`
- Create: `CMakePresets.json`
- Create: `tools/toolchain_check/CMakeLists.txt`, `tools/toolchain_check/main.cpp`

**Interfaces:**
- Produces: preset name `linux-native` (configure+build+test), target `toolchain_check` executable, `cmake/toolchains/linux-native.cmake` (consumed by every later task's presets).

- [ ] **Step 1: Write the zig wrapper scripts**

`cmake/zig-wrappers/zig-cc-linux`:
```bash
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
exec "$REPO_ROOT/toolchain/zig/zig" cc -target x86_64-linux-gnu -Wno-nullability-completeness "$@"
```

`cmake/zig-wrappers/zig-cxx-linux`:
```bash
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
exec "$REPO_ROOT/toolchain/zig/zig" c++ -target x86_64-linux-gnu -Wno-nullability-completeness "$@"
```

```bash
chmod +x cmake/zig-wrappers/zig-cc-linux cmake/zig-wrappers/zig-cxx-linux
```

**Why a wrapper script:** CMake invokes `CMAKE_CXX_COMPILER` as a single executable with no way to inject the `c++ -target ...` sub-command zig needs, so the wrapper is what makes zig look like a normal single-purpose compiler binary to CMake.

- [ ] **Step 2: Write the toolchain file**

`cmake/toolchains/linux-native.cmake`:
```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(RX_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(CMAKE_C_COMPILER   "${RX_REPO_ROOT}/cmake/zig-wrappers/zig-cc-linux"  CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${RX_REPO_ROOT}/cmake/zig-wrappers/zig-cxx-linux" CACHE FILEPATH "C++ compiler" FORCE)

set(RX_TARGET_TRIPLE "x86_64-linux-gnu" CACHE STRING "Target triple, used as a dependency-cache key component")
```

- [ ] **Step 3: Write the root CMakeLists.txt and presets**

`CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.21)
project(renderer_x LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_subdirectory(tools/toolchain_check)
```

`CMakePresets.json`:
```json
{
  "version": 3,
  "cmakeMinimumRequired": { "major": 3, "minor": 21, "patch": 0 },
  "configurePresets": [
    {
      "name": "linux-native",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/linux-native",
      "toolchainFile": "${sourceDir}/cmake/toolchains/linux-native.cmake",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo" }
    }
  ],
  "buildPresets": [
    { "name": "linux-native", "configurePreset": "linux-native" }
  ],
  "testPresets": [
    {
      "name": "linux-native",
      "configurePreset": "linux-native",
      "output": { "outputOnFailure": true }
    }
  ]
}
```

- [ ] **Step 4: Write the toolchain_check tool**

`tools/toolchain_check/CMakeLists.txt`:
```cmake
add_executable(toolchain_check main.cpp)
```

`tools/toolchain_check/main.cpp`:
```cpp
#include <cstdio>

int main() {
#if defined(_WIN32)
    std::puts("renderer_x toolchain_check: target=windows");
#elif defined(__linux__)
    std::puts("renderer_x toolchain_check: target=linux");
#else
    std::puts("renderer_x toolchain_check: target=unknown");
#endif
    return 0;
}
```

- [ ] **Step 5: Configure, build, and verify**

```bash
cmake --preset linux-native
cmake --build --preset linux-native
./build/linux-native/tools/toolchain_check/toolchain_check
```
Expected output: `renderer_x toolchain_check: target=linux`

(First run will be slower than usual: zig bootstraps its own bundled libc++ into `~/.cache/zig` the first time `zig c++` is invoked for a given target. This is zig's own cache, separate from this project's `.deps-cache/`, and only pays this cost once per machine.)

- [ ] **Step 6: Commit**

```bash
git add cmake/ CMakeLists.txt CMakePresets.json tools/toolchain_check/
git commit -m "Add native build scaffolding via zig toolchain"
```

---

### Task 2: Windows cross-compile toolchain

**Files:**
- Create: `cmake/zig-wrappers/zig-cc-windows`, `cmake/zig-wrappers/zig-cxx-windows`
- Create: `cmake/toolchains/windows-cross-zig.cmake`
- Modify: `CMakePresets.json` (add `windows-cross-zig` presets)

**Interfaces:**
- Produces: preset name `windows-cross-zig`, `RX_TARGET_TRIPLE=x86_64-windows-gnu`, `CMAKE_CROSSCOMPILING_EMULATOR` set to `wine` when available.

- [ ] **Step 1: Write the windows wrapper scripts**

`cmake/zig-wrappers/zig-cc-windows`:
```bash
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
exec "$REPO_ROOT/toolchain/zig/zig" cc -target x86_64-windows-gnu -Wno-nullability-completeness "$@"
```

`cmake/zig-wrappers/zig-cxx-windows`:
```bash
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
exec "$REPO_ROOT/toolchain/zig/zig" c++ -target x86_64-windows-gnu -Wno-nullability-completeness "$@"
```

```bash
chmod +x cmake/zig-wrappers/zig-cc-windows cmake/zig-wrappers/zig-cxx-windows
```

- [ ] **Step 2: Write the toolchain file**

`cmake/toolchains/windows-cross-zig.cmake`:
```cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(RX_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(CMAKE_C_COMPILER   "${RX_REPO_ROOT}/cmake/zig-wrappers/zig-cc-windows"  CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${RX_REPO_ROOT}/cmake/zig-wrappers/zig-cxx-windows" CACHE FILEPATH "C++ compiler" FORCE)

# Cross-compiling: CMake cannot execute the resulting binaries to probe the
# compiler, so tell it the compiler works rather than test-executing it.
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_CXX_COMPILER_WORKS TRUE)
set(CMAKE_CROSSCOMPILING TRUE)

set(RX_TARGET_TRIPLE "x86_64-windows-gnu" CACHE STRING "Target triple, used as a dependency-cache key component")

# If wine is present, ctest can run cross-compiled test binaries transparently.
find_program(RX_WINE_EXECUTABLE wine)
if(RX_WINE_EXECUTABLE)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${RX_WINE_EXECUTABLE}" CACHE STRING "Emulator used to run cross-compiled test binaries" FORCE)
endif()
```

- [ ] **Step 3: Add the preset to CMakePresets.json**

Add to `configurePresets`:
```json
    {
      "name": "windows-cross-zig",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/windows-cross-zig",
      "toolchainFile": "${sourceDir}/cmake/toolchains/windows-cross-zig.cmake",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo" }
    }
```
Add to `buildPresets`:
```json
    { "name": "windows-cross-zig", "configurePreset": "windows-cross-zig" }
```
Add to `testPresets`:
```json
    {
      "name": "windows-cross-zig",
      "configurePreset": "windows-cross-zig",
      "output": { "outputOnFailure": true }
    }
```

- [ ] **Step 4: Configure, build, and verify (reuses Task 1's toolchain_check target)**

```bash
cmake --preset windows-cross-zig
cmake --build --preset windows-cross-zig
file build/windows-cross-zig/tools/toolchain_check/toolchain_check.exe
wine build/windows-cross-zig/tools/toolchain_check/toolchain_check.exe
```
Expected: `file` reports `PE32+ executable (console) x86-64, for MS Windows`; `wine` prints `renderer_x toolchain_check: target=windows`.

- [ ] **Step 5: Commit**

```bash
git add cmake/zig-wrappers/zig-cc-windows cmake/zig-wrappers/zig-cxx-windows cmake/toolchains/windows-cross-zig.cmake CMakePresets.json
git commit -m "Add Windows cross-compile toolchain via zig"
```

---

### Task 3: Dependency cache module, proven with spdlog

**Files:**
- Create: `cmake/DepCache.cmake`
- Create: `third_party/CMakeLists.txt`
- Create: `tools/dep_cache_smoketest/CMakeLists.txt`, `tools/dep_cache_smoketest/main.cpp`
- Modify: `CMakeLists.txt` (add `third_party` and the smoketest subdirectory)

**Interfaces:**
- Produces: CMake function `rx_add_cached_dependency(NAME <n> REPO <url> TAG <pin> CMAKE_ARGS <...>)`, which on success sets `<NAME>_CACHE_DIR` in the parent scope and appends to `CMAKE_PREFIX_PATH`. Consumed by every later task that adds a compiled (non-header-only) dependency.

- [ ] **Step 1: Write the dependency-cache module**

`cmake/DepCache.cmake`:
```cmake
# Builds a CMake-based dependency exactly once per (name, pin, target
# triple, zig version) and reuses the cached install on every later
# configure. A cache hit costs zero compilation.

function(rx_dep_cache_key OUT_VAR NAME TAG)
  execute_process(
    COMMAND "${CMAKE_SOURCE_DIR}/toolchain/zig/zig" version
    OUTPUT_VARIABLE _zig_version
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(SHA256 _hash "${NAME}|${TAG}|${RX_TARGET_TRIPLE}|${_zig_version}")
  string(SUBSTRING "${_hash}" 0 16 _hash)
  set(${OUT_VAR} "${NAME}-${_hash}" PARENT_SCOPE)
endfunction()

function(rx_add_cached_dependency)
  set(oneValueArgs NAME REPO TAG)
  set(multiValueArgs CMAKE_ARGS)
  cmake_parse_arguments(DEP "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  rx_dep_cache_key(_key ${DEP_NAME} ${DEP_TAG})
  set(_cache_dir "${CMAKE_SOURCE_DIR}/.deps-cache/${_key}")
  set(_marker "${_cache_dir}/.rx-built")

  if(NOT EXISTS "${_marker}")
    message(STATUS "[dep-cache] MISS for ${DEP_NAME} (key=${_key}) - building once")

    set(_src_dir "${CMAKE_BINARY_DIR}/_deps-src/${DEP_NAME}")
    if(NOT EXISTS "${_src_dir}/.git")
      execute_process(COMMAND git clone --quiet "${DEP_REPO}" "${_src_dir}" RESULT_VARIABLE _rv)
      if(NOT _rv EQUAL 0)
        message(FATAL_ERROR "[dep-cache] git clone failed for ${DEP_NAME} (${DEP_REPO})")
      endif()
    endif()

    execute_process(COMMAND git -C "${_src_dir}" fetch --quiet --depth 1 origin "${DEP_TAG}" RESULT_VARIABLE _rv)
    if(NOT _rv EQUAL 0)
      message(FATAL_ERROR "[dep-cache] git fetch of pin '${DEP_TAG}' failed for ${DEP_NAME}")
    endif()

    execute_process(COMMAND git -C "${_src_dir}" checkout --quiet FETCH_HEAD RESULT_VARIABLE _rv)
    if(NOT _rv EQUAL 0)
      message(FATAL_ERROR "[dep-cache] git checkout of pin '${DEP_TAG}' failed for ${DEP_NAME}")
    endif()

    set(_build_dir "${CMAKE_BINARY_DIR}/_deps-build/${DEP_NAME}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -S "${_src_dir}" -B "${_build_dir}" -G "${CMAKE_GENERATOR}"
              "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
              "-DCMAKE_INSTALL_PREFIX=${_cache_dir}"
              "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
              ${DEP_CMAKE_ARGS}
      RESULT_VARIABLE _rv)
    if(NOT _rv EQUAL 0)
      message(FATAL_ERROR "[dep-cache] configure failed for ${DEP_NAME} - see output above")
    endif()

    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_build_dir}" --target install RESULT_VARIABLE _rv)
    if(NOT _rv EQUAL 0)
      message(FATAL_ERROR "[dep-cache] build/install failed for ${DEP_NAME} - see output above")
    endif()

    file(WRITE "${_marker}" "${DEP_TAG}\n")
  else()
    message(STATUS "[dep-cache] HIT for ${DEP_NAME} (key=${_key}) - reusing cached install, no compilation")
  endif()

  set(${DEP_NAME}_CACHE_DIR "${_cache_dir}" PARENT_SCOPE)
  set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH};${_cache_dir}" PARENT_SCOPE)
endfunction()
```

**Design note (for the reviewer):** only genuinely compiled dependencies go through this module (SDL3 here in Task 5, vk-bootstrap in Task 7). Header-only libraries (GLM, VMA, doctest) and single-file-compiled-into-the-consumer libraries (volk) have nothing worth caching, so later tasks fetch those directly with `FetchContent` instead of wrapping them in `rx_add_cached_dependency`.

- [ ] **Step 2: Write third_party/CMakeLists.txt with spdlog pinned**

`third_party/CMakeLists.txt`:
```cmake
include("${CMAKE_SOURCE_DIR}/cmake/DepCache.cmake")

set(RX_SPDLOG_TAG "v1.17.0")
rx_add_cached_dependency(
  NAME spdlog
  REPO https://github.com/gabime/spdlog.git
  TAG ${RX_SPDLOG_TAG}
  CMAKE_ARGS -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF
)
find_package(spdlog REQUIRED PATHS "${spdlog_CACHE_DIR}" NO_DEFAULT_PATH)
```

- [ ] **Step 3: Write a smoke-test executable proving spdlog links and runs**

`tools/dep_cache_smoketest/CMakeLists.txt`:
```cmake
add_executable(dep_cache_smoketest main.cpp)
target_link_libraries(dep_cache_smoketest PRIVATE spdlog::spdlog)
```

`tools/dep_cache_smoketest/main.cpp`:
```cpp
#include <spdlog/spdlog.h>

int main() {
    spdlog::info("hello from cached spdlog");
    return 0;
}
```

- [ ] **Step 4: Wire into the root CMakeLists.txt**

Add to `CMakeLists.txt` (after `enable_testing()`):
```cmake
add_subdirectory(third_party)
add_subdirectory(tools/dep_cache_smoketest)
```

- [ ] **Step 5: Verify cache MISS then HIT, and correct output**

```bash
rm -rf build/linux-native .deps-cache
cmake --preset linux-native 2>&1 | grep -m1 "\[dep-cache\] MISS for spdlog"
cmake --build --preset linux-native
./build/linux-native/tools/dep_cache_smoketest/dep_cache_smoketest
```
Expected: the grep finds the MISS line, the binary prints `[...] [info] hello from cached spdlog`.

Then, without touching the pin, force a reconfigure and confirm a cache hit with no rebuild of spdlog itself:
```bash
rm -rf build/linux-native
cmake --preset linux-native 2>&1 | grep -m1 "\[dep-cache\] HIT for spdlog"
```
Expected: the HIT line is found, and this reconfigure does not invoke spdlog's own build (no `_deps-build/spdlog` compiler output appears in the log).

- [ ] **Step 6: Commit**

```bash
git add cmake/DepCache.cmake third_party/ tools/dep_cache_smoketest/ CMakeLists.txt
git commit -m "Add content-hashed dependency cache, proven with spdlog"
```

---

### Task 4: rx_core (logging, handles, math)

**Files:**
- Create: `src/rx_core/CMakeLists.txt`
- Create: `src/rx_core/include/rx_core/log.h`, `src/rx_core/src/log.cpp`
- Create: `src/rx_core/include/rx_core/handle.h`
- Create: `src/rx_core/tests/log_test.cpp`, `src/rx_core/tests/handle_test.cpp`, `src/rx_core/tests/math_test.cpp`
- Modify: `third_party/CMakeLists.txt` (add doctest, GLM)
- Modify: `CMakeLists.txt` (add `src/rx_core`)

**Interfaces:**
- Produces: `rx::core::log::init()`, `RX_LOG_INFO(...)`/`RX_LOG_WARN(...)`/`RX_LOG_ERROR(...)` macros; `rx::core::Handle<Tag>` with `.index()`, `.generation()`, `.isValid()`, and `rx::core::HandlePool<Tag,T>::acquire()/release(Handle<Tag>)/get(Handle<Tag>)`. Target `rx_core` (static library) and `doctest::doctest`, `glm::glm` as reusable third-party targets for later tasks.

- [ ] **Step 1: Add doctest and GLM to third_party**

Append to `third_party/CMakeLists.txt`:
```cmake
include(FetchContent)

set(RX_DOCTEST_TAG "v2.5.3")
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG ${RX_DOCTEST_TAG}
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(doctest)

set(RX_GLM_TAG "1.0.3")
FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG ${RX_GLM_TAG}
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)
```

- [ ] **Step 2: Write the failing tests**

`src/rx_core/tests/log_test.cpp`:
```cpp
#include <doctest/doctest.h>
#include <rx_core/log.h>

TEST_CASE("log::init is idempotent and logging macros do not throw") {
    rx::core::log::init();
    rx::core::log::init();
    RX_LOG_INFO("test info {}", 1);
    RX_LOG_WARN("test warn {}", 2);
    RX_LOG_ERROR("test error {}", 3);
    CHECK(true);
}
```

`src/rx_core/tests/handle_test.cpp`:
```cpp
#include <doctest/doctest.h>
#include <rx_core/handle.h>

struct MeshTag {};

TEST_CASE("HandlePool acquire/get/release round-trips a value and invalidates stale handles") {
    rx::core::HandlePool<MeshTag, int> pool;

    auto h1 = pool.acquire(42);
    CHECK(h1.isValid());
    CHECK(*pool.get(h1) == 42);

    pool.release(h1);
    CHECK(pool.get(h1) == nullptr);

    auto h2 = pool.acquire(7);
    CHECK(h2.isValid());
    CHECK(*pool.get(h2) == 7);
    CHECK(h1.index() == h2.index());
    CHECK(h1.generation() != h2.generation());
}
```

`src/rx_core/tests/math_test.cpp`:
```cpp
#include <doctest/doctest.h>
#include <glm/glm.hpp>

TEST_CASE("GLM is linked and usable") {
    glm::vec3 a{1.0f, 2.0f, 3.0f};
    glm::vec3 b{4.0f, 5.0f, 6.0f};
    glm::vec3 c = a + b;
    CHECK(c.x == doctest::Approx(5.0f));
    CHECK(c.y == doctest::Approx(7.0f));
    CHECK(c.z == doctest::Approx(9.0f));
}
```

- [ ] **Step 3: Run to verify they fail to compile (headers don't exist yet)**

```bash
cmake --preset linux-native && cmake --build --preset linux-native --target rx_core_tests
```
Expected: FAIL — `rx_core/log.h: No such file or directory` (target doesn't exist yet either; this is expected at this point).

- [ ] **Step 4: Implement rx_core**

`src/rx_core/include/rx_core/log.h`:
```cpp
#pragma once
#include <spdlog/spdlog.h>

namespace rx::core::log {

void init();

}  // namespace rx::core::log

#define RX_LOG_INFO(...)  ::spdlog::info(__VA_ARGS__)
#define RX_LOG_WARN(...)  ::spdlog::warn(__VA_ARGS__)
#define RX_LOG_ERROR(...) ::spdlog::error(__VA_ARGS__)
```

`src/rx_core/src/log.cpp`:
```cpp
#include <rx_core/log.h>

namespace rx::core::log {

void init() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    initialized = true;
}

}  // namespace rx::core::log
```

`src/rx_core/include/rx_core/handle.h`:
```cpp
#pragma once
#include <cstdint>
#include <vector>

namespace rx::core {

template <typename Tag>
class Handle {
public:
    Handle() = default;
    Handle(uint32_t index, uint32_t generation) : index_(index), generation_(generation) {}

    uint32_t index() const { return index_; }
    uint32_t generation() const { return generation_; }
    bool isValid() const { return generation_ != 0; }

    bool operator==(const Handle& other) const {
        return index_ == other.index_ && generation_ == other.generation_;
    }

private:
    uint32_t index_ = 0;
    uint32_t generation_ = 0;
};

template <typename Tag, typename T>
class HandlePool {
public:
    Handle<Tag> acquire(T value) {
        if (!freeList_.empty()) {
            uint32_t idx = freeList_.back();
            freeList_.pop_back();
            slots_[idx].value = std::move(value);
            slots_[idx].generation += 1;
            slots_[idx].alive = true;
            return Handle<Tag>(idx, slots_[idx].generation);
        }
        slots_.push_back(Slot{std::move(value), /*generation=*/1, /*alive=*/true});
        return Handle<Tag>(static_cast<uint32_t>(slots_.size() - 1), 1);
    }

    void release(Handle<Tag> handle) {
        if (!isLive(handle)) {
            return;
        }
        slots_[handle.index()].alive = false;
        freeList_.push_back(handle.index());
    }

    T* get(Handle<Tag> handle) {
        if (!isLive(handle)) {
            return nullptr;
        }
        return &slots_[handle.index()].value;
    }

private:
    struct Slot {
        T value;
        uint32_t generation = 0;
        bool alive = false;
    };

    bool isLive(Handle<Tag> handle) const {
        return handle.index() < slots_.size() &&
               slots_[handle.index()].alive &&
               slots_[handle.index()].generation == handle.generation();
    }

    std::vector<Slot> slots_;
    std::vector<uint32_t> freeList_;
};

}  // namespace rx::core
```

`src/rx_core/CMakeLists.txt`:
```cmake
add_library(rx_core STATIC
    src/log.cpp
)
target_include_directories(rx_core PUBLIC include)
target_link_libraries(rx_core PUBLIC spdlog::spdlog glm::glm)

add_executable(rx_core_tests
    tests/log_test.cpp
    tests/handle_test.cpp
    tests/math_test.cpp
)
target_link_libraries(rx_core_tests PRIVATE rx_core doctest::doctest)
add_test(NAME rx_core_tests COMMAND rx_core_tests)
```

- [ ] **Step 5: Wire into root CMakeLists.txt**

Add to `CMakeLists.txt`:
```cmake
add_subdirectory(src/rx_core)
```

- [ ] **Step 6: Run tests and verify they pass**

```bash
cmake --build --preset linux-native --target rx_core_tests
ctest --preset linux-native -R rx_core_tests --output-on-failure
```
Expected: `100% tests passed, 0 tests failed out of 1`, doctest reports all `CHECK`s passing.

- [ ] **Step 7: Commit**

```bash
git add third_party/CMakeLists.txt src/rx_core/ CMakeLists.txt
git commit -m "Add rx_core: logging, generational handles, GLM math"
```

---

### Task 5: rx_platform (SDL3 window wrapper)

**Files:**
- Create: `src/rx_platform/CMakeLists.txt`
- Create: `src/rx_platform/include/rx_platform/window.h`, `src/rx_platform/src/window.cpp`
- Create: `src/rx_platform/tests/window_test.cpp`
- Modify: `third_party/CMakeLists.txt` (add SDL3 via dep-cache)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `rx::core::log` (Task 4).
- Produces: `rx::platform::Window` with `Window::create(title, width, height, visible) -> std::optional<Window>`, `.sdlWindow()`, `.pumpEvents()`, `.requiredVulkanInstanceExtensions() -> std::vector<const char*>`, `.createVulkanSurface(VkInstance) -> VkSurfaceKHR` (returns `VK_NULL_HANDLE` on failure). Target `rx_platform`, consumed by `rx_rhi_vk` starting Task 7.

- [ ] **Step 1: Add SDL3 to third_party via the dependency cache**

Append to `third_party/CMakeLists.txt`:
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

- [ ] **Step 2: Write the failing test**

`src/rx_platform/tests/window_test.cpp`:
```cpp
#include <doctest/doctest.h>
#include <rx_platform/window.h>

TEST_CASE("Window::create/destroy lifecycle succeeds under any video driver") {
    auto window = rx::platform::Window::create("rx_platform_test", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());
    CHECK(window->sdlWindow() != nullptr);
    window->pumpEvents();
}

TEST_CASE("Window reports Vulkan instance extensions when a real display backend is present") {
    auto window = rx::platform::Window::create("rx_platform_vk_test", 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping Vulkan-extension check");
        return;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping");
        return;
    }
    CHECK(extensions.size() > 0);
}
```

- [ ] **Step 3: Run to verify it fails (header doesn't exist yet)**

```bash
cmake --preset linux-native && cmake --build --preset linux-native --target rx_platform_tests
```
Expected: FAIL — `rx_platform/window.h: No such file or directory`.

- [ ] **Step 4: Implement rx_platform**

`src/rx_platform/include/rx_platform/window.h`:
```cpp
#pragma once
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <optional>
#include <vector>

namespace rx::platform {

class Window {
public:
    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    static std::optional<Window> create(const char* title, int width, int height, bool visible);

    SDL_Window* sdlWindow() const { return window_; }
    void pumpEvents();

    std::vector<const char*> requiredVulkanInstanceExtensions() const;
    VkSurfaceKHR createVulkanSurface(VkInstance instance) const;

private:
    explicit Window(SDL_Window* window) : window_(window) {}
    SDL_Window* window_ = nullptr;
};

}  // namespace rx::platform
```

`src/rx_platform/src/window.cpp`:
```cpp
#include <rx_platform/window.h>
#include <rx_core/log.h>

namespace rx::platform {

std::optional<Window> Window::create(const char* title, int width, int height, bool visible) {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            RX_LOG_WARN("SDL_Init(SDL_INIT_VIDEO) failed: {}", SDL_GetError());
            return std::nullopt;
        }
    }

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (!visible) {
        flags |= SDL_WINDOW_HIDDEN;
    }

    SDL_Window* window = SDL_CreateWindow(title, width, height, flags);
    if (!window) {
        RX_LOG_WARN("SDL_CreateWindow failed: {}", SDL_GetError());
        return std::nullopt;
    }
    return Window(window);
}

Window::Window(Window&& other) noexcept : window_(other.window_) {
    other.window_ = nullptr;
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        if (window_) {
            SDL_DestroyWindow(window_);
        }
        window_ = other.window_;
        other.window_ = nullptr;
    }
    return *this;
}

Window::~Window() {
    if (window_) {
        SDL_DestroyWindow(window_);
    }
}

void Window::pumpEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Intentionally empty for now: rx_platform only exposes the pump,
        // event handling policy belongs to whatever embeds this window.
    }
}

std::vector<const char*> Window::requiredVulkanInstanceExtensions() const {
    Uint32 count = 0;
    char const* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names) {
        return {};
    }
    return std::vector<const char*>(names, names + count);
}

VkSurfaceKHR Window::createVulkanSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface)) {
        RX_LOG_WARN("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
        return VK_NULL_HANDLE;
    }
    return surface;
}

}  // namespace rx::platform
```

`src/rx_platform/CMakeLists.txt`:
```cmake
find_package(Vulkan REQUIRED)

add_library(rx_platform STATIC
    src/window.cpp
)
target_include_directories(rx_platform PUBLIC include)
target_link_libraries(rx_platform PUBLIC SDL3::SDL3 Vulkan::Headers rx_core)

add_executable(rx_platform_tests
    tests/window_test.cpp
)
target_link_libraries(rx_platform_tests PRIVATE rx_platform doctest::doctest)
add_test(NAME rx_platform_tests COMMAND rx_platform_tests)
```

- [ ] **Step 5: Wire into root CMakeLists.txt**

Add to `CMakeLists.txt`:
```cmake
add_subdirectory(src/rx_platform)
```

- [ ] **Step 6: Run tests and verify they pass**

```bash
cmake --build --preset linux-native --target rx_platform_tests
ctest --preset linux-native -R rx_platform_tests --output-on-failure
```
Expected: both test cases pass on this machine (it has a real X11 `DISPLAY`, so the Vulkan-extension check runs for real rather than skipping).

- [ ] **Step 7: Commit**

```bash
git add third_party/CMakeLists.txt src/rx_platform/ CMakeLists.txt
git commit -m "Add rx_platform: SDL3 window wrapper"
```

---

### Task 6: rx_rhi_vk::Context (instance + validation layers)

**Files:**
- Create: `src/rx_rhi_vk/CMakeLists.txt`
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/context.h`, `src/rx_rhi_vk/src/context.cpp`
- Create: `src/rx_rhi_vk/tests/context_test.cpp`
- Modify: `third_party/CMakeLists.txt` (add volk via FetchContent, vk-bootstrap via dep-cache)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `rx::core::log` (Task 4).
- Produces: `rx::rhi::Context` with `Context::create(requiredExtensions, enableValidation) -> std::optional<Context>`, `.instance()`, `.hasValidationErrors()` (true if the debug messenger ever reported an error/warning during this Context's lifetime). Target `rx_rhi_vk`, consumed by every later RHI task.

- [ ] **Step 1: Add volk and vk-bootstrap to third_party**

Append to `third_party/CMakeLists.txt`:
```cmake
set(RX_VOLK_TAG "vulkan-sdk-1.4.357.0")
FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG ${RX_VOLK_TAG}
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(volk)

set(RX_VK_BOOTSTRAP_COMMIT "556b79b165386f6c1a18362d30f2a076fdaa2778")
rx_add_cached_dependency(
  NAME vk-bootstrap
  REPO https://github.com/charles-lunarg/vk-bootstrap.git
  TAG ${RX_VK_BOOTSTRAP_COMMIT}
  CMAKE_ARGS -DVK_BOOTSTRAP_TEST=OFF
)
find_package(vk-bootstrap REQUIRED PATHS "${vk-bootstrap_CACHE_DIR}" NO_DEFAULT_PATH)
```

- [ ] **Step 2: Write the failing test**

`src/rx_rhi_vk/tests/context_test.cpp`:
```cpp
#include <doctest/doctest.h>
#include <rx_rhi_vk/context.h>

TEST_CASE("Context::create succeeds with no required extensions and reports no validation errors") {
    auto ctx = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(ctx.has_value());
    CHECK(ctx->instance() != VK_NULL_HANDLE);
    CHECK_FALSE(ctx->hasValidationErrors());
}
```

- [ ] **Step 3: Run to verify it fails**

```bash
cmake --preset linux-native && cmake --build --preset linux-native --target rx_rhi_vk_tests
```
Expected: FAIL — `rx_rhi_vk/context.h: No such file or directory`.

- [ ] **Step 4: Implement Context**

`src/rx_rhi_vk/include/rx_rhi_vk/context.h`:
```cpp
#pragma once
#include <volk.h>
#include <optional>
#include <vector>

namespace rx::rhi {

class Context {
public:
    Context(Context&&) noexcept;
    Context& operator=(Context&&) noexcept;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context();

    static std::optional<Context> create(std::vector<const char*> requiredExtensions, bool enableValidation);

    VkInstance instance() const { return instance_; }
    VkDebugUtilsMessengerEXT debugMessenger() const { return debugMessenger_; }
    bool hasValidationErrors() const { return *errorCount_ > 0; }

private:
    Context(VkInstance instance, VkDebugUtilsMessengerEXT messenger, std::shared_ptr<int> errorCount)
        : instance_(instance), debugMessenger_(messenger), errorCount_(std::move(errorCount)) {}

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    std::shared_ptr<int> errorCount_;
};

}  // namespace rx::rhi
```

`src/rx_rhi_vk/src/context.cpp`:
```cpp
#include <rx_rhi_vk/context.h>
#include <rx_core/log.h>
#include <VkBootstrap.h>
#include <memory>

namespace rx::rhi {

namespace {

VkBool32 debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                        VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                        const VkDebugUtilsMessengerCallbackDataEXT* data,
                        void* userData) {
    auto* errorCount = static_cast<int*>(userData);
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        RX_LOG_ERROR("[vulkan validation] {}", data->pMessage);
        (*errorCount)++;
    } else {
        RX_LOG_INFO("[vulkan validation] {}", data->pMessage);
    }
    return VK_FALSE;
}

}  // namespace

std::optional<Context> Context::create(std::vector<const char*> requiredExtensions, bool enableValidation) {
    rx::core::log::init();

    if (volkInitialize() != VK_SUCCESS) {
        RX_LOG_ERROR("volkInitialize failed");
        return std::nullopt;
    }

    auto errorCount = std::make_shared<int>(0);

    vkb::InstanceBuilder builder;
    builder.set_app_name("renderer_x")
        .require_api_version(1, 3, 0)
        .set_headless(requiredExtensions.empty());

    for (const char* ext : requiredExtensions) {
        builder.enable_extension(ext);
    }

    if (enableValidation) {
        builder.request_validation_layers()
            .set_debug_callback(debugCallback)
            .set_debug_callback_user_data_pointer(errorCount.get());
    }

    auto result = builder.build();
    if (!result) {
        RX_LOG_ERROR("vkb::InstanceBuilder::build failed: {}", result.error().message());
        return std::nullopt;
    }

    vkb::Instance vkbInstance = result.value();
    volkLoadInstance(vkbInstance.instance);

    return Context(vkbInstance.instance, vkbInstance.debug_messenger, errorCount);
}

Context::Context(Context&& other) noexcept
    : instance_(other.instance_), debugMessenger_(other.debugMessenger_), errorCount_(std::move(other.errorCount_)) {
    other.instance_ = VK_NULL_HANDLE;
    other.debugMessenger_ = VK_NULL_HANDLE;
}

Context& Context::operator=(Context&& other) noexcept {
    if (this != &other) {
        if (instance_ != VK_NULL_HANDLE) {
            if (debugMessenger_ != VK_NULL_HANDLE) {
                vkb::destroy_debug_utils_messenger(instance_, debugMessenger_);
            }
            vkDestroyInstance(instance_, nullptr);
        }
        instance_ = other.instance_;
        debugMessenger_ = other.debugMessenger_;
        errorCount_ = std::move(other.errorCount_);
        other.instance_ = VK_NULL_HANDLE;
        other.debugMessenger_ = VK_NULL_HANDLE;
    }
    return *this;
}

Context::~Context() {
    if (instance_ != VK_NULL_HANDLE) {
        if (debugMessenger_ != VK_NULL_HANDLE) {
            vkb::destroy_debug_utils_messenger(instance_, debugMessenger_);
        }
        vkDestroyInstance(instance_, nullptr);
    }
}

}  // namespace rx::rhi
```

`src/rx_rhi_vk/CMakeLists.txt`:
```cmake
add_library(rx_rhi_vk STATIC
    src/context.cpp
    ${volk_SOURCE_DIR}/volk.c
)
target_include_directories(rx_rhi_vk PUBLIC include ${volk_SOURCE_DIR})
target_link_libraries(rx_rhi_vk PUBLIC rx_core vk-bootstrap::vk-bootstrap)
target_compile_definitions(rx_rhi_vk PUBLIC VK_NO_PROTOTYPES)

add_executable(rx_rhi_vk_tests
    tests/context_test.cpp
)
target_link_libraries(rx_rhi_vk_tests PRIVATE rx_rhi_vk doctest::doctest)
add_test(NAME rx_rhi_vk_tests COMMAND rx_rhi_vk_tests)
```

- [ ] **Step 5: Wire into root CMakeLists.txt**

Add to `CMakeLists.txt`:
```cmake
add_subdirectory(src/rx_rhi_vk)
```

- [ ] **Step 6: Run tests and verify they pass**

```bash
cmake --build --preset linux-native --target rx_rhi_vk_tests
ctest --preset linux-native -R rx_rhi_vk_tests --output-on-failure
```
Expected: instance creation succeeds and `hasValidationErrors()` is false.

- [ ] **Step 7: Commit**

```bash
git add third_party/CMakeLists.txt src/rx_rhi_vk/ CMakeLists.txt
git commit -m "Add rx_rhi_vk::Context: Vulkan instance and validation layers"
```

---

### Task 7: rx_rhi_vk::Device (physical+logical device, queues, swapchain)

**Files:**
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/device.h`, `src/rx_rhi_vk/src/device.cpp`
- Create: `src/rx_rhi_vk/tests/device_test.cpp`
- Modify: `src/rx_rhi_vk/CMakeLists.txt`

**Interfaces:**
- Consumes: `rx::rhi::Context` (Task 6), `rx::platform::Window::createVulkanSurface`/`requiredVulkanInstanceExtensions` (Task 5).
- Produces: `rx::rhi::Device` with `Device::create(Context&, VkSurfaceKHR) -> std::optional<Device>`, `.device()`, `.physicalDevice()`, `.graphicsQueue()`, `.graphicsQueueFamily()`, `.presentQueue()`, `.swapchain()`, `.swapchainImages()`, `.swapchainFormat()`.

- [ ] **Step 1: Write the failing test**

`src/rx_rhi_vk/tests/device_test.cpp`:
```cpp
#include <doctest/doctest.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_platform/window.h>

TEST_CASE("Device::create succeeds against a real window surface with a non-empty swapchain and no validation errors") {
    auto window = rx::platform::Window::create("rx_rhi_vk_device_test", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("no real display backend available, skipping device+swapchain test");
        return;
    }

    auto ctx = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(ctx.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(ctx->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    auto device = rx::rhi::Device::create(*ctx, surface);
    REQUIRE(device.has_value());
    CHECK(device->device() != VK_NULL_HANDLE);
    CHECK(device->swapchainImages().size() > 0);
    CHECK_FALSE(ctx->hasValidationErrors());
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build --preset linux-native --target rx_rhi_vk_tests
```
Expected: FAIL — `rx_rhi_vk/device.h: No such file or directory`.

- [ ] **Step 3: Implement Device**

`src/rx_rhi_vk/include/rx_rhi_vk/device.h`:
```cpp
#pragma once
#include <rx_rhi_vk/context.h>
#include <vector>

namespace rx::rhi {

class Device {
public:
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device();

    static std::optional<Device> create(Context& context, VkSurfaceKHR surface);

    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    VkQueue graphicsQueue() const { return graphicsQueue_; }
    uint32_t graphicsQueueFamily() const { return graphicsQueueFamily_; }
    VkQueue presentQueue() const { return presentQueue_; }
    VkSwapchainKHR swapchain() const { return swapchain_; }
    const std::vector<VkImage>& swapchainImages() const { return swapchainImages_; }
    VkFormat swapchainFormat() const { return swapchainFormat_; }

private:
    Device() = default;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily_ = 0;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages_;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
};

}  // namespace rx::rhi
```

`src/rx_rhi_vk/src/device.cpp`:
```cpp
#include <rx_rhi_vk/device.h>
#include <rx_core/log.h>
#include <VkBootstrap.h>

namespace rx::rhi {

std::optional<Device> Device::create(Context& context, VkSurfaceKHR surface) {
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    vkb::PhysicalDeviceSelector selector{
        vkb::InstanceBuilder{}.build().value()  // placeholder replaced below
    };
    (void)selector;  // see note below

    vkb::PhysicalDeviceSelector realSelector(
        vkb::Instance{context.instance(), context.debugMessenger(), nullptr, {}});
    // vkb::Instance requires more fields than are convenient to reconstruct here;
    // instead select directly against the raw handles vk-bootstrap also accepts:
    vkb::SystemInfo systemInfo = vkb::SystemInfo::get_system_info().value();
    (void)systemInfo;

    return std::nullopt;  // INTENTIONALLY INCOMPLETE - see Step 3b
}

}  // namespace rx::rhi
```

**Stop — Step 3 above is deliberately broken.** `vkb::PhysicalDeviceSelector` and `vkb::DeviceBuilder` are built from a `vkb::Instance` struct, not a raw `VkInstance`, and `Context` (Task 6) only stores the raw handles. Before writing the real implementation, extend `Context` to retain the `vkb::Instance` it already builds internally:

- [ ] **Step 3a: Extend Context to expose its vkb::Instance**

In `src/rx_rhi_vk/include/rx_rhi_vk/context.h`, add an accessor:
```cpp
    const vkb::Instance& vkbInstance() const { return vkbInstance_; }
```
and a member:
```cpp
    vkb::Instance vkbInstance_;
```
(`#include <VkBootstrap.h>` at the top of this header instead of forward-declaring, since `vkb::Instance` is stored by value.)

In `src/rx_rhi_vk/src/context.cpp`, change the private constructor and `create()`'s return to also store `vkbInstance`:
```cpp
    Context(vkb::Instance vkbInstance, std::shared_ptr<int> errorCount)
        : vkbInstance_(std::move(vkbInstance)), errorCount_(std::move(errorCount)) {}
```
and update `instance()`/`debugMessenger()` to read through `vkbInstance_.instance` / `vkbInstance_.debug_messenger`, and update every constructor/assignment/destructor accordingly to move/destroy `vkbInstance_` via `vkb::destroy_instance(vkbInstance_)` instead of raw `vkDestroyInstance`/`destroy_debug_utils_messenger` calls. Rebuild and re-run `rx_rhi_vk_tests` (Task 6's test) to confirm this refactor didn't break anything:
```bash
cmake --build --preset linux-native --target rx_rhi_vk_tests
ctest --preset linux-native -R rx_rhi_vk_tests --output-on-failure
```
Expected: still passes.

- [ ] **Step 3b: Implement Device for real, using Context::vkbInstance()**

Replace the placeholder `src/rx_rhi_vk/src/device.cpp` body with:
```cpp
#include <rx_rhi_vk/device.h>
#include <rx_core/log.h>
#include <VkBootstrap.h>

namespace rx::rhi {

std::optional<Device> Device::create(Context& context, VkSurfaceKHR surface) {
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    vkb::PhysicalDeviceSelector selector(context.vkbInstance());
    auto physResult = selector.set_surface(surface)
                           .set_minimum_version(1, 3)
                           .set_required_features_13(features13)
                           .select();
    if (!physResult) {
        RX_LOG_ERROR("PhysicalDeviceSelector::select failed: {}", physResult.error().message());
        return std::nullopt;
    }

    vkb::DeviceBuilder deviceBuilder(physResult.value());
    auto devResult = deviceBuilder.build();
    if (!devResult) {
        RX_LOG_ERROR("DeviceBuilder::build failed: {}", devResult.error().message());
        return std::nullopt;
    }
    vkb::Device vkbDevice = devResult.value();
    volkLoadDevice(vkbDevice.device);

    auto graphicsQueueResult = vkbDevice.get_queue(vkb::QueueType::graphics);
    auto graphicsQueueFamilyResult = vkbDevice.get_queue_index(vkb::QueueType::graphics);
    auto presentQueueResult = vkbDevice.get_queue(vkb::QueueType::present);
    if (!graphicsQueueResult || !graphicsQueueFamilyResult || !presentQueueResult) {
        RX_LOG_ERROR("failed to retrieve graphics/present queue");
        return std::nullopt;
    }

    vkb::SwapchainBuilder swapchainBuilder(vkbDevice, surface);
    auto swapResult = swapchainBuilder.build();
    if (!swapResult) {
        RX_LOG_ERROR("SwapchainBuilder::build failed: {}", swapResult.error().message());
        return std::nullopt;
    }
    vkb::Swapchain vkbSwapchain = swapResult.value();
    auto images = vkbSwapchain.get_images();
    if (!images) {
        RX_LOG_ERROR("failed to retrieve swapchain images");
        return std::nullopt;
    }

    Device device;
    device.instance_ = context.instance();
    device.surface_ = surface;
    device.physicalDevice_ = vkbDevice.physical_device;
    device.device_ = vkbDevice.device;
    device.graphicsQueue_ = graphicsQueueResult.value();
    device.graphicsQueueFamily_ = graphicsQueueFamilyResult.value();
    device.presentQueue_ = presentQueueResult.value();
    device.swapchain_ = vkbSwapchain.swapchain;
    device.swapchainImages_ = images.value();
    device.swapchainFormat_ = vkbSwapchain.image_format;
    return device;
}

Device::Device(Device&& other) noexcept { *this = std::move(other); }

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        }
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
        }
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        }
        instance_ = other.instance_;
        surface_ = other.surface_;
        physicalDevice_ = other.physicalDevice_;
        device_ = other.device_;
        graphicsQueue_ = other.graphicsQueue_;
        presentQueue_ = other.presentQueue_;
        swapchain_ = other.swapchain_;
        swapchainImages_ = std::move(other.swapchainImages_);
        swapchainFormat_ = other.swapchainFormat_;
        other.instance_ = VK_NULL_HANDLE;
        other.surface_ = VK_NULL_HANDLE;
        other.device_ = VK_NULL_HANDLE;
        other.swapchain_ = VK_NULL_HANDLE;
    }
    return *this;
}

Device::~Device() {
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
}

}  // namespace rx::rhi
```

Update `src/rx_rhi_vk/CMakeLists.txt`'s library sources to include `src/device.cpp` and the tests executable to include `tests/device_test.cpp` and link `rx_platform`:
```cmake
add_library(rx_rhi_vk STATIC
    src/context.cpp
    src/device.cpp
    ${volk_SOURCE_DIR}/volk.c
)
# ... (target_include_directories / target_link_libraries unchanged) ...

add_executable(rx_rhi_vk_tests
    tests/context_test.cpp
    tests/device_test.cpp
)
target_link_libraries(rx_rhi_vk_tests PRIVATE rx_rhi_vk rx_platform doctest::doctest)
```

- [ ] **Step 4: Run tests and verify they pass**

```bash
cmake --build --preset linux-native --target rx_rhi_vk_tests
ctest --preset linux-native -R rx_rhi_vk_tests --output-on-failure
```
Expected: both `context_test` and `device_test` cases pass; swapchain has at least one image; no validation errors.

- [ ] **Step 5: Commit**

```bash
git add src/rx_rhi_vk/
git commit -m "Add rx_rhi_vk::Device: physical/logical device, queues, swapchain"
```

---

### Task 8: rx_rhi_vk::Buffer (VMA-backed host-visible buffers)

**Files:**
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/buffer.h`, `src/rx_rhi_vk/src/buffer.cpp`
- Create: `src/rx_rhi_vk/tests/buffer_test.cpp`
- Modify: `third_party/CMakeLists.txt` (add VMA via FetchContent)
- Modify: `src/rx_rhi_vk/CMakeLists.txt`

**Interfaces:**
- Consumes: `rx::rhi::Device` (Task 7).
- Produces: `rx::rhi::Allocator` (owns the `VmaAllocator`) with `Allocator::create(Context&, Device&) -> std::optional<Allocator>`; `rx::rhi::Buffer` with `Allocator::createHostVisibleBuffer(size, usage) -> std::optional<Buffer>`, `Buffer::mappedData()`, `.size()`.

**Scope note:** this buffer is host-visible/host-coherent only — no device-local staging/transfer path. That belongs to the resource-management layer's own future spec (layer 5, deferred but not dropped — see the design spec); a small static vertex buffer has no meaningful performance cost either way.

**Scope note on descriptors:** the triangle shaders (Task 11) use zero descriptors — no textures, no uniform buffers, just hardcoded per-vertex data generated in the vertex shader itself — so there is nothing to make bindless in this sub-project. The design spec's "bindless-first descriptor design from day one" principle is a constraint on layer 5's future descriptor-management work, not a deliverable of this plan; it's recorded here so it isn't forgotten when that spec gets written.

- [ ] **Step 1: Add VMA to third_party**

Append to `third_party/CMakeLists.txt`:
```cmake
set(RX_VMA_TAG "v3.4.0")
FetchContent_Declare(vma
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG ${RX_VMA_TAG}
  GIT_SHALLOW TRUE)
FetchContent_Populate(vma)
```
(`FetchContent_Populate` rather than `MakeAvailable`: VMA's own `CMakeLists.txt` builds sample apps we don't want; we only need `${vma_SOURCE_DIR}/include`.)

- [ ] **Step 2: Write the failing test**

`src/rx_rhi_vk/tests/buffer_test.cpp`:
```cpp
#include <doctest/doctest.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_platform/window.h>
#include <cstring>

TEST_CASE("Allocator creates a host-visible buffer that round-trips written bytes") {
    auto window = rx::platform::Window::create("rx_rhi_vk_buffer_test", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("no real display backend available, skipping buffer test");
        return;
    }

    auto ctx = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(ctx.has_value());
    VkSurfaceKHR surface = window->createVulkanSurface(ctx->instance());
    REQUIRE(surface != VK_NULL_HANDLE);
    auto device = rx::rhi::Device::create(*ctx, surface);
    REQUIRE(device.has_value());

    auto allocator = rx::rhi::Allocator::create(*ctx, *device);
    REQUIRE(allocator.has_value());

    struct Vertex { float x, y, z; };
    Vertex pattern[3] = {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}, {7.0f, 8.0f, 9.0f}};

    auto buffer = allocator->createHostVisibleBuffer(sizeof(pattern), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    REQUIRE(buffer.has_value());
    CHECK(buffer->size() == sizeof(pattern));

    std::memcpy(buffer->mappedData(), pattern, sizeof(pattern));
    CHECK(std::memcmp(buffer->mappedData(), pattern, sizeof(pattern)) == 0);
    CHECK_FALSE(ctx->hasValidationErrors());
}
```

- [ ] **Step 3: Run to verify it fails**

```bash
cmake --build --preset linux-native --target rx_rhi_vk_tests
```
Expected: FAIL — `rx_rhi_vk/buffer.h: No such file or directory`.

- [ ] **Step 4: Implement Allocator and Buffer**

`src/rx_rhi_vk/include/rx_rhi_vk/buffer.h`:
```cpp
#pragma once
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

namespace rx::rhi {

class Buffer {
public:
    Buffer(Buffer&&) noexcept;
    Buffer& operator=(Buffer&&) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer();

    VkBuffer handle() const { return buffer_; }
    void* mappedData() const { return mappedData_; }
    VkDeviceSize size() const { return size_; }

private:
    friend class Allocator;
    Buffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation, void* mappedData, VkDeviceSize size)
        : allocator_(allocator), buffer_(buffer), allocation_(allocation), mappedData_(mappedData), size_(size) {}

    VmaAllocator allocator_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    void* mappedData_ = nullptr;
    VkDeviceSize size_ = 0;
};

class Allocator {
public:
    Allocator(Allocator&&) noexcept;
    Allocator& operator=(Allocator&&) noexcept;
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;
    ~Allocator();

    static std::optional<Allocator> create(Context& context, Device& device);
    std::optional<Buffer> createHostVisibleBuffer(VkDeviceSize size, VkBufferUsageFlags usage);

private:
    explicit Allocator(VmaAllocator allocator) : allocator_(allocator) {}
    VmaAllocator allocator_ = nullptr;
};

}  // namespace rx::rhi
```

`src/rx_rhi_vk/src/buffer.cpp`:
```cpp
#include <rx_rhi_vk/buffer.h>
#include <rx_core/log.h>

namespace rx::rhi {

std::optional<Allocator> Allocator::create(Context& context, Device& device) {
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    functions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
    functions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
    functions.vkAllocateMemory = vkAllocateMemory;
    functions.vkFreeMemory = vkFreeMemory;
    functions.vkMapMemory = vkMapMemory;
    functions.vkUnmapMemory = vkUnmapMemory;
    functions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
    functions.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
    functions.vkBindBufferMemory = vkBindBufferMemory;
    functions.vkBindImageMemory = vkBindImageMemory;
    functions.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
    functions.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
    functions.vkCreateBuffer = vkCreateBuffer;
    functions.vkDestroyBuffer = vkDestroyBuffer;
    functions.vkCreateImage = vkCreateImage;
    functions.vkDestroyImage = vkDestroyImage;
    functions.vkCmdCopyBuffer = vkCmdCopyBuffer;

    VmaAllocatorCreateInfo createInfo{};
    createInfo.physicalDevice = device.physicalDevice();
    createInfo.device = device.device();
    createInfo.instance = context.instance();
    createInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    createInfo.pVulkanFunctions = &functions;

    VmaAllocator allocator = nullptr;
    if (vmaCreateAllocator(&createInfo, &allocator) != VK_SUCCESS) {
        RX_LOG_ERROR("vmaCreateAllocator failed");
        return std::nullopt;
    }
    return Allocator(allocator);
}

std::optional<Buffer> Allocator::createHostVisibleBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo allocationInfo{};
    if (vmaCreateBuffer(allocator_, &bufferInfo, &allocInfo, &buffer, &allocation, &allocationInfo) != VK_SUCCESS) {
        RX_LOG_ERROR("vmaCreateBuffer failed");
        return std::nullopt;
    }
    return Buffer(allocator_, buffer, allocation, allocationInfo.pMappedData, size);
}

Allocator::Allocator(Allocator&& other) noexcept : allocator_(other.allocator_) {
    other.allocator_ = nullptr;
}

Allocator& Allocator::operator=(Allocator&& other) noexcept {
    if (this != &other) {
        if (allocator_) {
            vmaDestroyAllocator(allocator_);
        }
        allocator_ = other.allocator_;
        other.allocator_ = nullptr;
    }
    return *this;
}

Allocator::~Allocator() {
    if (allocator_) {
        vmaDestroyAllocator(allocator_);
    }
}

Buffer::Buffer(Buffer&& other) noexcept
    : allocator_(other.allocator_), buffer_(other.buffer_), allocation_(other.allocation_),
      mappedData_(other.mappedData_), size_(other.size_) {
    other.buffer_ = VK_NULL_HANDLE;
    other.allocation_ = nullptr;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        if (buffer_ != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator_, buffer_, allocation_);
        }
        allocator_ = other.allocator_;
        buffer_ = other.buffer_;
        allocation_ = other.allocation_;
        mappedData_ = other.mappedData_;
        size_ = other.size_;
        other.buffer_ = VK_NULL_HANDLE;
        other.allocation_ = nullptr;
    }
    return *this;
}

Buffer::~Buffer() {
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
}

}  // namespace rx::rhi
```

Add `src/buffer.cpp` to the library sources and `${vma_SOURCE_DIR}/include` to include dirs in `src/rx_rhi_vk/CMakeLists.txt`:
```cmake
target_include_directories(rx_rhi_vk PUBLIC include ${volk_SOURCE_DIR} ${vma_SOURCE_DIR}/include)
```
Add `tests/buffer_test.cpp` to `rx_rhi_vk_tests` sources.

- [ ] **Step 5: Run tests and verify they pass**

```bash
cmake --build --preset linux-native --target rx_rhi_vk_tests
ctest --preset linux-native -R rx_rhi_vk_tests --output-on-failure
```
Expected: buffer bytes round-trip exactly, no validation errors.

- [ ] **Step 6: Commit**

```bash
git add third_party/CMakeLists.txt src/rx_rhi_vk/
git commit -m "Add rx_rhi_vk::Buffer: VMA-backed host-visible buffers"
```

---

### Task 9: rx_rhi_vk command recording + dynamic rendering + sync2 (clear-color correctness test)

**Files:**
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/command.h`, `src/rx_rhi_vk/src/command.cpp`
- Create: `src/rx_rhi_vk/tests/clear_color_test.cpp`
- Modify: `src/rx_rhi_vk/CMakeLists.txt`

**Interfaces:**
- Consumes: `rx::rhi::Device`, `rx::rhi::Allocator`/`Buffer` (Tasks 7-8).
- Produces: `rx::rhi::CommandContext` with `CommandContext::create(Device&) -> std::optional<CommandContext>`, `.runOnce(std::function<void(VkCommandBuffer)>)` (allocates, begins, invokes the callback, ends, submits to the graphics queue, waits idle — a synchronous one-shot recorder, sufficient for setup/test code; a pooled per-frame recorder is future work once real frame pacing is needed). Free functions `transitionImage(VkCommandBuffer, VkImage, VkImageLayout oldLayout, VkImageLayout newLayout, ...)` using `VkImageMemoryBarrier2`/`vkCmdPipelineBarrier2`.

This task's test renders to a plain offscreen `VkImage` (not the swapchain), so it needs a Vulkan device but **no window, no display, no swapchain** — it runs in pure headless CI.

- [ ] **Step 1: Write the failing test**

`src/rx_rhi_vk/tests/clear_color_test.cpp`:
```cpp
#include <doctest/doctest.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <cstring>

namespace {

// Minimal headless device setup: no window/surface needed for this test,
// so we bypass rx_platform::Window and rx_rhi_vk::Device's swapchain path.
struct HeadlessDevice {
    rx::rhi::Context ctx;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;
};

std::optional<HeadlessDevice> createHeadlessDevice() {
    auto ctx = rx::rhi::Context::create({}, /*enableValidation=*/true);
    if (!ctx.has_value()) return std::nullopt;

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    vkb::PhysicalDeviceSelector selector(ctx->vkbInstance());
    auto physResult = selector.defer_surface_initialization()
                           .set_minimum_version(1, 3)
                           .set_required_features_13(features13)
                           .select();
    if (!physResult) return std::nullopt;

    vkb::DeviceBuilder deviceBuilder(physResult.value());
    auto devResult = deviceBuilder.build();
    if (!devResult) return std::nullopt;
    vkb::Device vkbDevice = devResult.value();
    volkLoadDevice(vkbDevice.device);

    auto queueResult = vkbDevice.get_queue(vkb::QueueType::graphics);
    auto queueIndexResult = vkbDevice.get_queue_index(vkb::QueueType::graphics);
    if (!queueResult || !queueIndexResult) return std::nullopt;

    HeadlessDevice result;
    result.ctx = std::move(*ctx);
    result.physicalDevice = vkbDevice.physical_device;
    result.device = vkbDevice.device;
    result.graphicsQueue = queueResult.value();
    result.graphicsQueueFamily = queueIndexResult.value();
    return result;
}

}  // namespace

TEST_CASE("Clearing an offscreen image via dynamic rendering produces the exact clear color") {
    auto headless = createHeadlessDevice();
    REQUIRE(headless.has_value());

    auto cmdCtx = rx::rhi::CommandContext::create(headless->device, headless->graphicsQueue, headless->graphicsQueueFamily);
    REQUIRE(cmdCtx.has_value());

    constexpr uint32_t kWidth = 4, kHeight = 4;
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kFormat;
    imageInfo.extent = {kWidth, kHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkImage image = VK_NULL_HANDLE;
    REQUIRE(vkCreateImage(headless->device, &imageInfo, nullptr, &image) == VK_SUCCESS);

    VkMemoryRequirements memReqs{};
    vkGetImageMemoryRequirements(headless->device, image, &memReqs);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(headless->physicalDevice, &memProps);
    uint32_t memTypeIndex = 0;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memTypeIndex = i;
            break;
        }
    }
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memTypeIndex;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    REQUIRE(vkAllocateMemory(headless->device, &allocInfo, nullptr, &imageMemory) == VK_SUCCESS);
    REQUIRE(vkBindImageMemory(headless->device, image, imageMemory, 0) == VK_SUCCESS);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView imageView = VK_NULL_HANDLE;
    REQUIRE(vkCreateImageView(headless->device, &viewInfo, nullptr, &imageView) == VK_SUCCESS);

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        rx::rhi::transitionImage(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = imageView;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{1.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, {kWidth, kHeight}};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(cmd, &renderingInfo);
        vkCmdEndRendering(cmd);

        rx::rhi::transitionImage(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    });

    auto allocator = rx::rhi::Allocator::createRaw(headless->physicalDevice, headless->device, headless->ctx.instance());
    REQUIRE(allocator.has_value());
    auto readback = allocator->createHostVisibleBuffer(kWidth * kHeight * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kWidth, kHeight, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
    });

    const auto* pixels = static_cast<const uint8_t*>(readback->mappedData());
    CHECK(pixels[0] == 255);  // R
    CHECK(pixels[1] == 0);    // G
    CHECK(pixels[2] == 0);    // B
    CHECK(pixels[3] == 255);  // A
    CHECK_FALSE(headless->ctx.hasValidationErrors());

    vkDestroyImageView(headless->device, imageView, nullptr);
    vkDestroyImage(headless->device, image, nullptr);
    vkFreeMemory(headless->device, imageMemory, nullptr);
    vkDestroyDevice(headless->device, nullptr);
}
```

**Note:** this test needs `Allocator::createRaw` (device/instance handles directly, no `rx::rhi::Device`/swapchain dependency) since it deliberately avoids creating a window/surface. Add this as an overload alongside the existing `Allocator::create(Context&, Device&)`.

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build --preset linux-native --target rx_rhi_vk_tests
```
Expected: FAIL — `rx_rhi_vk/command.h: No such file or directory`, and `Allocator::createRaw` undeclared.

- [ ] **Step 3: Implement CommandContext, transitionImage, and Allocator::createRaw**

`src/rx_rhi_vk/include/rx_rhi_vk/command.h`:
```cpp
#pragma once
#include <volk.h>
#include <functional>
#include <optional>

namespace rx::rhi {

class CommandContext {
public:
    CommandContext(CommandContext&&) noexcept;
    CommandContext& operator=(CommandContext&&) noexcept;
    CommandContext(const CommandContext&) = delete;
    CommandContext& operator=(const CommandContext&) = delete;
    ~CommandContext();

    static std::optional<CommandContext> create(VkDevice device, VkQueue queue, uint32_t queueFamily);

    // Synchronous one-shot recording: allocate, begin, run `record`, end, submit, wait idle.
    // The optional wait/signal semaphores exist for exactly one caller: Task 13's
    // present loop, which must wait on the swapchain's acquire semaphore before the
    // GPU touches the acquired image, and signal a render-finished semaphore that
    // present() waits on. Setup/test code (Tasks 8-9, 12) always uses the defaults.
    void runOnce(const std::function<void(VkCommandBuffer)>& record,
                 VkSemaphore waitSemaphore = VK_NULL_HANDLE,
                 VkPipelineStageFlags waitStage = 0,
                 VkSemaphore signalSemaphore = VK_NULL_HANDLE);

private:
    CommandContext(VkDevice device, VkQueue queue, VkCommandPool pool)
        : device_(device), queue_(queue), pool_(pool) {}

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
};

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);

}  // namespace rx::rhi
```

`src/rx_rhi_vk/src/command.cpp`:
```cpp
#include <rx_rhi_vk/command.h>
#include <rx_core/log.h>

namespace rx::rhi {

std::optional<CommandContext> CommandContext::create(VkDevice device, VkQueue queue, uint32_t queueFamily) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = queueFamily;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        RX_LOG_ERROR("vkCreateCommandPool failed");
        return std::nullopt;
    }
    return CommandContext(device, queue, pool);
}

void CommandContext::runOnce(const std::function<void(VkCommandBuffer)>& record,
                              VkSemaphore waitSemaphore,
                              VkPipelineStageFlags waitStage,
                              VkSemaphore signalSemaphore) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    record(cmd);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    if (waitSemaphore != VK_NULL_HANDLE) {
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &waitSemaphore;
        submitInfo.pWaitDstStageMask = &waitStage;
    }
    if (signalSemaphore != VK_NULL_HANDLE) {
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSemaphore;
    }
    vkQueueSubmit(queue_, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);

    vkFreeCommandBuffers(device_, pool_, 1, &cmd);
}

CommandContext::CommandContext(CommandContext&& other) noexcept
    : device_(other.device_), queue_(other.queue_), pool_(other.pool_) {
    other.pool_ = VK_NULL_HANDLE;
}

CommandContext& CommandContext::operator=(CommandContext&& other) noexcept {
    if (this != &other) {
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, pool_, nullptr);
        }
        device_ = other.device_;
        queue_ = other.queue_;
        pool_ = other.pool_;
        other.pool_ = VK_NULL_HANDLE;
    }
    return *this;
}

CommandContext::~CommandContext() {
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, pool_, nullptr);
    }
}

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};

    VkDependencyInfo depInfo{};
    depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &depInfo);
}

}  // namespace rx::rhi
```

Add to `buffer.h`/`buffer.cpp` an `Allocator::createRaw(VkPhysicalDevice, VkDevice, VkInstance) -> std::optional<Allocator>` that factors the `VmaVulkanFunctions`/`VmaAllocatorCreateInfo` setup out of `Allocator::create` into a shared private helper, with `create(Context&, Device&)` calling `createRaw(device.physicalDevice(), device.device(), context.instance())`.

Add `src/command.cpp` to library sources and `tests/clear_color_test.cpp` to `rx_rhi_vk_tests` sources.

- [ ] **Step 4: Run tests and verify they pass**

```bash
cmake --build --preset linux-native --target rx_rhi_vk_tests
ctest --preset linux-native -R rx_rhi_vk_tests --output-on-failure
```
Expected: the readback buffer's first pixel is exactly `(255, 0, 0, 255)`, and no validation errors were reported.

- [ ] **Step 5: Commit**

```bash
git add src/rx_rhi_vk/
git commit -m "Add rx_rhi_vk command recording, sync2 barriers, dynamic rendering clear-color test"
```

---

### Task 10: Slang prebuilt binary fetch

**Files:**
- Create: `tools/fetch_slang.cmake`
- Create: `tools/fetch_slang_test.sh`
- Modify: `CMakeLists.txt` (invoke the fetch script as a build-time step)

**Interfaces:**
- Produces: `third_party/slang-prebuilt/<platform>/bin/slangc` (+ `lib/`, `include/`), populated before any target that needs it configures. CMake variable `RX_SLANGC` pointing at the fetched `slangc` executable.

- [ ] **Step 1: Write the fetch script**

`tools/fetch_slang.cmake`:
```cmake
# Fetches official prebuilt Slang release binaries. Slang is a full
# compiler; it is never built from source by this project.

set(RX_SLANG_VERSION "2026.14.1")
set(RX_SLANG_DIR "${CMAKE_SOURCE_DIR}/third_party/slang-prebuilt")

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(RX_SLANG_PLATFORM "windows-x86_64")
  set(RX_SLANG_ARCHIVE_EXT "zip")
else()
  set(RX_SLANG_PLATFORM "linux-x86_64-glibc-2.27")
  set(RX_SLANG_ARCHIVE_EXT "tar.gz")
endif()

set(RX_SLANG_ARCHIVE "slang-${RX_SLANG_VERSION}-${RX_SLANG_PLATFORM}.${RX_SLANG_ARCHIVE_EXT}")
set(RX_SLANG_URL "https://github.com/shader-slang/slang/releases/download/v${RX_SLANG_VERSION}/${RX_SLANG_ARCHIVE}")
set(RX_SLANG_INSTALL_DIR "${RX_SLANG_DIR}/${RX_SLANG_PLATFORM}")
set(RX_SLANG_MARKER "${RX_SLANG_INSTALL_DIR}/.rx-fetched")

if(NOT EXISTS "${RX_SLANG_MARKER}")
  message(STATUS "[slang-fetch] downloading ${RX_SLANG_ARCHIVE}")
  file(MAKE_DIRECTORY "${RX_SLANG_INSTALL_DIR}")
  set(RX_SLANG_ARCHIVE_PATH "${CMAKE_BINARY_DIR}/${RX_SLANG_ARCHIVE}")
  file(DOWNLOAD "${RX_SLANG_URL}" "${RX_SLANG_ARCHIVE_PATH}" STATUS RX_SLANG_DOWNLOAD_STATUS)
  list(GET RX_SLANG_DOWNLOAD_STATUS 0 RX_SLANG_DOWNLOAD_CODE)
  if(NOT RX_SLANG_DOWNLOAD_CODE EQUAL 0)
    message(FATAL_ERROR "[slang-fetch] download failed: ${RX_SLANG_DOWNLOAD_STATUS}")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${RX_SLANG_ARCHIVE_PATH}" DESTINATION "${RX_SLANG_INSTALL_DIR}")
  file(WRITE "${RX_SLANG_MARKER}" "${RX_SLANG_VERSION}\n")
else()
  message(STATUS "[slang-fetch] ${RX_SLANG_VERSION} already present for ${RX_SLANG_PLATFORM}, skipping download")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(RX_SLANGC "${RX_SLANG_INSTALL_DIR}/bin/slangc.exe" CACHE FILEPATH "" FORCE)
else()
  set(RX_SLANGC "${RX_SLANG_INSTALL_DIR}/bin/slangc" CACHE FILEPATH "" FORCE)
endif()
```

- [ ] **Step 2: Wire into root CMakeLists.txt**

Add to `CMakeLists.txt` (before any subdirectory that will need `RX_SLANGC`, i.e. before `shaders` in Task 11):
```cmake
include(tools/fetch_slang.cmake)
```

- [ ] **Step 3: Write a verification script and run it**

`tools/fetch_slang_test.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
cmake --preset linux-native > /dev/null
SLANGC=$(cmake -N -L --preset linux-native 2>/dev/null | grep RX_SLANGC | cut -d= -f2)
if [ ! -x "$SLANGC" ]; then
  echo "FAIL: slangc not found or not executable at $SLANGC"
  exit 1
fi
VERSION_OUTPUT=$("$SLANGC" -v)
echo "$VERSION_OUTPUT" | grep -q "2026.14.1" || { echo "FAIL: unexpected slangc version: $VERSION_OUTPUT"; exit 1; }
echo "PASS: slangc ${VERSION_OUTPUT}"
```

```bash
chmod +x tools/fetch_slang_test.sh
./tools/fetch_slang_test.sh
```
Expected: `PASS: slangc ...2026.14.1...`

- [ ] **Step 4: Commit**

```bash
git add tools/fetch_slang.cmake tools/fetch_slang_test.sh CMakeLists.txt
git commit -m "Add Slang prebuilt binary fetch (no source compilation)"
```

---

### Task 11: Triangle shaders compiled via fetched slangc

**Files:**
- Create: `shaders/triangle.vert.slang`, `shaders/triangle.frag.slang`
- Create: `shaders/CMakeLists.txt`
- Create: `shaders/tests/spirv_validity_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `${CMAKE_BINARY_DIR}/shaders/triangle.vert.spv`, `${CMAKE_BINARY_DIR}/shaders/triangle.frag.spv`, CMake target `triangle_shaders` (a build-time custom-command target later targets can depend on).

- [ ] **Step 1: Write the shader sources**

`shaders/triangle.vert.slang`:
```
struct VSOutput {
    float4 position : SV_Position;
    float3 color : COLOR0;
};

static const float2 positions[3] = {
    float2(0.0, -0.5),
    float2(0.5, 0.5),
    float2(-0.5, 0.5),
};

static const float3 colors[3] = {
    float3(1.0, 1.0, 1.0),
    float3(1.0, 1.0, 1.0),
    float3(1.0, 1.0, 1.0),
};

[shader("vertex")]
VSOutput main(uint vertexIndex : SV_VertexID) {
    VSOutput out;
    out.position = float4(positions[vertexIndex], 0.0, 1.0);
    out.color = colors[vertexIndex];
    return out;
}
```

`shaders/triangle.frag.slang`:
```
[shader("fragment")]
float4 main(float3 color : COLOR0) : SV_Target {
    return float4(color, 1.0);
}
```

(The triangle is solid white so Task 12's readback check has an unambiguous color to assert on regardless of interpolation.)

- [ ] **Step 2: Write the failing test**

`shaders/tests/spirv_validity_test.cpp`:
```cpp
#include <doctest/doctest.h>
#include <fstream>
#include <vector>
#include <cstdint>

namespace {

bool hasValidSpirvMagicNumber(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    uint32_t magic = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    return magic == 0x07230203;
}

}  // namespace

TEST_CASE("compiled triangle shaders are valid SPIR-V modules") {
    CHECK(hasValidSpirvMagicNumber(RX_TRIANGLE_VERT_SPV));
    CHECK(hasValidSpirvMagicNumber(RX_TRIANGLE_FRAG_SPV));
}
```

- [ ] **Step 3: Run to verify it fails (nothing compiles the shaders yet)**

```bash
cmake --preset linux-native && cmake --build --preset linux-native --target shader_spirv_test
```
Expected: FAIL — target `shader_spirv_test` does not exist yet.

- [ ] **Step 4: Implement the shader build**

`shaders/CMakeLists.txt`:
```cmake
set(RX_TRIANGLE_VERT_SPV "${CMAKE_CURRENT_BINARY_DIR}/triangle.vert.spv")
set(RX_TRIANGLE_FRAG_SPV "${CMAKE_CURRENT_BINARY_DIR}/triangle.frag.spv")

add_custom_command(
  OUTPUT "${RX_TRIANGLE_VERT_SPV}"
  COMMAND "${RX_SLANGC}" "${CMAKE_CURRENT_SOURCE_DIR}/triangle.vert.slang" -target spirv -profile sm_6_0 -entry main -o "${RX_TRIANGLE_VERT_SPV}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/triangle.vert.slang"
  COMMENT "Compiling triangle.vert.slang"
)
add_custom_command(
  OUTPUT "${RX_TRIANGLE_FRAG_SPV}"
  COMMAND "${RX_SLANGC}" "${CMAKE_CURRENT_SOURCE_DIR}/triangle.frag.slang" -target spirv -profile sm_6_0 -entry main -o "${RX_TRIANGLE_FRAG_SPV}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/triangle.frag.slang"
  COMMENT "Compiling triangle.frag.slang"
)
add_custom_target(triangle_shaders DEPENDS "${RX_TRIANGLE_VERT_SPV}" "${RX_TRIANGLE_FRAG_SPV}")

set(RX_TRIANGLE_VERT_SPV "${RX_TRIANGLE_VERT_SPV}" CACHE INTERNAL "")
set(RX_TRIANGLE_FRAG_SPV "${RX_TRIANGLE_FRAG_SPV}" CACHE INTERNAL "")

add_executable(shader_spirv_test tests/spirv_validity_test.cpp)
add_dependencies(shader_spirv_test triangle_shaders)
target_link_libraries(shader_spirv_test PRIVATE doctest::doctest)
target_compile_definitions(shader_spirv_test PRIVATE
  RX_TRIANGLE_VERT_SPV="${RX_TRIANGLE_VERT_SPV}"
  RX_TRIANGLE_FRAG_SPV="${RX_TRIANGLE_FRAG_SPV}"
)
add_test(NAME shader_spirv_test COMMAND shader_spirv_test)
```

Add to `CMakeLists.txt`:
```cmake
add_subdirectory(shaders)
```

- [ ] **Step 5: Run and verify it passes**

```bash
cmake --build --preset linux-native --target shader_spirv_test
ctest --preset linux-native -R shader_spirv_test --output-on-failure
```
Expected: both `.spv` files exist with a valid SPIR-V magic number.

**If `slangc` rejects the `[shader(...)]` attribute syntax or `-profile sm_6_0`:** check `"${RX_SLANGC}" -h` for this pinned version's exact flag names for target/profile/entry-point selection and adjust the `add_custom_command` arguments accordingly — the flag names are stable across recent Slang releases but this is exactly the kind of detail this step's build-and-run gate exists to catch.

- [ ] **Step 6: Commit**

```bash
git add shaders/ CMakeLists.txt
git commit -m "Add triangle shaders compiled via fetched slangc to SPIR-V"
```

---

### Task 12: Triangle smoke-test app with automated headless pixel readback

**Files:**
- Create: `apps/triangle_smoketest/CMakeLists.txt`, `apps/triangle_smoketest/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `rx::rhi::Context/Device/Allocator/Buffer/CommandContext` (Tasks 6-9), `RX_TRIANGLE_VERT_SPV`/`RX_TRIANGLE_FRAG_SPV` (Task 11).
- Produces: executable `triangle_smoketest`. Renders into the swapchain's first image (never acquired/presented), reads back pixels, asserts triangle vs. background colors, and exits `0`/`1`. Task 13 adds a `--present` interactive mode on top of this same binary.

- [ ] **Step 1: Implement the app**

`apps/triangle_smoketest/main.cpp`:
```cpp
#include <rx_core/log.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_platform/window.h>
#include <fstream>
#include <vector>
#include <cstring>
#include <cstdio>

namespace {

std::vector<char> readFile(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    std::vector<char> buffer(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    return buffer;
}

VkShaderModule loadShaderModule(VkDevice device, const char* path) {
    auto code = readFile(path);
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &info, nullptr, &module);
    return module;
}

VkPipeline createTrianglePipeline(VkDevice device, VkPipelineLayout layout, VkFormat colorFormat) {
    VkShaderModule vert = loadShaderModule(device, RX_TRIANGLE_VERT_SPV);
    VkShaderModule frag = loadShaderModule(device, RX_TRIANGLE_FRAG_SPV);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    return pipeline;
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    // This task only implements the headless automated correctness gate:
    // render straight into swapchain image 0 (never acquired/presented) and
    // read back pixels. Task 13 adds a `--present` flag and a real
    // acquire/render/present loop with recreation for interactive,
    // on-hardware verification (Task 15's manual checklist).
    rx::core::log::init();

    auto window = rx::platform::Window::create("triangle_smoketest", 256, 256, /*visible=*/false);
    if (!window.has_value()) {
        RX_LOG_ERROR("failed to create window");
        return 1;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        RX_LOG_ERROR("no Vulkan-capable display backend available");
        return 1;
    }

    auto ctx = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    if (!ctx.has_value()) { RX_LOG_ERROR("Context::create failed"); return 1; }

    VkSurfaceKHR surface = window->createVulkanSurface(ctx->instance());
    if (surface == VK_NULL_HANDLE) { RX_LOG_ERROR("createVulkanSurface failed"); return 1; }

    auto device = rx::rhi::Device::create(*ctx, surface);
    if (!device.has_value()) { RX_LOG_ERROR("Device::create failed"); return 1; }

    auto allocator = rx::rhi::Allocator::create(*ctx, *device);
    if (!allocator.has_value()) { RX_LOG_ERROR("Allocator::create failed"); return 1; }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device->device(), &layoutInfo, nullptr, &pipelineLayout);
    VkPipeline pipeline = createTrianglePipeline(device->device(), pipelineLayout, device->swapchainFormat());

    auto cmdCtx = rx::rhi::CommandContext::create(device->device(), device->graphicsQueue(), device->graphicsQueueFamily());
    if (!cmdCtx.has_value()) { RX_LOG_ERROR("CommandContext::create failed"); return 1; }

    VkImage target = device->swapchainImages()[0];
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = target;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = device->swapchainFormat();
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView targetView = VK_NULL_HANDLE;
    vkCreateImageView(device->device(), &viewInfo, nullptr, &targetView);

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        rx::rhi::transitionImage(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = targetView;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, {256, 256}};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(cmd, &renderingInfo);
        VkViewport viewport{0, 0, 256, 256, 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {256, 256}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);

        rx::rhi::transitionImage(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    });

    auto readback = allocator->createHostVisibleBuffer(256 * 256 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {256, 256, 1};
        vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
    });

    const auto* pixels = static_cast<const uint8_t*>(readback->mappedData());
    auto pixelAt = [&](int x, int y) { return &pixels[(y * 256 + x) * 4]; };

    // Triangle center (per shaders/triangle.vert.slang's NDC coordinates) -> expect white.
    const uint8_t* center = pixelAt(128, 150);
    // A corner well outside the triangle -> expect the black clear color.
    const uint8_t* corner = pixelAt(10, 10);

    bool centerIsWhite = center[0] > 200 && center[1] > 200 && center[2] > 200;
    bool cornerIsBlack = corner[0] < 20 && corner[1] < 20 && corner[2] < 20;

    if (!centerIsWhite || !cornerIsBlack) {
        RX_LOG_ERROR("triangle readback FAILED: center=({},{},{}) corner=({},{},{})",
                      center[0], center[1], center[2], corner[0], corner[1], corner[2]);
        return 1;
    }
    if (ctx->hasValidationErrors()) {
        RX_LOG_ERROR("triangle readback FAILED: Vulkan validation reported errors");
        return 1;
    }
    RX_LOG_INFO("triangle readback PASSED");

    vkDestroyImageView(device->device(), targetView, nullptr);
    vkDestroyPipeline(device->device(), pipeline, nullptr);
    vkDestroyPipelineLayout(device->device(), pipelineLayout, nullptr);
    return 0;
}
```

`apps/triangle_smoketest/CMakeLists.txt`:
```cmake
add_executable(triangle_smoketest main.cpp)
add_dependencies(triangle_smoketest triangle_shaders)
target_link_libraries(triangle_smoketest PRIVATE rx_core rx_platform rx_rhi_vk)
target_compile_definitions(triangle_smoketest PRIVATE
  RX_TRIANGLE_VERT_SPV="${RX_TRIANGLE_VERT_SPV}"
  RX_TRIANGLE_FRAG_SPV="${RX_TRIANGLE_FRAG_SPV}"
)
add_test(NAME triangle_smoketest COMMAND triangle_smoketest)
```

Add to `CMakeLists.txt`:
```cmake
add_subdirectory(apps/triangle_smoketest)
```

- [ ] **Step 2: Build and run, verify PASS**

```bash
cmake --build --preset linux-native --target triangle_smoketest
./build/linux-native/apps/triangle_smoketest/triangle_smoketest
echo "exit code: $?"
```
Expected: log line `triangle readback PASSED`, exit code `0`.

- [ ] **Step 3: Register with ctest and verify**

```bash
ctest --preset linux-native -R triangle_smoketest --output-on-failure
```
Expected: `100% tests passed`.

- [ ] **Step 4: Commit**

```bash
git add apps/ CMakeLists.txt
git commit -m "Add triangle smoke-test app with automated headless pixel readback"
```

---

### Task 13: Real swapchain acquire/present loop with recreation

**Why this task exists:** every prior task rendered into `swapchainImages()[0]` directly without ever calling `vkAcquireNextImageKHR`/`vkQueuePresentKHR`. That's a legitimate simplification for the headless correctness gate (Task 12), but it means the swapchain itself — the thing layer 3 is actually supposed to deliver — has never been exercised as a swapchain. This task fixes that: real per-frame acquire, GPU-side synchronization via semaphores, present, and recreation on `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR`, per the design spec's error-handling section.

**Files:**
- Modify: `src/rx_rhi_vk/include/rx_rhi_vk/device.h`, `src/rx_rhi_vk/src/device.cpp` (add `SwapchainStatus`, `acquireNextImage`, `present`, `recreateSwapchain`)
- Modify: `src/rx_rhi_vk/tests/device_test.cpp` (add an acquire/present round-trip test)
- Modify: `apps/triangle_smoketest/main.cpp` (add `--present` flag and the real loop)
- Modify: `apps/triangle_smoketest/CMakeLists.txt` (no new deps, just noting the binary now supports the flag)

**Interfaces:**
- Consumes: `rx::rhi::CommandContext::runOnce` with semaphore parameters and `rx::rhi::transitionImage` (Task 9).
- Produces: `enum class SwapchainStatus { Ok, NeedsRecreate, DeviceLost }`; `Device::acquireNextImage(VkSemaphore signal) -> AcquireResult{SwapchainStatus status, uint32_t imageIndex}`; `Device::present(uint32_t imageIndex, VkSemaphore wait) -> SwapchainStatus`; `Device::recreateSwapchain(VkSurfaceKHR surface) -> bool`.

- [ ] **Step 1: Write the failing test**

Add to `src/rx_rhi_vk/tests/device_test.cpp`:
```cpp
#include <rx_rhi_vk/command.h>

TEST_CASE("Device::acquireNextImage and Device::present round-trip on a fresh swapchain") {
    auto window = rx::platform::Window::create("rx_rhi_vk_present_test", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("no real display backend available, skipping present round-trip test");
        return;
    }

    auto ctx = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(ctx.has_value());
    VkSurfaceKHR surface = window->createVulkanSurface(ctx->instance());
    REQUIRE(surface != VK_NULL_HANDLE);
    auto device = rx::rhi::Device::create(*ctx, surface);
    REQUIRE(device.has_value());

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore acquireSem = VK_NULL_HANDLE;
    REQUIRE(vkCreateSemaphore(device->device(), &semInfo, nullptr, &acquireSem) == VK_SUCCESS);

    auto acquireResult = device->acquireNextImage(acquireSem);
    REQUIRE(acquireResult.status == rx::rhi::SwapchainStatus::Ok);

    auto cmdCtx = rx::rhi::CommandContext::create(device->device(), device->graphicsQueue(), device->graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());
    VkImage image = device->swapchainImages()[acquireResult.imageIndex];
    cmdCtx->runOnce(
        [&](VkCommandBuffer cmd) {
            rx::rhi::transitionImage(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        },
        acquireSem, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // A hidden window's presentation behavior is implementation-defined on
    // some drivers/compositors, so both outcomes are a legitimate pass here
    // - what matters is that neither returns DeviceLost.
    auto presentStatus = device->present(acquireResult.imageIndex, VK_NULL_HANDLE);
    CHECK((presentStatus == rx::rhi::SwapchainStatus::Ok || presentStatus == rx::rhi::SwapchainStatus::NeedsRecreate));

    vkDestroySemaphore(device->device(), acquireSem, nullptr);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build --preset linux-native --target rx_rhi_vk_tests
```
Expected: FAIL — `acquireNextImage`/`present` undeclared on `rx::rhi::Device`.

- [ ] **Step 3: Implement acquire/present/recreate on Device**

Add to `src/rx_rhi_vk/include/rx_rhi_vk/device.h` (inside `namespace rx::rhi`, before the `Device` class):
```cpp
enum class SwapchainStatus { Ok, NeedsRecreate, DeviceLost };

struct AcquireResult {
    SwapchainStatus status;
    uint32_t imageIndex = 0;
};
```
Add to the `Device` class's public section:
```cpp
    AcquireResult acquireNextImage(VkSemaphore signalSemaphore);
    SwapchainStatus present(uint32_t imageIndex, VkSemaphore waitSemaphore);
    bool recreateSwapchain(VkSurfaceKHR surface);
```

Add to `src/rx_rhi_vk/src/device.cpp`:
```cpp
AcquireResult Device::acquireNextImage(VkSemaphore signalSemaphore) {
    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, signalSemaphore, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        return {SwapchainStatus::Ok, imageIndex};
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return {SwapchainStatus::NeedsRecreate, 0};
    }
    if (result == VK_ERROR_DEVICE_LOST) {
        RX_LOG_ERROR("vkAcquireNextImageKHR: device lost");
        return {SwapchainStatus::DeviceLost, 0};
    }
    RX_LOG_ERROR("vkAcquireNextImageKHR failed with VkResult {}", static_cast<int>(result));
    return {SwapchainStatus::NeedsRecreate, 0};
}

SwapchainStatus Device::present(uint32_t imageIndex, VkSemaphore waitSemaphore) {
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    if (waitSemaphore != VK_NULL_HANDLE) {
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &waitSemaphore;
    }
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;

    VkResult result = vkQueuePresentKHR(presentQueue_, &presentInfo);
    if (result == VK_SUCCESS) return SwapchainStatus::Ok;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) return SwapchainStatus::NeedsRecreate;
    if (result == VK_ERROR_DEVICE_LOST) {
        RX_LOG_ERROR("vkQueuePresentKHR: device lost");
        return SwapchainStatus::DeviceLost;
    }
    RX_LOG_ERROR("vkQueuePresentKHR failed with VkResult {}", static_cast<int>(result));
    return SwapchainStatus::NeedsRecreate;
}

bool Device::recreateSwapchain(VkSurfaceKHR surface) {
    vkDeviceWaitIdle(device_);
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    vkb::SwapchainBuilder builder(physicalDevice_, device_, surface);
    auto result = builder.build();
    if (!result) {
        RX_LOG_ERROR("recreateSwapchain failed: {}", result.error().message());
        return false;
    }
    vkb::Swapchain vkbSwapchain = result.value();
    auto images = vkbSwapchain.get_images();
    if (!images) {
        RX_LOG_ERROR("recreateSwapchain: failed to retrieve images");
        return false;
    }
    swapchain_ = vkbSwapchain.swapchain;
    swapchainImages_ = images.value();
    swapchainFormat_ = vkbSwapchain.image_format;
    return true;
}
```

**If `vkb::SwapchainBuilder(physicalDevice_, device_, surface)` doesn't match this pinned vk-bootstrap commit's constructor overload:** check `VkBootstrap.h` for the raw-handle `SwapchainBuilder` constructor (as opposed to the `vkb::Device&`-based one already used in `Device::create`) and adjust the argument list — this is exactly the kind of detail this step's build gate exists to catch.

- [ ] **Step 4: Run tests and verify they pass**

```bash
cmake --build --preset linux-native --target rx_rhi_vk_tests
ctest --preset linux-native -R rx_rhi_vk_tests --output-on-failure
```
Expected: the new present round-trip test passes (`Ok` or `NeedsRecreate`, never `DeviceLost`).

- [ ] **Step 5: Add the real `--present` loop to triangle_smoketest**

In `apps/triangle_smoketest/main.cpp`, add `#include <string>` back, and change `main`'s signature and window creation:
```cpp
int main(int argc, char** argv) {
    rx::core::log::init();
    bool present = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--present") present = true;
    }

    auto window = rx::platform::Window::create("triangle_smoketest", 256, 256, /*visible=*/present);
```
Leave the rest of the existing headless setup (window/extensions/ctx/surface/device/allocator/pipeline/cmdCtx creation) unchanged. Replace the final block — from the `VkImage target = device->swapchainImages()[0];` line through the end of `main` — with a branch: the existing headless body runs only `if (!present)`, and a new real loop runs `if (present)`:

```cpp
    if (!present) {
        VkImage target = device->swapchainImages()[0];
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = target;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = device->swapchainFormat();
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView targetView = VK_NULL_HANDLE;
        vkCreateImageView(device->device(), &viewInfo, nullptr, &targetView);

        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            rx::rhi::transitionImage(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = targetView;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = {{0, 0}, {256, 256}};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;

            vkCmdBeginRendering(cmd, &renderingInfo);
            VkViewport viewport{0, 0, 256, 256, 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {256, 256}};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);

            rx::rhi::transitionImage(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        });

        auto readback = allocator->createHostVisibleBuffer(256 * 256 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {256, 256, 1};
            vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
        });

        const auto* pixels = static_cast<const uint8_t*>(readback->mappedData());
        auto pixelAt = [&](int x, int y) { return &pixels[(y * 256 + x) * 4]; };
        const uint8_t* center = pixelAt(128, 150);
        const uint8_t* corner = pixelAt(10, 10);
        bool centerIsWhite = center[0] > 200 && center[1] > 200 && center[2] > 200;
        bool cornerIsBlack = corner[0] < 20 && corner[1] < 20 && corner[2] < 20;

        vkDestroyImageView(device->device(), targetView, nullptr);

        if (!centerIsWhite || !cornerIsBlack) {
            RX_LOG_ERROR("triangle readback FAILED: center=({},{},{}) corner=({},{},{})",
                          center[0], center[1], center[2], corner[0], corner[1], corner[2]);
            return 1;
        }
        if (ctx->hasValidationErrors()) {
            RX_LOG_ERROR("triangle readback FAILED: Vulkan validation reported errors");
            return 1;
        }
        RX_LOG_INFO("triangle readback PASSED");
        vkDestroyPipeline(device->device(), pipeline, nullptr);
        vkDestroyPipelineLayout(device->device(), pipelineLayout, nullptr);
        return 0;
    }

    RX_LOG_INFO("--present mode: rendering until the window is closed");
    bool running = true;
    while (running) {
        window->pumpEvents();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
        }
        if (!running) break;

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore acquireSem = VK_NULL_HANDLE;
        VkSemaphore renderFinishedSem = VK_NULL_HANDLE;
        vkCreateSemaphore(device->device(), &semInfo, nullptr, &acquireSem);
        vkCreateSemaphore(device->device(), &semInfo, nullptr, &renderFinishedSem);

        auto acquireResult = device->acquireNextImage(acquireSem);
        if (acquireResult.status == rx::rhi::SwapchainStatus::NeedsRecreate) {
            device->recreateSwapchain(surface);
            vkDestroySemaphore(device->device(), acquireSem, nullptr);
            vkDestroySemaphore(device->device(), renderFinishedSem, nullptr);
            continue;
        }
        if (acquireResult.status == rx::rhi::SwapchainStatus::DeviceLost) {
            RX_LOG_ERROR("device lost, exiting");
            break;
        }

        VkImage frameImage = device->swapchainImages()[acquireResult.imageIndex];
        VkImageViewCreateInfo frameViewInfo{};
        frameViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        frameViewInfo.image = frameImage;
        frameViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        frameViewInfo.format = device->swapchainFormat();
        frameViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView frameView = VK_NULL_HANDLE;
        vkCreateImageView(device->device(), &frameViewInfo, nullptr, &frameView);

        cmdCtx->runOnce(
            [&](VkCommandBuffer cmd) {
                rx::rhi::transitionImage(cmd, frameImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

                VkRenderingAttachmentInfo colorAttachment{};
                colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                colorAttachment.imageView = frameView;
                colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

                VkRenderingInfo renderingInfo{};
                renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                renderingInfo.renderArea = {{0, 0}, {256, 256}};
                renderingInfo.layerCount = 1;
                renderingInfo.colorAttachmentCount = 1;
                renderingInfo.pColorAttachments = &colorAttachment;

                vkCmdBeginRendering(cmd, &renderingInfo);
                VkViewport viewport{0, 0, 256, 256, 0.0f, 1.0f};
                VkRect2D scissor{{0, 0}, {256, 256}};
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdEndRendering(cmd);

                rx::rhi::transitionImage(cmd, frameImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            },
            acquireSem, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, renderFinishedSem);

        auto presentStatus = device->present(acquireResult.imageIndex, renderFinishedSem);
        if (presentStatus == rx::rhi::SwapchainStatus::NeedsRecreate) {
            device->recreateSwapchain(surface);
        } else if (presentStatus == rx::rhi::SwapchainStatus::DeviceLost) {
            RX_LOG_ERROR("device lost during present, exiting");
            running = false;
        }

        vkDestroyImageView(device->device(), frameView, nullptr);
        vkDestroySemaphore(device->device(), acquireSem, nullptr);
        vkDestroySemaphore(device->device(), renderFinishedSem, nullptr);
    }

    vkDestroyPipeline(device->device(), pipeline, nullptr);
    vkDestroyPipelineLayout(device->device(), pipelineLayout, nullptr);
    return 0;
}
```

- [ ] **Step 6: Build and manually verify the present loop on this dev machine**

```bash
cmake --build --preset linux-native --target triangle_smoketest
./build/linux-native/apps/triangle_smoketest/triangle_smoketest --present
```
Expected: a real window opens on this machine's `:1` display showing a white triangle on a black background; closing the window exits cleanly with no validation errors logged.

- [ ] **Step 7: Re-run the full test suite to confirm nothing regressed**

```bash
ctest --preset linux-native --output-on-failure
```
Expected: `100% tests passed`.

- [ ] **Step 8: Commit**

```bash
git add src/rx_rhi_vk/ apps/triangle_smoketest/
git commit -m "Add real swapchain acquire/present loop with recreation"
```

---

### Task 14: GitHub Actions CI matrix + build-time budget check

**Files:**
- Create: `.github/workflows/ci.yml`
- Create: `tools/check_build_budget.sh`

**Interfaces:**
- Produces: a CI workflow running on `ubuntu-latest` with two jobs (`linux-native`, `windows-cross-zig`), each installing zig, configuring, building, testing, and then enforcing the sub-1-minute warm-cache build budget.

- [ ] **Step 1: Write the build-time budget check**

`tools/check_build_budget.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
PRESET="$1"
BUDGET_SECONDS="${2:-60}"

# Warm build already happened before this script runs; touch nothing that
# would force a rebuild, then time a no-op-plus-real build.
START=$(date +%s)
cmake --build --preset "$PRESET"
END=$(date +%s)
ELAPSED=$((END - START))

echo "Build took ${ELAPSED}s (budget: ${BUDGET_SECONDS}s)"
if [ "$ELAPSED" -gt "$BUDGET_SECONDS" ]; then
  echo "FAIL: build exceeded the ${BUDGET_SECONDS}s budget"
  exit 1
fi
echo "PASS"
```
```bash
chmod +x tools/check_build_budget.sh
```

- [ ] **Step 2: Write the workflow**

`.github/workflows/ci.yml`:
```yaml
name: CI

on:
  push:
    branches: [main]
  pull_request:

jobs:
  linux-native:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install system packages
        run: sudo apt-get update && sudo apt-get install -y ninja-build mesa-vulkan-drivers vulkan-tools libvulkan-dev xvfb
      - name: Install zig
        run: |
          curl -L -o /tmp/zig.tar.xz https://ziglang.org/download/0.16.0/zig-x86_64-linux-0.16.0.tar.xz
          mkdir -p toolchain
          tar -xf /tmp/zig.tar.xz -C toolchain
          mv toolchain/zig-x86_64-linux-0.16.0 toolchain/zig
      - name: Cache third-party dependency builds
        uses: actions/cache@v4
        with:
          path: .deps-cache
          key: deps-linux-native-${{ hashFiles('third_party/CMakeLists.txt') }}
      - name: Configure
        run: cmake --preset linux-native
      - name: Build (cold or warm)
        run: cmake --build --preset linux-native
      - name: Test (headless, under Xvfb for the windowed cases)
        run: xvfb-run -a ctest --preset linux-native --output-on-failure
      - name: Enforce sub-1-minute warm build budget
        run: ./tools/check_build_budget.sh linux-native 60

  windows-cross-zig:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install system packages
        run: sudo apt-get update && sudo apt-get install -y ninja-build wine
      - name: Install zig
        run: |
          curl -L -o /tmp/zig.tar.xz https://ziglang.org/download/0.16.0/zig-x86_64-linux-0.16.0.tar.xz
          mkdir -p toolchain
          tar -xf /tmp/zig.tar.xz -C toolchain
          mv toolchain/zig-x86_64-linux-0.16.0 toolchain/zig
      - name: Cache third-party dependency builds
        uses: actions/cache@v4
        with:
          path: .deps-cache
          key: deps-windows-cross-zig-${{ hashFiles('third_party/CMakeLists.txt') }}
      - name: Configure
        run: cmake --preset windows-cross-zig
      - name: Build
        run: cmake --build --preset windows-cross-zig
      - name: Run toolchain_check via wine
        run: wine build/windows-cross-zig/tools/toolchain_check/toolchain_check.exe
      - name: Enforce sub-1-minute warm build budget
        run: ./tools/check_build_budget.sh windows-cross-zig 60
```

**Honest limitation:** this workflow can only be exercised for real once pushed — a local syntax check is the furthest this task can verify without a GitHub Actions run.

- [ ] **Step 3: Validate the workflow syntax locally**

```bash
python3 -c "import yaml, sys; yaml.safe_load(open('.github/workflows/ci.yml'))" && echo "YAML is well-formed"
```
Expected: `YAML is well-formed`. If `pyyaml` isn't available, install with `pip install --user pyyaml` first, or use `gh workflow view` after pushing as the fallback check.

- [ ] **Step 4: Commit and push, then confirm the run on GitHub**

```bash
git add .github/workflows/ci.yml tools/check_build_budget.sh
git commit -m "Add GitHub Actions CI matrix and build-time budget check"
git push
gh run watch
```
Expected: both jobs (`linux-native`, `windows-cross-zig`) complete successfully.

---

### Task 15: Manual on-hardware verification

**Files:**
- Create: `MANUAL_VERIFICATION.md`

**Interfaces:**
- None — this is a documentation deliverable. The spec's acceptance criteria explicitly require the triangle to render on real Windows, Linux, and Steam Deck hardware, which cannot be automated from this dev machine.

- [ ] **Step 1: Write the manual verification checklist**

`MANUAL_VERIFICATION.md`:
```markdown
# Manual hardware verification: sub-project 1 (toolchain + platform + RHI)

Run on each target after `triangle_smoketest` passes in CI. Check off each
box and record the machine/driver details next to it.

## Linux (native)
- [ ] `cmake --preset linux-native && cmake --build --preset linux-native`
- [ ] `./build/linux-native/apps/triangle_smoketest/triangle_smoketest --present`
- [ ] A window opens showing a white triangle on a black background.
- [ ] Record: distro, GPU, driver version.

## Windows (cross-compiled from this Linux host)
- [ ] Copy `build/windows-cross-zig/apps/triangle_smoketest/triangle_smoketest.exe`
      (and no other files — it's statically linked) to a real Windows machine.
- [ ] Run `triangle_smoketest.exe --present`.
- [ ] A window opens showing a white triangle on a black background.
- [ ] Record: Windows version, GPU, driver version.

## Steam Deck (SteamOS, linux-native build)
- [ ] Copy `build/linux-native/apps/triangle_smoketest/triangle_smoketest` to
      the Deck (Desktop Mode), or scp it over.
- [ ] Run it from a terminal (Desktop Mode) as `./triangle_smoketest --present`.
- [ ] A window opens showing a white triangle on a black background.
- [ ] Record: SteamOS version, Mesa/RADV version (`vulkaninfo --summary` if available).

## If any of the above fails
File the failure against the relevant task in this plan (or the design
spec at `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md`
if it points at a design gap rather than an implementation bug) before
considering sub-project 1 done.
```

- [ ] **Step 2: Commit**

```bash
git add MANUAL_VERIFICATION.md
git commit -m "Add manual on-hardware verification checklist"
```
