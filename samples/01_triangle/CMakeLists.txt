# RX_TRIANGLE_VERT_SPV/RX_TRIANGLE_FRAG_SPV and the triangle_shaders target
# come from shaders/CMakeLists.txt (add_subdirectory'd from the root
# CMakeLists.txt before this directory) -- see that file for how slangc
# compiles the .slang sources to SPIR-V. Same consumption pattern as
# shader_spirv_test in that same file.
add_executable(sample_01_triangle main.cpp)
# rx_frame_loop [Phase 5 Task 5, ticket #41]: the shared present-loop
# helper -- runPresent() consumes rx::frame_loop::PresentLoop instead of
# hand-rolling its own acquire/status/recreate/present orchestration.
target_link_libraries(sample_01_triangle PRIVATE rx_rhi_vk rx_platform rx_frame_loop)
target_compile_definitions(sample_01_triangle PRIVATE
  RX_TRIANGLE_VERT_SPV="${RX_TRIANGLE_VERT_SPV}"
  RX_TRIANGLE_FRAG_SPV="${RX_TRIANGLE_FRAG_SPV}"
)
add_dependencies(sample_01_triangle triangle_shaders)

# Deploys the two precompiled SPIR-V files next to the binary at build time
# (redistribution fix, Task 8): main.cpp's resolveSpvPath() looks for them
# exe-relative first (via SDL_GetBasePath()) before falling back to the
# compile-time absolute build-tree path above -- without this copy step, a
# redistributed copy of just this build-output directory would have nothing
# there to find, and the compile-time fallback path (which points inside
# THIS machine's build tree, under shaders/) would not exist on a different
# machine either. This sample ships no Slang runtime libraries [D2] -- its
# shaders are precompiled offline by slangc at build time, not compiled
# in-process -- so these two files are the entire redistribution surface a
# copy of this sample's directory needs beyond the binary itself.
#
# [Fix round, item 3] OUTPUT/DEPENDS-tracked, NOT a bare
# `add_custom_command(TARGET ... POST_BUILD)` -- that form only re-runs when
# the TARGET ITSELF relinks, so a `triangle_shaders` recompile (edit
# triangle.vert.slang/triangle.frag.slang -> shaders/CMakeLists.txt's own
# tracked slangc custom command regenerates RX_TRIANGLE_VERT_SPV/
# RX_TRIANGLE_FRAG_SPV) would leave THIS deployed copy stale until an
# unrelated relink -- the identical class of hazard samples/08_gltf_viewer/
# CMakeLists.txt's own material_shaders/tonemap deploy steps had (see that
# file's own comment for the full mechanism, including the in-session
# incident that made this fix mandatory project-wide).
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/triangle_shaders_deploy.stamp"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${RX_TRIANGLE_VERT_SPV}" "${RX_TRIANGLE_FRAG_SPV}"
          "$<TARGET_FILE_DIR:sample_01_triangle>/"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/triangle_shaders_deploy.stamp"
  DEPENDS "${RX_TRIANGLE_VERT_SPV}" "${RX_TRIANGLE_FRAG_SPV}"
  COMMENT "sample_01_triangle: deploying precompiled SPIR-V next to the binary"
  VERBATIM
)
add_custom_target(sample_01_triangle_shaders_deploy
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/triangle_shaders_deploy.stamp")
add_dependencies(sample_01_triangle sample_01_triangle_shaders_deploy)

# Runs the binary in headless mode with --validate: ctest is the developer/CI
# gate, so it explicitly opts into the Vulkan validation layers (requires the
# Vulkan SDK / validation layers installed -- see samples/README.md) on top
# of the default end-user behavior, which is headless with validation OFF.
# The binary also supports --present mode for interactive windowed rendering.
add_test(NAME sample_01_triangle_headless COMMAND sample_01_triangle --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(sample_01_triangle_headless PROPERTIES TIMEOUT 300)
