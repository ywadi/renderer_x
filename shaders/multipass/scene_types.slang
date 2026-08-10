// Sample 05_multipass: shared scene-data types.
//
// SINGLE SOURCE OF TRUTH for `ObjectTransform` -- the bindless storage-
// buffer row layout `shadow.vert.slang` and `lit.vert.slang`/
// `lit.frag.slang` both read via `gTransforms[0][gPush.transformIndex]`.
// Declared exactly ONCE, here, specifically to close a real bug class
// found during this sample's own development [fix round 1]: `shadow.vert.
// slang` used to declare its OWN separate copy of this struct (it is
// compiled as its own Slang module -- see below for why -- never
// concatenated with the lit files), and that copy silently drifted out of
// sync with `lit.vert.slang`'s when the latter grew two more fields
// (`albedo`, `lightDirWorld`). Every index-0 row (the floor) still read
// correctly regardless of which struct's stride was used (both agree at
// byte offset 0), which is exactly why the drift went unnoticed until a
// probe at a NON-zero row (the cube) read a garbled mix of two adjacent
// rows. Two hand-written copies with only a comment enforcing agreement
// is not a real guard against that recurring -- this file, and every
// consumer's mechanical inclusion of it (see below), is.
//
// WHY THIS FILE IS TEXTUALLY CONCATENATED, NOT `import`ed OR `__include`d
// -- read before reaching for either Slang mechanism on this file:
//   - `import scene_types;` (Slang's own module system -- the mechanism
//     shaders/material/forward_entry.slang uses for `import material;`,
//     and the one this task's own coordinator review named as preferred)
//     needs the compiling `slang::ISession` to have a search path
//     covering this file's real on-disk directory
//     (`slang::SessionDesc::searchPaths`) -- exactly what
//     `src/rx_material/material_system.cpp`'s own session setup adds
//     (`RX_MATERIAL_SHADER_DIR`), and exactly what `rx::shader::Compiler`
//     (`src/rx_shader/src/compiler.cpp`) does NOT: its `slang::SessionDesc`
//     is default-constructed with zero search paths, by design --
//     `material_system.cpp`'s own header comment on this exact point:
//     "Compiler's own sessions never set [search paths] (it has no
//     multi-file import to resolve)". samples/05_multipass/main.cpp
//     compiles every pass through THIS Compiler (not the raw Slang API
//     rx_material uses directly), so a real `import` here would need
//     either extending `rx::shader::Compiler` to accept search paths
//     (explicitly out of scope for this task per the coordinator's fix-
//     round instructions) or reimplementing this sample's entire compile-
//     +-reflect pipeline against the raw Slang API just to get one struct
//     shared -- disproportionate to the actual problem.
//   - `__include "scene_types.slang"` (Slang's textual-inclusion
//     directive, the coordinator's own stated fallback) reduces to
//     exactly the same requirement: with no session search path
//     configured, resolving even `__include`'s path needs either a
//     search path (same gap as `import`) or a literal, runtime-resolved
//     absolute path baked into the compiled source text at the point of
//     the directive -- which on Windows means a backslash-bearing path
//     string threaded through a Slang string-literal escape sequence with
//     no existing test coverage of that behavior in this codebase.
//   - This sample's `main.cpp` already established (before this fix
//     round, for pairing each pass's vertex+fragment source into one
//     Slang translation unit -- see lit.vert.slang's own "COMPILATION
//     NOTE") the exact mechanism this problem actually needs: read this
//     file's real, `SDL_GetBasePath()`-resolved bytes from disk and
//     concatenate them into the combined source string BEFORE any of it
//     reaches Slang at all -- Slang itself never sees a directive, never
//     needs a search path, and there is no path-string-escaping surface
//     to get wrong on any platform. `compileAndReflect()`'s callers now
//     list this file first for both the shadow and lit passes (see
//     buildShadowPipeline()/buildLitPipeline()) -- textually and
//     functionally equivalent to `__include`, implemented with a
//     mechanism this file already had and already ships correctly.
//
// `albedo`/`lightDirWorld` are float4 (unused 4th component) rather than
// float3 -- sidesteps any float3-in-a-StructuredBuffer packing ambiguity;
// see the (former) home of this struct, lit.vert.slang, for the sibling
// finding on push-constant scalar packing this same investigation
// produced.
struct ObjectTransform {
    float4x4 mvp;
    float4x4 lightMvp;
    float4 albedo;
    float4 lightDirWorld;
};
