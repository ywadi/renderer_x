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
# rx_scene: Phase 4 Stage 2 Task 18 -- Scene (D19 render-proxy submission,
# no ECS) + Camera (D13 reversed-Z projection source of truth). Entirely
# device-free; depends on rx_asset (MeshHandle/MaterialHandle/AABB types,
# and Registry via scene.cpp's meshBoundsFromRegistry() adapter). Added
# here, right after rx_asset, since it is Stage 2's first library and
# nothing in Stage 1 (rx_graph/rx_material) depends on it.
add_subdirectory(src/rx_scene)
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

# Phase 4 Stage 2 Task 22 -- rx_shadow: the scene shadow quality bridge
# (spec D21; gate matrix-issue23-shadow-bridge.md as amended by gate/
# rulings-2026-08-18.md's #23 + RC2/RC3). Depends on rx_rhi_vk (Device/
# BindlessTable/PipelineLayoutBuilder) and rx_shader (Compiler/reflect) --
# NOT rx_material: its own shadow-caster pipeline is built OUTSIDE
# MaterialSystem entirely (RC3), so it has no dependency on that library.
add_subdirectory(src/rx_shadow)

# Phase 5 Task 5 -- rx_frame_loop: the shared present-loop helper (ticket
# #41, gate ruling T5) -- an OPTIONAL engine module layered exactly like
# rx_debug_ui immediately below (same rx_platform/rx_rhi_vk/rx_graph/rx_core
# dependency shape); added here, after every rx_* CORE library above, for
# the identical reason rx_debug_ui is -- see that module's own comment.
add_subdirectory(src/rx_frame_loop)

# Phase 4 Stage 2 Task 21 -- rx_debug_ui: Dear ImGui overlay module (spec
# D20). Depends on rx_platform (Window)/rx_rhi_vk (Device/Uploader)/
# rx_graph (RenderGraph pass declaration) plus the vendored `imgui` target
# (third_party/CMakeLists.txt) -- added here, after every rx_* CORE
# library above, so the dependency-boundary check immediately below can
# assert none of THOSE core targets pulled it in transitively, and before
# the samples that will (Task 24) become its first real consumer.
add_subdirectory(src/rx_debug_ui)

# Configure-time link-boundary check [Phase 4 Stage 2 Task 21, gate matrix-
# issue16 row 12 as amended by gate/rulings-2026-08-18.md #16; see
# cmake/DependencyBoundaryCheck.cmake for the full mechanism comment]: every
# rx_* CORE library defined above (deliberately excludes rx_debug_ui
# itself, which is SUPPOSED to depend on imgui, and excludes samples/tools,
# which are hosts, not core libraries) must not transitively link the
# vendored `imgui` target -- the "core libraries and public ABI stay
# ImGui-free" hard boundary D20/ticket #16 both restate. Runs once, right
# here, so a violation fails THIS configure step before any compilation
# starts.
include(cmake/DependencyBoundaryCheck.cmake)
foreach(_rx_core_target rx_core rx_rhi_vk rx_graph rx_material rx_platform rx_task rx_shader rx_asset rx_scene)
    rx_assert_target_excludes_dependency(${_rx_core_target} imgui)
endforeach()

# Second configure-time link-boundary check [Phase 5 Task 5, ticket #41,
# review round Medium finding #1] -- mirrors the imgui check immediately
# above exactly, same core-target list, same mechanism
# (cmake/DependencyBoundaryCheck.cmake): every rx_* CORE library defined
# above (deliberately excludes rx_frame_loop itself, which is SUPPOSED to
# depend on rx_platform/rx_rhi_vk/rx_graph, and excludes rx_debug_ui/
# samples/tools, which are hosts or peers, not core libraries) must not
# transitively link rx_frame_loop -- the "core libraries stay free of the
# optional runner-convenience module" boundary rx_frame_loop's own header
# comment already states in prose (present_loop.h: "rx_graph/rx_rhi_vk/
# rx_platform themselves gain NO new dependency ... from this module's
# existence") is now configure-enforced, not merely documented. Proven
# load-bearing this round exactly like Task 21's own imgui check was: a
# violation (`target_link_libraries(rx_material PUBLIC rx_frame_loop)`,
# injected temporarily immediately after `add_subdirectory(src/
# rx_material)` above) was configured, observed to FATAL_ERROR with the
# expected "rx_material -> rx_frame_loop" dependency-chain message, then
# removed -- see task-05-report.md, which also records a first injection
# attempt via rx_platform that hit the walk's own recursion-depth guard
# instead (`_rx_dep_closure_contains`'s documented non-deduplicated design,
# cmake/DependencyBoundaryCheck.cmake:28-44): rx_platform is one of
# rx_frame_loop's OWN dependencies, so that injection created a genuine
# rx_platform->rx_frame_loop->rx_platform cycle the walk is not designed to
# short-circuit -- not a bug in the check as applied to this project's real
# (acyclic) target graph, but a real usability note for a future user
# picking an injection target to re-run this same proof: pick a target
# rx_frame_loop does NOT itself depend on (rx_material/rx_task/rx_shader/
# rx_asset/rx_scene), not one of rx_platform/rx_rhi_vk/rx_graph/rx_core.
foreach(_rx_core_target rx_core rx_rhi_vk rx_graph rx_material rx_platform rx_task rx_shader rx_asset rx_scene)
    rx_assert_target_excludes_dependency(${_rx_core_target} rx_frame_loop)
endforeach()

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
# samples/common: D17's reusable reference-PNG gate -- add_subdirectory'd
# before 08_gltf_viewer (the first sample to link it).
add_subdirectory(samples/common)
# Issue #31: generic two-PNG comparison host tool built on samples/common's
# own gate algorithm -- see tools/compare_rgba8_png/main.cpp's own header
# comment. Added after samples/common (its own dependency) and before
# 08_gltf_viewer (not a dependency of it, ordering is incidental).
add_subdirectory(tools/compare_rgba8_png)
add_subdirectory(samples/08_gltf_viewer)
add_subdirectory(samples/09_scene)
