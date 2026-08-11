# rx_shader_deploy_runtime_libs() is defined in src/rx_shader/CMakeLists.txt
# (add_subdirectory'd from the root CMakeLists.txt before this directory) --
# see that file's own comment for the directory-scope fix (Task 6) that
# makes it safe to call unqualified from here, no local re-glob needed.

add_executable(sample_07_stress main.cpp)
target_link_libraries(sample_07_stress PRIVATE rx_graph rx_rhi_vk rx_platform rx_shader)

# This sample does real in-process Slang compilation at startup (both
# modes -- see main.cpp's createScene()/buildForwardPipelines()/
# buildTonemapPipeline()), same as 02_hotreload/03_bindless_mesh/
# 04_streaming/05_multipass -- needs the Slang runtime libs deployed next
# to it.
rx_shader_deploy_runtime_libs(sample_07_stress)

# Deploys every one of this pass's four on-disk shader sources next to the
# binary at build time, flat (no "stress/" subdirectory in the deployed
# layout -- same flat-next-to-the-binary convention 02_hotreload's/
# 05_multipass's own shader deployment already established), so main.cpp's
# resolveAssetPath() (SDL_GetBasePath() + filename) finds them identically
# whether run from the build tree or a redistributed copy of just this
# directory's build output.
set(RX_STRESS_SHADERS
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/stress/instanced.vert.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/stress/instanced.frag.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/stress/tonemap.vert.slang"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/stress/tonemap.frag.slang"
)
add_custom_command(TARGET sample_07_stress POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RX_STRESS_SHADERS} "$<TARGET_FILE_DIR:sample_07_stress>/"
  COMMENT "sample_07_stress: deploying shaders/stress/*.slang next to the binary"
  VERBATIM
)

# Headless correctness gate: --draws 16 (a 4x4 grid), NOT the interactive
# --draws 30000 default [task-7-brief.md: "3 frames, then assert EXACT
# counters ... plus 4 analytic pixel probes"] -- deliberately small so the
# whole grid fits cleanly inside the fixed 256x256 headless framebuffer with
# every instance's own screen footprint individually resolvable (see
# main.cpp's own header comment on why a non-overlapping top-down grid is
# what makes the four analytic probes tractable at all). This does NOT
# reduce parallel-recording coverage: chunk count is derived purely from
# the scheduler's own worker count (rx::graph::detail::
# chunkCountForWorkerCount(), executor.h), never from drawCount, so a
# 16-draw run exercises the identical fan-out/pool/vkCmdExecuteCommands
# machinery a real 30000-draw run does. D18's own counter-only CI gating
# (this test) vs. wall-clock (published separately, never CI-blocking) is
# exactly why this is safe to keep small and deterministic.
add_test(NAME sample_07_stress_headless COMMAND sample_07_stress --validate --draws 16)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(sample_07_stress_headless PROPERTIES TIMEOUT 300)
