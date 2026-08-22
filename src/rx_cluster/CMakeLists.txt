# rx_cluster: the camera-frustum froxel grid + GPU-compute clustered light
# assignment [Phase 5 Stage 2 Task 14, #50]. Depends on rx_scene (device-
# free froxel grid math, froxel_grid.h, + Scene's own light data), rx_graph
# (compute-class Pass API, Task 2), rx_rhi_vk (ComputePipelineCache), and
# rx_shader (Compiler/reflect() -- the FULL target, since this library
# drives in-process Slang compilation itself, same shape as rx_ibl's own
# dependency). See include/rx_cluster/cluster_lighting.h's own "MODULE
# OWNERSHIP" comment for why this is a new, self-contained library rather
# than folded into rx_scene (which is explicitly, deliberately device-free).
add_library(rx_cluster STATIC
    src/cluster_lighting.cpp
)
target_include_directories(rx_cluster PUBLIC include)
target_link_libraries(rx_cluster PUBLIC rx_core rx_scene rx_graph rx_rhi_vk rx_shader)

add_subdirectory(tests)
