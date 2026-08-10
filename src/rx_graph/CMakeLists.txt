# rx_graph: the device-free front half of Phase 3's render graph -- pass
# declarations, name-based dependency resolution, culling, topological
# ordering, and physical-resource lifetime analysis (Task 1 brief), plus
# compile-time sync2 barrier derivation (Task 2 brief, barriers.cpp/h).
# render_graph.cpp/barriers.cpp depend on nothing but Vulkan-Headers
# (VkFormat/Vk*Flags2/Vk*Bit types only -- no loader, no volk, see
# render_graph.h's header-hygiene comment) and rx_core (RX_LOG_ERROR before
# every validation throw in render_graph.cpp, matching rx_shader's/
# rx_rhi_vk's own error-reporting convention).
#
# Task 3 adds rx_rhi_vk to this library's link line: executor.cpp/
# transient_pool.cpp are the first things in rx_graph that touch a real
# VkDevice (Texture2D/Buffer/Allocator/DeletionQueue realize the graph's
# physical resources; rx::rhi::Device supplies the physical device/
# instance/queue Executor::create() needs). This is additive, not a
# retraction of Tasks 1-2's device-free design: render_graph.cpp/
# barriers.cpp are unchanged in that respect, and rx_graph's PUBLIC headers
# (render_graph.h/pass.h/resources.h/barriers.h/executor.h) still carry no
# volk and no rx_rhi_vk type -- see executor.h's and transient_pool.h's own
# header-hygiene comments for exactly where the device-touching boundary
# now sits.
add_library(rx_graph STATIC
    render_graph.cpp
    barriers.cpp
    executor.cpp
    transient_pool.cpp
)
target_include_directories(rx_graph PUBLIC include)
target_link_libraries(rx_graph PUBLIC Vulkan::Headers rx_core rx_rhi_vk)

add_subdirectory(tests)
