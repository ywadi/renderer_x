# tools/rx_cluster_bench -- Phase 5 Stage 2 Task 14 [#50]: standalone
# synthetic-high-light-count froxelization+assignment timing benchmark, NOT
# ctest-registered (see main.cpp's own header comment for why: a stress-
# scale run is too slow/noisy to gate every CI push on, and this tool's job
# is publishing honest numbers for the task report, not a merge gate --
# T36/#72, RC8, owns the permanent CI perf-regression gate mechanism).
add_executable(rx_cluster_bench main.cpp)
target_link_libraries(rx_cluster_bench PRIVATE rx_cluster rx_rhi_vk rx_platform rx_scene)
target_compile_definitions(rx_cluster_bench PRIVATE
  RX_CLUSTER_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders/cluster"
)
rx_shader_deploy_runtime_libs(rx_cluster_bench)
