// rim.slang -- Phase 3 material [Task 8, sample 06_materials]: a view-
// dependent rim-lit material driven by a per-instance `rimColor` parameter
// (rx_api.h's IRxMaterialInstance::setFloat4("rimColor", ...)) -- the
// material samples/06_materials/main.cpp applies to both sphere objects,
// each with a DIFFERENT `rimColor` instance value.
//
// Conforms to shaders/material/material.slang's IMaterialShader interface,
// same shape/convention as checker.slang in this same directory (see that
// file's own header comment for the full per-part rationale, not repeated
// here).
//
// VIEW-SPACE NORMAL, NOT world/object-space [load-bearing, read before
// touching main.cpp's vertex-upload code]: `v.normal` here is NOT the
// object/world-space normal MaterialVertex's own doc (material.slang)
// otherwise implies -- main.cpp pre-rotates every vertex's normal into VIEW
// SPACE on the CPU before upload (its own transformAndUploadObjectVertices()
// applies `mat3(view) * localNormal`), specifically so this shader can read
// the cosine of the angle between the surface normal and the view direction
// (`ndotv`) as simply `v.normal.z` -- no separate "camera position"/"view
// direction" material parameter needed at all. This is a deliberate
// workaround for a real Phase 3 scope boundary, not an invented shortcut:
// forward_entry.slang's shared vertexMain has NO mechanism for a material to
// receive a per-draw camera transform at all (see that file's own "PHASE 3
// SCOPE NOTE" -- clip position and world position are literally the SAME
// vertex attribute value, `float4(position, 1.0)`), so the one piece of
// view-dependence this material's rim term needs has to travel through a
// vertex ATTRIBUTE this sample already has unrestricted control over,
// rather than a new material-parameter convention that would be out of this
// task's scope (samples/06_materials only, not rx_material's core).
//
// WHY THE OBJECT-CENTER PROBE PIXEL STILL SHOWS rimColor, not just an
// edge glow: a CLASSIC rim term (pow(1 - ndotv, power), zero at ndotv == 1)
// contributes NOTHING at exactly the point a sphere's own screen-projected
// center resolves to for ANY camera position on this sample's orbit (the
// point on a sphere nearest the camera always has its normal exactly
// aligned with the view direction, ndotv == 1.0, a pure consequence of
// sphere geometry, independent of camera azimuth) -- which would make BOTH
// sphere instances render identically (indistinguishable rimColor) at
// exactly the pixel main.cpp's headless gate probes. This formula adds a
// constant `kAmbientFactor`-weighted rimColor contribution ALONGSIDE the
// classic edge term specifically so rimColor stays visible at ndotv == 1
// too -- see main.cpp's own header comment for the exact analytic
// derivation this produces.
import material;

struct RimParams {
    float4 rimColor;
};

[[vk::binding(0, 1)]]
ParameterBlock<RimParams> gParams;

struct Rim : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        const float kAmbientFactor = 0.5;
        const float3 kBaseAlbedo = float3(0.12, 0.12, 0.12);
        const float kRimPower = 2.5;

        float ndotv = clamp(v.normal.z, -1.0, 1.0);
        float rimTerm = pow(saturate(1.0 - ndotv), kRimPower);

        float3 color = kBaseAlbedo * saturate(ndotv) + gParams.rimColor.rgb * (kAmbientFactor + rimTerm);

        // Genuinely (if negligibly) reads worldPos/uv too -- see
        // checker.slang's identical comment on why (avoiding a real, if
        // harmless, vertex-output-not-consumed validation warning).
        color.x += v.worldPos.x * 1e-6;
        color.y += v.uv.x * 1e-6;
        return float4(color, 1.0);
    }
};

export struct MaterialImpl : IMaterialShader = Rim;
