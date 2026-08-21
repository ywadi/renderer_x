# rx_ibl_gpu_tests -- Phase 5 Stage 1 Task 9 [#45]. Every TEST_CASE needs a
# real windowed VkDevice (compute dispatch, real image readback) --
# matches rx_graph_gpu_tests' own binary shape/naming convention exactly
# (this codebase's established "one *_gpu_tests binary per module that has
# no device-free half worth a separate target" precedent -- see
# src/rx_material/tests/CMakeLists.txt's own comment for the same
# reasoning applied there).
add_executable(rx_ibl_gpu_tests
    doctest_main_gpu.cpp
    test_ibl_cube_face_convention_gpu.cpp
    test_ibl_analytic_gpu.cpp
    test_ibl_energy_compensation_gpu.cpp
    # [gate matrix-p5t09-ibl-bake-chain.md file list: "tests (+ tiny
    # committed HDR fixtures)"] real Task 6 decodeStbImageHdr() path ->
    # bake, using data/uniform_test_env.hdr.
    test_ibl_hdr_fixture_gpu.cpp
    # [review round, task-09-review.md Verdict 1] the plan/ticket's own
    # SECOND required analytic-ground-truth proof ("directional impulse
    # -> known lobe") -- see this file's own header comment for the
    # OR-mischaracterization this closes.
    test_ibl_directional_impulse_gpu.cpp
)
target_link_libraries(rx_ibl_gpu_tests PRIVATE rx_ibl rx_rhi_vk rx_platform rx_shader rx_asset doctest::doctest)
rx_shader_deploy_runtime_libs(rx_ibl_gpu_tests)

target_compile_definitions(rx_ibl_gpu_tests PRIVATE
  RX_IBL_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders/ibl"
  RX_MATERIAL_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders/material"
  RX_IBL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

# --validate: same developer/CI-gate opt-in every other GPU-backed test
# binary's own ctest registration documents -- this binary's own fixture
# builds every rx::rhi::Context with enableValidation=true unconditionally;
# each TEST_CASE's own CHECK_FALSE(context.hasValidationErrors()) is what
# turns a validation message into a real ctest failure.
add_test(NAME rx_ibl_gpu_tests COMMAND rx_ibl_gpu_tests --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(rx_ibl_gpu_tests PROPERTIES TIMEOUT 300)
