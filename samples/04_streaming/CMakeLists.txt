# rx_shader_deploy_runtime_libs() is defined in src/rx_shader/CMakeLists.txt
# (add_subdirectory'd from the root CMakeLists.txt before this directory) --
# see that file's own comment for the directory-scope fix (Task 6) that
# makes it safe to call unqualified from here, no local re-glob needed.

add_executable(sample_04_streaming main.cpp)
target_link_libraries(sample_04_streaming PRIVATE rx_rhi_vk rx_platform rx_shader)

# This sample does real in-process Slang compilation at startup (both
# modes -- see main.cpp's kShaderSource/buildScenePipeline()), same as
# 02_hotreload/03_bindless_mesh -- needs the Slang runtime libs deployed
# next to it. Every texture this sample uses is procedurally generated in
# code (see main.cpp's computeExpectedColors()/makeFlatColorTexture()) --
# unlike 03_bindless_mesh, there is no external asset file to deploy.
rx_shader_deploy_runtime_libs(sample_04_streaming)

# Runs the binary in headless mode with --validate: ctest is the developer/CI
# gate, so it explicitly opts into the Vulkan validation layers (requires the
# Vulkan SDK / validation layers installed -- see samples/README.md) on top
# of the default end-user behavior, which is headless with validation OFF.
# The binary also supports --present mode for interactive windowed
# rendering with a continuously cycling grid (see main.cpp / samples/README.md).
add_test(NAME sample_04_streaming_headless COMMAND sample_04_streaming --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(sample_04_streaming_headless PROPERTIES TIMEOUT 300)
