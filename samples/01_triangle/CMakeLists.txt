# RX_TRIANGLE_VERT_SPV/RX_TRIANGLE_FRAG_SPV and the triangle_shaders target
# come from shaders/CMakeLists.txt (add_subdirectory'd from the root
# CMakeLists.txt before this directory) -- see that file for how slangc
# compiles the .slang sources to SPIR-V. Same consumption pattern as
# shader_spirv_test in that same file.
add_executable(sample_01_triangle main.cpp)
target_link_libraries(sample_01_triangle PRIVATE rx_rhi_vk rx_platform)
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
add_custom_command(TARGET sample_01_triangle POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${RX_TRIANGLE_VERT_SPV}" "${RX_TRIANGLE_FRAG_SPV}"
          "$<TARGET_FILE_DIR:sample_01_triangle>/"
  COMMENT "sample_01_triangle: deploying precompiled SPIR-V next to the binary"
  VERBATIM
)

# Runs the binary in headless mode with --validate: ctest is the developer/CI
# gate, so it explicitly opts into the Vulkan validation layers (requires the
# Vulkan SDK / validation layers installed -- see samples/README.md) on top
# of the default end-user behavior, which is headless with validation OFF.
# The binary also supports --present mode for interactive windowed rendering.
add_test(NAME sample_01_triangle_headless COMMAND sample_01_triangle --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(sample_01_triangle_headless PROPERTIES TIMEOUT 300)
