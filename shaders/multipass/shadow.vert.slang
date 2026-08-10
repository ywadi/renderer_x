// Sample 05_multipass: shadow pass vertex shader.
//
// Depth-only pass -- there is deliberately no shadow.frag.slang at all:
// with no color attachments and rasterizerDiscardEnable = VK_FALSE, Vulkan
// needs no fragment shader stage to write depth at all (the depth test/
// write consumes SV_Position.z's interpolated value); samples/05_multipass/
// main.cpp's shadow pipeline has exactly one shader stage (VERTEX) because
// of this, not by omission.
//
// Reads the SAME bindless object-transform storage buffer
// lit.vert.slang/lit.frag.slang read (row layout must match byte-for-byte
// -- see lit.vert.slang's own header comment) via
// rx::rhi::BindlessTable's fixed binding 2 (kStorageBufferBinding), but
// only ever consumes `.lightMvp` here: this pass renders the scene from
// the light's own point of view, so a caster's CAMERA-space MVP is
// irrelevant to it. `transformIndex` (a push constant, uniform across the
// whole draw call) selects which object's row to read -- never wrapped in
// NonUniformResourceIndex() [spec Fixed decision #6, matching every other
// sample's push-constant-indexed bindless read].
// MUST stay byte-for-byte identical (same fields, same order, same size)
// to lit.vert.slang's OWN ObjectTransform struct -- this is a SEPARATE
// Slang compilation module (see this file's header comment: no
// lit.vert.slang concatenation here), so Slang computes THIS struct's own
// stride/size independently when indexing `gTransforms[0][transformIndex]`.
// A real, found-during-development bug this comment now documents: an
// earlier revision of this file only had the first two fields (mvp,
// lightMvp) after lit.vert.slang's own struct grew two more (albedo,
// lightDirWorld) -- every index-0 read (the floor, whose row happens to
// start at byte 0 under EITHER stride) still worked, but every index 1+
// read (cube, sphere) silently computed the wrong byte offset into the
// SAME real buffer (128-byte stride here vs. the real 160-byte stride the
// C++ writer and lit.vert.slang both use), reading a garbled mix of the
// previous row's tail and the current row's head -- a plausible-looking
// but WRONG matrix, not a crash or an obviously-garbage value. This is
// exactly the class of bug this sample's own headless gate (shadow-vs-lit
// probe brightness) is designed to catch.
struct ObjectTransform {
    float4x4 mvp;
    float4x4 lightMvp;
    float4 albedo;
    float4 lightDirWorld;
};

struct PushConstants {
    uint transformIndex;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(2, 0)]]
StructuredBuffer<ObjectTransform> gTransforms[];

struct VSIn {
    float3 position : POSITION;
};

[shader("vertex")]
float4 vsMain(VSIn input) : SV_Position
{
    // gTransforms[0]: this sample registers exactly ONE bindless storage
    // buffer (the shared, double-buffered-by-row transform table), exactly
    // like samples/03_bindless_mesh's own gTransforms[0] -- a compile-time-
    // constant descriptor-array index, uniform by construction.
    float4x4 lightMvp = gTransforms[0][gPush.transformIndex].lightMvp;
    return mul(lightMvp, float4(input.position, 1.0));
}
