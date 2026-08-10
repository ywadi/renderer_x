// rx_material's engine-defined Slang interface every material module
// conforms to [spec Phase 3 design, D6]. Follows the Shader Components
// lineage (He et al. 2017) and its production evolution in Slang --
// Falcor's material system is the direct shipping precedent for
// "material = Slang module implementing an engine interface"
// (.superpowers/sdd/2026-08-10-phase3-render-graph-materials/
// research-abi-materials.md S2.2).
//
// `public` on every declaration here is load-bearing, not decorative: this
// file is a Slang MODULE (imported via `import material;` from both
// forward_entry.slang and every material module), and Slang's access
// control defaults an unmodified top-level declaration to `internal`
// (module-local) visibility -- an importing module cannot see IMaterialShader
// or MaterialVertex at all without the explicit `public` on each
// declaration (and, for MaterialVertex, on each field: a struct's default
// member visibility only inherits its enclosing type's own visibility
// starting at Slang language version 2026, and this file does not opt into
// `#language slang 2026`, so fields need their own explicit `public` too)
// [Slang user guide, "Modules and Access Control" -- Access Control].

public struct MaterialVertex {
    public float3 worldPos;
    public float3 normal;
    public float2 uv;
}

// Every material module (e.g. a material's own .slang file, loaded via
// MaterialSystem::loadMaterial()) declares a struct conforming to this
// interface plus a `ParameterBlock<TParams> gParams` global holding its
// bound parameters [D6, D8] -- see forward_entry.slang's own header
// comment for the exact link-time-specialization contract a conforming
// module must follow, and shaders/material/tests-equivalent
// src/rx_material/tests/data/test_unlit.slang for a worked minimal
// example.
public interface IMaterialShader {
    float4 evaluate(MaterialVertex v);
}
