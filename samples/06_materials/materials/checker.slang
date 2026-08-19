// checker.slang -- Phase 3 material [Task 8, sample 06_materials]: a
// procedural UV checkerboard, modulated by a per-instance `tint` parameter
// (rx_api.h's IRxMaterialInstance::setFloat4("tint", ...)) -- the material
// samples/06_materials/main.cpp applies to both cube objects, each with a
// DIFFERENT `tint` instance value.
//
// Conforms to shaders/material/material.slang's IMaterialShader interface
// exactly like src/rx_material/tests/data/test_unlit.slang/test_solid.slang/
// test_textured.slang do (see those files' own header comments for the full
// per-part convention this follows: `import material;`, an explicit
// `[[vk::binding(0, 1)]] ParameterBlock<TParams> gParams;`, a struct
// conforming to IMaterialShader, and the link-time `export ... = ...;`
// alias forward_entry.slang's `extern struct MaterialImpl` resolves
// against).
//
// `checkerTexIndex` [task-8 binding global constraint: "Use at least one
// createTexture2D-created texture in one material ... via setTexture ...
// public texture path exercised end-to-end"] -- main.cpp creates a real,
// procedurally generated small checkerboard texture via the PUBLIC
// IRxMaterialSystem::createTexture2D() and binds it here via
// IRxMaterialInstance::setTexture("checkerTexIndex", texture), exercising
// createTexture2D()/setTexture()'s full public contract (bindless
// registration, blob write, addRef/release lifetime) end to end. It is read
// here NUMERICALLY (see evaluate() below) rather than sampled as real image
// data: MaterialSystem's own reflection (rx_material/material_system.cpp,
// reflectMaterialLayout()) rejects any top-level global parameter other
// than `gParams` itself ("Phase 3 materials support exactly one
// ParameterBlock<...> gParams and nothing else at global scope"), so a
// material module has no way to declare its own bindless `Texture2D[]`
// array to actually sample through -- exactly the same documented,
// deliberate Phase 3 scope boundary test_textured.slang's own
// `albedoIndex` field already established (that fixture, too, only reads
// its bindless index numerically, never samples a real texture). Real
// bindless texture SAMPLING from inside a material's own evaluate() is
// future work, not yet wired into the shared material.slang/
// forward_entry.slang contract -- this sample does not invent that wiring
// itself (out of Task 8's scope: it would mean editing rx_material's core
// reflection rules, not just samples/06_materials).
import material;

struct CheckerParams {
    float4 tint;
    uint checkerTexIndex;
};

[[vk::binding(0, 1)]]
ParameterBlock<CheckerParams> gParams;

struct Checker : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        // kCellsPerAxis = 3 (odd), NOT 2 or 4 -- deliberate: this sample's
        // fixed headless camera/object arrangement (see main.cpp's header
        // comment) makes every cube's own screen-projected center land at
        // EXACTLY uv = (0.5, 0.5). floor(0.5 * 3) = 1 on both axes -- the
        // grid's MIDDLE cell, a full 0.5-cell margin away from either
        // neighboring boundary (1.0 and 2.0) -- robust to any floating-point
        // jitter in the interpolated UV. An EVEN cell count would instead
        // put that exact point ON a cell boundary (0.5 * 2 == 1.0 exactly),
        // an unstable case for a headless pixel assertion that depends on
        // which side of the boundary rounds which way.
        const float kCellsPerAxis = 3.0;
        float2 cell = floor(v.uv * kCellsPerAxis);
        float parity = fmod(cell.x + cell.y, 2.0);
        // Middle cell (1, 1) -> parity 0.0 -> the "light" cell (full tint,
        // lightness 1.0) -- see main.cpp's own header comment for the
        // resulting exact analytic derivation (expected color == tint).
        float lightness = (parity < 0.5) ? 1.0 : 0.35;

        float4 color = gParams.tint * lightness;

        // Reads checkerTexIndex without perturbing the analytic color above
        // -- multiplying by exactly 0.0 is IEEE-754-safe (never constant-
        // folded away: gParams.checkerTexIndex is a genuine runtime uniform
        // read from a bound parameter block, not a compile-time constant),
        // matching test_unlit.slang's own "genuinely, if negligibly, read
        // every field" convention, applied here to the bindless-index field
        // this file's header comment explains.
        color.w += float(gParams.checkerTexIndex) * 0.0;
        // Genuinely (if negligibly) reads worldPos/normal too, avoiding the
        // same per-entry-point dead-interpolant validation warning
        // test_unlit.slang's own header comment documents in detail --
        // forward_entry.slang's shared vertex stage writes these
        // unconditionally, and a fragment stage that never reads one at all
        // triggers a real (if harmless) vertex-output-not-consumed mismatch
        // under this project's --validate gate.
        color.x += v.worldPos.x * 1e-6;
        color.y += v.normal.y * 1e-6;
        // [Phase 4 Task 16] MaterialVertex grew tangent/lightDirWorld/
        // lightColor/ambientColor -- same dead-code-elimination-avoidance
        // rationale as worldPos/normal above.
        color.z += v.tangent.x * 1e-6;
        color.w += v.lightDirWorld.x * 1e-6;
        color.x += v.lightColor.x * 1e-6;
        color.y += v.ambientColor.x * 1e-6;
        color.z += v.cameraPosWorld.x * 1e-6;
        return color;
    }
};

export struct MaterialImpl : IMaterialShader = Checker;
