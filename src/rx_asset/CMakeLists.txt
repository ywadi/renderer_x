# rx_asset: Phase 4 Stage 1 Task 12 -- GeometryPool, the engine's pooled
# global vertex/index storage (spec D8/D9, BDA enablement D26.4). Depends
# on rx_rhi_vk directly (rx::rhi::Allocator/Device/Uploader/Buffer --
# chunk buffers are ordinary device-local Buffers allocated through the
# SAME Allocator::createDeviceLocalBuffer() choke point every other
# device-local buffer in this engine uses, tagged
# MemoryCategory::GeometryPool [D24(a)]). Links VMA's virtual-block API
# transitively via rx_rhi_vk/buffer.h's already-configured
# <vk_mem_alloc.h> include (VMA_STATIC_VULKAN_FUNCTIONS=0/
# VMA_DYNAMIC_VULKAN_FUNCTIONS=1 defined once there -- see that header's
# own comment), so this library needs no VMA-specific CMake wiring of its
# own beyond the existing rx_rhi_vk PUBLIC link.

# Phase 4 Stage 1 Task 13 -- glTF 2.0 import core (spec D7/D11/D12/D16,
# gate matrix-issue02 as amended by gate/rulings-2026-08-18.md #2):
# byte_source (the IO-source abstraction invariant, 2026-08-12), the
# mesh/material/scene data model (mesh_asset.h), the Registry (handle
# ownership + D11 fallbacks + D24 eviction invariant), and the importer
# pipeline itself (import_gltf.cpp -- fastgltf parse -> accessor read ->
# MikkTSpace/meshoptimizer -> AABB -> GeometryPool upload). rx_task
# PUBLIC: import_gltf.h names rx::task::Scheduler& in Registry::
# importGltf()'s own signature (matrix row 15's internal-parallelism
# requirement has no reasonable implementation without a live Scheduler
# reference, and D2 commits to no engine-wide singleton -- the plan's
# illustrative interface sketch predates the gate hardening pass and
# omitted it, exactly the same class of necessary, documented deviation
# Task 12 recorded for GeometryPool::create()'s own Allocator& parameter;
# see the task report).
add_library(rx_asset STATIC
    geometry_pool.cpp
    byte_source.cpp
    mesh_asset.cpp
    import_error.cpp
    gltf_error.cpp
    mikktspace_bridge.cpp
    gltf_pipeline.cpp
    import_gltf.cpp
    registry.cpp
    fallbacks.cpp
    # Phase 4 Stage 1 Task 14 -- KTX2/Basis compressed texture pipeline +
    # sampler cache (spec D10/D11, gate matrix-issue03 as amended by
    # gate/rulings-2026-08-18.md #3). texture_decode.{h,cpp} is the
    # device-free parse/transcode/decode layer (mirrors gltf_pipeline.cpp's
    # own device-free/GPU split -- see tests/CMakeLists.txt); texture_cache.
    # {h,cpp} is the GPU-facing layer (Texture2D/Uploader/BindlessTable/
    # DeletionQueue/sampler cache, D5 main-thread-only).
    texture_decode.cpp
    texture_cache.cpp
    # [texture-path round, D10 Option A] stb_image_resize2's own single
    # implementation TU -- see stb_image_resize_impl.cpp's own comment for
    # why this lives in rx_asset (the actual runtime-mip-generation
    # consumer) rather than rx_rhi_vk's existing stb_impl.cpp.
    stb_image_resize_impl.cpp
    # MikkTSpace [G2]: no CMakeLists.txt upstream (two loose files,
    # "designed to be stand-alone... copied and used as is" per its own
    # header comment) -- compiled straight into rx_asset, matching this
    # project's stb/volk-style "no build system -> compile into the
    # consumer" convention (see third_party/CMakeLists.txt's own comment
    # on the FetchContent_Populate that fetched mikktspace_SOURCE_DIR).
    "${mikktspace_SOURCE_DIR}/mikktspace.c"
)
target_include_directories(rx_asset PUBLIC include)
target_include_directories(rx_asset PRIVATE "${mikktspace_SOURCE_DIR}")
target_link_libraries(rx_asset PUBLIC rx_rhi_vk rx_task fastgltf::fastgltf meshoptimizer::meshoptimizer draco::draco KTX::ktx)

# Draco dead-stripping [gate matrix row 9, third_party/CMakeLists.txt's
# own comment on Draco's vendoring]: Draco's single `draco::draco`
# library unconditionally contains both encoder and decoder object code
# (no decode-only build option exists upstream) and was compiled with
# -ffunction-sections/-fdata-sections at ITS OWN build step; --gc-sections
# here is the other half -- applied PUBLICLY so every executable that
# transitively links rx_asset (this library's own test binaries, and any
# future sample) gets it at its own final link step, which is the only
# point gc-sections has anything to prune (a static-library archive step,
# like rx_asset's own, does not link in the sense gc-sections needs).
target_link_options(rx_asset PUBLIC -Wl,--gc-sections)

add_subdirectory(tests)
