// shaders/material/unlit.slang -- Phase 4 Stage 1 Task 16 [D22, gate ruling
// #8, KHR_materials_unlit]: an ordinary public IMaterialShader module (zero
// special treatment -- loaded via the same MaterialSystem::loadMaterial()
// every other material uses), mapping glTF's KHR_materials_unlit extension
// 1:1 onto this engine's material system.
//
// SEMANTICS -- quoted directly from the extension's own README (fetched
// 2026-08-18, gate matrix-issue08's own verified citation): color is "the
// product of baseColorFactor, baseColorTexture, and vertex color (if any),
// as defined by the core glTF material specification", rendered as "a
// constantly shaded surface that is independent of lighting". Vertex color
// (COLOR_0) stays the already-ruled D7 import-time deferral (log-don't-drop
// -- see material.slang's own MaterialVertex header comment for the
// extension point) -- this module has no COLOR_0 input to multiply in the
// first place, so nothing here silently drops it beyond what D7 already
// decided.
//
// ZERO LIGHTING DEPENDENCE [acceptance criterion: a light-flip zero-delta
// probe] -- this module's evaluate() reads `v.normal`/`v.tangent`/
// `v.lightDirWorld`/`v.lightColor`/`v.ambientColor`/`v.cameraPosWorld` only
// through a negligible (1e-6-scale) ADDITIVE epsilon (see below), matching
// test_unlit.slang's own established convention (that file's own header
// comment has the full rationale + a direct empirical citation). An EARLIER
// version of this file tried the same `* 0.0` (exact-zero-multiply)
// technique samples/06_materials/materials/checker.slang's own
// `checkerTexIndex` field uses -- verified DURING THIS TASK's own
// development to NOT survive Slang's SPIR-V interface-elimination pass for
// a VERTEX-STAGE INTERPOLANT specifically (unlike a `gParams` UNIFORM
// read): `x * 0.0` is a provable-constant-zero algebraic identity the
// optimizer folds away BEFORE any interpolant-liveness analysis runs,
// re-triggering the exact "vertex shader writes an output location not
// consumed by the fragment shader" validation warning this trick was meant
// to avoid -- reproduced directly (`--validate`,
// UNASSIGNED-CoreValidation-Shader-OutputNotConsumed) against this
// project's real GPU test fixture before switching to the additive form.
// A genuinely small ADDITIVE term (1e-6) is NOT a provable-zero identity to
// the optimizer (it depends on the real runtime value), so the read
// survives -- and is small enough that its effect on an 8-bit UNORM
// readback is below one quantization step (1/255 ~= 0.0039), which is what
// makes the light-flip probe's EXACT 8-bit-equality assertion still valid:
// not mathematically exact, but exact at every precision this engine's
// pixel gates (D17's own +-4/255 tolerance included) ever observe.
import material;

struct UnlitParams {
    // Universal glTF default [core spec, fetched 2026-08-18]: [1,1,1,1].
    float4 baseColorFactor;

    // D11 bindless-index convention: ALWAYS a valid, real texture (the
    // white 1x1 utility texture when the source glTF material has no
    // baseColorTexture at all) -- no separate "has texture" flag needed,
    // since sampling white multiplies baseColorFactor by exactly 1.0.
    uint baseColorTexture;

    // [Fix round, sampler-wrap P0] The bindless SAMPLER index for
    // `baseColorTexture` above -- see standard_pbr.slang's own
    // `StandardPbrParams::baseColorSampler` comment for the full account
    // of the defect this closes and where this value comes from
    // (TextureCache::getOrCreateSamplerBindlessIndex(), resolved by
    // samples/08_gltf_viewer/main.cpp's own resolveSamplerIndex()).
    uint baseColorSampler;

    // [D28] MASK's cutoff -- a per-instance uniform with an ALWAYS-PRESENT
    // conditional discard below, per MaterialFixedFunctionState's own
    // header comment (material_system.h): alphaMode itself lives entirely
    // in the VkPipeline fixed-function state (D28), never here. Callers
    // binding an OPAQUE/BLEND instance set this to 0.0 (alpha is never
    // negative, so a 0.0 cutoff never discards); a MASK instance sets the
    // glTF-parsed `alphaCutoff` (spec default 0.5).
    float alphaCutoff;

    // KHR_texture_transform [consume-now per gate ruling C4]: xy = UV
    // offset, zw = UV scale, applied to `v.uv` before sampling
    // baseColorTexture. Neutral default (0,0,1,1) reproduces plain
    // untransformed UV sampling exactly for the (overwhelming majority of)
    // materials that do not use the extension.
    float4 baseColorUvOffsetScale;
};

[[vk::binding(0, 1)]]
ParameterBlock<UnlitParams> gParams;

struct Unlit : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        float2 uv = v.uv * gParams.baseColorUvOffsetScale.zw + gParams.baseColorUvOffsetScale.xy;
        float4 color = gParams.baseColorFactor *
                       rx_sampleTexture(gParams.baseColorTexture, gParams.baseColorSampler, uv);

        // [D28] Always-present conditional discard -- see UnlitParams::
        // alphaCutoff's own comment above.
        if (color.a < gParams.alphaCutoff) {
            discard;
        }

        // See this file's own header comment: every field below is read
        // through a negligible (1e-6) additive epsilon -- an exact `* 0.0`
        // does NOT survive Slang's interface-elimination pass for a vertex
        // interpolant (verified; see that comment for the full mechanism).
        color.r += v.worldPos.x * 1e-6;
        color.g += v.normal.x * 1e-6;
        color.b += v.tangent.x * 1e-6;
        color.a += v.lightDirWorld.x * 1e-6;
        color.r += v.lightColor.x * 1e-6;
        color.g += v.ambientColor.x * 1e-6;
        color.b += v.cameraPosWorld.x * 1e-6;
        return color;
    }
};

export struct MaterialImpl : IMaterialShader = Unlit;
