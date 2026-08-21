# [Phase 5 Task 5, ticket #41] draw_recording.{h,cpp}/fly_camera.h/
# mouse_capture.h/window_resize.h and their own dedicated test files are
# GONE -- promoted engine-side (matrix-p5t05 rows 1-2, 3, 4, 6, 7):
#   - splitByBlockAndGroup()/RecordSpan/materialIndexForSpan (row 6) now
#     live in src/rx_scene/include/rx_scene/draw_list.h, tested by
#     src/rx_scene/tests/draw_list_test.cpp.
#   - FlyCamera/flyCameraLocalMoveDelta()/keyboardDrivesCamera() (row 4) now
#     live in src/rx_scene/include/rx_scene/fly_camera.h, tested by
#     src/rx_scene/tests/fly_camera_test.cpp.
#   - FlyThroughCaptureState (renamed MouseCaptureToggle)/
#     mouseDeltaDrivesCamera()/escTogglesCapture() (row 3) now live in
#     src/rx_platform/include/rx_platform/mouse_capture_toggle.h, tested by
#     src/rx_platform/tests/test_mouse_capture_toggle.cpp.
#   - f11TogglesFullscreen()/shouldSkipTeardownAfterDeviceLoss() (row 7) now
#     live in src/rx_frame_loop/include/rx_frame_loop/present_loop.h, tested
#     by src/rx_frame_loop/tests/pure_decisions_test.cpp.
#     pixelSizeRequiresRecreate() is consumed indirectly via
#     rx::frame_loop::PresentLoop::shouldRecreateForPixelSize(), same test
#     binary. graphNeedsRecompileForExtent() has no successor to test here
#     at all -- its decision now lives inside RenderGraph::compile() itself
#     (row 10), tested by src/rx_graph/tests/test_compile.cpp.
#
# Device-free (no VkDevice) -- test_grid_layout.cpp covers ../grid_layout.h
# (row 5 ruling: stays sample-local -- demo-content-generation logic for
# this one sample's fallback scene, not an engine facility). Issue #35's
# dropped-asset-node-rotation regression coverage (the default DamagedHelmet
# grid rendered every helmet lying on its back; the D17 pixel-diff gate
# alone proved blind to it, since the committed reference was itself baked
# FROM the buggy render -- see that test file's own top comment).
#
# test_mouse_capture_focus_composition.cpp is the ONE exception to
# "device-free" in this binary: it links rx_platform to drive a real
# (headless/hidden, no VkDevice) rx::platform::Window through THIS SAMPLE's
# own runPresent()-shaped composition of the (now engine-owned)
# MouseCaptureToggle with Window::setRelativeMouseMode() -- see that file's
# own top comment for why this sample-local regression coverage stays here
# even though the toggle primitive itself moved engine-side: it proves this
# sample's own call sequence, not the promoted type in isolation.
add_executable(sample_09_scene_tests
    doctest_main.cpp
    test_grid_layout.cpp
    test_mouse_capture_focus_composition.cpp
)
target_link_libraries(sample_09_scene_tests PRIVATE rx_scene rx_platform doctest::doctest)
add_test(NAME sample_09_scene_tests COMMAND sample_09_scene_tests)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(sample_09_scene_tests PROPERTIES TIMEOUT 120)
