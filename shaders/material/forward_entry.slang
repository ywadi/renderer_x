// The shared forward-pass entry-point module every material pipeline is
// linked against [spec Phase 3 design, D6]: declares the vertex+fragment
// entry points generic over a material, and is composed + specialized to
// one concrete material module at link time via Slang's documented
// link-time-specialization workflow -- `extern`/`export` type aliasing,
// resolved by `ISession::createCompositeComponentType()` +
// `IComponentType::link()` -- NOT `IComponentType::specialize()` with
// explicit type arguments, and NOT existential/`anyValueSize` dynamic
// dispatch (reserved for a future bindless-material-array case, per D6).
//
// This is Slang's own documented recommended path for exactly this shape
// of problem: see the worked "Link-time Types" example in
// https://shader-slang.org/slang/user-guide/link-time-specialization
// (`extern struct Sampler : ISampler;` in a "main"-equivalent module,
// resolved by `export struct Sampler : ISampler = FooSampler;` in a
// separately-compiled module, composed together and linked). MaterialSystem
// (rx_material/material_system.cpp) follows that example verbatim: this
// file plays "main.slang"'s role (the `extern` declaration below, used by
// name in the entry points that follow it), and every material module
// (e.g. test_unlit.slang) plays "sampler.slang"'s role (the matching
// `export ... = ConcreteType;` alias) -- the two are composed together with
// this module's own two IEntryPoint objects into one
// createCompositeComponentType() call, then linked once per unique
// (material, pass) key [D7]. Verified directly against the project's
// shipped Slang v2026.14.1 build before writing this file (not just the
// docs): a throwaway 4-part composite of exactly this shape (entry-point
// module + 2 entry points + a `export ... = Concrete;`-providing module)
// compiles, links, and produces correct SPIR-V for the concrete type's
// `evaluate()` body -- see task-5-report.md for the probe.
//
// PHASE 4 TASK 16 UPDATE -- superseding the "PHASE 3 SCOPE NOTE" this
// comment used to carry verbatim (kept below, struck through in spirit, for
// the historical "why no transform" context): `vertexMain` now reads a REAL
// model/view/projection transform, per D26.1's bindless-per-draw addressing
// -- see material.slang's own `RxDrawData`/`gDrawData` header comment for
// the full mechanism. `MaterialSystem::create()` registers a small DEFAULT
// draw-data buffer (one row, every field at its identity/neutral default --
// model/normalMatrix/viewProj all identity, so `mul(identity, mul(identity,
// float4(position,1))) == float4(position,1)` EXACTLY) so every pre-D26.1
// caller (samples 06/07's own test materials, which never issue a draw with
// `firstInstance != 0` and therefore always read that one default row) is
// UNAFFECTED byte-for-byte -- this is not a breaking change to
// forward_entry.slang's existing callers, just a superset. A real per-draw
// caller (StandardPBR/Unlit, samples/08_gltf_viewer) registers its OWN
// StructuredBuffer<RxDrawData> (one row per submesh instance) and pushes
// its real bindless index via `gMaterialGlobals.drawDataBufferIndex`.
//
// ORIGINAL PHASE 3 SCOPE NOTE, for archaeology: "no camera/transform system
// exists yet... vertexMain treats its incoming `position` attribute AS the
// world-space position verbatim... A real sample wires an actual transform
// in before this stage" -- samples/06_materials/main.cpp's own
// `transformAndUploadObjectVertices()` is exactly that CPU-side workaround,
// pre-baking its own view-proj into vertex positions on the CPU BEFORE this
// stage existed; it still works unchanged today only because the identity
// draw-data row above makes this stage's own GPU-side transform a no-op for
// it.
import material;

// The link-time-resolved concrete material type -- see this file's header
// comment above. Used by name in fragmentMain below exactly like
// "main.slang"'s `Sampler sampler;` in the cited example; left unresolved
// (no default) deliberately, since a forward_entry composite with no
// material module supplied is never a real, useful program in this
// engine -- every MaterialSystem::getPipeline() call always supplies one.
extern struct MaterialImpl : IMaterialShader;

// [Phase 4 Task 16, hardening] `normalize()` of an exact zero vector is a
// 0/0 NaN, silent at the point it happens and only visible once it
// propagates into a material's own output color -- reproduced directly
// during this task's own development (samples/06_materials's procedural
// sphere/cube generators originally left `tangent` zero-initialized;
// forward_entry.slang's own unconditional `normalize(mul(modelMat3,
// tangent.xyz))` turned that into NaN, which every material here reads
// back via a `v.tangent.x * ...` epsilon term for an unrelated reason --
// see MaterialVertex's own header comment -- corrupting the whole pixel).
// Real imported content (D8's pooled vertex format, MikkTSpace-generated at
// import time) never produces a degenerate zero tangent/normal, but a
// hand-authored procedural mesh (any current or future sample) can, by a
// simple omission that costs nothing to guard against here, once, for
// every material this engine will ever load -- cheaper and more robust
// than trusting every vertex-data producer to remember it independently.
float3 safeNormalize(float3 v) {
    float lenSq = dot(v, v);
    return (lenSq > 1e-12) ? (v * rsqrt(lenSq)) : float3(0.0, 0.0, 1.0);
}

// VERTEX INPUT LAYOUT -- read before touching MaterialSystem's pipeline
// vertex-input state (material_system.cpp): Phase 2 never fixed one
// canonical host-side `Vertex` struct in a shared library header (each
// sample -- 03_bindless_mesh, 04_streaming -- declares its own local
// `struct Vertex { float position[3]; float uv[2]; };`, with no `normal`
// field at all); `rx_rhi_vk::MeshBuffers` (mesh_buffers.h) is deliberately
// layout-agnostic (two raw device-local buffers, vertex layout supplied
// entirely by the caller's own VkVertexInputBindingDescription/
// VkVertexInputAttributeDescription at pipeline-creation time). Task 5
// therefore defined ITS OWN fixed vertex layout, chosen to carry exactly
// what `MaterialVertex` (material.slang) needs -- [Phase 4 Task 16, D8]
// GREW BY ONE FIELD: `tangent` (float4, w = handedness), matching D8's
// pooled `position f32x3 | normal f32x3 | tangent f32x4 | uv0 f32x2`
// vertex format exactly (`rx::asset::PoolVertex`, geometry_pool.h) -- this
// was a prerequisite for normal mapping no earlier task's file list named
// (see material.slang's own MaterialVertex comment). Four attributes,
// `position`/`normal`/`tangent`/`uv`, matching this entry point's own
// parameter list one-to-one and bound as a single interleaved
// vertex-buffer stream (binding 0) at locations 0/1/2/3 respectively. See
// material_system.cpp's pipeline-building code for the exact
// VkVertexInputAttributeDescription table this assumes (offsetof-style
// float3+float3+float4+float2, tightly packed, no padding, 48 bytes total
// -- D8 pins this exact stride) -- a sample wiring real mesh geometry
// through this material system must upload vertex data in that exact
// interleaving (`rx::asset::GeometryPool`'s own pooled buffers already do).
struct VertexStageOutput {
    float4 clipPosition : SV_Position;
    float3 worldPos;
    float3 normal;
    float4 tangent;
    float2 uv;
    float3 lightDirWorld;
    float3 lightColor;
    // [Phase 5 Task 10, #46] `ambientColor` DROPPED from this interpolant
    // struct (unlike RxDrawData/MaterialVertex, which both KEEP the field
    // -- see MaterialVertex::ambientColor's own header comment, material.
    // slang): no conforming material reads `MaterialVertex::ambientColor`
    // any more (StandardPbr's own evaluate() retired it this task; Unlit
    // never read it), so Slang's own whole-program dead-code elimination
    // strips the FRAGMENT-side consumption of it entirely, which left this
    // struct's own vertex-stage WRITE (`output.ambientColor = ...`, below)
    // as a genuine "vertex shader writes an output the fragment shader
    // does not consume" mismatch -- a real Vulkan validation performance
    // warning, reproduced directly during this task's own development,
    // not a hypothetical. Removing the now-pointless carry-through here
    // (fragmentMain sets `v.ambientColor` to a literal zero instead, right
    // before calling `material.evaluate()`) closes the mismatch at its
    // source rather than suppressing the warning.
    float3 cameraPosWorld;
    // [Phase 4 Stage 2 Task 22 fix round, F1, spec D21] The draw's own
    // bindless row index -- carried through so fragmentMain can re-read
    // THIS SAME `RxDrawData` row for its shadow fields
    // (`lightViewProj`/`shadowMapTextureIndex`/...), which vertexMain
    // does not otherwise forward individually. An integer varying is
    // implicitly flat/`nointerpolation` (SPIR-V has no linear-interpolation
    // decoration for integer types) -- exactly the same per-draw-constant-
    // travels-through-the-vertex-output idiom `lightDirWorld`/`lightColor`/
    // `ambientColor` above already use, just for an index instead of a
    // value.
    nointerpolation uint drawDataRow;
};

// `instanceId` binds to `SV_VulkanInstanceID`, NOT `SV_InstanceID` -- see
// material.slang's own `RxDrawData`/`gDrawData` header comment (D26.1,
// gate ruling RC1/#8) for the full, verified Slang/SPIR-V/D3D semantic
// mismatch this is deliberately sidestepping: Slang's `SV_InstanceID`
// SUBTRACTS `firstInstance` back out (D3D-compatible semantics), which
// would silently break the moment a real caller issues a draw with
// `firstInstance > 0` (the SoA per-draw addressing D26.1 exists for) --
// `SV_VulkanInstanceID` is the raw, `firstInstance`-INCLUSIVE index this
// engine's bindless per-draw buffer is indexed by.
[shader("vertex")]
VertexStageOutput vertexMain(float3 position : POSITION, float3 normal : NORMAL, float4 tangent : TANGENT,
                              float2 uv : TEXCOORD0, uint instanceId : SV_VulkanInstanceID) {
    RxDrawData draw = gDrawData[gMaterialGlobals.drawDataBufferIndex][instanceId];

    float4 worldPos4 = mul(draw.model, float4(position, 1.0));
    float3x3 normalMat3 = (float3x3)draw.normalMatrix;
    // Tangents (like positions) transform with the ordinary model matrix,
    // NOT its inverse-transpose -- only normals need the inverse-transpose
    // treatment to stay perpendicular to the surface under non-uniform
    // scale. Renormalized here (a non-uniform-scaled model matrix does not
    // preserve length); handedness (`tangent.w`) is a sign, never scaled.
    float3x3 modelMat3 = (float3x3)draw.model;

    VertexStageOutput output;
    output.clipPosition = mul(draw.viewProj, worldPos4);
    output.worldPos = worldPos4.xyz;
    output.normal = safeNormalize(mul(normalMat3, normal));
    output.tangent = float4(safeNormalize(mul(modelMat3, tangent.xyz)), tangent.w);
    output.uv = uv;
    output.lightDirWorld = draw.lightDirWorld.xyz;
    output.lightColor = draw.lightColor.xyz;
    output.cameraPosWorld = draw.cameraPosWorld.xyz;
    output.drawDataRow = instanceId;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexStageOutput input) : SV_Target {
    // [Phase 4 Stage 2 Task 22 fix round, F1, spec D21] Re-read THIS
    // draw's own RxDrawData row for its shadow fields -- vertexMain
    // already read the SAME row once (for model/viewProj/light*), but
    // fragmentMain has no direct access to that local; re-reading by the
    // row index forwarded via `input.drawDataRow` is one extra
    // StructuredBuffer fetch per fragment, the same cost class as any
    // other per-fragment bindless read this shader already performs
    // (rx_sampleTexture's own gTextures[]/gSamplers[] indexing).
    RxDrawData draw = gDrawData[gMaterialGlobals.drawDataBufferIndex][input.drawDataRow];
    float shadowFactor = rx_sampleShadowPCF(draw.shadowMapTextureIndex, draw.shadowCompareSamplerIndex, input.worldPos,
                                             draw.lightViewProj, draw.shadowTexelSize);

    MaterialVertex v;
    v.worldPos = input.worldPos;
    v.normal = safeNormalize(input.normal);
    v.tangent = float4(safeNormalize(input.tangent.xyz), input.tangent.w);
    v.uv = input.uv;
    v.lightDirWorld = input.lightDirWorld;
    // Shadow attenuates DIRECT light ONLY -- scaling `lightColor` here
    // (not the material's own final returned color) means every
    // conforming material's own `directContribution = NdotL * lightColor
    // * brdf(...)`-shaped term (standard_pbr.slang's own `directLight`)
    // is correctly attenuated while ambient/emissive terms (driven by
    // `ambientColor`, untouched) are not -- exactly D21's own "a REAL
    // direct-light visibility term" framing (standard_pbr.slang's own
    // occlusion-vs-shadow comment, written when this shader-side half of
    // Task 22 was still future work, now landed here).
    v.lightColor = input.lightColor * shadowFactor;
    // [Phase 5 Task 10, #46] Retired literal -- see MaterialVertex::
    // ambientColor's own header comment (material.slang) and this file's
    // own VertexStageOutput comment above for why this is a plain zero
    // constant rather than a carried-through vertex-stage interpolant.
    v.ambientColor = float3(0.0, 0.0, 0.0);
    v.cameraPosWorld = input.cameraPosWorld;

    // [Phase 5 Task 10, #46] The scene environment's own bindless bundle --
    // read directly off the SAME `draw` row re-read above for the shadow
    // fields (not carried through VertexStageOutput/interpolated: every
    // field here is per-pass-constant, and this draw's own row is already
    // sitting in a local, so there is nothing to gain from also widening
    // the vertex-stage output struct for it).
    v.envIrradianceCubeIndex = draw.envIrradianceCubeIndex;
    v.envPrefilteredCubeIndex = draw.envPrefilteredCubeIndex;
    v.envDfgLutIndex = draw.envDfgLutIndex;
    v.envCubeSamplerIndex = draw.envCubeSamplerIndex;
    v.envDfgSamplerIndex = draw.envDfgSamplerIndex;
    v.envMaxPrefilteredLod = draw.envMaxPrefilteredLod;
    v.envIntensity = draw.envIntensity;

    // Default-constructed: every conforming material struct this engine
    // links against is data-free by convention (its bound parameters live
    // in its own module-scope `ParameterBlock<TParams> gParams`, per D8 --
    // see material.slang's header comment and test_unlit.slang for the
    // worked example), so there is no instance state to initialize here.
    MaterialImpl material;
    float4 color = material.evaluate(v);

    // [Phase 5 Task 4/#40, gate ruling rulings-2026-08-20.md T4] The
    // Phase-4 `--exposure` post-multiply that used to sit here
    // (`color.rgb *= exp2(gMaterialGlobals.exposure);`) is DELETED, not
    // relocated: Filament-style PRE-EXPOSURE means exposure is already
    // baked into `v.lightColor`/`v.ambientColor` above (pre-multiplied by
    // `rx::scene::Camera::exposure()` at each RxDrawData producer's own
    // CPU-side source, before this pass ever runs), so `material.evaluate()`
    // above already returns a correctly-exposed color -- a second
    // multiply here would double-apply exposure. See
    // gMaterialGlobals' own header comment (material.slang) for the full
    // ruling.
    return color;
}
