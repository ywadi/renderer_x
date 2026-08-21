// shaders/material/brdf.slang -- Phase 5 Stage 1 Task 7 [#43, gate rulings
// RC1/T7]: the composable Cook-Torrance BRDF math module the charter's own
// "Shader architecture" bullet names (docs/superpowers/specs/
// 2026-08-09-toolchain-platform-rhi-design.md:388-398) -- GGX distribution,
// height-correlated Smith visibility, Schlick Fresnel (both the plain and
// the computed-f90 forms), Lambert/Burley diffuse, and single-scattering
// energy compensation.
//
// PORT PROVENANCE -- ported from google/filament v1.75.0 (tag; commit
// 0e58877c09afb1aacd09ff640f74d2adcd2a7e80, Apache-2.0, whole-repo LICENSE
// -- Filament's shader-snippet files under shaders/src/ carry no per-file
// SPDX header of their own, unlike e.g. libs/ibl/src/CubemapIBL.cpp, which
// does; both are covered by the same repo-root LICENSE), fetched via the
// GitHub Contents API and read in full against this exact commit this
// session (RC1's phase-wide pin -- re-verified byte-identical to the
// Stage-1-research matrix's own main-HEAD citations, confirming no drift
// between 721ec80 and this pin for the functions below):
//   - shaders/src/surface_brdf.fs -- D_GGX, V_SmithGGXCorrelated, F_Schlick
//     (all three overloads), fresnel()'s computed-f90 dispatch, Fd_Lambert,
//     Fd_Burley.
//   - shaders/src/surface_shading_lit.fs -- getEnergyCompensationPixelParams()
//     (the `1.0 + f0*(1.0/dfg.y - 1.0)` closed form) and
//     surface_shading_model_standard.fs's own surfaceShading() (`color =
//     Fd + Fr * pixel.energyCompensation` -- the exact multiplicative
//     contract energyCompensation() below follows: it multiplies the
//     specular lobe's response only, never the diffuse lobe).
// Per the techniques-phase charter's explicit instruction: port from
// Filament's CURRENT shaders/ implementation, never its documentation
// prose -- every formula below was read directly from the fetched source,
// not from Filament's PBR guide/docs site.
//
// MODULE BOUNDARY [gate matrix "Composable module layout" row] -- every
// function below takes scalar/vector inputs ONLY: no ParameterBlock, no
// bindless global (gTextures/gSamplers/gDrawData), no IMaterialShader
// dependency at all. Verified by this module's own standalone-compile test
// (test_brdf_module_gpu.cpp's "brdf.slang compiles standalone" case), which
// compiles THIS FILE ALONE, importing nothing else, with zero requested
// entry points -- exactly Filament's own surface_brdf.fs precedent (pure
// math, no material/lighting state; surface_shading_lit.fs is the separate
// file that consumes PixelParams).
//
// NOT YET CONSUMED BY PRODUCTION [ticket's own scope line] -- standard_pbr.
// slang is NOT modified by this task: its existing inline D_GGX/
// V_SmithGGXCorrelated/F_Schlick math is untouched, so the existing 08/09
// gates stay byte-identical trivially (nothing that produces them changed).
// Rewiring StandardPbr onto this module (and choosing which of the
// Fresnel/diffuse variants below it actually calls) is Task 8's job
// ("StandardPBR rework onto the module architecture") -- this file exists,
// compiles standalone, and is fully tested on its own, per this ticket's
// "this task is the MATH + MODULES + MECHANISM... do not rewrite the
// production material path here beyond what proving the modules requires."
//
// fp32-ONLY, NO MEDIUMP TARGET [gate matrix row dispositions] -- Filament's
// own PREVENT_DIV0 macro/453.5 D_GGX clamp/five separate fp16-derived
// epsilons (e.g. "0.0000077 = nextafter(0.5 / MEDIUMP_FLT_MAX, 1.0) in
// fp16") are precision guards for its OPTIONAL mediump/fp16 mobile target,
// which this project does not have (Vulkan 1.3 desktop/Deck baseline, fp32
// shading throughout) -- ported with ONE shared, generously-safe fp32
// epsilon (kDiv0Epsilon below) in place of Filament's per-call fp16-tuned
// magic numbers. This is a precision-GUARD-constant decision, not a
// formula difference: standard_pbr.slang's own pre-existing
// V_SmithGGXCorrelated inline already made the identical call (max(...,
// 1e-5)), matched here rather than reinvented.
public static const float kBrdfPi = 3.14159265358979323846;

// Shared fp32 div-by-zero guard -- see this file's header comment above for
// why one constant replaces Filament's five separate fp16-derived ones.
static const float kDiv0Epsilon = 1e-5;

// [surface_brdf.fs:54-79, Walter et al. 2007, "Microfacet Models for
// Refraction through Rough Surfaces"] Numerically-stable GGX normal
// distribution via Lagrange's identity (`||N x H||^2 = 1 - NoH^2`,
// Filament's own comment) -- algebraically identical to the textbook
// `alpha^2 / (PI * (NoH^2*(alpha^2-1)+1)^2)` rearrangement (both reduce to
// the same `1 - NoH^2*(1-alpha^2)` inner term, hand-verified during this
// task's own research). `roughness` here is Filament's own convention:
// already alpha (perceptualRoughness^2), matching standard_pbr.slang's own
// `alpha = roughness * roughness` pre-multiply before it calls its
// (pre-T7, still-inline, untouched) D_GGX-equivalent math. Filament's own
// `min(..., 453.5)` fp16-overflow guard is dropped -- see this file's
// header comment (fp32 has no such overflow risk at any physically
// reachable alpha/NoH).
public float D_GGX(float alpha, float NoH) {
    float oneMinusNoHSquared = 1.0 - NoH * NoH;
    float a = NoH * alpha;
    float k = alpha / (oneMinusNoHSquared + a * a);
    float d = k * k * (1.0 / kBrdfPi);
    return d;
}

// [surface_brdf.fs:102-113, Heitz 2014, "Understanding the
// Masking-Shadowing Function in Microfacet-Based BRDFs"] Height-correlated
// Smith visibility -- already divides out the classic 4*NoL*NoV
// denominator, matching standard_pbr.slang's own pre-existing (untouched)
// inline form and CubemapIBL.cpp's own `Visibility()` (the SAME formula,
// reused verbatim by this module's own white-furnace test harness's
// Monte-Carlo DFV kernel -- test_brdf_white_furnace_gpu.cpp).
public float V_SmithGGXCorrelated(float alpha, float NoV, float NoL) {
    float a2 = alpha * alpha;
    float lambdaV = NoL * sqrt((NoV - a2 * NoV) * NoV + a2);
    float lambdaL = NoV * sqrt((NoL - a2 * NoL) * NoL + a2);
    return 0.5 / max(lambdaV + lambdaL, kDiv0Epsilon);
}

// [surface_brdf.fs:145-148 (vec3 f0 overload) and :155-157 (scalar
// overload), Schlick 1994, "An Inexpensive BRDF Model for Physically-Based
// Rendering"] Explicit-f90 Fresnel -- the general form every other
// overload below reduces to. Two overloads, matching Filament's own split:
// vec3 f0 (the real per-material call shape) and scalar f0 (Fd_Burley's
// own call shape below, which always passes f0=1.0).
//
// DISCLOSED DEVIATION [review round]: all three F_Schlick overloads below
// clamp `1.0 - VoH` to [0,1] before pow5, unlike Filament's own pinned
// `pow5(1.0 - VoH)` (unclamped) -- inherited from standard_pbr.slang's own
// pre-T7 inline convention (`pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0)`), not
// reinvented here. Provably a no-op at every call site in this codebase:
// VoH is always pre-saturated to [0,1] before reaching these functions
// (every caller computes it via `saturate(dot(...))`/`clamp(...)`), so
// `1.0 - VoH` is already in [0,1] and the clamp never fires.
public float3 F_Schlick(float3 f0, float f90, float VoH) {
    float p5 = pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0);
    return f0 + (float3(f90, f90, f90) - f0) * p5;
}

public float F_Schlick(float f0, float f90, float VoH) {
    float p5 = pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0);
    return f0 + (f90 - f0) * p5;
}

// [surface_brdf.fs:150-153] Implicit-f90=1.0 overload -- Filament's own
// FILAMENT_QUALITY_LOW tier, algebraically F_Schlick(f0, 1.0, VoH). This is
// the EXACT form standard_pbr.slang's own pre-T7 inline Fresnel already
// shipped -- kept here, named and tested, as a real, cheap fallback tier
// (e.g. a future Deck-floor low-quality path), per the gate matrix's Open
// Question recommendation. Also clamps before pow5 -- see the disclosed
// deviation note on the F_Schlick(float3,float,float) overload above.
public float3 F_Schlick(float3 f0, float VoH) {
    float f = pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0);
    return float3(f, f, f) + f0 * (1.0 - f);
}

// [surface_brdf.fs:177-186, the `FILAMENT_QUALITY >= FILAMENT_QUALITY_HIGH`
// branch of fresnel() -- gate ruling T7: THIS project's DEFAULT, not an
// optional upgrade] f90 computed from f0's own magnitude
// (`saturate(dot(f0, vec3(50.0*0.33)))`) rather than fixed at 1.0. This is
// Filament's own shipped default quality tier (the implicit-f90=1.0 form
// above is Filament's LOW tier, which is what this project's pre-T7
// standard_pbr.slang happened to ship instead -- an unintentional choice
// predating any Filament source consultation, per the gate matrix's Open
// Question).
//
// DIVERGENCE CONDITION, verified directly against this exact formula (not
// assumed from the gate matrix's own paraphrase, which frames the
// divergence as a "high-F0" case -- re-derived here and found to be the
// opposite): `f90 = saturate(dot(f0, 16.5))` saturates to EXACTLY 1.0 (byte
// identical to the implicit form) for any f0 with `f0.r+f0.g+f0.b >=
// 1/16.5 ~= 0.0606` -- true for the glTF/Disney dielectric baseline
// (0.04*3 = 0.12) and essentially every physically-plausible metal, so the
// two Fresnel forms are IDENTICAL for almost all standard PBR materials.
// The two forms only diverge for LOW total reflectance (e.g. a
// KHR_materials_specular-attenuated dielectric pushed below that ~0.0606
// sum) -- test_brdf_module_gpu.cpp's own f90-discrimination case uses
// exactly such a point (f0 = (0.01, 0.01, 0.01)), not a metal.
public float3 fresnelDefault(float3 f0, float VoH) {
    float f90 = saturate(dot(f0, float3(50.0 * 0.33, 50.0 * 0.33, 50.0 * 0.33)));
    return F_Schlick(f0, f90, VoH);
}

// [surface_brdf.fs:237-239] Lambertian diffuse -- the charter's own binding
// "Shader architecture" bullet ("lobe structure diffuse(Lambert)") and this
// project's pre-existing StandardPbr choice; also Filament's own compiled
// default (`BRDF_DIFFUSE = DIFFUSE_LAMBERT`, surface_brdf.fs:29).
public float Fd_Lambert() {
    return 1.0 / kBrdfPi;
}

// [surface_brdf.fs:241-247, Burley 2012, "Physically-Based Shading at
// Disney"] Disney's retro-reflective grazing-angle diffuse term. The
// ticket's own text defers Lambert-vs-Burley to "Task 1's spec ruling";
// no dedicated Task 1 spec document landed (docs/superpowers/specs/
// 2026-08-2x-phase5-techniques-design.md does not exist), and the
// charter's own binding "Shader architecture" bullet already names Lambert
// as the flagship material's lobe choice -- so this module ships Burley
// alongside Lambert, exported and tested on its own, without wiring it
// into any production material, per the gate matrix's own pre-specified
// disposition for this exact outcome ("brdf.slang still exposes
// Fd_Burley() as an unused, tested module function... either is
// acceptable, but silence is not").
public float Fd_Burley(float alpha, float NoV, float NoL, float LoH) {
    float f90 = 0.5 + 2.0 * alpha * LoH * LoH;
    float lightScatter = F_Schlick(1.0, f90, NoL);
    float viewScatter = F_Schlick(1.0, f90, NoV);
    return lightScatter * viewScatter * (1.0 / kBrdfPi);
}

// [surface_shading_lit.fs:230-238, "Multiple-Scattering Microfacet BSDFs
// with the Smith Model" (Heitz/Hery/Misty 2016); Fdez-Aguera's DFG-based
// derivation is the same family] Compensates the single-scattering GGX/
// Smith BRDF above for energy lost to unaccounted multiple scattering
// between microfacets -- a measurable darkening on rough, high-F0
// (metallic) surfaces the terms above under-integrate on their own.
//
// CALLING CONTRACT [gate matrix's own resolved cross-ticket dependency]:
// `dfgY` is taken as a PLAIN PARAMETER, not a global/binding -- this
// function carries NO runtime dependency on IBL infrastructure. Task 9's
// baked DFG LUT (not built yet) will eventually supply the real value at
// production call sites (via `pixel.dfg.y`, Filament's own name);
// test_brdf_white_furnace_gpu.cpp supplies a LOCALLY-computed one instead
// (a small Monte-Carlo importance-sampling kernel following
// CubemapIBL::DFV_Multiscatter's own structure), which is what lets this
// module close standalone, per the ticket's own "modules compile
// standalone" acceptance line.
//
// MULTIPLICATIVE CONTRACT [surface_shading_model_standard.fs:128, `color =
// Fd + Fr * pixel.energyCompensation`]: the caller multiplies this
// function's result into the SPECULAR lobe's response only (`Fr`), never
// the diffuse lobe (`Fd`) -- this function does not perform that multiply
// itself, matching Filament's own call-site shape exactly.
public float3 energyCompensation(float3 f0, float dfgY) {
    return float3(1.0, 1.0, 1.0) + f0 * (1.0 / dfgY - 1.0);
}

// [Phase 5 Task 10, #46, FG1 closure; corrected in the T10 fix round per
// task-10-review.md Finding 1 (CRITICAL)] Split-sum IBL specular
// reflectance scale [Karis 2013, "Real Shading in Unreal Engine 4"; ported
// here as Filament's own `specularDFG()` -- shaders/src/
// surface_light_indirect.fs, google/filament v1.75.0, commit
// 0e58877c09afb1aacd09ff640f74d2adcd2a7e80, Apache-2.0, :135]:
// `E = mix(dfg.x, dfg.y, f0) = (1-f0)*dfg.x + f0*dfg.y`, where `dfg` is the
// pre-integrated BRDF LUT texel at (NoV, roughness) sampled from T9's own
// `dfgLut` bake output -- T9's own `dfg_lut.slang` header comment (a
// byte-for-byte structural port of CubemapIBL::DFV_Multiscatter, which T9
// bakes UNCONDITIONALLY, never the plain non-multiscatter `DFV()`) already
// quotes this exact reconstruction verbatim: `"Er() = (1-f0)*DFV.x +
// f0*DFV.y = mix(DFV.xxx, DFV.yyy, f0)"` (CubemapIBL.cpp:790-833's own
// comment for `DFV_Multiscatter()`). The PRIOR version of this function
// (`f0*dfg.x + dfg.y`) was `CubemapIBL.cpp:778`'s reconstruction for the
// plain, non-multiscatter `DFV()` -- the variant T9 does NOT bake -- a
// citation/formula mismatch verified against a fresh fetch of the pinned
// v1.75.0 source and reproduced live (see task-10-report.md "Fix round 1").
// `E` is BOTH the specular lobe's own reflectance scale (`Fr = E *
// prefilteredRadiance`) AND, via `(1-E)`, the fraction of incident energy
// specular did NOT reflect that the diffuse lobe is entitled to (`Fd =
// diffuseColor * diffuseIrradiance * (1-E)`) -- a real coupling between the
// two lobes, not two independently-added terms; see standard_pbr.slang's
// own evaluateIBL()-equivalent call site for the exact composition.
// Distinct from `energyCompensation()` above (which corrects the
// DIRECT-lit specular response for energy LOST to unaccounted multiple
// scattering, `1 + f0*(1/dfg.y - 1)`) -- this function is the split-sum
// single-scattering APPROXIMATION itself, the mechanism IBL needs to turn a
// prefiltered environment sample into a physically-plausible specular
// response at all, independent of whether energy compensation is
// separately applied on top (Filament's own `evaluateIBL()` applies BOTH:
// `Fr *= pixel.energyCompensation` follows the split-sum `Fr = E *
// prefilteredRadiance` -- standard_pbr.slang's own call site does the
// same, reusing the SAME `EnergyComp` feature-permutation point Task 8
// already wired for direct lighting).
public float3 iblSpecularReflectance(float3 f0, float2 dfg) {
    return lerp(float3(dfg.x, dfg.x, dfg.x), float3(dfg.y, dfg.y, dfg.y), f0);
}

// --- Extension point [Task 21, clearcoat/anisotropy] ---------------------
// Filament's own clearcoat lobe (surface_brdf.fs's distributionClearCoat()/
// visibilityClearCoat()) reuses D_GGX above unchanged and adds exactly one
// new visibility term, V_Kelemen(LoH) = 0.25/(LoH*LoH) (Kelemen 2001, "A
// Microfacet Based Coupled Specular-Matte BRDF Model with Importance
// Sampling"), plus a fixed-IOR-1.5 F_Schlick(0.04, 1.0, LoH) Fresnel
// (surface_shading_model_standard.fs's clearCoatLobe()). Task 21 adds
// V_Kelemen()/a clearcoat dispatch HERE, verified against Filament's
// CURRENT shader code at whatever commit Task 21 pins (per the charter's
// own "never from documentation prose" instruction -- the June-2026
// clearcoat doc-vs-code discrepancy the charter names is resolved
// correctly only in shader code) -- this ticket does not resolve that
// discrepancy itself, matching D13/D7's own "code comment pointing at the
// future task" precedent.

// --- Link-time feature composition [gate ruling T7: the mechanism that
// GUARANTEES a feature's code is absent from SPIR-V when unused, verified
// at the SPIR-V level] --------------------------------------------------
// Proves the mechanism Task 8 grows into StandardPbr's own full permutation
// axis, using THIS module's own energyCompensation() term as the first
// real, working example -- not an abstract toy interface. Follows
// forward_entry.slang's own extern/export + `ISession::
// createCompositeComponentType()` + `IComponentType::link()` contract
// verbatim (already verified working in this codebase's production
// material path, not a new technique this ticket invents) -- see that
// file's own header comment for the underlying mechanism.
// test_brdf_spirv_link_composition_gpu.cpp links this interface against
// EACH conforming struct below into two SEPARATE SPIR-V modules and proves
// (via spirv-dis) that the OFF variant's compiled code contains no trace
// of the ON variant's division instruction -- the code is never generated,
// not merely dead-code-eliminated after the fact.
public interface IEnergyCompensationFeature {
    float3 apply(float3 specular, float3 f0, float dfgY);
}

public struct EnergyCompensationOn : IEnergyCompensationFeature {
    public float3 apply(float3 specular, float3 f0, float dfgY) {
        return specular * energyCompensation(f0, dfgY);
    }
};

public struct EnergyCompensationOff : IEnergyCompensationFeature {
    public float3 apply(float3 specular, float3 f0, float dfgY) {
        return specular;
    }
};
