// Task 4 (seed-10 carried) fixture: unlike test_textured.slang's
// `albedoIndex` field (read only numerically, per Phase 3 Task 8's own
// review -- see .superpowers/sdd/2026-08-10-phase3-render-graph-materials/
// task-8-review.md, "The central question: texture path vs. texture
// sampling"), this module's `evaluate()` actually calls `rx_sampleTexture`
// against material.slang's newly-declared bindless texture+sampler arrays
// -- the real, rendered-pixel-changing path Task 4 wires up. Deliberately
// still declares only the one `uint albedoIndex` field in `gParams` (the
// SAME shape as test_textured.slang), so this fixture also proves the
// reflection allow-list change is additive: a material's own set-1
// ParameterBlock is completely unaffected by whether it also references
// the new set-0 bindless globals.
import material;

struct TexturedSampleParams {
    uint albedoIndex;
};

[[vk::binding(0, 1)]]
ParameterBlock<TexturedSampleParams> gParams;

struct TexturedSample : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        // `worldPos`/`normal` read negligibly (same dead-code-elimination-
        // avoidance rationale as test_unlit.slang/test_textured.slang's own
        // header comments) so this fixture keeps the same "every
        // MaterialVertex field genuinely read" convention; the real content
        // of the returned color is the actual bindless-sampled texel.
        float4 sampled = rx_sampleTexture(gParams.albedoIndex, v.uv);
        sampled.x += v.worldPos.x * 1e-6;
        sampled.y += v.normal.y * 1e-6;
        return sampled;
    }
};

export struct MaterialImpl : IMaterialShader = TexturedSample;
