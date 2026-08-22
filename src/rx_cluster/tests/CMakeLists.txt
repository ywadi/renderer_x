# rx_cluster_tests -- device-free (no VkDevice): rx::cluster::
# buildClusterLightList() touches no Vulkan type at all -- matching
# src/rx_scene/tests/CMakeLists.txt's identical device-free-binary
# convention.
add_executable(rx_cluster_tests
    doctest_main.cpp
    test_build_cluster_light_list.cpp
)
target_link_libraries(rx_cluster_tests PRIVATE rx_cluster doctest::doctest)
add_test(NAME rx_cluster_tests COMMAND rx_cluster_tests)
set_tests_properties(rx_cluster_tests PROPERTIES TIMEOUT 120)

# rx_cluster_gpu_tests -- Phase 5 Stage 2 Task 14 [#50]. Every TEST_CASE
# needs a real windowed VkDevice (compute dispatch, real buffer readback) --
# matches rx_ibl_gpu_tests'/rx_graph's own compute-GPU-test binary shape
# exactly (this codebase's established "one *_gpu_tests binary per module
# that has no device-free half worth folding this content into" precedent).
add_executable(rx_cluster_gpu_tests
    doctest_main_gpu.cpp
    test_cluster_membership_gpu.cpp
    test_cluster_capacity_determinism_gpu.cpp
)
target_link_libraries(rx_cluster_gpu_tests PRIVATE rx_cluster rx_rhi_vk rx_platform rx_shader rx_scene doctest::doctest)
rx_shader_deploy_runtime_libs(rx_cluster_gpu_tests)

target_compile_definitions(rx_cluster_gpu_tests PRIVATE
  RX_CLUSTER_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders/cluster"
)

# --validate: same developer/CI-gate opt-in every other GPU-backed test
# binary's own ctest registration documents -- this binary's own fixture
# builds every rx::rhi::Context with enableValidation=true unconditionally;
# each TEST_CASE's own CHECK_FALSE(context.hasValidationErrors()) is what
# turns a validation message into a real ctest failure.
add_test(NAME rx_cluster_gpu_tests COMMAND rx_cluster_gpu_tests --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours.
set_tests_properties(rx_cluster_gpu_tests PROPERTIES TIMEOUT 300)
