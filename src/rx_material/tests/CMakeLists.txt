# Every test_material_system.cpp case builds a real VkDevice-backed
# MaterialSystem and ends up calling vkCreateGraphicsPipelines at least
# once (getPipeline()) -- there is no device-free half of this library
# worth a separate plain-doctest_main target the way rx_graph's Tasks 1-2
# compile()/barrier logic had. So, unlike rx_graph's rx_graph_tests/
# rx_graph_gpu_tests split, this is the ONE test binary for rx_material,
# named rx_material_gpu_tests to both match rx_graph_gpu_tests' own naming
# convention and give CI's windows-cross-zig job (no real Vulkan device
# under Wine -- .github/workflows/ci.yml) an unambiguous substring to
# exclude on, exactly like it already does for rx_rhi_vk*/rx_graph_gpu*.
#
# doctest_main.cpp here is the SAME vk-bootstrap-instance-function-cache
# warm-up pattern src/rx_rhi_vk/tests/doctest_main.cpp and
# src/rx_graph/tests/doctest_main_gpu.cpp both already establish (copied,
# not shared, matching those two files' own precedent -- see either one's
# header comment for the full defect this avoids: the pinned vk-bootstrap
# commit caches instance-level Vulkan function pointers process-wide from
# whichever vkb::Instance is built FIRST, forever).
add_executable(rx_material_gpu_tests
    doctest_main.cpp
    test_material_system.cpp
)
target_link_libraries(rx_material_gpu_tests PRIVATE rx_material rx_rhi_vk rx_platform doctest::doctest)
rx_shader_deploy_runtime_libs(rx_material_gpu_tests)

target_compile_definitions(rx_material_gpu_tests PRIVATE
  RX_MATERIAL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

# --validate: same developer/CI-gate opt-in every other GPU-backed test
# binary's own ctest registration already documents (rx_graph/tests/
# CMakeLists.txt, rx_rhi_vk/tests -- via each TEST_CASE's own
# CHECK_FALSE(context.hasValidationErrors())); this binary's own fixtures
# build every rx::rhi::Context with enableValidation=true unconditionally.
add_test(NAME rx_material_gpu_tests COMMAND rx_material_gpu_tests --validate)
