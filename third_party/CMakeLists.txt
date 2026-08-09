include("${CMAKE_SOURCE_DIR}/cmake/DepCache.cmake")

set(RX_SPDLOG_TAG "v1.17.0")
rx_add_cached_dependency(
  NAME spdlog
  REPO https://github.com/gabime/spdlog.git
  TAG ${RX_SPDLOG_TAG}
  CMAKE_ARGS -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF
)
find_package(spdlog REQUIRED PATHS "${spdlog_CACHE_DIR}" NO_DEFAULT_PATH)

# CMake 3.22 has no find_package(... GLOBAL) (that arrived in 3.24), and
# imported targets are directory-scoped by default: without this, the
# spdlog::spdlog target created here would be invisible to sibling
# add_subdirectory() calls (e.g. tools/dep_cache_smoketest) that need to
# link against it. Promote it to global scope explicitly instead.
set_target_properties(spdlog::spdlog PROPERTIES IMPORTED_GLOBAL TRUE)

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

# Vulkan-Headers: header-only (provides the Vulkan::Headers INTERFACE target
# via its own CMakeLists.txt, no loader library involved), fetched directly
# like GLM/doctest rather than through find_package(Vulkan). find_package's
# bundled FindVulkan.cmake (CMake 3.22 here) gates Vulkan_FOUND -- and
# therefore Vulkan::Headers existing at all -- behind ALSO locating the
# platform's Vulkan *loader* import library (vulkan-1.lib on Windows), which
# does not exist anywhere when cross-compiling to Windows on this Linux host.
# Nothing in this project links the loader at compile/link time: the RHI
# layer (Task 6+) loads Vulkan entry points dynamically at runtime via volk,
# so only the headers (types/constants/signatures) are ever needed. Pinned
# to vulkan-sdk-1.3.280.0, whose vulkan_core.h reports
# VK_HEADER_VERSION_COMPLETE == 1.3.280, an exact match for this project's
# Vulkan 1.3 baseline (this machine's driver reports Instance Version
# 1.3.280 via `vulkaninfo --summary`).
set(RX_VULKAN_HEADERS_TAG "vulkan-sdk-1.3.280.0")
FetchContent_Declare(Vulkan-Headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG ${RX_VULKAN_HEADERS_TAG}
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Vulkan-Headers)

set(RX_SDL3_TAG "release-3.4.14")
rx_add_cached_dependency(
  NAME SDL3
  REPO https://github.com/libsdl-org/SDL.git
  TAG ${RX_SDL3_TAG}
  CMAKE_ARGS -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF
)
find_package(SDL3 REQUIRED PATHS "${SDL3_CACHE_DIR}" NO_DEFAULT_PATH)

# Same CMake 3.22 limitation as spdlog above. Unlike spdlog::spdlog, SDL3's
# own config makes "SDL3::SDL3" an ALIAS (to SDL3::SDL3-static here, since
# we build static-only) rather than a real imported target, and
# set_target_properties() cannot be applied to an ALIAS target directly.
# Worse, an ALIAS is scoped to the directory it was created in regardless of
# whether the target it points to is global, so "SDL3::SDL3" itself stays
# invisible outside this directory even after the fix below. Promote the
# real underlying imported targets to global scope instead, and have
# consumers (src/rx_platform) link against "SDL3::SDL3-static" directly
# rather than the alias.
set_target_properties(SDL3::SDL3-static PROPERTIES IMPORTED_GLOBAL TRUE)
set_target_properties(SDL3::Headers PROPERTIES IMPORTED_GLOBAL TRUE)
