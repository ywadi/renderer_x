cmake_minimum_required(VERSION 3.21)
project(renderer_x LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_subdirectory(third_party)

include(tools/fetch_slang.cmake)
add_subdirectory(shaders)

add_subdirectory(src/rx_core)
add_subdirectory(src/rx_platform)
# rx_shader before rx_rhi_vk: rx_rhi_vk's PipelineLayoutBuilder
# (pipeline_layout.h) links rx_shader's `rx_shader_layout_types` INTERFACE
# target (defined in src/rx_shader/CMakeLists.txt) to get
# ShaderLayoutInfo's header without slang::slang -- defining that target
# before the directory that consumes it removes any doubt about
# add_subdirectory ordering, even though target_link_libraries against a
# not-yet-defined target name is otherwise fine in modern CMake.
add_subdirectory(src/rx_shader)
add_subdirectory(src/rx_rhi_vk)
add_subdirectory(tools/dep_cache_smoketest)
add_subdirectory(tools/toolchain_check)
add_subdirectory(samples/01_triangle)
add_subdirectory(samples/02_hotreload)
add_subdirectory(samples/03_bindless_mesh)
add_subdirectory(samples/04_streaming)
