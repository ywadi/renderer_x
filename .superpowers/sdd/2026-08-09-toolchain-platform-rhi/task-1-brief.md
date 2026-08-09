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

