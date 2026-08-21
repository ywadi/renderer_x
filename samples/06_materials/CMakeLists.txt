# rx_shader_deploy_runtime_libs() is defined in src/rx_shader/CMakeLists.txt
# (add_subdirectory'd before src/rx_material and this directory from the
# root CMakeLists.txt) -- same cross-directory function-name visibility
# every other sample's own CMakeLists.txt already relies on.

add_executable(sample_06_materials main.cpp)
# rx_frame_loop [Phase 5 Task 5, ticket #41]: shared present-loop helper.
target_link_libraries(sample_06_materials PRIVATE rx_graph rx_rhi_vk rx_platform rx_material rx_frame_loop)

# This sample links rx_material, which links slang::slang and does real
# in-process Slang compilation at startup (loadMaterial() for both
# checker.slang/rim.slang, plus the shared material.slang/forward_entry.slang
# every material composes against) -- needs the Slang runtime libs deployed
# next to it, same as 02_hotreload/03_bindless_mesh/04_streaming/05_multipass.
rx_shader_deploy_runtime_libs(sample_06_materials)

# Deploys this sample's OWN materials -- checker.slang/rim.slang -- into a
# materials/ subdirectory next to the binary, mirroring this source
# directory's own layout, so main.cpp's SDL_GetBasePath()-relative
# loadMaterial() calls find them identically in the build tree and in a
# redistributed copy of this directory.
#
# [Fix round, item 3] OUTPUT/DEPENDS-tracked, NOT a bare
# `add_custom_command(TARGET ... POST_BUILD)` -- see samples/08_gltf_viewer/
# CMakeLists.txt's own material_shaders deploy comment for the full
# mechanism this avoids (a POST_BUILD recipe only re-runs when the TARGET
# itself relinks, leaving an edited .slang source's deployed copy stale).
set(RX_MATERIALS_OWN_SHADERS
  "${CMAKE_CURRENT_SOURCE_DIR}/materials/checker.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/materials/rim.slang"
)
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/materials_own_deploy.stamp"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:sample_06_materials>/materials"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RX_MATERIALS_OWN_SHADERS}
          "$<TARGET_FILE_DIR:sample_06_materials>/materials/"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/materials_own_deploy.stamp"
  DEPENDS ${RX_MATERIALS_OWN_SHADERS}
  COMMENT "sample_06_materials: deploying materials/*.slang next to the binary"
  VERBATIM
)
add_custom_target(sample_06_materials_own_deploy
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/materials_own_deploy.stamp")
add_dependencies(sample_06_materials sample_06_materials_own_deploy)

# Deploys the SHARED rx_material shaders (shaders/material/material.slang +
# forward_entry.slang) into a SIBLING material_shaders/ directory next to
# the binary [Task 8 ledger item carried from Task 5]: rx_material's own
# MaterialSystem::create() previously located these two files exclusively
# via RX_MATERIAL_SHADER_DIR, a compile-time absolute path anchored at this
# checked-out repository's own CMAKE_SOURCE_DIR -- correct for a build-tree
# run but meaningless for a redistributed binary, which has no
# CMAKE_SOURCE_DIR at all. main.cpp resolves this material_shaders/
# directory at runtime (SDL_GetBasePath()-relative, same mechanism as its
# own materials/ directory above) and passes it as
# MaterialSystem::create()'s new `sharedShaderDir` parameter
# (src/rx_material/include/rx_material/material_system.h) -- RX_MATERIAL_
# SHADER_DIR stays the default for every OTHER caller (rx_material's own
# unit/GPU tests), unchanged.
# [Fix round, item 3] OUTPUT/DEPENDS-tracked, NOT a bare
# `add_custom_command(TARGET ... POST_BUILD)` -- see samples/08_gltf_viewer/
# CMakeLists.txt's own material_shaders deploy comment for the full
# mechanism this avoids (a POST_BUILD recipe only re-runs when the TARGET
# itself relinks, leaving an edited .slang source's deployed copy stale).
set(RX_MATERIALS_SHARED_SHADERS
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/material.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/forward_entry.slang"
)
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/materials_shared_shader_deploy.stamp"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:sample_06_materials>/material_shaders"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RX_MATERIALS_SHARED_SHADERS}
          "$<TARGET_FILE_DIR:sample_06_materials>/material_shaders/"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/materials_shared_shader_deploy.stamp"
  DEPENDS ${RX_MATERIALS_SHARED_SHADERS}
  COMMENT "sample_06_materials: deploying the shared rx_material shaders next to the binary"
  VERBATIM
)
add_custom_target(sample_06_materials_shared_shader_deploy
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/materials_shared_shader_deploy.stamp")
add_dependencies(sample_06_materials sample_06_materials_shared_shader_deploy)

# Runs the binary in headless mode with --validate: ctest is the developer/CI
# gate, so it explicitly opts into the Vulkan validation layers on top of the
# default end-user behavior (headless with validation OFF). The binary also
# supports --present mode for interactive windowed rendering with an
# orbiting camera and material hot-reload polling (see main.cpp /
# samples/README.md).
add_test(NAME sample_06_materials_headless COMMAND sample_06_materials --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(sample_06_materials_headless PROPERTIES TIMEOUT 300)
