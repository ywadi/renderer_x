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
add_library(rx_asset STATIC
    geometry_pool.cpp
)
target_include_directories(rx_asset PUBLIC include)
target_link_libraries(rx_asset PUBLIC rx_rhi_vk)

add_subdirectory(tests)
