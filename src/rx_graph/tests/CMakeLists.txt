# Device-free (no rx::platform::Window, no VkSurfaceKHR, no vk-bootstrap
# instance) -- unlike rx_rhi_vk_tests' doctest_main.cpp, this binary needs
# no warm-up before its first TEST_CASE runs, so doctest_main.cpp here is
# the plain DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN translation unit rx_core_tests
# uses. Task 1/2 compile()/barrier tests only -- Task 3's GPU-backed tests
# live in the separate rx_graph_gpu_tests target below, never in this one.
add_executable(rx_graph_tests
    doctest_main.cpp
    test_compile.cpp
    test_barriers.cpp
)
target_link_libraries(rx_graph_tests PRIVATE rx_graph doctest::doctest)
add_test(NAME rx_graph_tests COMMAND rx_graph_tests)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(rx_graph_tests PROPERTIES TIMEOUT 120)

# Task 3: GPU-backed Executor tests -- a real headless-but-validated
# rx::rhi::Device (via rx::platform::Window, exactly like
# rx_rhi_vk_tests' own windowed test fixtures), so this binary needs the
# SAME warm-up-before-any-TEST_CASE main() rx_rhi_vk/tests/doctest_main.cpp
# established (copied into doctest_main_gpu.cpp here -- see that file's own
# comment for why it is not shared with rx_graph_tests' plain main above:
# only THIS binary's tests build a real vk-bootstrap instance at all).
# rx_shader is linked because test_execute_gpu.cpp compiles its "invert"
# shader in-process (rx::shader::Compiler + reflect(), same reflection-
# driven pipeline-layout path samples/03_bindless_mesh/main.cpp
# established) rather than through a slangc build-time precompile --
# rx_shader_deploy_runtime_libs() (defined in src/rx_shader/CMakeLists.txt,
# add_subdirectory'd before src/rx_graph in the root CMakeLists.txt) copies
# the Slang runtime shared libraries this binary needs at load time next
# to it, exactly like every other rx_shader-linking binary in this repo.
add_executable(rx_graph_gpu_tests
    doctest_main_gpu.cpp
    test_execute_gpu.cpp
)
target_link_libraries(rx_graph_gpu_tests PRIVATE rx_graph rx_rhi_vk rx_platform rx_shader doctest::doctest)
rx_shader_deploy_runtime_libs(rx_graph_gpu_tests)

# --validate: appended for the same reason every sample's own headless
# ctest gate appends it (samples/01_triangle/CMakeLists.txt etc.) -- ctest
# is the developer/CI gate, opting into the Vulkan validation layers on
# top of whatever a plain manual run would default to. This binary's
# fixtures already build every rx::rhi::Context with enableValidation=true
# unconditionally (unlike a sample's runtime --validate flag, which
# toggles validation on/off), and each TEST_CASE's own
# CHECK_FALSE(context.hasValidationErrors()) is what actually turns any
# validation message into a real ctest failure, matching every existing
# rx_rhi_vk_tests test case's own idiom -- doctest itself does not
# recognize this argument and silently ignores it, so this flag's role
# here is documentation/consistency with the rest of this repo's GPU-gate
# invocations, not a behavior switch doctest itself reads.
add_test(NAME rx_graph_gpu_tests COMMAND rx_graph_gpu_tests --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(rx_graph_gpu_tests PROPERTIES TIMEOUT 300)
