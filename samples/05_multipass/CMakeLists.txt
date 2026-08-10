# rx_shader_deploy_runtime_libs() is defined in src/rx_shader/CMakeLists.txt
# (add_subdirectory'd from the root CMakeLists.txt before this directory) --
# see that file's own comment for the directory-scope fix (Task 6) that
# makes it safe to call unqualified from here, no local re-glob needed.

add_executable(sample_05_multipass main.cpp)
target_link_libraries(sample_05_multipass PRIVATE rx_graph rx_rhi_vk rx_platform rx_shader)

# This sample does real in-process Slang compilation at startup (both
# modes -- see main.cpp's compileAndReflect()/createScene()), same as
# 02_hotreload/03_bindless_mesh/04_streaming -- needs the Slang runtime
# libs deployed next to it.
rx_shader_deploy_runtime_libs(sample_05_multipass)

# Deploys every one of this pass's six on-disk shader sources next to the
# binary at build time, flat (no "multipass/" subdirectory in the deployed
# layout -- same flat-next-to-the-binary convention 02_hotreload's
# hotreload.slang deployment already established), so main.cpp's
# resolveAssetPath() (SDL_GetBasePath() + filename) finds them identically
# whether run from the build tree or a redistributed copy of just this
# directory's build output. scene_types.slang [fix round 1] is the single
# source of truth for ObjectTransform, concatenated ahead of both
# shadow.vert.slang and lit.vert.slang at compile time (main.cpp's
# compileAndReflect() call sites) -- it ships here for the identical
# reason every other file in this list does: it must be readable from disk
# at runtime, next to the binary, for either build-tree or packaged runs.
set(RX_MULTIPASS_SHADERS
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/multipass/scene_types.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/multipass/shadow.vert.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/multipass/lit.vert.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/multipass/lit.frag.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/multipass/tonemap.vert.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/multipass/tonemap.frag.slang"
)
add_custom_command(TARGET sample_05_multipass POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RX_MULTIPASS_SHADERS} "$<TARGET_FILE_DIR:sample_05_multipass>/"
  COMMENT "sample_05_multipass: deploying shaders/multipass/*.slang next to the binary"
  VERBATIM
)

# Runs the binary in headless mode with --validate: ctest is the developer/CI
# gate, so it explicitly opts into the Vulkan validation layers (requires the
# Vulkan SDK / validation layers installed -- see samples/README.md) on top
# of the default end-user behavior, which is headless with validation OFF.
# The binary also supports --present mode for interactive windowed
# rendering with an orbiting light (see main.cpp / samples/README.md).
add_test(NAME sample_05_multipass_headless COMMAND sample_05_multipass --validate)
