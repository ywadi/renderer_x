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
    draw_list_test.cpp
    # [Phase 5 Task 5, ticket #41 row 4] FlyCamera -- promoted from
    # samples/09_scene/fly_camera.h.
    fly_camera_test.cpp
    # [Phase 5 Stage 2 Task 13, #49] KHR_lights_punctual attenuation math.
    light_math_test.cpp
)
target_link_libraries(rx_scene_tests PRIVATE rx_scene doctest::doctest)
add_test(NAME rx_scene_tests COMMAND rx_scene_tests)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(rx_scene_tests PROPERTIES TIMEOUT 120)
