# Device-free: fitted-ortho + texel-snapping math only (shadow_frustum.h) --
# no VkDevice, mirrors rx_graph_tests' own split.
add_executable(rx_shadow_tests
    doctest_main.cpp
    test_shadow_frustum.cpp
)
target_link_libraries(rx_shadow_tests PRIVATE rx_shadow doctest::doctest)
add_test(NAME rx_shadow_tests COMMAND rx_shadow_tests)
set_tests_properties(rx_shadow_tests PROPERTIES TIMEOUT 120)

# GPU-backed: the real shadow-caster pipeline (device feature checks,
# dynamic depth bias, depth clamp) plus the acne/peter-panning/PCF/
# texel-snap-shimmer probes -- same warm-up-before-any-TEST_CASE pattern
# every other GPU test binary in this codebase establishes.
add_executable(rx_shadow_gpu_tests
    doctest_main_gpu.cpp
    test_shadow_caster_gpu.cpp
)
target_link_libraries(rx_shadow_gpu_tests PRIVATE rx_shadow rx_rhi_vk rx_platform rx_shader doctest::doctest)
rx_shader_deploy_runtime_libs(rx_shadow_gpu_tests)

add_test(NAME rx_shadow_gpu_tests COMMAND rx_shadow_gpu_tests --validate)
set_tests_properties(rx_shadow_gpu_tests PROPERTIES TIMEOUT 300)
