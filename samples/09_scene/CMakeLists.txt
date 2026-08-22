# rx_shader_deploy_runtime_libs() is defined in src/rx_shader/CMakeLists.txt
# (add_subdirectory'd before this directory) -- same cross-directory
# function-name visibility every other sample's own CMakeLists.txt relies on.

# [Phase 5 Task 5, ticket #41] draw_recording.{h,cpp}/fly_camera.h/
# mouse_capture.h/window_resize.h are GONE -- promoted engine-side (rows
# 1-2, 3, 4, 6, 7 of the gate matrix); rx_frame_loop is the new shared
# present-loop helper. grid_layout.h alone stays sample-local (row 5
# ruling).
add_executable(sample_09_scene main.cpp)
# rx_ibl [Phase 5 Task 10, #46 fix round, task-10-review.md Finding 3/LOW]:
# rx::ibl::bakeEnvironment(), consumed by this sample's own default-
# environment bind (setupEnvironment() in main.cpp).
target_link_libraries(sample_09_scene PRIVATE
    rx_graph rx_rhi_vk rx_platform rx_material rx_asset rx_scene rx_shadow rx_debug_ui rx_task rx_shader rx_frame_loop rx_ibl sample_common
)

# RX_REPO_ROOT_DIR: same "bake an absolute CMAKE_SOURCE_DIR-relative path at
# compile time" idiom samples/08_gltf_viewer's own CMakeLists.txt already
# establishes -- used by resolveHelmetScenePath()/resolveSponzaScenePath()'s
# dev-tree fallback (assets/fetched/{DamagedHelmet,Sponza},
# tools/fetch_assets.sh's own output) when this binary is NOT running from a
# packaged (pre-staged assets/) redistribution.
target_compile_definitions(sample_09_scene PRIVATE
  RX_REPO_ROOT_DIR="${CMAKE_SOURCE_DIR}"
)

# Real in-process Slang compilation (material.slang/forward_entry.slang/
# standard_pbr.slang/unlit.slang, the shared multipass tonemap shaders, and
# rx_shadow's own shadow_caster.vert.slang) -- needs the Slang runtime libs
# deployed next to it, same as every other Slang-compiling sample.
rx_shader_deploy_runtime_libs(sample_09_scene)

# Deploys the SHARED rx_material shaders this sample's materials are --
# material.slang/forward_entry.slang (D6's link-time-specialization
# machinery) + standard_pbr.slang (DamagedHelmet) + unlit.slang (the
# --stress workload's own 4 material variants) -- into a material_shaders/
# directory next to the binary, mirroring samples/08_gltf_viewer's own
# identical deployment. OUTPUT/DEPENDS-tracked (a real custom_command +
# custom_target), NOT a bare POST_BUILD recipe -- see that file's own
# comment for the incremental-rebuild-staleness hazard this avoids.
set(RX_SCENE_MATERIAL_SHADERS
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/material.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/forward_entry.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/standard_pbr.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/unlit.slang"
  # [Phase 5 Stage 1 Task 8, #44] See samples/08_gltf_viewer/CMakeLists.txt's
  # identical addition's own comment -- standard_pbr.slang now `import
  # brdf;`s Task 7's BRDF module and links against one of these two
  # companion variant-selector files.
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/brdf.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/energy_compensation_off.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/energy_compensation_on.slang"
  # [Phase 5 Stage 2 Task 15, #51] standard_pbr.slang now `import
  # cluster_lighting;`s the clustered Forward+ shading-side lookup module.
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/cluster_lighting.slang"
)
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/material_shaders_deploy.stamp"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:sample_09_scene>/material_shaders"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RX_SCENE_MATERIAL_SHADERS}
          "$<TARGET_FILE_DIR:sample_09_scene>/material_shaders/"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/material_shaders_deploy.stamp"
  DEPENDS ${RX_SCENE_MATERIAL_SHADERS}
  COMMENT "sample_09_scene: deploying the shared rx_material shaders next to the binary"
  VERBATIM
)
add_custom_target(sample_09_scene_material_shaders
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/material_shaders_deploy.stamp")
add_dependencies(sample_09_scene sample_09_scene_material_shaders)

# rx_shadow's own shadow-caster shader -- deployed into a shadow_shaders/
# directory next to the binary (ShadowCasterPipeline::create()'s own
# `shaderDir` parameter, resolved at runtime via SDL_GetBasePath() in
# main.cpp's basePathDirectory()) -- this sample is rx_shadow's first real
# runtime-deployed consumer (Task 22's own GPU tests link it straight out
# of the build tree via its compile-time RX_SHADOW_SHADER_DIR default).
set(RX_SCENE_SHADOW_SHADERS
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/shadow/shadow_caster.vert.slang"
)
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shadow_shaders_deploy.stamp"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:sample_09_scene>/shadow_shaders"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RX_SCENE_SHADOW_SHADERS}
          "$<TARGET_FILE_DIR:sample_09_scene>/shadow_shaders/"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/shadow_shaders_deploy.stamp"
  DEPENDS ${RX_SCENE_SHADOW_SHADERS}
  COMMENT "sample_09_scene: deploying rx_shadow's shadow_caster.vert.slang next to the binary"
  VERBATIM
)
add_custom_target(sample_09_scene_shadow_shaders
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/shadow_shaders_deploy.stamp")
add_dependencies(sample_09_scene sample_09_scene_shadow_shaders)

# The forward pass's own tonemap half reuses shaders/multipass/tonemap.{vert,
# frag}.slang VERBATIM, deployed flat next to the binary -- same convention
# samples/05_multipass/07_stress/08_gltf_viewer's own copies already
# establish (each sample deploys its own copy, this project's own
# per-sample-independence convention).
set(RX_SCENE_TONEMAP_SHADERS
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/multipass/tonemap.vert.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/multipass/tonemap.frag.slang"
)
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/tonemap_shaders_deploy.stamp"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RX_SCENE_TONEMAP_SHADERS}
          "$<TARGET_FILE_DIR:sample_09_scene>/"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/tonemap_shaders_deploy.stamp"
  DEPENDS ${RX_SCENE_TONEMAP_SHADERS}
  COMMENT "sample_09_scene: deploying the shared tonemap shaders next to the binary"
  VERBATIM
)
add_custom_target(sample_09_scene_tonemap_shaders
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/tonemap_shaders_deploy.stamp")
add_dependencies(sample_09_scene sample_09_scene_tonemap_shaders)

# [Phase 5 Task 10, #46 fix round, task-10-review.md Finding 3/LOW] rx_ibl's
# own bake-chain compute kernels (rx::ibl::bakeEnvironment()'s own
# `shaderDir` parameter) -- deployed into an `ibl_shaders/` directory next
# to the binary, mirroring samples/08_gltf_viewer's own identical deploy
# block EXACTLY, minus shaders/ibl/skybox.slang (this sample builds no
# skybox pass -- out of this fix round's own scope).
set(RX_SCENE_IBL_SHADERS
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/ibl/equirect_to_cubemap.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/ibl/irradiance_convolve.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/ibl/prefilter_specular.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/ibl/dfg_lut.slang"
)
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ibl_shaders_deploy.stamp"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:sample_09_scene>/ibl_shaders"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RX_SCENE_IBL_SHADERS}
          "$<TARGET_FILE_DIR:sample_09_scene>/ibl_shaders/"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/ibl_shaders_deploy.stamp"
  DEPENDS ${RX_SCENE_IBL_SHADERS}
  COMMENT "sample_09_scene: deploying rx_ibl's bake-chain shaders next to the binary"
  VERBATIM
)
add_custom_target(sample_09_scene_ibl_shaders
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/ibl_shaders_deploy.stamp")
add_dependencies(sample_09_scene sample_09_scene_ibl_shaders)

# [Phase 5 Task 10, #46 fix round, task-10-review.md Finding 3/LOW] Reuses
# samples/08_gltf_viewer's own committed procedural .hdr fixture VERBATIM
# (NO NEW ASSET) -- deployed a second time next to THIS binary, same
# pattern as samples/08_gltf_viewer/CMakeLists.txt's own
# `environments_deploy.stamp` block.
set(RX_SCENE_ENVIRONMENT_FIXTURES
  "${CMAKE_CURRENT_SOURCE_DIR}/../08_gltf_viewer/environments/gate_test_env.hdr"
)
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/environments_deploy.stamp"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:sample_09_scene>/environments"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RX_SCENE_ENVIRONMENT_FIXTURES}
          "$<TARGET_FILE_DIR:sample_09_scene>/environments/"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/environments_deploy.stamp"
  DEPENDS ${RX_SCENE_ENVIRONMENT_FIXTURES}
  COMMENT "sample_09_scene: deploying the shared (samples/08_gltf_viewer-owned) .hdr environment fixture next to the binary"
  VERBATIM
)
add_custom_target(sample_09_scene_environments
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/environments_deploy.stamp")
add_dependencies(sample_09_scene sample_09_scene_environments)

# D17 committed reference PNG (256x256, lavapipe-rendered) for the default
# headless DamagedHelmet-grid composition (no HUD -- see main.cpp's own
# header comment for why the D17-gated capture and the HUD-smoke-test
# capture are deliberately two different frames) -- deployed next to the
# binary, regenerated only via tools/regen_references.sh (extended by this
# task to cover this sample too -- no second regeneration mechanism, per
# the gate's own binding instruction). OUTPUT/DEPENDS-tracked -- see
# samples/08_gltf_viewer/CMakeLists.txt's own references.stamp block for
# the full incremental-rebuild-staleness rationale this mirrors exactly.
set(RX_SCENE_REFERENCE_PNGS
  "${CMAKE_CURRENT_SOURCE_DIR}/references/grid_scene.png"
)
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/references.stamp"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:sample_09_scene>/references"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RX_SCENE_REFERENCE_PNGS}
          "$<TARGET_FILE_DIR:sample_09_scene>/references/"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/references.stamp"
  DEPENDS ${RX_SCENE_REFERENCE_PNGS}
  COMMENT "sample_09_scene: deploying the committed D17 reference PNG next to the binary"
  VERBATIM
)
add_custom_target(sample_09_scene_references DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/references.stamp")
add_dependencies(sample_09_scene sample_09_scene_references)

# --- Headless correctness gate [gate ruling #15, matrix row 24]: the
# default (DamagedHelmet-grid) composition, fixed startup camera -- EXACT
# counter assertions (imported/visible/culled/recordsIn/drawsSubmitted +
# the blessed collapse-ratio formula) plus the D17 tolerance-pixel gate. --
add_test(NAME sample_09_scene_headless COMMAND sample_09_scene --validate)
set_tests_properties(sample_09_scene_headless PROPERTIES TIMEOUT 300)

# --- Stress-v2 headless gate [gate ruling #15, matrix row 23]: a SMALL,
# deterministic instance count through the FULL scene path -- exact counters
# (drawsSubmitted, culled, chunk count == the derived worker-count value,
# the collapse ratio) for a fixed 3-frame run. Deliberately small (NOT the
# interactive 30000-instance default) for the same reason
# sample_07_stress_headless keeps its own gate small -- see that sample's
# own CMakeLists.txt comment: chunk-count derivation is workerCount-only,
# never instance-count-dependent, so a small gate exercises the identical
# parallel-recording machinery a real 30000-instance run does. ------------
add_test(NAME sample_09_scene_stress_headless COMMAND sample_09_scene --validate --stress --stress-draws 64)
set_tests_properties(sample_09_scene_stress_headless PROPERTIES TIMEOUT 300)

add_subdirectory(tests)
