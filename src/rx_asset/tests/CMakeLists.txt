# GPU-backed only -- device-free is impossible for GeometryPool's real
# behavior (every upload() call allocates a real device-local VkBuffer
# pair and records a real GPU copy) [Task 12 brief]. doctest_main.cpp
# builds a real vk-bootstrap instance at warm-up, so this binary needs the
# same process-wide instance-function-pointer-caching warm-up
# src/rx_rhi_vk/tests/doctest_main.cpp / src/rx_graph/tests/
# doctest_main_gpu.cpp both establish -- see that file's own comment.
# rx_shader is linked because geometry_pool_test.cpp's exhaustion-draw
# test compiles a minimal push-constant-color fill shader in-process
# (rx::shader::Compiler + reflect(), the same reflection-driven
# pipeline-layout path samples/03_bindless_mesh/main.cpp and
# rx_graph_gpu_tests' own "Fill" pipeline establish) rather than through a
# slangc build-time precompile -- rx_shader_deploy_runtime_libs() (defined
# in src/rx_shader/CMakeLists.txt, add_subdirectory'd before src/rx_asset
# in the root CMakeLists.txt) copies the Slang runtime shared libraries
# this binary needs at load time next to it, exactly like every other
# rx_shader-linking binary in this repo.
add_executable(rx_asset_tests
    doctest_main.cpp
    geometry_pool_test.cpp
    bda_test.cpp
    accounting_test.cpp
)
target_link_libraries(rx_asset_tests PRIVATE rx_asset rx_rhi_vk rx_platform rx_shader doctest::doctest)
rx_shader_deploy_runtime_libs(rx_asset_tests)

# --validate: same documentation/consistency role as every other GPU-gate
# invocation in this repo (rx_graph_gpu_tests, every sample's own headless
# ctest gate) -- every fixture here already builds its rx::rhi::Context
# with enableValidation=true unconditionally, and each TEST_CASE's own
# CHECK_FALSE(context.hasValidationErrors()) is what actually turns a
# validation message into a ctest failure; doctest itself ignores this
# flag.
add_test(NAME rx_asset_tests COMMAND rx_asset_tests --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(rx_asset_tests PROPERTIES TIMEOUT 300)
