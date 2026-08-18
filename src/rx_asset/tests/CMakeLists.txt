# GPU-backed only -- device-free is impossible for GeometryPool's real
# behavior (every upload() call allocates a real device-local VkBuffer
# pair and records a real GPU copy) [Task 12 brief]. doctest_main.cpp
# builds a real vk-bootstrap instance at warm-up, so this binary needs the
# same process-wide instance-function-pointer-caching warm-up
# src/rx_rhi_vk/tests/doctest_main.cpp / src/rx_graph/tests/
# doctest_main_gpu.cpp both establish -- see that file's own comment.
# rx_shader is linked because geometry_pool_test.cpp's exhaustion-draw
# test compiles a minimal push-constant-color fill shader in-process
# (rx::shader::Compiler + reflect(), the same reflection-driven
# pipeline-layout path samples/03_bindless_mesh/main.cpp and
# rx_graph_gpu_tests' own "Fill" pipeline establish) rather than through a
# slangc build-time precompile -- rx_shader_deploy_runtime_libs() (defined
# in src/rx_shader/CMakeLists.txt, add_subdirectory'd before src/rx_asset
# in the root CMakeLists.txt) copies the Slang runtime shared libraries
# this binary needs at load time next to it, exactly like every other
# rx_shader-linking binary in this repo.
#
# Phase 4 Stage 1 Task 14 -- KTX2/Basis compressed texture pipeline +
# sampler cache (spec D10/D11/D24/D25, gate matrix-issue03 as amended by
# gate/rulings-2026-08-18.md #3): texture_cache_test.cpp joins this SAME
# GPU-backed binary (not a new executable) -- it needs exactly this
# fixture's real Device/Allocator/Uploader (plus BindlessTable/
# DeletionQueue, which its own fixture adds) and the SAME rx_shader-linked
# in-process pipeline-compile path geometry_pool_test.cpp's own
# exhaustion-draw test already established here, for its own quadrant-
# pixel readback GPU test.
add_executable(rx_asset_tests
    doctest_main.cpp
    geometry_pool_test.cpp
    bda_test.cpp
    accounting_test.cpp
    thread_guard_test.cpp
    texture_cache_test.cpp
)
target_link_libraries(rx_asset_tests PRIVATE rx_asset rx_rhi_vk rx_platform rx_shader doctest::doctest)
rx_shader_deploy_runtime_libs(rx_asset_tests)
target_compile_definitions(rx_asset_tests PRIVATE
    RX_ASSET_ROOT_DIR="${CMAKE_SOURCE_DIR}"
)

# --validate: same documentation/consistency role as every other GPU-gate
# invocation in this repo (rx_graph_gpu_tests, every sample's own headless
# ctest gate) -- every fixture here already builds its rx::rhi::Context
# with enableValidation=true unconditionally, and each TEST_CASE's own
# CHECK_FALSE(context.hasValidationErrors()) is what actually turns a
# validation message into a ctest failure; doctest itself ignores this
# flag.
add_test(NAME rx_asset_tests COMMAND rx_asset_tests --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(rx_asset_tests PROPERTIES TIMEOUT 300)

# ---------------------------------------------------------------------
# Phase 4 Stage 1 Task 13 -- glTF import (spec D7/D11/D12/D16, gate
# matrix-issue02). Two binaries, mirroring rx_graph's device-free/GPU
# split (src/rx_graph/tests/CMakeLists.txt):
#
#   rx_asset_gltf_tests (device-free): gltf_pipeline.h's per-primitive
#   CPU pipeline, mikktspace_bridge.h, byte_source.h -- none of these
#   need a GPU or even fastgltf itself (see gltf_pipeline_test.cpp's own
#   comment). Links straight against meshoptimizer::meshoptimizer for
#   its own independent position-only-weld comparison.
#
#   rx_asset_gltf_gpu_tests: the fastgltf-aware end-to-end path
#   (Registry::importGltf -> GeometryPool::upload), which cannot be
#   device-free -- every successful import ends in a real GPU upload.
#   Needs rx_task (Scheduler, D5's internal-parallelism requirement) in
#   addition to geometry_pool_test.cpp's own fixture dependencies.
# ---------------------------------------------------------------------
add_executable(rx_asset_gltf_tests
    doctest_main_devicefree.cpp
    gltf_pipeline_test.cpp
    # Phase 4 Stage 1 Task 14 -- texture_decode.h's parse/transcode
    # DECISION layer (KTX2 container parse via ktxTexture2_CreateFromMemory,
    # role->format selection, colorspace-agreement check, stb PNG/JPG
    # decode) is genuinely device-free -- no GPU, no VkDevice -- so it
    # joins THIS binary, matching gltf_pipeline_test.cpp's own precedent
    # (the file header comment on this executable already documents the
    # "device-free... none of these need a GPU" rationale this shares).
    texture_decode_test.cpp
)
target_include_directories(rx_asset_gltf_tests PRIVATE ..)
target_link_libraries(rx_asset_gltf_tests PRIVATE rx_asset meshoptimizer::meshoptimizer doctest::doctest)
target_compile_definitions(rx_asset_gltf_tests PRIVATE
    RX_ASSET_ROOT_DIR="${CMAKE_SOURCE_DIR}"
)
add_test(NAME rx_asset_gltf_tests COMMAND rx_asset_gltf_tests)
set_tests_properties(rx_asset_gltf_tests PROPERTIES TIMEOUT 120)

add_executable(rx_asset_gltf_gpu_tests
    doctest_main_gltf_gpu.cpp
    import_gltf_gpu_test.cpp
    damaged_helmet_test.cpp
    # Phase 4 Stage 1 Task 14 -- KHR_texture_basisu wiring from
    # Registry::importGltf() THROUGH a real TextureCache (spec D10, gate
    # matrix-issue03's own "KHR_texture_basisu wiring from import" row) --
    # joins THIS binary since it needs the exact same fastgltf-aware
    # import path import_gltf_gpu_test.cpp already exercises, plus a real
    # TextureCache (BindlessTable/DeletionQueue) this binary's own
    # rx_rhi_vk link already provides everything for.
    import_gltf_basisu_test.cpp
    # Phase 4 Stage 1 Task 15 -- Registry::importGltfAsync() (spec D5/
    # D18-RC6/D24/D25, gate matrix-issue22). Joins THIS binary for the
    # same reason import_gltf_basisu_test.cpp does: the exact same
    # fastgltf-aware Registry/GeometryPool/Scheduler fixture shape, plus
    # rx_task's Scheduler::runOnWorkerThread()/runOnIoThread()/pumpMain()
    # this binary's own rx_task link already provides everything for.
    async_import_test.cpp
)
target_include_directories(rx_asset_gltf_gpu_tests PRIVATE ..)
target_link_libraries(rx_asset_gltf_gpu_tests PRIVATE rx_asset rx_rhi_vk rx_platform rx_task fastgltf::fastgltf doctest::doctest)
target_compile_definitions(rx_asset_gltf_gpu_tests PRIVATE
    RX_ASSET_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    RX_ASSET_ROOT_DIR="${CMAKE_SOURCE_DIR}"
)
add_test(NAME rx_asset_gltf_gpu_tests COMMAND rx_asset_gltf_gpu_tests --validate)
set_tests_properties(rx_asset_gltf_gpu_tests PROPERTIES TIMEOUT 300)
