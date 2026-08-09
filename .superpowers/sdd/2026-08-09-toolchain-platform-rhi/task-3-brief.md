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

