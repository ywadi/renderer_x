# tests/conformance -- Phase 5 Stage 1 Task 11 (#47), gate ruling T11: the
# glTF PBR conformance harness (rx_conformance_render). A NEW top-level
# test-tree location (this ticket's own file-list literally names a bare
# `tests/` directory, unlike every existing per-module `src/*/tests`
# convention -- matrix-p5t11-conformance-harness.md's own "New gaps"
# section flags this; adopted here as the genuinely new, cross-cutting
# convention it called for: conformance spans asset/material/graph/ibl,
# it does not belong to any one module's own tests/ tree).
#
# UNLIKE samples/08_gltf_viewer, this tool does NOT deploy shaders/
# environment/reference assets next to its own binary via custom_command
# stamp targets -- it reads them directly from the checked-out SOURCE tree
# via the RX_REPO_ROOT_DIR compile definition (main.cpp's own
# resolveXxxPath()-style logic), exactly like damaged_helmet_test.cpp's
# own RX_ASSET_ROOT_DIR convention. This is a deliberate simplification
# available BECAUSE this is a ctest-only gate tool, never a redistributed
# sample (tools/package_samples.sh has no reason to know about it) -- see
# main.cpp's own top comment.

add_executable(rx_conformance_render
    main.cpp
    camera_fit.cpp
)
target_link_libraries(rx_conformance_render PRIVATE
    rx_graph rx_rhi_vk rx_platform rx_material rx_asset rx_scene rx_task rx_shader rx_ibl sample_common
)
target_compile_definitions(rx_conformance_render PRIVATE
    RX_REPO_ROOT_DIR="${CMAKE_SOURCE_DIR}"
)
# rx::shader::Compiler (this tool compiles standard_pbr.slang/unlit.slang/
# the conformance tonemap pair/the rx_ibl bake kernels in-process, exactly
# like every other Slang-compiling sample) needs the Slang runtime shared
# libraries deployed next to the binary -- same call every other such
# sample/tool already makes.
rx_shader_deploy_runtime_libs(rx_conformance_render)

# --- ctest registration ---------------------------------------------------
# One gate per MANDATORY model (tests/conformance/models.h's own registry)
# -- each skips gracefully (exit 0, logged) if its own asset/reference is
# absent locally, matching damaged_helmet_test.cpp's own established
# convention, though in practice tools/fetch_assets.sh fetches every
# mandatory model's asset by default (CI included) and this ticket commits
# every mandatory model's own reference.png/provenance.json, so these
# never actually skip in CI.
#
# BOTH DRIVERS, UNCONDITIONALLY ENFORCED (ticket's own text: "gate our
# renderer against them with tolerance comparisons on both drivers") --
# UNLIKE D17's sample-08/09 gate (lavapipe-only hard-enforced, real-GPU
# informational): a cross-renderer conformance tolerance is, by
# construction, wide enough to absorb ordinary driver-to-driver
# rasterization noise on TOP of the cross-renderer noise it already has to
# absorb (models.h's own tolerance-derivation comment) -- there is no
# principled reason to let a real GPU run silently diverge further than
# lavapipe without failing.
set(RX_CONFORMANCE_MODELS
    MetalRoughSpheres
    MetalRoughSpheresNoTextures
    EmissiveStrengthTest
    CompareEmissiveStrength
    TextureTransformTest
    AlphaBlendModeTest
)
foreach(_rx_conformance_model ${RX_CONFORMANCE_MODELS})
    set(_rx_conformance_test_name "rx_conformance_${_rx_conformance_model}")
    add_test(NAME ${_rx_conformance_test_name}
             COMMAND rx_conformance_render --model ${_rx_conformance_model} --validate)
    set_tests_properties(${_rx_conformance_test_name} PROPERTIES TIMEOUT 300)
endforeach()

# EnvironmentTest -- OPTIONAL (fetch-on-demand only, proprietary Adobe
# Stock license on the model content -- tools/fetch_assets.sh's own
# `--environment-test` section); CI never fetches it, so this test SKIPS
# (exit 0) there. Local/manual verification only.
add_test(NAME rx_conformance_EnvironmentTest
         COMMAND rx_conformance_render --model EnvironmentTest --validate)
set_tests_properties(rx_conformance_EnvironmentTest PROPERTIES TIMEOUT 300)

# --- Discrimination proof [ticket's own acceptance line: "per-model
# discrimination proof (perturb roughness constant -> gate fails)"] ------
# A deliberately WRONG render (every StandardPBR material's own
# roughnessFactor OVERRIDDEN to a fixed 0.02 -- see main.cpp's own
# overrideFloatMaterialParamIfPresent()/kPerturbedRoughnessValue for why
# an absolute override, not a relative scale/offset, is what this tool's
# own --perturb-roughness flag applies) MUST fail the SAME gate the real
# MetalRoughSpheres run above passes -- CTest's own WILL_FAIL property
# inverts the pass/fail verdict this test itself expects (a real, standard
# CTest mechanism for exactly this "this run must fail" shape, not a
# hand-rolled shell inversion).
add_test(NAME rx_conformance_MetalRoughSpheres_discrimination_proof
         COMMAND rx_conformance_render --model MetalRoughSpheres --perturb-roughness --validate)
set_tests_properties(rx_conformance_MetalRoughSpheres_discrimination_proof PROPERTIES
    TIMEOUT 300
    WILL_FAIL TRUE
)
