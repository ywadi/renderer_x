set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(RX_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(CMAKE_C_COMPILER   "${RX_REPO_ROOT}/cmake/zig-wrappers/zig-cc-linux"  CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${RX_REPO_ROOT}/cmake/zig-wrappers/zig-cxx-linux" CACHE FILEPATH "C++ compiler" FORCE)

set(RX_TARGET_TRIPLE "x86_64-linux-gnu" CACHE STRING "Target triple, used as a dependency-cache key component")
