# Builds a CMake-based dependency exactly once per (name, pin, target
# triple, zig version, CMAKE_ARGS) and reuses the cached install on every
# later configure. A cache hit costs zero compilation.
#
# Cache key format: SHA256(name|tag|triple|zig-version|CMAKE_ARGS joined)
# truncated to 16 hex chars, then prefixed with name: "name-<hash>".
# Changing CMAKE_ARGS invalidates the key and forces a rebuild.

function(rx_dep_cache_key OUT_VAR NAME TAG CMAKE_ARGS_LIST)
  execute_process(
    COMMAND "${CMAKE_SOURCE_DIR}/toolchain/zig/zig" version
    OUTPUT_VARIABLE _zig_version
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(JOIN "|" _cmake_args_str ${CMAKE_ARGS_LIST})
  string(SHA256 _hash "${NAME}|${TAG}|${RX_TARGET_TRIPLE}|${_zig_version}|${_cmake_args_str}")
  string(SUBSTRING "${_hash}" 0 16 _hash)
  set(${OUT_VAR} "${NAME}-${_hash}" PARENT_SCOPE)
endfunction()

function(rx_add_cached_dependency)
  set(oneValueArgs NAME REPO TAG)
  set(multiValueArgs CMAKE_ARGS)
  cmake_parse_arguments(DEP "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  rx_dep_cache_key(_key ${DEP_NAME} ${DEP_TAG} "${DEP_CMAKE_ARGS}")
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
