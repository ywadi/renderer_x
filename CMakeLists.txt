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
# Device-free in Task 1 (pass declarations, culling, ordering, lifetimes):
# depends on Vulkan::Headers + rx_core only, not rx_rhi_vk -- see
# src/rx_graph/CMakeLists.txt's own comment for why that dependency is
# deferred to the tasks that actually need a VkDevice.
add_subdirectory(src/rx_graph)
# Materials as Slang modules, pass signatures, and the lazy content-hash-
# keyed pipeline cache [spec Phase 3 design, D6-D8] -- depends on rx_graph
# (PassSignature), rx_shader (ShaderLayoutInfo's slang-free layout-types
# target only), and rx_rhi_vk (PipelineLayoutBuilder/BindlessTable/Device),
# so it is add_subdirectory'd after all three.
add_subdirectory(src/rx_material)
add_subdirectory(tools/dep_cache_smoketest)
add_subdirectory(tools/toolchain_check)
add_subdirectory(samples/01_triangle)
add_subdirectory(samples/02_hotreload)
add_subdirectory(samples/03_bindless_mesh)
add_subdirectory(samples/04_streaming)
add_subdirectory(samples/05_multipass)
