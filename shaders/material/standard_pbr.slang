// shaders/material/standard_pbr.slang -- Phase 4 Stage 1 Task 16 [D22, D28,
// gate ruling #8/RC1]: an ordinary public IMaterialShader module (zero
// special treatment) implementing glTF's metallic-roughness core: base
// color, metallic/roughness, tangent-space normal mapping (BC5
// reconstruction), occlusion, emissive, alphaMode's shader-side half (MASK
// discard -- alphaMode's PIPELINE-STATE half, BLEND/doubleSided, is D28's
// MaterialRecord fixed-function axis, never referenced here), and
// KHR_texture_transform offset/scale per texture slot.
//
// LIGHTING MODEL [BRDF baseline, gate matrix-issue08's own three-
// first-tier-reference convergence]: Lambertian diffuse (Filament's own
// documented choice over Burley -- "delivers results close enough to more
// complex models" -- and glTF-Sample-Renderer's/three.js's own shipped
// choice) + Cook-Torrance specular with a GGX normal distribution, Smith
// height-correlated visibility, and Schlick Fresnel (Filament's own
// documented baseline; glTF-Sample-Renderer's `pbr.frag`
// BRDF_specularGGX()/F_Schlick() confirm the identical shape). Plus [FG1,
// issue comment 2] an interim flat ambient/environment term (uniform color
// x occlusion) so metals never render pure black pre-IBL -- the full
// skybox+IBL environment path is registered for the techniques phase, not
// built here.
//
// TEXTURE SLOTS -- every texture-index field below is D11's bindless-
// index convention: ALWAYS a valid, real texture (Registry/TextureCache's
// own role-appropriate fallback -- white for baseColor/occlusion/
// metallicRoughness/emissive, flat-normal for normalTexture -- when the
// source glTF material has no reference for that slot at all), so this
// module carries no separate "has texture" boolean anywhere; sampling a
// role-neutral fallback multiplies/blends to exactly the "texture absent"
// behavior glTF's own factor defaults already specify.
import material;

struct StandardPbrParams {
    // --- Base color [universal glTF default: (1,1,1,1)] ---
    float4 baseColorFactor;

    // --- Metallic-roughness [universal glTF defaults: both 1.0] ---
    float metallicFactor;
    float roughnessFactor;

    // --- Normal map scale [glTF default: 1.0] ---
    float normalScale;

    // --- Occlusion strength [glTF default: 1.0] ---
    float occlusionStrength;

    // [D28] MASK's cutoff -- see unlit.slang's identical field for the full
    // "always-present conditional discard, alphaMode itself lives in
    // VkPipeline fixed-function state" rationale (MaterialFixedFunctionState,
    // material_system.h). OPAQUE/BLEND instances bind 0.0 (never discards).
    float alphaCutoff;

    // xyz = emissiveFactor [glTF default: (0,0,0)], PRE-MULTIPLIED by
    // KHR_materials_emissive_strength at bind time (the multiply this
    // module would otherwise need a fourth uniform for -- folding it into
    // the CPU-side bind avoids widening this struct for a log-don't-drop
    // extension per the gate matrix's own disposition). w unused (padding).
    float4 emissiveFactorAndPad;

    // D11 bindless indices -- see this file's own header comment.
    uint baseColorTexture;
    uint metallicRoughnessTexture;
    uint normalTexture;
    uint occlusionTexture;
    uint emissiveTexture;

    // KHR_texture_transform per texture slot [consume-now, gate ruling C4]:
    // xy = UV offset, zw = UV scale. Neutral default (0,0,1,1) per slot.
    float4 baseColorUvOffsetScale;
    float4 metallicRoughnessUvOffsetScale;
    float4 normalUvOffsetScale;
    float4 occlusionUvOffsetScale;
    float4 emissiveUvOffsetScale;
};

[[vk::binding(0, 1)]]
ParameterBlock<StandardPbrParams> gParams;

static const float kPi = 3.14159265358979323846;

// GGX alpha (roughness^2, the Disney/UE "remap" every first-tier engine
// applies before feeding roughness into D/V) is clamped away from exactly
// 0 -- an unclamped roughness=0 drives D_GGX's denominator to exactly 0 at
// NdotH==1 (a true singularity, not just a large-but-finite number), which
// would return NaN/Inf for a perfectly smooth mirror surface. 0.045 is the
// Filament-precedent minimum perceptual roughness (documented in Filament's
// own PBR chapter as the smallest value its own implementation clamps to
// for the identical reason).
static const float kMinRoughness = 0.045;

struct StandardPbr : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        // --- Base color ---------------------------------------------------
        float2 baseColorUv = v.uv * gParams.baseColorUvOffsetScale.zw + gParams.baseColorUvOffsetScale.xy;
        float4 baseColor = gParams.baseColorFactor * rx_sampleTexture(gParams.baseColorTexture, baseColorUv);

        // [D28] Always-present conditional discard -- see
        // StandardPbrParams::alphaCutoff's own comment above.
        if (baseColor.a < gParams.alphaCutoff) {
            discard;
        }

        // --- Metallic-roughness [core spec, quoted in the gate matrix:
        // "green channel contains roughness... blue channel contains
        // metalness"] --------------------------------------------------
        float2 mrUv = v.uv * gParams.metallicRoughnessUvOffsetScale.zw + gParams.metallicRoughnessUvOffsetScale.xy;
        float4 mrSample = rx_sampleTexture(gParams.metallicRoughnessTexture, mrUv);
        float roughness = clamp(gParams.roughnessFactor * mrSample.g, kMinRoughness, 1.0);
        float metallic = clamp(gParams.metallicFactor * mrSample.b, 0.0, 1.0);

        // --- Tangent-space normal mapping, BC5 two-channel + Z
        // reconstruction [D10: normal -> BC5_UNORM, "Z reconstructed in
        // shader"] -----------------------------------------------------
        float2 nUv = v.uv * gParams.normalUvOffsetScale.zw + gParams.normalUvOffsetScale.xy;
        float4 nSample = rx_sampleTexture(gParams.normalTexture, nUv);
        // [0,1] texel -> [-1,1] tangent-space XY, scaled by normalScale
        // BEFORE reconstruction (standard convention: scale perturbs the
        // tangent-plane offset, not the reconstructed unit length).
        float2 tangentXY = (nSample.rg * 2.0 - 1.0) * gParams.normalScale;
        // Radicand clamp [acceptance criterion]: a known BC5 artifact can
        // push a decoded (x,y) marginally outside the unit circle, which
        // would otherwise produce a negative radicand (NaN via sqrt) --
        // max(0, ...) clamps it to the equator (z=0) instead of failing.
        float radicand = max(0.0, 1.0 - dot(tangentXY, tangentXY));
        float3 tangentSpaceNormal = float3(tangentXY, sqrt(radicand));

        float3 geometricNormal = normalize(v.normal);
        // Gram-Schmidt re-orthogonalize against the (already normalized)
        // geometric normal -- linear interpolation across a triangle does
        // not preserve exact tangent/normal perpendicularity even when
        // both inputs were unit-length and orthogonal at every vertex.
        float3 tangent = normalize(v.tangent.xyz - geometricNormal * dot(geometricNormal, v.tangent.xyz));
        // MikkTSpace/glTF convention [acceptance criterion, sign test]:
        // bitangent = cross(normal, tangent.xyz) * tangent.w.
        float3 bitangent = cross(geometricNormal, tangent) * v.tangent.w;
        // Rows T/B/N: mul(vector, M) in Slang/HLSL treats `vector` as a ROW
        // vector against M's own rows as basis vectors -- i.e. this computes
        // exactly `tangentSpaceNormal.x * T + tangentSpaceNormal.y * B +
        // tangentSpaceNormal.z * N`, the standard TBN transform, tangent- ->
        // world-space.
        float3x3 tbn = float3x3(tangent, bitangent, geometricNormal);
        float3 N = normalize(mul(tangentSpaceNormal, tbn));

        // --- Occlusion [core spec closed-form formula, quoted in the gate
        // matrix: "1.0 + strength * (occlusionTexture - 1.0)"] ----------
        float2 occUv = v.uv * gParams.occlusionUvOffsetScale.zw + gParams.occlusionUvOffsetScale.xy;
        float occSample = rx_sampleTexture(gParams.occlusionTexture, occUv).r;
        float occlusion = 1.0 + gParams.occlusionStrength * (occSample - 1.0);

        // --- Emissive -------------------------------------------------
        float2 emUv = v.uv * gParams.emissiveUvOffsetScale.zw + gParams.emissiveUvOffsetScale.xy;
        float3 emissive = gParams.emissiveFactorAndPad.xyz * rx_sampleTexture(gParams.emissiveTexture, emUv).rgb;

        // --- Lambertian diffuse + GGX/Smith-correlated/Schlick specular,
        // one directional light [BRDF baseline, this file's own header
        // comment] --------------------------------------------------
        float3 viewDir = normalize(v.cameraPosWorld - v.worldPos);
        float3 lightDir = normalize(v.lightDirWorld);
        float3 halfVec = normalize(viewDir + lightDir);
        float NdotL = max(dot(N, lightDir), 0.0);
        float NdotV = max(dot(N, viewDir), 1e-4);  // clamped away from 0, not 0-able geometry (grazing/back-facing).
        float NdotH = max(dot(N, halfVec), 0.0);
        float VdotH = max(dot(viewDir, halfVec), 0.0);

        // Dielectric F0 = 0.04 [glTF/Disney-convention default, matching
        // this project's own already-fixed constant per the gate matrix's
        // KHR_materials_ior row]; metals use their own baseColor as F0
        // (the standard metallic-workflow lerp).
        float3 dielectricF0 = float3(0.04, 0.04, 0.04);
        float3 F0 = lerp(dielectricF0, baseColor.rgb, metallic);
        float3 diffuseColor = baseColor.rgb * (1.0 - metallic);  // metals have zero diffuse.

        float alpha = roughness * roughness;
        float alpha2 = alpha * alpha;

        // D_GGX.
        float denomD = (NdotH * NdotH) * (alpha2 - 1.0) + 1.0;
        float D = alpha2 / (kPi * denomD * denomD);

        // V_SmithGGXCorrelated -- height-correlated visibility, already
        // divides out the classic 4*NdotL*NdotV denominator (Filament's own
        // documented form, matching this file's header comment).
        float lambdaV = NdotL * sqrt(NdotV * NdotV * (1.0 - alpha2) + alpha2);
        float lambdaL = NdotV * sqrt(NdotL * NdotL * (1.0 - alpha2) + alpha2);
        float vis = 0.5 / max(lambdaV + lambdaL, 1e-5);

        // F_Schlick.
        float3 oneMinusF0 = float3(1.0, 1.0, 1.0) - F0;
        float3 fresnel = F0 + oneMinusF0 * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);

        float3 specular = D * vis * fresnel;
        float3 diffuse = diffuseColor / kPi;

        // [Fix round, coordinator ruling] `occlusion` deliberately does NOT
        // multiply direct light: the glTF spec's own occlusionTexture text
        // and all three first-tier engine references (Filament/glTF-
        // Sample-Renderer/three.js) treat ambient occlusion as an
        // INDIRECT-only term, and the gate matrix's own occlusion row
        // scopes the acceptance criterion to exactly the ambient
        // contribution below ("ambientColor x occlusionValue x
        // baseColor... the ONLY contribution"). An earlier version of this
        // file multiplied `directLight` by `occlusion` too -- spec-
        // incorrect (double-darkens a direct-lit, occluded surface) and,
        // forward-looking, would have doubled up with Task 22's own future
        // shadow maps (a REAL direct-light visibility term) once that
        // lands. Direct light is governed by NdotL/shadowing alone.
        float3 directLight = (diffuse + specular) * v.lightColor * NdotL;

        // [FG1] Interim flat ambient/environment term -- uniform color x
        // occlusion x baseColor, the EXACT closed-form the acceptance
        // criterion's non-black-metal probe checks, independent of
        // metallic/NdotL (a fully-metallic surface has zero diffuse but
        // still gets this contribution -- the whole point of FG1).
        float3 ambient = v.ambientColor * occlusion * baseColor.rgb;

        float3 color = directLight + ambient + emissive;
        return float4(color, baseColor.a);
    }
};

export struct MaterialImpl : IMaterialShader = StandardPbr;
