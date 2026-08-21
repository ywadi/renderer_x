# rx_shader_deploy_runtime_libs() is defined in src/rx_shader/CMakeLists.txt,
# add_subdirectory'd from the root CMakeLists.txt before this directory --
# CMake function *names* are visible to sibling directories processed later
# by the same parent (empirically verified: a function defined in one
# add_subdirectory'd listfile is callable from a second, later
# add_subdirectory'd listfile of the same parent).
#
# Used to need a local re-glob workaround here: an unqualified variable
# reference inside a CMake function body resolves through the *calling*
# scope, not the function's *definition* scope, so the function's old
# reference to a plain, non-CACHE `RX_SLANG_RUNTIME_LIBS` (file(GLOB)'d in
# rx_shader/CMakeLists.txt's own directory scope) resolved to nothing here.
# Fixed properly in src/rx_shader/CMakeLists.txt (Task 6): the function now
# re-globs internally using the CACHE-PATH `RX_SLANG_TARGET_ROOT` (a
# `CACHE PATH` variable set in tools/fetch_slang.cmake, visible in any
# directory scope), so every caller -- this one included -- just calls it
# below with nothing to set up first.

add_executable(sample_02_hotreload main.cpp)
# rx_frame_loop [Phase 5 Task 5, ticket #41]: shared present-loop helper.
target_link_libraries(sample_02_hotreload PRIVATE rx_rhi_vk rx_platform rx_shader rx_frame_loop)

# This sample does real in-process Slang compilation on every run (both
# modes) -- it is the redistribution proof for the Task 1 runtime-lib
# deployment mechanism [R:D2, spec Fixed decision #11].
rx_shader_deploy_runtime_libs(sample_02_hotreload)

# Deploys the live-reloadable shader source next to the binary at build
# time, so --present mode's on-disk mtime-poll loop (main.cpp,
# resolveShaderPath()) has something to watch and edit without needing to
# run from this source directory -- and so a redistributed copy of just
# this build output directory (binary + hotreload.slang + the Slang
# runtime libs above) works identically, which is the whole point of the
# redistribution proof.
#
# [Fix round, item 3] OUTPUT/DEPENDS-tracked, NOT a bare
# `add_custom_command(TARGET ... POST_BUILD)` -- see samples/08_gltf_viewer/
# CMakeLists.txt's own material_shaders deploy comment for the full
# mechanism this avoids (a POST_BUILD recipe only re-runs when the TARGET
# itself relinks, leaving an edited .slang source's deployed copy stale).
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/hotreload_shader_deploy.stamp"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/hotreload.slang"
          "$<TARGET_FILE_DIR:sample_02_hotreload>/hotreload.slang"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/hotreload_shader_deploy.stamp"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hotreload.slang"
  COMMENT "sample_02_hotreload: deploying hotreload.slang next to the binary"
  VERBATIM
)
add_custom_target(sample_02_hotreload_shader_deploy
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/hotreload_shader_deploy.stamp")
add_dependencies(sample_02_hotreload sample_02_hotreload_shader_deploy)

# Runs the binary in headless mode with --validate: ctest is the developer/CI
# gate, so it explicitly opts into the Vulkan validation layers (requires the
# Vulkan SDK / validation layers installed -- see samples/README.md) on top
# of the default end-user behavior, which is headless with validation OFF.
# The binary also supports --present mode for interactive windowed
# rendering with live shader editing (see main.cpp / samples/README.md).
add_test(NAME sample_02_hotreload_headless COMMAND sample_02_hotreload --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(sample_02_hotreload_headless PROPERTIES TIMEOUT 300)
