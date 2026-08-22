// shaders/material/standard_pbr.slang -- Phase 4 Stage 1 Task 16 [D22, D28,
// gate ruling #8/RC1] baseline, REWORKED in Phase 5 Stage 1 Task 8 [#44,
// gate rulings rulings-2026-08-20.md T7/T8] onto Task 7's `brdf.slang`
// module (D_GGX/V_SmithGGXCorrelated/F_Schlick/Fd_Lambert/
// energyCompensation -- consumed here, never reimplemented): an ordinary
// public IMaterialShader module (zero special treatment) implementing
// glTF's metallic-roughness core: base color, metallic/roughness,
// tangent-space normal mapping (BC5 reconstruction), occlusion, emissive,
// alphaMode's shader-side half (MASK discard -- alphaMode's PIPELINE-STATE
// half, BLEND/doubleSided, is D28's MaterialRecord fixed-function axis,
// never referenced here), KHR_texture_transform offset/scale per texture
// slot, and (Task 8, new) KHR_materials_ior + KHR_materials_specular's own
// dielectric F0/F90 derivation.
//
// LIGHTING MODEL [BRDF baseline]: Lambertian diffuse (`Fd_Lambert()`,
// brdf.slang) + Cook-Torrance specular with GGX distribution (`D_GGX`),
// Smith height-correlated visibility (`V_SmithGGXCorrelated`), and Schlick
// Fresnel (`F_Schlick`, explicit-f90 overload -- brdf.slang), all consumed
// from Task 7's module rather than duplicated inline (matrix-issue08's own
// three-first-tier-reference convergence on this exact baseline is
// unchanged by the rework; only WHERE the math lives changed). PLUS
// [Phase 5 Task 10, #46, FG1 closure] real image-based lighting -- diffuse
// irradiance + specular prefiltered-cubemap sampling, fed by
// `MaterialVertex::env*` (material.slang, populated from
// `rx::scene::Scene::environment()` via forward_entry.slang's own re-read
// of the draw's RxDrawData row) -- see this file's own `evaluate()` body
// for the full split-sum composition. This REPLACES [FG1, issue comment 2]
// the former interim flat ambient/environment term (`v.ambientColor *
// occlusion * baseColor.rgb`, uniform color, independent of
// roughness/metallic beyond the diffuse-only multiply) -- that field is
// RETIRED (no longer read here; see MaterialVertex::ambientColor's own
// header comment for why it stays declared, unconsumed) per the ticket's
// own "replacing... not adding a toggle" framing: a draw with no
// environment bound now gets ZERO indirect light, not a placeholder one.
//
// TASK 8 -- KHR_materials_ior / KHR_materials_specular [gate matrix rows,
// same file]: `computeDielectricF0F90()` below derives the dielectric F0/
// F90 pair from `ior`/`specularFactor`/`specularColorFactorAndPad`
// (glTF-default neutral values reproduce this file's PRE-T8 hardcoded
// `float3(0.04,0.04,0.04)` + implicit-f90=1.0 Fresnel byte-identically --
// see that function's own header comment for the closed-form proof). Kept
// in THIS file, not brdf.slang, per the gate matrix's own Open Question
// recommendation: these are glTF-EXTENSION-specific F0/f90 derivations,
// not general Filament-ported BRDF math (Filament has no equivalent to
// either extension) -- brdf.slang stays pure, reusable math; this file
// stays the glTF-vocabulary-aware consumer. [Gate ruling RC3] Neither
// extension is parsed from glTF content yet (importer plumbing is T21's/
// T23's job) -- every current producer (samples 08/09) binds the neutral
// glTF-default values, so this is real, tested, value-asserted SHADER
// math today, with non-default real-asset values arriving once T21/T23
// land.
//
// TASK 8 -- feature-permutation mechanism [gate ruling T8: "permutation
// mechanism now, no pre-added unused parameter fields"; gate matrix
// "Feature-permutation mechanism" row]: `EnergyComp` (the `extern` below)
// is brdf.slang's own `IEnergyCompensationFeature` link-time composition
// point [T7's shipped precedent, test_brdf_spirv_link_composition_gpu.cpp],
// extended here into the REAL MaterialSystem pipeline -- see
// energy_compensation_{off,on}.slang's own header comments and
// material_system.cpp's "energy-compensation variant" comment for the full
// mechanism (specializationBits, previously a dead field per T2's own
// finding, now genuinely selects distinct linked SPIR-V). `dfgY`
// (StandardPbrParams, below) is the ONE new field this mechanism costs --
// not a "future lobe" field the T8 ruling's Open Question warns against
// pre-adding, but the minimum real, testable surface the MECHANISM itself
// needs to be provably real (a non-constant-foldable input the ON variant
// genuinely reads and the OFF variant provably does not) -- every current
// producer binds 1.0 (the honest "no correction" neutral value) until
// Task 9's baked DFG LUT supplies a real per-pixel value.
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
import brdf;
import cluster_lighting;

// [Task 8] The energy-compensation permutation point -- see this file's
// own header comment above and energy_compensation_{off,on}.slang for the
// full mechanism. Left unresolved here deliberately (no default), matching
// forward_entry.slang's own `extern struct MaterialImpl : IMaterialShader;`
// precedent: MaterialSystem::create()'s two companion modules are what
// resolve it, per (material, specializationBits) at link time.
extern struct EnergyComp : IEnergyCompensationFeature;

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

    // --- [Task 8] KHR_materials_ior / KHR_materials_specular ------------
    // See computeDielectricF0F90()'s own header comment below for the full
    // formulas and the regression-guard proof that these glTF-spec
    // defaults reproduce this file's pre-T8 hardcoded Fresnel exactly.
    // `ior` default 1.5 [KHR_materials_ior]; `specularFactor` default 1.0,
    // `specularColorFactorAndPad.xyz` default (1,1,1) [KHR_materials_specular
    // -- w unused padding, same "pad vec3 to vec4" convention as
    // emissiveFactorAndPad below].
    float ior;
    float specularFactor;
    float4 specularColorFactorAndPad;

    // [Task 8] The energy-compensation feature's own dfgY input -- see
    // this file's own header comment above ("feature-permutation
    // mechanism") for why this field exists and what every current
    // producer binds it to (1.0, a neutral no-op) pending Task 9's real
    // per-pixel DFG-LUT source.
    float dfgY;

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

    // [Fix round, sampler-wrap P0] One bindless SAMPLER index per texture
    // slot above -- the actual fix for the shipped sampler-wrap defect
    // (material.slang's own rx_sampleTexture header comment has the full
    // account): every slot used to sample through ONE hardcoded
    // process-wide CLAMP_TO_EDGE sampler regardless of that texture's own
    // glTF wrap mode, which silently broke any asset relying on
    // glTF-default REPEAT for UVs outside [0,1] (DamagedHelmet's own
    // TEXCOORD_0 V, wholly in [1.0005,1.9987]). Populated at material-
    // resolution time (samples/08_gltf_viewer/main.cpp's own
    // resolveSamplerIndex()) from `TextureCache::
    // getOrCreateSamplerBindlessIndex()` -- the REAL per-texture sampler
    // this engine already built and bindless-registered from each
    // texture's own glTF sampler object (or the glTF spec's own
    // "unspecified" default -- REPEAT wrap, auto filtering -- when a
    // texture references no sampler object at all), analogous to the
    // texture-index fields immediately above.
    uint baseColorSampler;
    uint metallicRoughnessSampler;
    uint normalSampler;
    uint occlusionSampler;
    uint emissiveSampler;

    // KHR_texture_transform per texture slot [consume-now, gate ruling C4]:
    // xy = UV offset, zw = UV scale. Neutral default (0,0,1,1) per slot.
    float4 baseColorUvOffsetScale;
    float4 metallicRoughnessUvOffsetScale;
    float4 normalUvOffsetScale;
    float4 occlusionUvOffsetScale;
    float4 emissiveUvOffsetScale;
};

// --- Extension points [declared-but-gated slots, gate ruling T8's Open
// Question "grow the struct only for what it actually consumes now"] ---
// The nine glTF material extensions this ticket's own scope line names as
// "declared-but-gated" (clearcoat/anisotropy/sheen/transmission/thickness/
// attenuation/dispersion/iridescence/diffuseTransmission) do NOT get
// StandardPbrParams fields today -- see this file's own header comment
// and the gate matrix's Open Question for why (D8's "bound parameters
// change per-instance with zero recompilation" contract presumes the
// parameter block reflects what's actually wired, not a maximal
// superset; every declared-but-unused field would cost real per-instance
// storage regardless of use, the exact "pay for what you use" violation
// the ruling rejects). Clearcoat/anisotropy already have a dedicated
// code-location breadcrumb in brdf.slang's own "Extension point [Task
// 21]" comment (that lobe's own D_GGX-reuse/V_Kelemen math belongs
// there, general BRDF math, not here); the remaining seven land HERE, in
// this glTF-vocabulary-aware module, matching computeDielectricF0F90()'s
// own module-boundary reasoning above -- this is the code location a
// future implementer adding any of them finds first, per D13/D7's
// established "future task lands here" idiom:
//   - KHR_materials_transmission (Task 23, #59): a genuinely new BRDF
//     layer (light passing through, preserving specular reflection), not
//     a factor tweak on the existing lobes -- adds its own
//     transmissionFactor/transmissionTexture and a refraction-ray
//     evaluation, per RC4's own TRANSMISSION draw-partition ruling
//     (ViewLists gains a third, never-instancing-collapsed partition).
//   - KHR_materials_volume (Task 24, #60): thicknessTexture +
//     attenuationColor/attenuationDistance -- Beer-Lambert colored-glass
//     attenuation, coupled to (sequenced strictly after) the transmission
//     row above; RC3's own importer-plumbing fold-in names T24 as the
//     scope owner.
//   - KHR_materials_sheen (Task 34, #70, stretch tier): a fabric/cloth
//     retroreflective lobe ported from the Khronos Sample Viewer per
//     that ruling -- adds sheenColorFactor/sheenRoughnessFactor and a
//     second, additive specular term.
//   - KHR_materials_iridescence (Task 34, #70, stretch tier): thin-film
//     interference (soap-bubble/oil-film effect) -- a view-dependent F0
//     modulation, adds iridescenceFactor/iridescenceIor/
//     iridescenceThickness.
//   - KHR_materials_dispersion (Task 34, #70, stretch tier): chromatic
//     aberration through transmissive volumes, ported from Filament
//     v1.75.0 per that ruling -- strictly downstream of the transmission
//     row above (meaningless without it).
//   - KHR_materials_diffuse_transmission (Task 34, #70, stretch tier): a
//     live, currently-ratified extension the Phase-4 gate matrix
//     (matrix-issue08) found absent from that ticket's own named list
//     (thin, one-sided transmissive materials -- leaves, thin fabric) --
//     adds diffuseTransmissionFactor/diffuseTransmissionColorFactor.
//   - KHR_materials_emissive_strength beyond its CURRENT CPU-side
//     pre-multiply (no future task assigned -- this is this ticket's own
//     matrix row, not a registry item): `emissiveFactorAndPad` is
//     ALREADY consume-now (folded into the CPU-side bind at material-
//     resolution time, matrix-issue08's own disposition, unchanged by
//     this rework) -- the open question is purely an internal-
//     consistency one (should the multiply move on-shader for symmetry
//     with ior/specular's own on-shader derivation above?), not a
//     functional gap; flagged here, per the gate matrix's own text
//     raising it explicitly without resolving it, rather than left
//     silent.
[[vk::binding(0, 1)]]
ParameterBlock<StandardPbrParams> gParams;

// GGX alpha (roughness^2, the Disney/UE "remap" every first-tier engine
// applies before feeding roughness into D/V) is clamped away from exactly
// 0 -- an unclamped roughness=0 drives D_GGX's denominator to exactly 0 at
// NdotH==1 (a true singularity, not just a large-but-finite number), which
// would return NaN/Inf for a perfectly smooth mirror surface. 0.045 is the
// Filament-precedent minimum perceptual roughness (documented in Filament's
// own PBR chapter as the smallest value its own implementation clamps to
// for the identical reason).
static const float kMinRoughness = 0.045;

// [Task 8] The result of the KHR_materials_ior/_specular dielectric
// derivation -- see computeDielectricF0F90()'s own header comment.
struct DielectricF0F90 {
    float3 f0;
    float f90;
};

// [Task 8, gate matrix Open Question "Where does the ior->f0 / specular->
// (f0,f90) derivation live" -- RECOMMEND standard_pbr.slang, not
// brdf.slang: resolved here] Derives the DIELECTRIC F0/F90 pair from
// `ior`/`specularFactor`/`specularColorFactor` -- glTF-extension-specific
// formulas, not general Filament-ported BRDF math (Filament has no
// equivalent to either KHR_materials_ior or KHR_materials_specular). This
// is the SINGLE shared helper the gate matrix's "metal-exclusion
// enforcement" Open Question recommends, so the "the metal BRDF is not
// affected by [either] parameter" invariant [both extensions' own spec
// text] is enforced in ONE place (evaluate()'s own metallic `lerp` below),
// not duplicated per call site.
//
// KHR_materials_ior (quoted, gate matrix): `dielectric_f0 =
// ((ior-1)/(ior+1))^2` -- at the spec's own default (ior=1.5) this
// evaluates to exactly 0.04, this file's pre-T8 hardcoded constant.
// Special case (quoted): `ior=0` "forces the Fresnel term to evaluate to
// exactly 1.0 regardless of view/light angle" (a specular-glossiness
// backward-compat mode) -- modeled below as f0=f90=1.0, which makes
// `F_Schlick(1,1,VoH) == 1 + (1-1)*pow5(...) == 1.0` identically for EVERY
// VoH, satisfying the spec's own "MUST evaluate to 1.0 independently of
// view or light direction" text exactly (a real, different code path, not
// merely a different F0 constant -- per the gate matrix's own "genuinely
// different code path" framing).
//
// KHR_materials_specular (quoted, gate matrix): `dielectric_f0 =
// min(0.04 * specularColor, 1.0) * specular`, `dielectric_f90 = specular`
// (NOT 1.0 -- brdf.slang's explicit-f90 `F_Schlick` overload exists
// specifically for this, per that module's own header comment and the T7
// matrix's Fresnel-overload recommendation). Explicit interaction rule
// (quoted): "If KHR_materials_ior is used in combination with
// KHR_materials_specular, the constant 0.04 is replaced by the value
// computed from the IOR" -- applied unconditionally below (both
// extensions' formulas always compose; a source asset that sets neither
// simply supplies both extensions' own neutral defaults).
//
// REGRESSION GUARD [gate matrix acceptance criterion, ior row]: at the
// glTF-default values (ior=1.5, specularFactor=1.0,
// specularColorFactor=(1,1,1)) -- what every CURRENT producer binds, since
// neither extension is parsed from glTF content yet [gate ruling RC3] --
// this reduces to EXACTLY f0=(0.04,0.04,0.04), f90=1.0: `((1.5-1)/
// (1.5+1))^2 = (0.5/2.5)^2 = 0.2^2 = 0.04`; `min(0.04*1,1.0)*1.0 = 0.04`;
// f90=1.0. Byte-identical to this file's pre-T8 hardcoded
// `float3(0.04,0.04,0.04)` + implicit-f90=1.0 Fresnel (`F_Schlick(f0, 1.0,
// VoH)` is algebraically the SAME expression the old inline
// `F0 + (1-F0)*pow5(1-VdotH)` computed).
DielectricF0F90 computeDielectricF0F90(float ior, float specularFactor, float3 specularColorFactor) {
    if (ior <= 0.0) {
        DielectricF0F90 result;
        result.f0 = float3(1.0, 1.0, 1.0);
        result.f90 = 1.0;
        return result;
    }

    float iorTerm = (ior - 1.0) / (ior + 1.0);
    float dielectricF0Base = iorTerm * iorTerm;

    DielectricF0F90 result;
    result.f0 = min(dielectricF0Base * specularColorFactor, float3(1.0, 1.0, 1.0)) * specularFactor;
    result.f90 = specularFactor;
    return result;
}

struct StandardPbr : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        // --- Base color ---------------------------------------------------
        float2 baseColorUv = v.uv * gParams.baseColorUvOffsetScale.zw + gParams.baseColorUvOffsetScale.xy;
        float4 baseColor = gParams.baseColorFactor *
                            rx_sampleTexture(gParams.baseColorTexture, gParams.baseColorSampler, baseColorUv);

        // [D28] Always-present conditional discard -- see
        // StandardPbrParams::alphaCutoff's own comment above.
        if (baseColor.a < gParams.alphaCutoff) {
            discard;
        }

        // --- Metallic-roughness [core spec, quoted in the gate matrix:
        // "green channel contains roughness... blue channel contains
        // metalness"] --------------------------------------------------
        float2 mrUv = v.uv * gParams.metallicRoughnessUvOffsetScale.zw + gParams.metallicRoughnessUvOffsetScale.xy;
        float4 mrSample = rx_sampleTexture(gParams.metallicRoughnessTexture, gParams.metallicRoughnessSampler, mrUv);
        float roughness = clamp(gParams.roughnessFactor * mrSample.g, kMinRoughness, 1.0);
        float metallic = clamp(gParams.metallicFactor * mrSample.b, 0.0, 1.0);

        // --- Tangent-space normal mapping, BC5 two-channel + Z
        // reconstruction [D10: normal -> BC5_UNORM, "Z reconstructed in
        // shader"] -----------------------------------------------------
        float2 nUv = v.uv * gParams.normalUvOffsetScale.zw + gParams.normalUvOffsetScale.xy;
        float4 nSample = rx_sampleTexture(gParams.normalTexture, gParams.normalSampler, nUv);
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
        float occSample = rx_sampleTexture(gParams.occlusionTexture, gParams.occlusionSampler, occUv).r;
        float occlusion = 1.0 + gParams.occlusionStrength * (occSample - 1.0);

        // --- Emissive -------------------------------------------------
        float2 emUv = v.uv * gParams.emissiveUvOffsetScale.zw + gParams.emissiveUvOffsetScale.xy;
        float3 emissive = gParams.emissiveFactorAndPad.xyz *
                           rx_sampleTexture(gParams.emissiveTexture, gParams.emissiveSampler, emUv).rgb;

        // --- Lambertian diffuse + GGX/Smith-correlated/Schlick specular,
        // ONE punctual light -- Directional/Point/Spot [BRDF baseline,
        // this file's own header comment; math consumed from brdf.slang
        // -- Task 8 rework; GENERALIZED beyond directional-only by Phase
        // 5 Stage 2 Task 13, #49] --------------------------------------
        //
        // [Task 13] `v.lightType` selects the punctual-light unit/
        // attenuation model this ONE light-slot term applies -- ported
        // from `rx::scene::lightmath` (src/rx_scene/light_math.h, the
        // SAME closed-form functions a device-free C++ unit test asserts
        // against KHR_lights_punctual's own reference code, README.md,
        // Khronos glTF commit 2b29723d025a995971726f2989697cdc49b1222a).
        // Directional stays UNATTENUATED (KHR spec: "Because it is at an
        // infinite distance, the light is not attenuated") -- `v.
        // lightDirWorld` is already the constant per-pass "toward light"
        // unit vector every pre-Task-13 producer populates, so this path
        // is BYTE-IDENTICAL to the pre-Task-13 code for lightType==0
        // (every existing DrawDataGpu producer's own default).
        //
        // [Task 15's own plan text: "The lit path consumes cluster lists
        // for point/spot (directional stays direct)"] This single-light
        // term is a FOUNDATIONAL, forward-compatible mechanism: Task 14/
        // 15's clustered Forward+ path will later REUSE these exact
        // attenuation formulas per-light inside a per-pixel froxel light
        // loop, superseding (for Point/Spot only) the "one light per
        // draw" data source below with a per-pixel light LIST -- the math
        // itself does not change, only where the light's own data comes
        // from.
        float3 viewDir = normalize(v.cameraPosWorld - v.worldPos);
        float3 lightDir;
        float lightAttenuation = 1.0;
        if (v.lightType == 0u) {
            // Directional -- constant per-pass direction, no attenuation.
            lightDir = normalize(v.lightDirWorld);
        } else {
            // Point/Spot -- `lightDir` (the "toward light" unit vector
            // NdotL/the half-vector need) varies per-fragment, derived
            // from the light's own world position and this fragment's
            // own (already-per-fragment-varying) worldPos.
            float3 toLight = v.lightPositionWorld - v.worldPos;
            float distSq = max(dot(toLight, toLight), 1e-8);
            float dist = sqrt(distSq);
            lightDir = toLight / dist;

            // KHR_lights_punctual range-property window [README.md#range-
            // property, quoted verbatim: "attenuation = max(min(1.0-
            // (current_distance/range)^4,1),0)/current_distance^2"] --
            // the LITERAL, un-squared form (gate matrix's own Open
            // Question resolution: this is what the spec text, its own
            // reference implementation, AND the pinned glTF-Sample-
            // Renderer conformance target all agree on; Filament's own
            // shader squares the window term an extra time, a deliberate
            // "cinematic" softening this project does NOT port). `v.
            // lightRange <= 0.0` == "no configured range" (this project's
            // own established sentinel) -- pure inverse-square, no window.
            float rangeWindow = 1.0;
            if (v.lightRange > 0.0) {
                float ratio = dist / v.lightRange;
                float ratio4 = ratio * ratio;
                ratio4 = ratio4 * ratio4;
                rangeWindow = saturate(1.0 - ratio4);
            }
            lightAttenuation = rangeWindow / distSq;

            if (v.lightType == 2u) {
                // Spot -- additional cone attenuation [KHR reference code,
                // README.md: "float cd = dot(spotlightDir,
                // normalizedLightVector); float angularAttenuation =
                // saturate(cd*lightAngleScale+lightAngleOffset);
                // angularAttenuation *= angularAttenuation;"].
                // `normalizedLightVector` there is the vector FROM the
                // light TOWARD the fragment (the light's own travel
                // direction at this fragment) -- i.e. `-lightDir` here,
                // NOT `lightDir` itself (which points the OPPOSITE way,
                // toward the light, matching NdotL's own convention) --
                // cross-checked directly against the pinned glTF-Sample-
                // Renderer's own `getSpotAttenuation()` (punctual.glsl:
                // `actualCos = dot(normalize(spotDirection),
                // normalize(-pointToLight))` where `pointToLight =
                // light.position - worldPosition`, i.e. `-pointToLight ==
                // worldPosition - light.position`, the SAME travel-
                // direction vector).
                float cd = dot(normalize(v.lightSpotDirWorld), -lightDir);
                float angularAttenuation = saturate(cd * v.lightAngleScale + v.lightAngleOffset);
                angularAttenuation *= angularAttenuation;
                lightAttenuation *= angularAttenuation;
            }
        }
        float3 halfVec = normalize(viewDir + lightDir);
        float NdotL = max(dot(N, lightDir), 0.0);
        float NdotV = max(dot(N, viewDir), 1e-4);  // clamped away from 0, not 0-able geometry (grazing/back-facing).
        float NdotH = max(dot(N, halfVec), 0.0);
        float VdotH = max(dot(viewDir, halfVec), 0.0);

        // [Task 8] Dielectric F0/F90 via KHR_materials_ior/_specular --
        // see computeDielectricF0F90()'s own header comment. Metal
        // exclusion ["the metal BRDF is not affected by the parameter",
        // both extensions' own spec text]: `lerp` toward baseColor/1.0 at
        // metallic=1 exactly as this file's pre-T8 code already did for
        // the plain-0.04 case -- the SAME invariant, now enforced for the
        // ior/specular-derived F0/F90 too.
        DielectricF0F90 dielectric =
            computeDielectricF0F90(gParams.ior, gParams.specularFactor, gParams.specularColorFactorAndPad.xyz);
        float3 F0 = lerp(dielectric.f0, baseColor.rgb, metallic);
        float F90 = lerp(dielectric.f90, 1.0, metallic);
        float3 diffuseColor = baseColor.rgb * (1.0 - metallic);  // metals have zero diffuse.

        float alpha = roughness * roughness;

        // [Phase 5 Task 10, #46, FG1 closure] The scene environment's own
        // DFG-LUT sample -- taken ONCE, here, before either the direct- or
        // indirect-lighting specular terms below, because BOTH consume it:
        // this is the "wire the LUT through... for IBL-lit materials"
        // activation the ticket's own landed context names explicitly --
        // `gParams.dfgY`'s pre-Task-10 neutral CPU constant (1.0, "no
        // unaccounted multi-scattering loss assumed") is superseded by the
        // REAL per-pixel `dfg.y` Task 9's baked LUT supplies, for every
        // draw that has an environment bound (`hasEnvironment` below) --
        // Filament's own `pixel.dfg` is populated exactly this way
        // (`getEnergyCompensationPixelParams()`, surface_shading_lit.fs).
        // A draw with NO environment bound keeps `gParams.dfgY`'s own
        // CPU-bound value verbatim (byte-identical to every pre-Task-10
        // producer, which never populates `v.env*` at all) -- see
        // `EnergyComp`'s own header comment for why a caller must ALSO
        // request the ON specialization (`kSpecializationEnergyCompensation`,
        // material_system.h) for this real value to have any effect at all
        // (the OFF variant's `apply()` ignores its `dfgY` argument
        // entirely, by construction).
        bool hasEnvironment = v.envIrradianceCubeIndex != 0xFFFFFFFFu;
        float2 dfg = float2(0.0, 1.0);
        if (hasEnvironment) {
            dfg = rx_sampleTexture(v.envDfgLutIndex, v.envDfgSamplerIndex, float2(NdotV, 1.0 - roughness)).rg;
        }
        float dfgY = hasEnvironment ? dfg.y : gParams.dfgY;

        float D = D_GGX(alpha, NdotH);
        float vis = V_SmithGGXCorrelated(alpha, NdotV, NdotL);
        float3 fresnel = F_Schlick(F0, F90, VdotH);

        float3 specular = D * vis * fresnel;
        // [Task 8, gate ruling T7/T8] Energy-compensation permutation
        // point -- see this file's own header comment ("feature-
        // permutation mechanism") and brdf.slang's own
        // energyCompensation()/IEnergyCompensationFeature header comments
        // for the MULTIPLICATIVE CONTRACT this follows exactly: the
        // specular lobe's response only, never the diffuse lobe.
        EnergyComp energyFeature;
        specular = energyFeature.apply(specular, F0, dfgY);

        float3 diffuse = diffuseColor * Fd_Lambert();

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
        // [Task 13] `lightAttenuation` is 1.0 for Directional (unattenuated,
        // by construction above) -- byte-identical to the pre-Task-13
        // expression for every existing directional-only producer.
        // `v.lightColor` is candela for Point/Spot (see this file's own
        // punctual-light comment above) -- multiplying by the inverse-
        // square/window/cone attenuation term converts it into the SAME
        // lux-equivalent-irradiance space the diffuse/specular BRDF terms
        // already assume, matching Filament's own `light.colorIntensity.
        // rgb * light.attenuation * NoL` composition (shaders/src/
        // surface_light_punctual.fs, google/filament v1.75.0).
        float3 directLight = (diffuse + specular) * v.lightColor * lightAttenuation * NdotL;

        // [Phase 5 Stage 2 Task 15, #51] Clustered Forward+ Point/Spot --
        // ADDITIVE alongside the single-slot punctual term immediately
        // above (which stays byte-identical to its pre-Task-15 form; see
        // this file's own header comment and cluster_lighting.slang's own
        // top comment for the full "ADDITIVE, never replacing" rationale).
        // `v.clusterEnabled == 0` (every pre-Task-15 producer's own
        // default) skips this entirely -- a scene with only a directional
        // light renders BYTE-IDENTICAL to the pre-Task-15 gates [matrix-
        // p5t15's own row 1 acceptance criterion].
        if (v.clusterEnabled != 0u) {
            float3 clusteredDiffuse = float3(0.0, 0.0, 0.0);
            float3 clusteredSpecular = float3(0.0, 0.0, 0.0);
            rx_evaluateClusteredLights(v, N, viewDir, NdotV, F0, F90, diffuseColor, roughness, alpha,
                                        clusteredDiffuse, clusteredSpecular);
            // Energy compensation applied ONCE, to the accumulated total --
            // see cluster_lighting.slang's own header comment for the
            // linearity proof this relies on.
            directLight += clusteredDiffuse + energyFeature.apply(clusteredSpecular, F0, dfgY);
        }

        // [Phase 5 Task 10, #46, FG1 closure] Image-based lighting --
        // REPLACES the retired FG1 flat ambient/environment term (see
        // MaterialVertex::ambientColor's own header comment, material.slang)
        // with real diffuse irradiance + specular prefiltered-cubemap
        // sampling, per the charter's own "each lobe fed by both direct
        // lighting and IBL" requirement. Split-sum composition [Karis 2013;
        // Filament `evaluateIBL()`, shaders/src/surface_light_indirect.fs,
        // google/filament v1.75.0, :717-758 -- see brdf.slang's own
        // `iblSpecularReflectance()` header comment for the full citation,
        // corrected in the T10 fix round per task-10-review.md Finding 1]:
        //   E  = mix(dfg.x, dfg.y, f0) = (1-f0)*dfg.x+f0*dfg.y (specularDFG)
        //   Fr = E * prefilteredRadiance(R, roughness-LOD)   (specular)
        //   Fd = diffuseColor * diffuseIrradiance * (1-E)    (diffuse, energy-
        //                                                     conserving: gets
        //                                                     what specular
        //                                                     did NOT reflect)
        // A draw with no environment bound (`hasEnvironment == false`,
        // `v.envIrradianceCubeIndex` still the "no environment" sentinel)
        // contributes EXACTLY ZERO indirect light -- matching Filament's own
        // "no IndirectLight bound" behavior, a real, physically-honest
        // behavior change from FG1's own always-on placeholder (see this
        // file's own header comment and the T10 gate matrix's
        // discrimination row: "the old flat-ambient code is retired after
        // ... not 'adding a toggle'" -- there is deliberately no
        // ambientColor-shaped fallback here).
        float3 ibl = float3(0.0, 0.0, 0.0);
        if (hasEnvironment) {
            // Mirror reflection direction about N -- the SAME `NdotV` this
            // file already clamped away from 0 above, reused rather than a
            // second `dot(N, viewDir)` (negligible divergence at the
            // clamped grazing angle this guards).
            float3 R = 2.0 * NdotV * N - viewDir;
            // [prefilter_specular.slang / dfg_lut.slang's own shared
            // roughness<->coord parameterization, bake.cpp's own header
            // comment: `linearRoughness = coord*coord`, i.e.
            // `coord == sqrt(linearRoughness) == perceptualRoughness`] The
            // SAME `roughness` (perceptual, already clamped to
            // [kMinRoughness,1]) variable indexes BOTH the prefiltered
            // mip-chain's own LOD *and* the DFG LUT's own y-axis above
            // (`1.0 - roughness`) -- a mismatch between the two would
            // silently sample a roughness-inconsistent (mip, dfg) pair.
            float lod = roughness * v.envMaxPrefilteredLod;
            float3 irradianceSample =
                rx_sampleTextureCubeLod(v.envIrradianceCubeIndex, v.envCubeSamplerIndex, N, 0.0).rgb;
            float3 prefilteredSample =
                rx_sampleTextureCubeLod(v.envPrefilteredCubeIndex, v.envCubeSamplerIndex, R, lod).rgb;

            float3 E = iblSpecularReflectance(F0, dfg);
            float3 iblSpecular = energyFeature.apply(prefilteredSample * E, F0, dfgY);
            float3 iblDiffuse = diffuseColor * irradianceSample * (float3(1.0, 1.0, 1.0) - E);

            // [glTF core spec, occlusionTexture: "indirect light only" --
            // this file's own existing `directLight` comment above already
            // establishes the same "occlusion never touches direct light"
            // rule; IBL is exactly the indirect term that DOES get it,
            // matching FG1's own retired `ambient * occlusion` shape one
            // for one] `envIntensity` is PRE-EXPOSED (T4's convention,
            // RxDrawData::envIntensity's own header comment, material.slang)
            // -- no separate exposure multiply here, matching
            // `v.lightColor`'s own already-pre-exposed convention.
            ibl = (iblDiffuse + iblSpecular) * occlusion * v.envIntensity;
        }

        float3 color = directLight + ibl + emissive;
        return float4(color, baseColor.a);
    }
};

export struct MaterialImpl : IMaterialShader = StandardPbr;
