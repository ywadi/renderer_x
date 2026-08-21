# Two test binaries, matching rx_graph's own tests/rx_graph_tests +
# tests/rx_graph_gpu_tests split (src/rx_graph/tests/CMakeLists.txt): this
# module's PURE decision functions (pixelSizeRequiresRecreate()/
# f11TogglesFullscreen()/shouldSkipTeardownAfterDeviceLoss()) are
# device-free and TDD-first per this phase's binding constraint;
# PresentLoop's own stateful orchestration is inherently GPU-bound (it
# wraps a real rx::rhi::Device's acquire/present cycle) and gets its own
# windowed-but-headless GPU test binary, mirroring rx_debug_ui_gpu_tests'
# own precedent exactly (src/rx_debug_ui/CMakeLists.txt's own comment: "No
# device-free ...tests target exists [beyond the pure seam]... this
# module's own logic is inherently GPU-bound").
add_executable(rx_frame_loop_tests
    doctest_main.cpp
    pure_decisions_test.cpp
)
target_link_libraries(rx_frame_loop_tests PRIVATE rx_frame_loop doctest::doctest)
add_test(NAME rx_frame_loop_tests COMMAND rx_frame_loop_tests)
set_tests_properties(rx_frame_loop_tests PROPERTIES TIMEOUT 120)

add_executable(rx_frame_loop_gpu_tests
    doctest_main_gpu.cpp
    present_loop_gpu_test.cpp
)
target_link_libraries(rx_frame_loop_gpu_tests PRIVATE rx_frame_loop rx_task doctest::doctest)
# --validate: same reason every other GPU-gate ctest invocation in this repo
# appends it -- this binary's fixtures already build every rx::rhi::Context
# with enableValidation=true unconditionally; the flag documents/keeps this
# invocation consistent with the rest of this repo's GPU gates (doctest
# itself ignores it).
add_test(NAME rx_frame_loop_gpu_tests COMMAND rx_frame_loop_gpu_tests --validate)
# Hang guard: a wedged test fails in minutes with output, instead of
# stalling CI for hours (windows-cross Wine hang, run 31512073559) -- same
# convention as every other GPU-test binary's own ctest registration in
# this repo. This target is EXCLUDED from the windows-cross-zig job's
# ctest invocation entirely (.github/workflows/ci.yml), same reason
# rx_rhi_vk_tests/rx_graph_gpu_tests/rx_material_gpu_tests/
# rx_debug_ui_gpu_tests already are: Wine has no real Vulkan device to hand
# it.
set_tests_properties(rx_frame_loop_gpu_tests PROPERTIES TIMEOUT 120)
