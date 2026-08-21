# rx_shader_deploy_runtime_libs() is defined in src/rx_shader/CMakeLists.txt
# (add_subdirectory'd from the root CMakeLists.txt before this directory) --
# see that file's own comment for the directory-scope fix (Task 6) that
# makes it safe to call unqualified from here, no local re-glob needed.

add_executable(sample_03_bindless_mesh main.cpp)
# rx_frame_loop [Phase 5 Task 5, ticket #41]: shared present-loop helper.
target_link_libraries(sample_03_bindless_mesh PRIVATE rx_rhi_vk rx_platform rx_shader rx_frame_loop)

# This sample does real in-process Slang compilation at startup (both
# modes -- see main.cpp's kShaderSource/buildScenePipeline()), same as
# 02_hotreload -- needs the Slang runtime libs deployed next to it.
rx_shader_deploy_runtime_libs(sample_03_bindless_mesh)

# Deploys the one real PNG this sample decodes via stb_image (see
# main.cpp's resolveTexturePath()/createScene()) next to the binary at
# build time, so a redistributed copy of just this build-output directory
# (binary + texture.png + the Slang runtime libs above) works identically
# outside the build tree -- same mechanism/rationale as 02_hotreload's
# hotreload.slang deployment.
add_custom_command(TARGET sample_03_bindless_mesh POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/texture.png"
          "$<TARGET_FILE_DIR:sample_03_bindless_mesh>/texture.png"
  COMMENT "sample_03_bindless_mesh: deploying texture.png next to the binary"
  VERBATIM
)

# Runs the binary in headless mode with --validate: ctest is the developer/CI
# gate, so it explicitly opts into the Vulkan validation layers (requires the
# Vulkan SDK / validation layers installed -- see samples/README.md) on top
# of the default end-user behavior, which is headless with validation OFF.
# The binary also supports --present mode for interactive windowed
# rendering with an orbiting camera (see main.cpp / samples/README.md).
add_test(NAME sample_03_bindless_mesh_headless COMMAND sample_03_bindless_mesh --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(sample_03_bindless_mesh_headless PROPERTIES TIMEOUT 300)
