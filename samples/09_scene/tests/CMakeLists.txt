# Device-free (no VkDevice, no rx::platform::Window) -- splitByBlockAndGroup()
# (../draw_recording.h) is pure data transformation over plain rx::scene
# types, matching src/rx_scene/tests/CMakeLists.txt's own device-free
# binary convention. Compiles ../draw_recording.cpp directly rather than
# linking a separate library target -- this seam is sample-09-local, not a
# reusable engine module (no other sample needs it). test_fly_camera.cpp
# covers ../fly_camera.h (also device-free, header-only -- no matching .cpp
# to add) -- the W/S-inversion regression coverage. test_mouse_capture.cpp
# covers ../mouse_capture.h (same device-free, header-only shape) -- Issue
# #33's relative-mouse-capture toggle/ImGui-composition coverage.
#
# test_mouse_capture_focus_composition.cpp is the ONE exception to
# "device-free" in this binary: it links rx_platform to drive a real
# (headless/hidden, no VkDevice) rx::platform::Window -- see that file's
# own top comment for why the toggle/ImGui-composition coverage above is
# insufficient on its own to prove Issue #33's "re-arm on focus-gain
# composes with the toggle state" requirement, and why this needs the real
# Window rather than re-mocking window.cpp's own already-tested
# (src/rx_platform/tests/window_test.cpp) FOCUS_LOST/FOCUS_GAINED logic.
add_executable(sample_09_scene_tests
    doctest_main.cpp
    test_draw_recording.cpp
    test_fly_camera.cpp
    test_mouse_capture.cpp
    test_mouse_capture_focus_composition.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../draw_recording.cpp"
)
target_link_libraries(sample_09_scene_tests PRIVATE rx_scene rx_platform doctest::doctest)
add_test(NAME sample_09_scene_tests COMMAND sample_09_scene_tests)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(sample_09_scene_tests PROPERTIES TIMEOUT 120)
