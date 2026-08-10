// Minimal material module for test_material_system.cpp's "load-reflect"
// case: flat, unlit color from a single bound parameter. Every material
// module follows this same shape [D6, D8]:
//   1. `import material;` for IMaterialShader/MaterialVertex.
//   2. Its own parameter struct + a `ParameterBlock<TParams> gParams`
//      global, EXPLICITLY bound to (binding 0, set 1) via `[[vk::binding]]`
//      -- see the comment on `gParams` below for why this is explicit
//      rather than left to Slang's own automatic assignment.
//   3. A struct conforming to IMaterialShader, reading `gParams` in its
//      `evaluate()` body.
//   4. The link-time type alias (`export struct MaterialImpl ... = ...;`)
//      forward_entry.slang's `extern struct MaterialImpl : IMaterialShader;`
//      resolves against at composite/link time -- see that file's header
//      comment for the full mechanism and citation.
import material;

struct UnlitParams {
    float4 tint;
};

// EXPLICIT (binding, set) rather than automatic assignment: MaterialSystem
// (material_system.cpp) requires descriptor set 1 for every material's
// parameter block (set 0 is the external bindless set) and enforces it by
// reading the real set number back out of reflection and rejecting a
// mismatch -- see material_system.cpp's own comment on
// `SlangParameterCategory::SubElementRegisterSpace` for the verified detail
// that matters here: a top-level `ParameterBlock<T>` global reports THAT
// category (not `DescriptorTableSlot`, which rx_shader::reflect() itself
// only handles), and for that category
// `VariableLayoutReflection::getBindingIndex()` -- not getBindingSpace() --
// is the call that returns the real descriptor set number (verified
// directly against this project's shipped Slang v2026.14.1 build via
// spirv-dis on the real emitted SPIR-V at three different explicit
// `[[vk::binding]]` set values; see task-5-report.md for the probe). This
// attribute is what actually PUTS `gParams` at set 1 in the real SPIR-V --
// reflection only ever reads that fact back to validate it, never invents
// or corrects it.
[[vk::binding(0, 1)]]
ParameterBlock<UnlitParams> gParams;

struct Unlit : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        // Genuinely (if negligibly) reads every MaterialVertex field --
        // deliberate, not filler: forward_entry.slang's vertexMain writes
        // worldPos/normal/uv unconditionally (it has no way to know ahead
        // of time which fields a given material's evaluate() will touch),
        // and Slang's SPIR-V backend optimizes each entry point's I/O
        // interface independently -- a material that never reads one of
        // these fields gets that exact interpolant dead-code-eliminated
        // from ITS OWN (fragment-side) input interface, while the shared
        // vertex stage still writes it, producing a real (if harmless --
        // Vulkan's spec explicitly permits it) vertex-output-not-consumed
        // mismatch that this project's validation gate (--validate; every
        // test in this suite asserts zero validation messages) flags at
        // WARNING severity. Verified directly: dropping this and simply
        // returning `gParams.tint` reproduces exactly that warning against
        // this project's real GPU test fixture; this formula (each `* 1e-4`
        // term is a real, non-constant-foldable dependency on runtime
        // interpolated data, so the compiler cannot prove it away) does
        // not. The `1e-4` terms are numerically negligible for any normal
        // input -- this material's actual color is still tint-driven.
        float4 tint = gParams.tint;
        tint.x += v.worldPos.x * 1e-4;
        tint.y += v.normal.y * 1e-4;
        tint.z += v.uv.x * 1e-4;
        return tint;
    }
};

export struct MaterialImpl : IMaterialShader = Unlit;
