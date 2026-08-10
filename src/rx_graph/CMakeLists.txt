# rx_graph: the device-free front half of Phase 3's render graph (Task 1
# brief) -- pass declarations, name-based dependency resolution, culling,
# topological ordering, and physical-resource lifetime analysis. Depends on
# nothing but Vulkan-Headers (VkFormat/Vk*Flags2/Vk*Bit types only -- no
# loader, no volk, see render_graph.h's header-hygiene comment) and rx_core
# (RX_LOG_ERROR before every validation throw in render_graph.cpp, matching
# rx_shader's/rx_rhi_vk's own error-reporting convention).
#
# rx_rhi_vk is deliberately NOT linked here: Task 1 never touches a real
# VkDevice. Barrier derivation (Task 2) and the executor (Task 3) are the
# first things in this library that need it -- add that dependency there,
# not preemptively here.
add_library(rx_graph STATIC
    render_graph.cpp
)
target_include_directories(rx_graph PUBLIC include)
target_link_libraries(rx_graph PUBLIC Vulkan::Headers rx_core)

add_subdirectory(tests)
