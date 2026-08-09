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
