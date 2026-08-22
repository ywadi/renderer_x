#pragma once
// tests/conformance/models.h -- Phase 5 Stage 1 Task 11 (#47) conformance
// model registry. Mirrors tools/gltf_conformance/generate_reference.mjs's
// own MODELS table -- intentionally duplicated, not shared (two
// independent language runtimes; a JSON/text registry file neither side
// needed its own parser for was not worth adding as a THIRD source of
// truth just to avoid seven lines of duplication -- see task-11-report.md
// for the full "why not shared" note). Keep the two lists in sync by
// hand; this is a small, rarely-changed table (the ticket's own "≥6 at
// Stage 1 close, growing in Stages 2-4" — additions are infrequent,
// deliberate events, not a churny generated artifact).
//
// TOLERANCE [ticket's own "MetalRoughSpheres within ruled tolerance"
// text; this ticket's own binding rulings named NO specific number --
// task-11-report.md records the pilot measurement this value is derived
// from]: `perChannelToleranceOutOf255`/`failingPixelBudgetFraction` feed
// samples/common/reference_gate.h's own compareToReference() (D17's
// established tolerance-gate PRIMITIVE, reused algorithm -- see that
// header's own doc comment) with WIDER values than D17's same-renderer
// byte-identical-regression defaults (+-4/255, <0.5%): this gate compares
// TWO INDEPENDENTLY WRITTEN renderers (RendererX's own Slang/Vulkan PBR
// path vs. the Khronos Sample Viewer's own WebGL2/GLSL path), which is a
// materially different, structurally noisier comparison than one
// renderer's own commit-to-commit regression check -- see
// task-11-report.md's own "tolerance derivation" section for the actual
// measured MetalRoughSpheres divergence distribution this number is set
// above.
//
// EXTENSION SCOPE: only models whose glTF content stays entirely within
// material/texture features RendererX ALREADY consumes are gated here --
// see fetch_assets.sh's own AlphaBlendModeTest-not-TextureTransform
// MultiTest comment for a concrete example of a model excluded for this
// reason (KHR_materials_clearcoat, Stage 3).

#include <filesystem>
#include <string>
#include <vector>

namespace rx::conformance {

struct ConformanceModel {
    // Directory name under assets/fetched/<name>/glTF/<name>.gltf and
    // tests/conformance/references/<name>/{reference.png,provenance.json}.
    std::string name;
    // CI-fetched by default (tools/fetch_assets.sh, no flag needed) vs.
    // fetch-on-demand-only (currently only EnvironmentTest -- proprietary
    // Adobe Stock license on the model content, gate ruling T11; see
    // fetch_assets.sh's own --environment-test section). An optional
    // model's own ctest registration SKIPS (not fails) when its asset or
    // reference is absent, mirroring damaged_helmet_test.cpp's own
    // established "not fetched -> MESSAGE + early return" convention.
    bool optional = false;
    // Per-channel absolute tolerance (out of 255) and failing-pixel-count
    // budget fraction -- see this header's own top comment for the
    // derivation. Every MANDATORY model currently shares ONE pilot-derived
    // value (measured against MetalRoughSpheres, the ticket's own named
    // headline model, then applied uniformly rather than hand-tuned
    // per-model, which would risk quietly widening tolerance exactly
    // where a real bug hides) -- EnvironmentTest is not part of that
    // pilot (local-only, never CI-gated) and reuses the same constants
    // for its own local run's informational report.
    int perChannelToleranceOutOf255 = 0;
    double failingPixelBudgetFraction = 0.0;
};

[[nodiscard]] inline const std::vector<ConformanceModel>& conformanceModels() {
    // Pilot-derived tolerance (task-11-report.md has the full derivation
    // + per-model measured numbers): +-32/255 per channel, <2.5% failing-
    // pixel budget. Wide relative to D17's same-renderer +-4/255 / <0.5%
    // by design (see this header's own top comment) -- still tight enough
    // that the discrimination proof (overriding every StandardPBR
    // material's own roughnessFactor to a fixed 0.02 -- see main.cpp's own
    // kPerturbedRoughnessValue) fails it with a real margin. Measured
    // lavapipe divergence against the committed references, all six
    // mandatory models: MetalRoughSpheres 0.6748%,
    // MetalRoughSpheresNoTextures 1.1482%, EmissiveStrengthTest 0.2201%,
    // CompareEmissiveStrength 0.8198%, TextureTransformTest 1.6758% (the
    // worst real-content case -- thin UV-transform edge antialiasing,
    // expected), AlphaBlendModeTest 0.9972% -- every measured value is
    // under 1.7%. The discrimination
    // proof measures 3.87% -- 2.5% sits with real margin on both sides
    // (>45% headroom over the worst real case, >35% below the
    // discrimination value) without being so wide it stops
    // discriminating. A multiplicative (roughness=0.0 is a fixed point)
    // and an additive-clamped (this model's own roughnessFactor already
    // sits at glTF's neutral 1.0 default, itself a clamp fixed point)
    // perturbation were tried FIRST and rejected -- both left the render
    // bit-identical to the unperturbed baseline regardless of magnitude;
    // see main.cpp's own overrideFloatMaterialParamIfPresent() header
    // comment for the full account of why only an absolute override
    // actually moves every case.
    static const std::vector<ConformanceModel> kModels = {
        {"MetalRoughSpheres", /*optional=*/false, 32, 0.025},
        {"MetalRoughSpheresNoTextures", /*optional=*/false, 32, 0.025},
        {"EmissiveStrengthTest", /*optional=*/false, 32, 0.025},
        {"CompareEmissiveStrength", /*optional=*/false, 32, 0.025},
        {"TextureTransformTest", /*optional=*/false, 32, 0.025},
        {"AlphaBlendModeTest", /*optional=*/false, 32, 0.025},
        {"EnvironmentTest", /*optional=*/true, 32, 0.025},
    };
    return kModels;
}

[[nodiscard]] inline const ConformanceModel* findConformanceModel(const std::string& name) {
    for (const ConformanceModel& model : conformanceModels()) {
        if (model.name == name) {
            return &model;
        }
    }
    return nullptr;
}

}  // namespace rx::conformance
