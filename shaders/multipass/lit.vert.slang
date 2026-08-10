// Sample 05_multipass: forward-pass vertex shader.
//
// COMPILATION NOTE -- read before editing either file: rx::shader::Compiler
// compiles exactly one module (one source string) per call, and
// rx::shader::reflect() needs every entry point of a pass in ONE linked
// program to produce a single, correctly-merged descriptor-set/push-
// constant layout (the same reason 02_hotreload/03_bindless_mesh/
// 04_streaming each embed their vertex+fragment source together as one
// string). samples/05_multipass/main.cpp's compileAndReflect() reproduces
// that same shape for THIS pass's on-disk files by reading and
// concatenating them (scene_types.slang first, this file second,
// lit.frag.slang third) into one Slang translation unit before compiling
// -- so every `[[vk::binding(...)]]`/`[[vk::push_constant]]` global
// declared below, and `ObjectTransform` (scene_types.slang), are visible
// to lit.frag.slang's fsMain too, exactly as if they had all been written
// in one file. Do not re-declare any of these globals in lit.frag.slang.
//
// SHADOW SAMPLING: manual single-tap depth comparison in lit.frag.slang, no
// VkSampler compare mode [coordinator ambiguity resolution] -- this file's
// job is only to compute and pass through `lightSpacePosition`, an
// unmodified light-clip-space position lit.frag.slang projects itself.
//
// OBJECT TRANSFORMS: one bindless storage buffer holds BOTH matrices this
// sample ever needs per object -- `.mvp` (camera clip-space, used here) and
// `.lightMvp` (light clip-space, passed through as a varying for
// lit.frag.slang's shadow lookup) -- addressed via a push-constant
// `transformIndex`, exactly samples/03_bindless_mesh's bindless-storage-
// buffer-plus-push-constant-index pattern [coordinator ambiguity
// resolution]. shadow.vert.slang reads the identical row layout for its own
// `.lightMvp` half.
//
// NO PER-OBJECT ROTATION -- see samples/05_multipass/main.cpp's header
// comment for why this sample's scene objects only ever translate, never
// rotate: under a translation-only model matrix, a direction vector
// (a vertex normal) transforms identically in object space and world
// space, so this shader passes `input.normal` straight through with no
// normal-matrix multiply at all. Reintroducing per-object rotation without
// also reintroducing a normal-matrix transform here would silently break
// lit.frag.slang's Lambert term.
// `ObjectTransform` itself now lives in scene_types.slang (concatenated
// ahead of this file at compile time -- see the COMPILATION NOTE above
// and that file's own header comment for the full "why not import/
// __include" rationale) [fix round 1: this struct used to be declared
// here directly, and separately again in shadow.vert.slang, with only a
// comment guarding the two copies from drifting -- they drifted; see
// scene_types.slang's header for the postmortem].
//
// `albedo`/`lightDirWorld` live in the SAME bindless row as the two
// matrices (as float4 -- the 4th component unused padding, sidestepping
// any float3-packing ambiguity) rather than in push constants: EMPIRICALLY
// VERIFIED against the shipped Slang build (reflect()'s own reported
// push-constant range size, checked directly during this sample's
// development) that a push-constant block's real size is NOT rounded up
// to any fixed multiple the way samples/04_streaming's own struct
// (which contained a float4x4) suggested -- a lone `uint` reflects at
// exactly 4 bytes, not 16. Rather than reverse-engineer Slang's exact
// float3/vec-alignment rule inside a push-constant block from scratch,
// this sample keeps every push-constant field a plain scalar `uint`
// (empirically confirmed to pack with zero padding, consecutive 4-byte
// fields) and puts every vector value in the storage buffer instead, whose
// StructuredBuffer element layout this codebase already has a proven-
// correct convention for (samples/03_bindless_mesh/04_streaming's own
// mat4 rows). `lightDirWorld` is the same value on every object's row
// each frame (a per-pass, not per-object, constant) -- redundant across
// rows, but simpler than a second one-row bindless buffer just for it.
struct PushConstants {
    uint transformIndex;
    uint shadowTextureIndex;
    uint shadowSamplerIndex;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(0, 0)]]
Texture2D gTextures[];

[[vk::binding(1, 0)]]
SamplerState gSamplers[];

[[vk::binding(2, 0)]]
StructuredBuffer<ObjectTransform> gTransforms[];

struct VSIn {
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct VSOut {
    float4 clipPosition : SV_Position;
    float3 worldNormal : NORMAL;
    float4 lightSpacePosition : TEXCOORD0;
};

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    ObjectTransform xf = gTransforms[0][gPush.transformIndex];

    VSOut o;
    o.clipPosition = mul(xf.mvp, float4(input.position, 1.0));
    o.worldNormal = input.normal;
    o.lightSpacePosition = mul(xf.lightMvp, float4(input.position, 1.0));
    return o;
}
