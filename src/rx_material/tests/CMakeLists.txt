# Every test_material_system.cpp case builds a real VkDevice-backed
# MaterialSystem and ends up calling vkCreateGraphicsPipelines at least
# once (getPipeline()) -- there is no device-free half of THAT library's
# tests worth a separate plain-doctest_main target the way rx_graph's
# Tasks 1-2 compile()/barrier logic had. So, unlike rx_graph's
# rx_graph_tests/rx_graph_gpu_tests split, this remained the ONE test
# binary for rx_material through Task 5, named rx_material_gpu_tests to
# both match rx_graph_gpu_tests' own naming convention and give CI's
# windows-cross-zig job (no real Vulkan device under Wine --
# .github/workflows/ci.yml) an unambiguous substring to exclude on,
# exactly like it already does for rx_rhi_vk*/rx_graph_gpu*.
#
# doctest_main.cpp here is the SAME vk-bootstrap-instance-function-cache
# warm-up pattern src/rx_rhi_vk/tests/doctest_main.cpp and
# src/rx_graph/tests/doctest_main_gpu.cpp both already establish (copied,
# not shared, matching those two files' own precedent -- see either one's
# header comment for the full defect this avoids: the pinned vk-bootstrap
# commit caches instance-level Vulkan function pointers process-wide from
# whichever vkb::Instance is built FIRST, forever).
add_executable(rx_material_gpu_tests
    doctest_main.cpp
    test_material_system.cpp
    # Task 6: rxCreateMaterialSystem + IRxMaterialSystem::loadMaterial's
    # happy path against a real VkDevice-backed internal MaterialSystem
    # (test_unlit.slang/test_textured.slang) -- the ABI's device-free
    # contract lives in rx_material_tests below; this is the ONE case
    # this task's tests need a real device for.
    test_api_factory.cpp
    # Task 7 fix round 1 (F2): standalone ParamArena tests against a bare
    # VkDevice + rx::rhi::Allocator -- no MaterialSystem/BindlessTable/
    # RenderGraph needed, matching rx_rhi_vk/tests/descriptor_arena_test.cpp's
    # own minimal headless fixture pattern.
    test_param_arena.cpp
    # Phase 4 Stage 1 Task 16 [D22, D26.1, D28, gate ruling #8/RC1]:
    # standard_pbr.slang/unlit.slang GPU tests (real vkCmdDrawIndexed +
    # pixel readback -- the first tests in this binary to actually draw
    # and read back, not just prove a bind/pipeline-build is
    # validation-clean), the D28 pipeline-cache-counter test, and the
    # D26.1 two-draw SV_VulkanInstanceID test.
    test_standard_pbr_unlit.cpp
    # Phase 4 Stage 2 Task 22 fix round [F1, spec D21]: the comparison-
    # sampler 3x3 PCF mechanism (material.slang's own gShadowCompareSamplers/
    # rx_sampleShadowPCF, forward_entry.slang's fragmentMain re-read/
    # shadowFactor wiring) exercised through the REAL production lit path
    # (MaterialSystem::getPipeline() against standard_pbr.slang), not a
    # standalone probe rig -- links rx_shadow for its
    # ShadowCasterPipeline/fitShadowFrustum (device-free math + the real
    # shadow-caster pipeline building the shadow map this test's receiver
    # draws sample).
    test_standard_pbr_shadow_gpu.cpp
)
# [Phase 5 Task 4/#40] rx_scene: test_standard_pbr_unlit.cpp's own
# pre-exposure TEST_CASE constructs a real rx::scene::Camera and reads
# camera.exposure()/rx::scene::exposure::exposure(ev100) to compute the
# expected pre-multiplied lightColor it asserts against -- the ONE case in
# this binary that needs Camera's exposure math linked in.
target_link_libraries(rx_material_gpu_tests PRIVATE rx_material rx_shadow rx_scene rx_rhi_vk rx_platform doctest::doctest)
rx_shader_deploy_runtime_libs(rx_material_gpu_tests)

target_compile_definitions(rx_material_gpu_tests PRIVATE
  RX_MATERIAL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  # [Phase 4 Stage 1 Task 16] test_standard_pbr_unlit.cpp loads the REAL
  # shipped shaders/material/{standard_pbr,unlit}.slang (not this
  # directory's own data/ test fixtures) -- rx_material's own
  # RX_MATERIAL_SHADER_DIR (src/rx_material/CMakeLists.txt) is PRIVATE to
  # that library target and does not propagate to this executable, so it
  # is redeclared here, verbatim, for the one test file that needs it.
  RX_MATERIAL_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders/material"
)

# --validate: same developer/CI-gate opt-in every other GPU-backed test
# binary's own ctest registration already documents (rx_graph/tests/
# CMakeLists.txt, rx_rhi_vk/tests -- via each TEST_CASE's own
# CHECK_FALSE(context.hasValidationErrors())); this binary's own fixtures
# build every rx::rhi::Context with enableValidation=true unconditionally.
add_test(NAME rx_material_gpu_tests COMMAND rx_material_gpu_tests --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(rx_material_gpu_tests PROPERTIES TIMEOUT 300)

# Task 6 [spec Phase 3 design D5]: rx_api.h's COM-lite ABI contract is the
# first genuinely device-free surface this library has (queryInterface
# identity, refcount round-trips, error codes on bad input, header self-
# containment) -- none of it touches a VkDevice, so this is a plain
# DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN binary, matching rx_graph_tests' own
# doctest_main.cpp precedent (src/rx_graph/tests/CMakeLists.txt) rather
# than rx_material_gpu_tests' vk-bootstrap warm-up main above. Still
# links (transitively) slang::slang through rx_material -- see
# CMakeLists.txt one level up for why -- so it still needs
# rx_shader_deploy_runtime_libs() even though none of its own test cases
# call into Slang (a device-free IRxMaterialSystem's loadMaterial() fails
# fast on its null internal pointer before ever touching Slang -- see
# api_impl.cpp's own loadMaterial()).
add_executable(rx_material_tests
    doctest_main_plain.cpp
    test_api_contract.cpp
    test_api_header_self_contained.cpp
)
target_link_libraries(rx_material_tests PRIVATE rx_material doctest::doctest)
rx_shader_deploy_runtime_libs(rx_material_tests)
add_test(NAME rx_material_tests COMMAND rx_material_tests)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(rx_material_tests PROPERTIES TIMEOUT 120)

# --- Phase 5 Stage 1 Task 7 [#43]: shaders/material/brdf.slang's own
# compute-based numerical test harness -- a NEW, sibling binary rather than
# folding into rx_material_gpu_tests above: these tests drive slang::
# ISession directly (brdf_test_harness.h, mirroring material_system.cpp's
# own createMaterialSession()/compileMaterial(), NOT MaterialSystem/
# ParameterBlock machinery) and need rx_rhi_vk::ComputePipelineCache
# (Task 2), neither of which any existing file in this directory touches.
# doctest_main.cpp is reused verbatim (the same vk-bootstrap warm-up
# pattern, added as a SOURCE to a second executable target -- harmless,
# matches every other doctest_main.cpp/doctest_main_gpu.cpp precedent in
# this codebase of one file compiled into exactly the binary that needs it).
find_program(RX_SPIRV_DIS_EXECUTABLE NAMES spirv-dis)
if(NOT RX_SPIRV_DIS_EXECUTABLE)
  message(FATAL_ERROR
    "[rx_material_brdf_gpu_tests] 'spirv-dis' not found on PATH -- required "
    "by test_brdf_spirv_link_composition_gpu.cpp's SPIR-V-absence proof "
    "(gate ruling T7). Install the spirv-tools package (apt: spirv-tools; "
    "the CI images install it alongside vulkan-validationlayers -- see "
    ".github/workflows/ci.yml).")
endif()

add_executable(rx_material_brdf_gpu_tests
    doctest_main.cpp
    test_brdf_module_gpu.cpp
    test_brdf_white_furnace_gpu.cpp
    test_brdf_spirv_link_composition_gpu.cpp
)
target_link_libraries(rx_material_brdf_gpu_tests PRIVATE rx_shader rx_rhi_vk rx_platform doctest::doctest)
rx_shader_deploy_runtime_libs(rx_material_brdf_gpu_tests)
target_compile_definitions(rx_material_brdf_gpu_tests PRIVATE
  # Same fixed, checked-out-repo-relative shaders/material/ path
  # rx_material's own RX_MATERIAL_SHADER_DIR resolves to (that macro is
  # PRIVATE to the rx_material library target and does not propagate here)
  # -- brdf.slang lives in the SAME directory as material.slang/
  # standard_pbr.slang/forward_entry.slang, per the ticket's own file list.
  RX_MATERIAL_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders/material"
  RX_SPIRV_DIS="${RX_SPIRV_DIS_EXECUTABLE}"
)
add_test(NAME rx_material_brdf_gpu_tests COMMAND rx_material_brdf_gpu_tests --validate)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(rx_material_brdf_gpu_tests PROPERTIES TIMEOUT 300)
