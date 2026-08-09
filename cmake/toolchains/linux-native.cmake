set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(RX_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(CMAKE_C_COMPILER   "${RX_REPO_ROOT}/cmake/zig-wrappers/zig-cc-linux"  CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${RX_REPO_ROOT}/cmake/zig-wrappers/zig-cxx-linux" CACHE FILEPATH "C++ compiler" FORCE)

set(RX_TARGET_TRIPLE "x86_64-linux-gnu" CACHE STRING "Target triple, used as a dependency-cache key component")

# zig cc's `-v` diagnostic output isn't in the format CMake's compiler-ABI
# detection expects from gcc/clang, so CMAKE_<LANG>_IMPLICIT_LINK_DIRECTORIES
# and CMAKE_LIBRARY_ARCHITECTURE come back empty. That silently drops the
# Debian/Ubuntu multiarch directory (/usr/lib/x86_64-linux-gnu) from every
# find_library()/find_package() search -- e.g. FindVulkan.cmake's
# find_library(Vulkan_LIBRARY) and SDL3's own FindX11-based video-driver
# detection both live there. Add it back explicitly.
list(APPEND CMAKE_SYSTEM_LIBRARY_PATH "/usr/lib/${RX_TARGET_TRIPLE}")
