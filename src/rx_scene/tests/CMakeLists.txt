# Device-free (no VkDevice, no rx::platform::Window) -- rx_scene has no
# GPU-touching code at all, matching src/rx_task/tests/CMakeLists.txt's/
# src/rx_graph/tests/CMakeLists.txt's identical device-free binary
# convention. thread_guard_test.cpp needs no fixture beyond a plain
# rx::scene::Scene (unlike rx_asset/tests/thread_guard_test.cpp's
# GeometryPool case, which needs a real window/device) -- Scene owns no
# Vulkan object at all.
add_executable(rx_scene_tests
    doctest_main.cpp
    camera_test.cpp
    scene_test.cpp
    thread_guard_test.cpp
)
target_link_libraries(rx_scene_tests PRIVATE rx_scene doctest::doctest)
add_test(NAME rx_scene_tests COMMAND rx_scene_tests)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(rx_scene_tests PROPERTIES TIMEOUT 120)
