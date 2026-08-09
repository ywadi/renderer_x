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
