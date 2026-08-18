cmake_minimum_required(VERSION 3.21)
project(renderer_x LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

# RX_TRACY: engine-wide switch for the Tracy profiler client integration
# [spec D3, Phase 4 Stage 0 Task 3] -- ON by default (and explicitly ON in
# both dev presets, CMakePresets.json) but trivially OFF-able
# (`-DRX_TRACY=OFF`, or any preset that overrides it). Read by
# third_party/CMakeLists.txt (added immediately below, so this option must
# be declared first) to decide whether Tracy is fetched/built/linked at
# all, and by src/rx_core/CMakeLists.txt to decide whether TRACY_ENABLE is
# defined and Tracy::TracyClient linked -- both PUBLIC on rx_core, which
# every other library and sample transitively depends on. When OFF, Tracy
# is never fetched, never compiled, and never linked into any target in
# this build -- not merely disabled at runtime -- so the resulting binaries
# contain zero Tracy symbols (verified in task-3-report.md via nm/strings
# on an RX_TRACY=OFF build).
# NOTE: the windows-cross-zig preset overrides this to OFF (see CMakePresets.json).
# Tracy is a dev-host profiling tool used on Linux-native where captures actually
# work; profiling through Wine has zero value, and Tracy's profiler background
# thread does not tear down cleanly at process exit under CI cold-prefix Wine
# (tests print SUCCESS but the process never exits -> ctest timeout; CI runs
# 31512073559/31521284064/31523638220). RX_TRACY=OFF is a first-class,
# symbol-verified configuration (Task 3), so the Windows sample binaries simply
# ship without the profiler client, and the Tracy-ON path stays fully exercised
# (build + run + on-demand passivity) by the linux-native CI job. A dedicated
# build-only step in the windows job still compiles the RX_TRACY=ON Windows path
# so a cross-compile break in TracyClient.cpp is still caught.
option(RX_TRACY "Enable the Tracy profiler client (CPU + Vulkan GPU zones); OFF removes Tracy entirely from the build" ON)

# RX_DEBUG_CHECKS: engine-wide switch for dev-time runtime assertions
# (RX_ASSERT_MAIN_THREAD, rx_core/debug_checks.h) [Phase 4 Task 7 fix round
# 1, spec D4's "Chunk-0 main-thread guarantee" paragraph: "main-thread-only
# subsystems carry debug thread-affinity assertions (dev-preset-active
# regardless of NDEBUG)"] -- ON by default (and explicitly ON in both dev
# presets, CMakePresets.json) but trivially OFF-able (`-DRX_DEBUG_CHECKS=OFF`),
# independent of RX_TRACY (a different concern) and independent of NDEBUG
# (this project's own presets both compile with `-DNDEBUG`, which would
# silently remove a bare `assert()` -- this option exists specifically so
# a real violation is caught in the SAME builds this project actually
# produces). Read by src/rx_core/CMakeLists.txt to decide whether
# RX_DEBUG_CHECKS is defined (PUBLIC, transitively) and debug_checks.cpp's
# real body compiles in at all -- when OFF, every RX_ASSERT_MAIN_THREAD
# call site expands to nothing and the resulting binaries contain zero
# guard-related symbols (verified in task-7-report.md's fix-round section
# via nm/strings, mirroring RX_TRACY's own OFF-verification convention
# above).
option(RX_DEBUG_CHECKS "Enable dev-time thread-affinity assertions (RX_ASSERT_MAIN_THREAD); independent of NDEBUG" ON)

add_subdirectory(third_party)

include(tools/fetch_slang.cmake)
add_subdirectory(shaders)

add_subdirectory(src/rx_core)
# rx_task: the engine's sole task scheduler wrapper [spec D2, Phase 4
# Stage 0 Task 2] -- device-free, depends only on rx_core (logging) +
# enkiTS (third_party/CMakeLists.txt). Added here, independent of the
# rx_platform/rx_shader/rx_rhi_vk/rx_graph/rx_material chain below: nothing
# in this task's scope links rx_task yet (Task 7/12 are the first
# consumers), and nothing it depends on requires any of those libraries.
add_subdirectory(src/rx_task)
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
# rx_asset: Phase 4 Stage 1 Task 12 -- GeometryPool, pooled global vertex/
# index storage (spec D8/D9/D26.4). Depends on rx_rhi_vk (Allocator/
# Device/Uploader) directly; its GPU test binary additionally links
# rx_shader + rx_platform (same windowed-device + in-process shader
# compile pattern as rx_graph's own GPU test binary, src/rx_graph/tests/
# CMakeLists.txt) to build a minimal draw pipeline for its readback
# assertions -- see src/rx_asset/CMakeLists.txt.
add_subdirectory(src/rx_asset)
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
# Phase 4 Stage 1 Task 13: host-only fixture regenerator (assets/test/
# cube_meshopt.*/cube_draco.*) -- see its own CMakeLists.txt comment.
add_subdirectory(tools/gen_gltf_compression_fixtures)
add_subdirectory(samples/01_triangle)
add_subdirectory(samples/02_hotreload)
add_subdirectory(samples/03_bindless_mesh)
add_subdirectory(samples/04_streaming)
add_subdirectory(samples/05_multipass)
add_subdirectory(samples/06_materials)
add_subdirectory(samples/07_stress)
