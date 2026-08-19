// Second, deliberately-different material module for
// test_material_system.cpp's "cache-key-material" case: same shape as
// test_unlit.slang (flat color from a bound parameter), but distinct
// source bytes (different struct/field names, fixed-multiplier body) so
// its module content hash -- and therefore its pipeline-variant cache key
// -- differs from test_unlit.slang's even when requested against the
// identical PassSignature.
import material;

struct SolidParams {
    float4 color;
};

[[vk::binding(0, 1)]]
ParameterBlock<SolidParams> gParams;

struct Solid : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        // See test_unlit.slang's Unlit::evaluate() for why every
        // MaterialVertex field is genuinely (if negligibly) read here --
        // same forward_entry.slang per-entry-point dead-code-elimination
        // reasoning, verified against this project's real GPU test
        // fixture. [Phase 4 Task 16] Extended to tangent/lightDirWorld/
        // lightColor/ambientColor.
        float4 color = gParams.color * 0.5;
        color.x += v.worldPos.x * 1e-4;
        color.y += v.normal.y * 1e-4;
        color.z += v.uv.x * 1e-4;
        color.w += v.tangent.x * 1e-4;
        color.x += v.lightDirWorld.x * 1e-4;
        color.y += v.lightColor.x * 1e-4;
        color.z += v.ambientColor.x * 1e-4;
        color.w += v.cameraPosWorld.x * 1e-4;
        return color;
    }
};

export struct MaterialImpl : IMaterialShader = Solid;
