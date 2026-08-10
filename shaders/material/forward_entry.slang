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
// PHASE 3 SCOPE NOTE -- no camera/transform system exists yet (that is a
// samples-05/06-and-later concern, not this task's): `vertexMain` below
// treats its incoming `position` attribute AS the world-space position
// verbatim (no model/view/projection transform anywhere in this file), and
// clip-space output is the same untransformed value with w=1. A real
// sample wires an actual transform in before this stage; MaterialSystem's
// own tests (src/rx_material/tests/test_material_system.cpp) only exercise
// pipeline creation/reflection/caching, never rasterization, so this
// simplification is invisible to them.
import material;

// The link-time-resolved concrete material type -- see this file's header
// comment above. Used by name in fragmentMain below exactly like
// "main.slang"'s `Sampler sampler;` in the cited example; left unresolved
// (no default) deliberately, since a forward_entry composite with no
// material module supplied is never a real, useful program in this
// engine -- every MaterialSystem::getPipeline() call always supplies one.
extern struct MaterialImpl : IMaterialShader;

// VERTEX INPUT LAYOUT -- read before touching MaterialSystem's pipeline
// vertex-input state (material_system.cpp): Phase 2 never fixed one
// canonical host-side `Vertex` struct in a shared library header (each
// sample -- 03_bindless_mesh, 04_streaming -- declares its own local
// `struct Vertex { float position[3]; float uv[2]; };`, with no `normal`
// field at all); `rx_rhi_vk::MeshBuffers` (mesh_buffers.h) is deliberately
// layout-agnostic (two raw device-local buffers, vertex layout supplied
// entirely by the caller's own VkVertexInputBindingDescription/
// VkVertexInputAttributeDescription at pipeline-creation time). Task 5
// therefore defines ITS OWN fixed vertex layout, chosen to carry exactly
// what `MaterialVertex` (material.slang) needs and nothing else: three
// attributes, `position`/`normal`/`uv`, matching this entry point's own
// parameter list one-to-one and bound as a single interleaved
// vertex-buffer stream (binding 0) at locations 0/1/2 respectively. See
// material_system.cpp's pipeline-building code for the exact
// VkVertexInputAttributeDescription table this assumes (offsetof-style
// float3+float3+float2, tightly packed, no padding) -- a future sample
// wiring real mesh geometry through this material system must upload
// vertex data in that exact interleaving.
struct VertexStageOutput {
    float4 clipPosition : SV_Position;
    float3 worldPos;
    float3 normal;
    float2 uv;
};

[shader("vertex")]
VertexStageOutput vertexMain(float3 position : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD0) {
    VertexStageOutput output;
    // See this file's header "PHASE 3 SCOPE NOTE": no transform exists yet,
    // so object space, world space, and (homogeneous, w=1) clip space are
    // all the same value at this point in the pipeline.
    output.clipPosition = float4(position, 1.0);
    output.worldPos = position;
    output.normal = normal;
    output.uv = uv;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexStageOutput input) : SV_Target {
    MaterialVertex v;
    v.worldPos = input.worldPos;
    v.normal = input.normal;
    v.uv = input.uv;

    // Default-constructed: every conforming material struct this engine
    // links against is data-free by convention (its bound parameters live
    // in its own module-scope `ParameterBlock<TParams> gParams`, per D8 --
    // see material.slang's header comment and test_unlit.slang for the
    // worked example), so there is no instance state to initialize here.
    MaterialImpl material;
    return material.evaluate(v);
}
