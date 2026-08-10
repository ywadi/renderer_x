# rx_graph: the device-free front half of Phase 3's render graph -- pass
# declarations, name-based dependency resolution, culling, topological
# ordering, and physical-resource lifetime analysis (Task 1 brief), plus
# compile-time sync2 barrier derivation (Task 2 brief, barriers.cpp/h).
# Depends on nothing but Vulkan-Headers (VkFormat/Vk*Flags2/Vk*Bit types
# only -- no loader, no volk, see render_graph.h's header-hygiene comment)
# and rx_core (RX_LOG_ERROR before every validation throw in
# render_graph.cpp, matching rx_shader's/rx_rhi_vk's own error-reporting
# convention).
#
# rx_rhi_vk is deliberately NOT linked here: neither Task 1 nor Task 2
# touches a real VkDevice -- barrier derivation is pure per-resource
# stage/access/layout accounting over already-declared data. Task 3's
# executor is the first thing in this library that needs it -- add that
# dependency there, not preemptively here.
add_library(rx_graph STATIC
    render_graph.cpp
    barriers.cpp
)
target_include_directories(rx_graph PUBLIC include)
target_link_libraries(rx_graph PUBLIC Vulkan::Headers rx_core)

add_subdirectory(tests)
