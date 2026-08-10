// Sample 05_multipass: forward-pass fragment shader.
//
// Concatenated AFTER lit.vert.slang at compile time (see that file's own
// header comment) -- every global this file references (gPush, gTextures,
// gSamplers, gTransforms, ObjectTransform, PushConstants) is declared
// there, not here.
//
// SHADOW SAMPLING [coordinator ambiguity resolution]: manual single-tap
// depth comparison against the shadow map, sampled through the SAME
// bindless Texture2D+SamplerState convention every other sample in this
// codebase uses (no VK_COMPARE_OP-enabled VkSampler) -- `gSamplers[
// shadowSamplerIndex]` is a plain NEAREST/CLAMP_TO_EDGE sampler (see
// main.cpp's createScene()), and this shader does the depth >
// storedDepth + bias comparison itself, in ordinary float arithmetic, no
// different from sampling any other single-channel texture. A small
// constant bias (kShadowBias) pushes the compared depth slightly toward
// the light to absorb shadow-mapping self-shadowing/acne at grazing
// angles -- a fixed constant is enough for this sample's fixed, modest
// light elevation; a production shadow system would typically scale it by
// slope/texel size instead.
static const float kShadowBias = 0.0025;

float sampleShadowFactor(float4 lightSpacePosition, uint shadowTextureIndex, uint shadowSamplerIndex)
{
    // Orthographic light projection (see main.cpp's makeLightViewProj) --
    // w is always 1.0, but dividing through anyway costs nothing and keeps
    // this correct if a future edit ever made the light projection
    // perspective instead.
    float3 ndc = lightSpacePosition.xyz / lightSpacePosition.w;

    // NDC xy in [-1, 1] -> shadow-map UV in [0, 1]; Vulkan's own NDC is
    // already Y-down (see main.cpp's makeLightViewProj comment on the
    // proj[1][1] flip it applies for exactly this reason), so no extra Y
    // flip is needed here -- ndc.y already increases downward, matching
    // texture-space V.
    float2 shadowUV = ndc.xy * 0.5 + 0.5;
    float fragmentDepth = ndc.z;

    // Outside the light's own ortho frustum (can happen in --present mode,
    // whose animated light azimuth is not guaranteed to keep every point
    // of this fixed scene inside frustum bounds sized for the headless
    // pose) -- CLAMP_TO_EDGE addressing (main.cpp's sampler) already makes
    // an out-of-[0,1] sample well-defined (it just re-samples the nearest
    // edge texel) rather than needing a manual bounds check here; treating
    // it as unconditionally lit rather than trusting an edge-clamped depth
    // comparison is the simpler, still-correct choice for a sample whose
    // whole point is the shadow near the fixed headless-camera probes, not
    // exhaustive frustum-edge behavior.
    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0) {
        return 1.0;
    }

    float storedDepth = gTextures[shadowTextureIndex].Sample(gSamplers[shadowSamplerIndex], shadowUV).r;
    return (fragmentDepth - kShadowBias > storedDepth) ? 0.0 : 1.0;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    // Re-reads this object's own row -- see lit.vert.slang's header
    // comment for why `albedo`/`lightDirWorld` live here rather than in
    // gPush; gTransforms[0] is the same one bindless storage buffer both
    // shader stages of this pass share.
    ObjectTransform xf = gTransforms[0][gPush.transformIndex];

    float3 normal = normalize(input.worldNormal);
    // xf.lightDirWorld: the direction the light travels (from the light
    // toward the scene) -- the Lambert term wants the direction FROM the
    // surface TOWARD the light, i.e. the negation.
    float3 toLight = -normalize(xf.lightDirWorld.xyz);
    float lambert = max(dot(normal, toLight), 0.0);

    float shadow = sampleShadowFactor(input.lightSpacePosition, gPush.shadowTextureIndex, gPush.shadowSamplerIndex);

    // A small constant ambient term keeps a fully-shadowed surface visibly
    // non-black (distinguishable from the clear color) without needing a
    // second light or an ambient-occlusion pass this sample has no other
    // use for.
    const float kAmbient = 0.08;
    float lit = kAmbient + (1.0 - kAmbient) * lambert * shadow;

    return float4(xf.albedo.rgb * lit, 1.0);
}
