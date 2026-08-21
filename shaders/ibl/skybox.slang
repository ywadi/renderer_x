// shaders/ibl/skybox.slang -- Phase 5 Stage 1 Task 10 [#46, gate matrix
// matrix-p5t10-ibl-runtime-skybox.md, FG1 closure]: the background pass
// sampling the scene environment's own BASE cubemap (rx::ibl::BakeResult::
// baseCubemap -- NOT the irradiance/prefiltered ones standard_pbr.slang's
// IBL lobes consume, per the ticket's own file list distinguishing
// "skybox pass sampling the cubemap" from the lit-path lobes) at the
// camera ray's own world-space direction, depth-tested BEHIND every real
// draw (D29 conventions -- see this file's own pipeline-state comment
// below).
//
// NOT LINKED AGAINST material.slang/forward_entry.slang [gate matrix's own
// Open Question, resolved]: the skybox pass reuses the EXISTING forward
// pass's own camera/view-proj plumbing only in the sense of sharing the
// SAME depth attachment within one render-graph Pass scope (see samples/
// 08_gltf_viewer/main.cpp's own recordSkybox() header comment for the full
// "why a second draw inside the SAME pass, not a new render-graph
// primitive" reasoning) -- it is its OWN small, independent shader module
// (own vertex+fragment entry points, own tiny per-frame data bundle) with
// no material/lighting-model dependency at all, matching this codebase's
// established "shaders/multipass/*.slang declares its own globals at the
// same external bindless bindings, never imports material.slang" precedent
// (tonemap.{vert,frag}.slang is the closest structural template: a
// fullscreen-triangle pass reading ONE bindless-indexed resource via a
// tiny per-frame struct, though tonemap's own struct travels via a push
// constant since its shape is smaller -- this pass's own `RxSkyboxData`
// needs a 4x4 matrix, which this project's own established convention
// avoids inside a push-constant block [material.slang's own RxMaterialGlobals
// header comment cites the exact reason], so it travels via the SAME
// bindless-storage-buffer-plus-push-constant-index idiom RxDrawData/
// gMaterialGlobals.drawDataBufferIndex already establishes -- one row,
// registered fresh each frame the camera moves). This file therefore does
// NOT `import material;` -- it declares its OWN LOCAL `gTexturesCube`/
// `gSamplers` bindless arrays at the SAME binding numbers material.slang
// uses (4 and 1 respectively), exactly like tonemap.frag.slang's own
// local `gTextures`/`gSamplers` at bindings 0/1 -- two never-linked-
// together shader modules declaring the same EXTERNAL bindless binding
// number is not a conflict (each compiles into its own independent SPIR-V
// module); importing material.slang instead would additionally pull in
// its own `gDrawData` StructuredBuffer at binding 2 (`kStorageBufferBinding`),
// directly colliding with THIS file's own differently-typed `gSkyboxData`
// at that same binding within one linked program -- confirmed directly
// during this task's own development before settling on this shape.
//
// CAMERA-RAY RECONSTRUCTION: the vertex stage emits a screen-filling
// triangle (the standard 3-vertex, no-vertex-buffer trick -- vertices at
// (-1,-1),(3,-1),(-1,3) in NDC, covering the whole [-1,1]x[-1,1] viewport
// with one overdrawn triangle rather than two) with its OWN clip position
// forced to NDC z=0.0 -- this project's reversed-Z convention's FAR value
// (D13; near=1.0, far=0.0) -- so every fragment's own depth write-candidate
// sits exactly at the far plane, matching this pass's own depth-test setup
// below. The fragment stage unprojects that SAME (ndcXY, z=0) point back
// to world space via `gSkyboxData[...].invViewProj` (the one new small
// CPU-computed inverse matrix this pass needs -- glm::inverse() on the
// CPU side, cheap, once per frame, per the gate matrix's own Open Question
// disposition "GPU-side inverse() cheap either way... CPU-side push
// constant... cheap either way", resolved here as a CPU-side compute
// since it is needed only once per frame, not per-vertex/per-fragment) and
// derives the ray direction as `normalize(worldPoint - cameraPosWorld)` --
// correct regardless of which NDC z the unprojection uses (every world
// point sharing one (ndcX,ndcY) pair lies on the SAME ray through the eye,
// for a standard perspective projection), so the z=0 choice is arbitrary
// convenience, not a correctness requirement.
//
// DEPTH TEST -- D29/RC3 [gate ruling rulings-2026-08-20.md]: this project's
// reversed-Z convention (near=1.0, far=0.0, `VK_COMPARE_OP_GREATER_OR_EQUAL`
// -- material_system.cpp's own `getPipeline()`) means a fragment passes the
// depth test when `newDepth >= storedDepth`. This pass's own fragment
// output depth is the constant 0.0 (the vertex stage's own NDC z, per the
// comment above) -- `0.0 >= storedDepth` is true ONLY where `storedDepth`
// is still exactly the render graph's own clear value (0.0, i.e. no real
// geometry drew there yet); any real draw's own depth is a positive value
// approaching 1.0 as it nears the camera, which the skybox's own 0.0 can
// never dethrone. `depthWriteEnable=false` (samples/08_gltf_viewer's own
// pipeline-state construction) -- the skybox never OVERWRITES a real
// draw's own stored depth, consistent with drawing strictly BEHIND
// everything.
// Local bindless declarations -- see this file's own header comment above
// for why this is NOT `import material;`. Only the two bindings this pass
// actually needs (SAME binding numbers/types the real BindlessTable
// declares -- bindless.h's own `kSamplerBinding`/`kCubeSampledImageBinding`).
[[vk::binding(1, 0)]]
SamplerState gSamplers[];

[[vk::binding(4, 0)]]
TextureCube gTexturesCube[];

// [Task 10] The scene environment's own per-frame bundle -- ONE row,
// rebuilt whenever the camera moves (every frame, in practice). Field
// layout mirrors `rx::material::SkyboxDataGpu` (samples/08_gltf_viewer/
// main.cpp) EXACTLY -- see that struct's own header comment for the
// cross-file drift risk this shares with RxDrawData/DrawDataGpu.
public struct RxSkyboxData {
    public float4x4 invViewProj;  // clip -> world. [MATRIX LAYOUT] row-major, transposed before upload -- see material.slang's own RxDrawData comment.
    public float4 cameraPosWorld;  // xyz used; w unused padding.
    public uint baseCubeIndex;     // bindless CUBE index (BindlessTable::kCubeSampledImageBinding).
    public uint cubeSamplerIndex;  // bindless SAMPLER index.
    // PRE-EXPOSED (Task 4 convention, SAME single CPU-side multiply point
    // as RxDrawData::envIntensity -- material.slang's own header comment)
    // -- no separate exposure read/multiply here.
    public float intensity;
    public uint _pad0;
}

// Reuses the SAME external bindless set-0 (BindlessTable) every other
// pass in this codebase binds -- `gDrawData`-shaped StructuredBuffer array
// at `BindlessTable::kStorageBufferBinding`, but with THIS file's own
// `RxSkyboxData` element type (Slang/SPIR-V requires one binding per
// distinct element TYPE; this is a SEPARATE shader module from material.
// slang, so declaring the SAME binding NUMBER with a DIFFERENT element
// type is exactly shaders/multipass/lit.vert.slang's own established
// `gTransforms` precedent, not a conflict -- the two shader modules are
// never linked into the same SPIR-V program).
[[vk::binding(2, 0)]]
StructuredBuffer<RxSkyboxData> gSkyboxData[];

public struct SkyboxPush {
    public uint skyboxDataBufferIndex;  // bindless STORAGE BUFFER index of the one-row gSkyboxData[] above.
}

[[vk::push_constant]]
public ConstantBuffer<SkyboxPush> gSkyboxPush;

public struct VertexStageOutput {
    public float4 clipPosition : SV_Position;
    public float2 ndcXY;
}

// Standard "3 vertices, no vertex buffer, one overdrawn screen-filling
// triangle" fullscreen trick -- SAME technique shaders/multipass/
// tonemap.vert.slang already uses (that file's own header comment cites
// the identical rationale: cheaper than a 4-vertex quad + index buffer,
// covers the same [-1,1]x[-1,1] NDC rectangle).
[shader("vertex")]
public VertexStageOutput vsMain(uint vertexId: SV_VertexID) {
    // [Syntax verified directly against this project's shipped tonemap.
    // vert.slang, which already compiles this exact idiom] Slang's array-
    // literal constructor form, not a C-style brace-init list.
    float2 positions[3] = float2[3](float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0));
    float2 ndc = positions[vertexId];

    VertexStageOutput output;
    // [D13/D29] NDC z = 0.0 -- this pass's own reversed-Z FAR value, see
    // this file's own header comment ("DEPTH TEST") for the full
    // depth-test derivation this feeds.
    output.clipPosition = float4(ndc, 0.0, 1.0);
    output.ndcXY = ndc;
    return output;
}

[shader("fragment")]
public float4 fsMain(VertexStageOutput input) : SV_Target {
    RxSkyboxData data = gSkyboxData[gSkyboxPush.skyboxDataBufferIndex][0];

    // [This file's own header comment, "CAMERA-RAY RECONSTRUCTION"] Any
    // NDC z unprojects to a world point on the SAME ray through the eye --
    // z=0.0 here matches this fragment's own emitted depth, no functional
    // significance beyond that.
    float4 worldPos4 = mul(data.invViewProj, float4(input.ndcXY, 0.0, 1.0));
    float3 worldPos = worldPos4.xyz / worldPos4.w;
    float3 rayDir = normalize(worldPos - data.cameraPosWorld.xyz);

    float3 color = gTexturesCube[data.baseCubeIndex].SampleLevel(gSamplers[data.cubeSamplerIndex], rayDir, 0.0).rgb;
    color *= data.intensity;
    return float4(color, 1.0);
}
