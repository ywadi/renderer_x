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

