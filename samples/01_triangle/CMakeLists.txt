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

# Runs the binary with no arguments -- headless mode, the only mode this
# task builds: full stack up, offscreen render, pixel readback, exit 0/1.
add_test(NAME sample_01_triangle_headless COMMAND sample_01_triangle)
