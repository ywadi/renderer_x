# tools/rx_ibl_bench -- Phase 5 Stage 1 Task 9 [#45]: standalone bake-
# timing benchmark, NOT ctest-registered (see main.cpp's own header
# comment for why: a production-scale bake is too slow to gate every CI
# push on, and this tool's job is publishing honest numbers for the task
# report, not a merge gate -- T36/#72, RC8, owns the permanent CI perf-
# regression gate mechanism).
add_executable(rx_ibl_bench main.cpp)
target_link_libraries(rx_ibl_bench PRIVATE rx_ibl rx_rhi_vk rx_platform)
target_compile_definitions(rx_ibl_bench PRIVATE
  RX_IBL_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders/ibl"
)
rx_shader_deploy_runtime_libs(rx_ibl_bench)
