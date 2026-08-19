# Phase 4 Stage 2 Task 21 -- rx_debug_ui: Dear ImGui debug overlay (spec
# D20, gate matrix-issue16 as amended by gate/rulings-2026-08-18.md #16).
# The vendored `imgui` target (third_party/CMakeLists.txt) is PUBLIC on
# this library -- deliberately: this is the one, documented seam ImGui
# symbols are meant to cross into a consumer (samples, this module's own
# tests) through, unlike every rx_* CORE library, whose link closure is
# asserted free of it at configure time (cmake/DependencyBoundaryCheck.cmake,
# called from the root CMakeLists.txt right after this module is added).
add_library(rx_debug_ui STATIC
    overlay.cpp
)
target_include_directories(rx_debug_ui PUBLIC include)
target_link_libraries(rx_debug_ui PUBLIC imgui rx_platform rx_rhi_vk rx_graph rx_core)

# GPU smoke test [Task 21 brief's own Steps line: "GPU smoke test (overlay
# pass renders; readback shows non-empty overlay region with a forced demo
# window; zero validation errors)"] -- same windowed-device fixture pattern
# and warm-up-before-any-TEST_CASE main() as rx_graph/tests/test_execute_gpu.
# cpp/doctest_main_gpu.cpp (copied, not shared -- see that file's own
# comment). No device-free rx_debug_ui_tests target exists: this module's
# own logic is inherently GPU-bound (wrapping the vendored Vulkan/SDL3
# backends) -- the one genuinely device-free seam this task added
# (rx::platform::Window::pumpEvents()'s new `preDispatch` parameter) is
# tested where it lives, src/rx_platform/tests/window_test.cpp, not
# duplicated here.
add_executable(rx_debug_ui_gpu_tests
    tests/doctest_main_gpu.cpp
    tests/test_overlay_gpu.cpp
)
target_link_libraries(rx_debug_ui_gpu_tests PRIVATE rx_debug_ui rx_task doctest::doctest)

# --validate: same reason every other GPU-gate ctest invocation in this
# repo appends it (samples/*/CMakeLists.txt, rx_graph/tests/CMakeLists.txt)
# -- this binary's fixtures already build every rx::rhi::Context with
# enableValidation=true unconditionally; the flag documents/keeps this
# invocation consistent with the rest of this repo's GPU gates (doctest
# itself ignores it).
add_test(NAME rx_debug_ui_gpu_tests COMMAND rx_debug_ui_gpu_tests --validate)
# Hang guard: a wedged test fails in minutes with output, instead of
# stalling CI for hours (windows-cross Wine hang, run 31512073559) --
# same convention as every other GPU-test binary's own ctest registration
# in this repo. This target is EXCLUDED from the windows-cross-zig job's
# ctest invocation entirely (.github/workflows/ci.yml, same reason
# rx_rhi_vk_tests/rx_graph_gpu_tests/rx_material_gpu_tests already are:
# Wine has no real Vulkan device to hand it), so this timeout only ever
# matters on linux-native + a local `xvfb-run -a ctest` run.
set_tests_properties(rx_debug_ui_gpu_tests PROPERTIES TIMEOUT 120)
