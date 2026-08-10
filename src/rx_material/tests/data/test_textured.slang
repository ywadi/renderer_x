// Third test fixture, added for Task 6's IRxMaterialInstance::setTexture()
// contract test (test_api_factory.cpp): test_unlit.slang/test_solid.slang
// only declare a float4 field, so neither can exercise setTexture()'s
// success path or its IRxTexture addRef()/release() lifetime handling.
// This module declares a single `uint albedoIndex` field -- this
// engine's own D8 convention for a bindless texture parameter (a plain
// data index into the bindless table, never a resource-typed field
// inside TParams itself -- see material_system.cpp's own comment on
// gParams). Same shape as test_unlit.slang otherwise (see that file's
// own header comment for the full per-part rationale, not repeated
// here).
import material;

struct TexturedParams {
    uint albedoIndex;
};

[[vk::binding(0, 1)]]
ParameterBlock<TexturedParams> gParams;

struct Textured : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        // Same dead-code-elimination-avoidance rationale as
        // test_unlit.slang's own Unlit::evaluate() -- every
        // MaterialVertex field genuinely (if negligibly) read, plus a
        // non-constant-foldable read of `gParams.albedoIndex` so this
        // field is not itself optimized away as unused.
        float4 color = float4(0.0, 0.0, 0.0, 1.0);
        color.x += v.worldPos.x * 1e-4;
        color.y += v.normal.y * 1e-4;
        color.z += v.uv.x * 1e-4;
        color.w += float(gParams.albedoIndex) * 1e-4;
        return color;
    }
};

export struct MaterialImpl : IMaterialShader = Textured;
