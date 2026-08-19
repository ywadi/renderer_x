# Device-free (no VkDevice, no rx::platform::Window) -- splitByBlockAndGroup()
# (../draw_recording.h) is pure data transformation over plain rx::scene
# types, matching src/rx_scene/tests/CMakeLists.txt's own device-free
# binary convention. Compiles ../draw_recording.cpp directly rather than
# linking a separate library target -- this seam is sample-09-local, not a
# reusable engine module (no other sample needs it).
add_executable(sample_09_scene_tests
    doctest_main.cpp
    test_draw_recording.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../draw_recording.cpp"
)
target_link_libraries(sample_09_scene_tests PRIVATE rx_scene doctest::doctest)
add_test(NAME sample_09_scene_tests COMMAND sample_09_scene_tests)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(sample_09_scene_tests PROPERTIES TIMEOUT 120)
