# Slang runtime linking: find_package(slang CONFIG) locates the prebuilt
# tree tools/fetch_slang.cmake fetched -- RX_SLANG_TARGET_ROOT/slang_ROOT
# are set there (included from the root CMakeLists.txt before this
# directory is added; see that file's header comment for why "target" and
# "host" are different trees under windows-cross-zig, and why exposing
# slang_ROOT alone, with no PATHS/NO_DEFAULT_PATH here, is enough for
# CMake's Config-mode search to find slangConfig.cmake under either the
# Linux layout (<root>/lib/cmake/slang/) or the Windows layout
# (<root>/cmake/)).
#
# Link slang::slang ONLY [R:E3, spec Fixed decision #1] -- never
# slang::gfx (deprecated upstream in favor of slang-rhi) and never ship/
# link slang-llvm (the 25-84 MB CPU/host JIT backend, irrelevant to a
# SPIR-V-only compiler).
find_package(slang CONFIG REQUIRED)

# slang::slang is an IMPORTED (non-GLOBAL by default) target created deep
# inside slangTargets.cmake, in this directory's scope. rx_shader links it
# PUBLIC (any executable calling into rx_shader's Compiler needs Slang's
# headers/library too), and those executables live in sibling directories
# (this library's own tests/, and samples/ from Task 5 onward) that never
# call find_package(slang ...) themselves -- so, exactly like every other
# imported dependency in this project (see third_party/CMakeLists.txt's
# spdlog/Vulkan-Headers/SDL3/vk-bootstrap promotions), it must be promoted
# to global scope or those sibling directories simply won't resolve the
# target name at all.
set_target_properties(slang::slang PROPERTIES IMPORTED_GLOBAL TRUE)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
  # zig cc's own linker-driver front-end does its own file-type sniffing by
  # extension and does not recognize a SONAME-versioned suffix like
  # "libslang-compiler.so.0.2026.14.1" as a valid link input
  # ("unrecognized file extension"), even though the underlying LLD linker
  # has no such restriction -- verified directly: this exact CMake target,
  # used unmodified, fails to link with that message under this project's
  # zig-cxx-linux wrapper (plain gcc/clang drivers accept the same file
  # fine, which is why this was not caught by the standalone API
  # validation done earlier in this task using g++ directly). Slang's own
  # shipped CMake package points slang::slang's IMPORTED_LOCATION at
  # exactly that versioned real file
  # (lib/cmake/slang/slangTargets-release.cmake). "libslang-compiler.so" --
  # a symlink Slang itself ships right next to it, with the plain ".so"
  # extension zig's sniffer accepts -- resolves to the exact same inode
  # with the exact same embedded DT_SONAME, so retargeting
  # IMPORTED_LOCATION at the symlink changes nothing about what ends up in
  # a consumer's DT_NEEDED entry (still
  # "libslang-compiler.so.0.2026.14.1", the exact file this module's own
  # runtime-lib copy step below places next to every consumer) -- it only
  # changes which path the compiler driver is asked to sniff at link time.
  # Windows needs no equivalent fix: slang-compiler.lib's plain ".lib"
  # extension was never the problem there.
  set(_rx_slang_unversioned_so "${RX_SLANG_TARGET_ROOT}/lib/libslang-compiler.so")
  if(NOT EXISTS "${_rx_slang_unversioned_so}")
    message(FATAL_ERROR
      "[rx_shader] Expected 'libslang-compiler.so' symlink not found under "
      "'${RX_SLANG_TARGET_ROOT}/lib' -- the Slang archive layout may have "
      "changed; inspect that directory and update this workaround.")
  endif()
  set_target_properties(slang::slang PROPERTIES IMPORTED_LOCATION_RELEASE "${_rx_slang_unversioned_so}")
endif()

# Slang-free boundary for Task 2's ShaderLayoutInfo: an INTERFACE (header-
# only) target exposing ONLY this directory's include/ path, with ONLY
# Vulkan::Headers as a dependency -- no slang::slang, no rx_core. rx_shader
# itself links this (see below) so its own headers/tests see
# shader_layout_info.h exactly as before; rx_rhi_vk's CMakeLists links THIS
# target instead of `rx_shader` so PipelineLayoutBuilder can take a
# `const rx::shader::ShaderLayoutInfo&` without pulling slang::slang (or any
# other rx_shader header) into rx_rhi_vk's own headers or link interface --
# see shader_layout_info.h's own comment for why that separation matters.
add_library(rx_shader_layout_types INTERFACE)
target_include_directories(rx_shader_layout_types INTERFACE include)
target_link_libraries(rx_shader_layout_types INTERFACE Vulkan::Headers)

add_library(rx_shader STATIC
    src/compiler.cpp
    src/reflection.cpp
)
target_include_directories(rx_shader PUBLIC include)
target_link_libraries(rx_shader PUBLIC rx_core slang::slang rx_shader_layout_types)

# --- Runtime lib placement for any binary linking rx_shader -------------
#
# Slang compiles in-process via a shared library (libslang-compiler.so /
# slang-compiler.dll), never statically linked in, so any executable that
# links rx_shader (transitively, slang::slang) needs that library --
# plus the plugins it dlopens on demand for certain compile paths
# (slang-glslang, slang-glsl-module) and its companion runtime (slang-rt)
# -- sitting next to it on disk at run time [R:D2]. NEVER ship slang-llvm
# (the CPU/host JIT backend, irrelevant to SPIR-V-only use) or gfx (the
# deprecated GFX layer) [spec Fixed decision #11].
#
# Globbed (inside rx_shader_deploy_runtime_libs() itself, below) rather
# than hardcoded to the pinned version string: the Linux filenames embed
# the Slang version (e.g. "libslang-compiler.so.0.2026.14.1"), and Slang's
# own release history has changed archive layout/naming before [R:A6] --
# glob patterns tolerate a version bump without needing an edit here,
# matching the same "verify the layout, don't assume paths" posture
# fetch_slang.cmake already takes with its sentinel-file check. The
# Windows names carry no version suffix (verified directly against the
# real archive), so those are exact.
#
# Deploys the Slang runtime libs above next to `target`, and points its
# RPATH (Linux/ELF only -- CMake ignores RPATH properties on Windows, so
# this is unconditional and harmless there) at its own directory rather
# than the original fetched location under third_party/.
#
# BUILD_WITH_INSTALL_RPATH + INSTALL_RPATH="$ORIGIN" (rather than just
# relying on CMake's default automatic build-tree RPATH, which would
# already embed the *absolute* path to third_party/slang-prebuilt/.../lib
# and quietly work without this) is deliberate: it makes the build-tree
# binary's RPATH identical to what a redistributed copy would carry --
# nothing but "$ORIGIN" -- so running it from the build tree actually
# proves the redistribution-relevant mechanism (copy-adjacent + relative
# RPATH), not merely that the original fetch directory still happens to be
# on disk.
#
# DIRECTORY-SCOPE FIX (Task 6, tracked since Task 5's review): this used to
# reference the plain, non-CACHE `RX_SLANG_RUNTIME_LIBS` variable
# file(GLOB)'d above, in THIS listfile's own directory scope. A CMake
# function body resolves an unqualified variable through the *calling*
# scope, not its *definition* scope -- verified directly (see Task 5's own
# finding) -- so calling this function from a sibling directory (any
# samples/*/CMakeLists.txt) resolved to nothing there, and
# samples/02_hotreload/CMakeLists.txt had to re-glob the identical pattern
# itself as a local workaround just to give this function's
# add_custom_command something to copy. Re-globbing inside the function
# body itself, using the already-CACHE-INTERNAL `RX_SLANG_TARGET_ROOT`
# (visible in any scope, unlike the old plain `RX_SLANG_RUNTIME_LIBS`),
# closes this for good: every caller, in any directory, now gets a fresh,
# correct glob with no re-globbing workaround required on their end.
function(rx_shader_deploy_runtime_libs target)
  if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    file(GLOB _rx_shader_runtime_libs
      "${RX_SLANG_TARGET_ROOT}/bin/slang-compiler.dll"
      "${RX_SLANG_TARGET_ROOT}/bin/slang-glslang.dll"
      "${RX_SLANG_TARGET_ROOT}/bin/slang-glsl-module.dll"
      "${RX_SLANG_TARGET_ROOT}/bin/slang-rt.dll"
    )
  else()
    file(GLOB _rx_shader_runtime_libs
      "${RX_SLANG_TARGET_ROOT}/lib/libslang-compiler.so*"
      "${RX_SLANG_TARGET_ROOT}/lib/libslang-glslang-*.so"
      "${RX_SLANG_TARGET_ROOT}/lib/libslang-glsl-module-*.so"
      "${RX_SLANG_TARGET_ROOT}/lib/libslang-rt.so*"
    )
  endif()
  if(NOT _rx_shader_runtime_libs)
    message(FATAL_ERROR
      "[rx_shader_deploy_runtime_libs] No Slang runtime libraries found under "
      "'${RX_SLANG_TARGET_ROOT}' for target '${target}' -- fetch_slang.cmake's "
      "archive layout assumption may be stale; inspect that tree before continuing.")
  endif()

  set_target_properties(${target} PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN"
  )
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_rx_shader_runtime_libs} "$<TARGET_FILE_DIR:${target}>"
    COMMENT "rx_shader: copying Slang runtime libraries next to $<TARGET_FILE_NAME:${target}>"
    VERBATIM
  )
endfunction()

# --- Windows link smoke test --------------------------------------------
# De-risks the one real unknown in this task before anything else got
# built on top of it: does zig/LLD's COFF linker accept an MSVC-built
# import library? [R:A6/D2] -- see tests/link_smoketest.cpp and the task
# report for the empirical result. Deploy the runtime libs to it too (not
# just built, but able to actually run) since that was verified directly
# as part of this task rather than left as an assumption.
add_executable(rx_shader_link_smoketest tests/link_smoketest.cpp)
target_link_libraries(rx_shader_link_smoketest PRIVATE slang::slang)
rx_shader_deploy_runtime_libs(rx_shader_link_smoketest)

# rx_shader's tests do real, in-process Slang compilation (no fake/mocked
# Slang session) and are deliberately run straight from the build tree by
# ctest (no install step) specifically so the RPATH+copy mechanism above
# gets proven for real: if it didn't work, these tests would fail to even
# start (missing shared library at load time), not just fail an assertion.
add_executable(rx_shader_tests
    tests/doctest_main.cpp
    tests/compiler_test.cpp
    tests/reflection_test.cpp
)
target_link_libraries(rx_shader_tests PRIVATE rx_shader doctest::doctest)
rx_shader_deploy_runtime_libs(rx_shader_tests)
add_test(NAME rx_shader_tests COMMAND rx_shader_tests)
