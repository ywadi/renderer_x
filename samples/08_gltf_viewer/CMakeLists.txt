# rx_shader_deploy_runtime_libs() is defined in src/rx_shader/CMakeLists.txt
# (add_subdirectory'd before this directory) -- same cross-directory
# function-name visibility every other sample's own CMakeLists.txt relies on.

add_executable(sample_08_gltf_viewer main.cpp)
target_link_libraries(sample_08_gltf_viewer PRIVATE
    rx_graph rx_rhi_vk rx_platform rx_material rx_asset rx_task rx_shader sample_common
)

# RX_REPO_ROOT_DIR: the same "bake an absolute CMAKE_SOURCE_DIR-relative
# path at compile time" idiom rx_material's own RX_MATERIAL_SHADER_DIR and
# rx_asset's own RX_ASSET_ROOT_DIR (test-only) already establish -- used by
# resolveDefaultScenePath()'s dev-tree fallback (assets/fetched/
# DamagedHelmet, tools/fetch_assets.sh's own output) when this binary is
# NOT running from a packaged (pre-staged assets/) redistribution.
target_compile_definitions(sample_08_gltf_viewer PRIVATE
  RX_REPO_ROOT_DIR="${CMAKE_SOURCE_DIR}"
)

# This sample links rx_material (real in-process Slang compilation for
# standard_pbr.slang/unlit.slang/material.slang/forward_entry.slang, plus
# the shared multipass tonemap shaders compiled directly via rx_shader::
# Compiler -- see main.cpp's own buildTonemapPipeline()) -- needs the Slang
# runtime libs deployed next to it, same as every other Slang-compiling
# sample.
rx_shader_deploy_runtime_libs(sample_08_gltf_viewer)

# Deploys the SHARED rx_material shaders this sample's own materials are --
# material.slang/forward_entry.slang (D6's link-time-specialization
# machinery) + standard_pbr.slang/unlit.slang (D22's shipped material
# library, ordinary IMaterialShader modules, zero special treatment) -- into
# a material_shaders/ directory next to the binary, mirroring samples/
# 06_materials/CMakeLists.txt's own identical deployment for the identical
# reason (MaterialSystem::create()'s `sharedShaderDir` parameter, resolved
# at runtime via SDL_GetBasePath() in main.cpp's basePathDirectory()).
add_custom_command(TARGET sample_08_gltf_viewer POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:sample_08_gltf_viewer>/material_shaders"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/material.slang"
          "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/forward_entry.slang"
          "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/standard_pbr.slang"
          "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material/unlit.slang"
          "$<TARGET_FILE_DIR:sample_08_gltf_viewer>/material_shaders/"
  COMMENT "sample_08_gltf_viewer: deploying the shared rx_material shaders next to the binary"
  VERBATIM
)

# The forward pass's own tonemap half reuses shaders/multipass/tonemap.{vert,
# frag}.slang VERBATIM (this task's own binding constraint: "you may NOT
# touch shaders/multipass/" -- --exposure is applied inside the material
# pass instead, see forward_entry.slang's own fragmentMain) -- deployed next
# to the binary exactly like samples/05_multipass's own two files (flat, no
# "multipass/" subdirectory in the deployed layout).
set(RX_GLTF_VIEWER_TONEMAP_SHADERS
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/multipass/tonemap.vert.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/multipass/tonemap.frag.slang"
)
add_custom_command(TARGET sample_08_gltf_viewer POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RX_GLTF_VIEWER_TONEMAP_SHADERS}
          "$<TARGET_FILE_DIR:sample_08_gltf_viewer>/"
  COMMENT "sample_08_gltf_viewer: deploying the shared tonemap shaders next to the binary"
  VERBATIM
)

# D17 committed reference PNGs (256x256, lavapipe-rendered) -- deployed next
# to the binary so main.cpp's basePathDirectory()-relative resolution finds
# them identically in the build tree and any redistributed copy. Regenerated
# only via tools/regen_references.sh (documented, never auto-run -- see that
# script's own header comment).
#
# OUTPUT/DEPENDS-tracked (a real custom_command + custom_target), NOT a bare
# `add_custom_command(TARGET ... POST_BUILD)` like the shader-deploy steps
# above: a POST_BUILD recipe only re-runs when the TARGET ITSELF is
# relinked, so editing just these two PNGs (exactly what
# tools/regen_references.sh does) would leave an INCREMENTAL build's already-
# built binary silently reading a stale deployed copy until something else
# (e.g. touching main.cpp) forces a relink -- reproduced directly during
# this task's own development (an md5sum diff between the freshly
# regenerated source PNG and the still-stale deployed one, after an
# otherwise-successful incremental rebuild). Declaring the two source PNGs
# as real DEPENDS makes ninja/make re-run the copy on their own mtime alone,
# exactly like every other tracked build input.
set(RX_GLTF_VIEWER_REFERENCE_PNGS
  "${CMAKE_CURRENT_SOURCE_DIR}/references/loading_state.png"
  "${CMAKE_CURRENT_SOURCE_DIR}/references/loaded_scene.png"
)
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/references.stamp"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:sample_08_gltf_viewer>/references"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RX_GLTF_VIEWER_REFERENCE_PNGS}
          "$<TARGET_FILE_DIR:sample_08_gltf_viewer>/references/"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/references.stamp"
  DEPENDS ${RX_GLTF_VIEWER_REFERENCE_PNGS}
  COMMENT "sample_08_gltf_viewer: deploying committed D17 reference PNGs next to the binary"
  VERBATIM
)
add_custom_target(sample_08_gltf_viewer_references DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/references.stamp")
add_dependencies(sample_08_gltf_viewer sample_08_gltf_viewer_references)

# Headless correctness gate: imports the default scene (DamagedHelmet,
# fetched by tools/fetch_assets.sh -- see .github/workflows/ci.yml's own
# "Fetch test assets" step) ASYNCHRONOUSLY (Task 15's own pipeline, the
# default and only headless path -- this sample's whole point is to be the
# async-import demonstration vehicle), captures a loading-state frame and a
# loaded-scene frame, and gates each against its own committed D17 reference
# PNG (tolerance-based, +-4/255 per channel, <0.5% failing-pixel budget).
add_test(NAME sample_08_gltf_viewer_headless COMMAND sample_08_gltf_viewer --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(sample_08_gltf_viewer_headless PROPERTIES TIMEOUT 300)

# Quit-during-load: starts the async import, cancels it after >=1 real GPU
# resource (a texture or geometry upload) is already live, then tears down
# -- the standing lesson this stage's own review history keeps finding
# rollback bugs in (abandon/teardown paths get real-GPU-resource tests, not
# mocks). Asserts zero unfiltered validation errors through a REAL
# cancel-mid-import-then-destroy sequence, not a synthetic stand-in.
add_test(NAME sample_08_gltf_viewer_quit_during_load COMMAND sample_08_gltf_viewer --validate --quit-during-load)
set_tests_properties(sample_08_gltf_viewer_quit_during_load PROPERTIES TIMEOUT 300)
