// src/rx_material/tests/test_brdf_white_furnace_gpu.cpp -- Phase 5 Stage 1
// Task 7 [#43]: the white-furnace energy-conservation test the ticket's own
// acceptance sketch names ("with energy compensation ON, integrated
// response at F0=1 ~= 1 across roughness; without it, the deficit is
// measurable").
//
// METHODOLOGY [gate matrix's own resolved calling contract, brdf.slang's
// own energyCompensation() header comment] -- ported from google/filament
// v1.75.0 (commit 0e58877c09afb1aacd09ff640f74d2adcd2a7e80, Apache-2.0)
// libs/ibl/src/CubemapIBL.cpp's `DFV_Multiscatter()` (hammersley-distributed
// importance sampling of the GGX visible-normal distribution, height-
// correlated Smith visibility -- reusing brdf.slang's OWN
// V_SmithGGXCorrelated(), already independently port-parity-verified by
// test_brdf_module_gpu.cpp), NOT the production DFG LUT (Task 9 has not
// built it yet -- see brdf.slang's own calling-contract comment). Under
// `DFV_Multiscatter`'s own construction, the accumulated `.y` term IS
// exactly `Ess(NoV, alpha)`, the F0=1 (white furnace) single-scattering
// directional albedo -- CubemapIBL.cpp's own comment: "Er() = (1 - f0) *
// DFV.x + f0 * DFV.y = mix(DFV.xxx, DFV.yyy, f0)", which reduces to
// `Er(f0=1) = DFV.y` exactly.
//
// A HONEST NOTE ON WHAT THIS TEST CAN AND CANNOT PROVE -- read before
// trusting the "compensated ~= 1" assertions below at face value:
// `energyCompensation(f0=1, dfgY=Ess) = 1 + 1*(1/Ess - 1) = 1/Ess`, so
// `Ess * energyCompensation(1, Ess) = Ess * (1/Ess) = 1` EXACTLY, by
// algebraic construction, for ANY nonzero `Ess` -- this is NOT a
// coincidence of this test's own setup; it is the whole point of the
// Kulla-Conty/Fdez-Aguera derivation the formula comes from (brdf.slang's
// own header comment cites it). A test that only checked "compensated ~=
// 1" would therefore be TAUTOLOGICAL: it would pass even if the Monte-Carlo
// `Ess` measurement itself were wrong (e.g. a broken importance sampler
// that always returned NoL=1 regardless of roughness). This test's REAL
// discriminating power comes from two INDEPENDENT, non-tautological checks
// instead: (1) `Ess` itself must be near 1.0 at near-zero roughness and
// MEASURABLY, MONOTONICALLY smaller as roughness grows (a real physical
// expectation -- rough microfacet surfaces genuinely lose energy under a
// single-scatter-only model -- that does not depend on the compensation
// formula at all); (2) the compensated response must land at EXACTLY 1.0
// to fp32-rounding-only precision (not a loose Monte-Carlo-noise
// tolerance), which DOES catch a coding bug in energyCompensation() itself
// (a sign flip or wrong operator would NOT cancel to 1). Together, (1)+(2)
// give genuine ON-vs-OFF discrimination in substance, matching the
// ticket's own acceptance line, even though the "restores to ~1" half is
// influenced by exact algebraic cancellation rather than independent
// measurement.
#include <doctest/doctest.h>

#include "brdf_gpu_fixture.h"
#include "brdf_test_harness.h"

#include <rx_rhi_vk/compute_pipeline.h>
#include <rx_shader/reflection.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <vector>

using namespace rx::brdf_test;

namespace {

struct FurnaceQueryGpu {
    float NoV;
    float alpha;
};

struct FurnaceResultGpu {
    float ess;           // DFV_Multiscatter(...).y -- the F0=1 single-scatter directional albedo.
    float compensated;   // ess * energyCompensation(float3(1,1,1), ess).x.
};

// 16384 samples/point -- matches this task's own research-session Python
// cross-check (see task-07-report.md's white-furnace numbers table), low
// enough noise that Ess's own roughness trend is unambiguous well past any
// plausible estimator-noise band.
constexpr uint32_t kNumSamples = 16384;

// Q0..Q3: NoV=1.0, alpha sweeping near-mirror -> max-rough (the ticket's
// own "across roughness" wording). Q4: an off-normal view angle at high
// roughness, proving the trend is not an NoV=1-only artifact.
const std::vector<FurnaceQueryGpu>& furnaceQueries() {
    static const std::vector<FurnaceQueryGpu> kQueries = {
        {1.0F, 0.02F},  // Q0: near-mirror.
        {1.0F, 0.30F},  // Q1.
        {1.0F, 0.70F},  // Q2.
        {1.0F, 1.00F},  // Q3: max roughness.
        {0.3F, 0.70F},  // Q4: off-normal view, high roughness.
    };
    return kQueries;
}

}  // namespace

TEST_CASE("brdf.slang white-furnace energy conservation: energyCompensation() restores F0=1 single-scatter "
          "response to 1.0 across a roughness sweep; the uncompensated deficit is measurable and monotonic") {
    auto fixture = makeBrdfGpuFixture("rx_brdf_white_furnace");
    if (!fixture.has_value()) {
        return;
    }

    auto session = createBrdfSession(RX_MATERIAL_SHADER_DIR);
    REQUIRE(session.has_value());

    // kNumSamples is spliced into the shader source textually below (Slang
    // has no C-preprocessor #define this test would otherwise use for a
    // single shared literal) -- see this file's own header comment for the
    // Filament source (CubemapIBL.cpp's DFV_Multiscatter()) this kernel
    // ports.
    std::string source = R"(
import brdf;

struct FurnaceQueryGpu { float NoV; float alpha; };
struct FurnaceResultGpu { float ess; float compensated; };

[[vk::binding(0, 1)]]
StructuredBuffer<FurnaceQueryGpu> gQueries;

[[vk::binding(1, 1)]]
RWStructuredBuffer<FurnaceResultGpu> gResults;

float2 hammersley(uint i, float iN) {
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float2(float(i) * iN, float(bits) * (1.0 / 4294967296.0));
}

float3 hemisphereImportanceSampleDggx(float2 u, float a) {
    float phi = 2.0 * kBrdfPi * u.x;
    float cosTheta2 = (1.0 - u.y) / (1.0 + (a + 1.0) * ((a - 1.0) * u.y));
    float cosTheta = sqrt(max(0.0, cosTheta2));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta2));
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

float2 dfvMultiscatter(float NoV, float alpha, uint numSamples) {
    float2 r = float2(0.0, 0.0);
    float3 V = float3(sqrt(max(0.0, 1.0 - NoV * NoV)), 0.0, NoV);
    float invN = 1.0 / float(numSamples);
    for (uint i = 0; i < numSamples; i++) {
        float2 u = hammersley(i, invN);
        float3 H = hemisphereImportanceSampleDggx(u, alpha);
        float dotVH = dot(V, H);
        float3 L = 2.0 * dotVH * H - V;
        float VoH = saturate(dotVH);
        float NoL = saturate(L.z);
        float NoH = saturate(H.z);
        if (NoL > 0.0) {
            float v = V_SmithGGXCorrelated(alpha, NoV, NoL) * NoL * (VoH / max(NoH, 1e-6));
            float Fc = pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0);
            r.x += v * Fc;
            r.y += v;
        }
    }
    return r * (4.0 / float(numSamples));
}

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID) {
    FurnaceQueryGpu q = gQueries[id.x];
    float2 dfv = dfvMultiscatter(q.NoV, q.alpha, )" +
             std::to_string(kNumSamples) + R"(u);

    float ess = dfv.y;
    float3 comp = energyCompensation(float3(1.0, 1.0, 1.0), ess);

    FurnaceResultGpu r;
    r.ess = ess;
    r.compensated = ess * comp.x;
    gResults[id.x] = r;
}
)";

    auto compiled = compileBrdfComputeModule(session->session.get(), "RxBrdfWhiteFurnace", source, "csMain");
    REQUIRE_MESSAGE(compiled.ok, compiled.diagnostics);

    auto layoutInfo = rx::shader::reflect(compiled);
    REQUIRE(layoutInfo.has_value());

    auto cache = rx::rhi::ComputePipelineCache::create(fixture->device.device(),
                                                         std::filesystem::temp_directory_path() / "rx_brdf_white_furnace.cache");
    REQUIRE(cache.has_value());
    auto pso = cache->getOrCreate(compiled.entryPointCode[0].code, *layoutInfo);
    REQUIRE(pso.has_value());

    const auto& queries = furnaceQueries();
    std::vector<uint8_t> inputBytes(queries.size() * sizeof(FurnaceQueryGpu));
    std::memcpy(inputBytes.data(), queries.data(), inputBytes.size());

    auto outputBytes = dispatchOneInOneOut(*fixture, *pso, inputBytes, queries.size() * sizeof(FurnaceResultGpu),
                                            static_cast<uint32_t>(queries.size()));
    std::vector<FurnaceResultGpu> results(queries.size());
    std::memcpy(results.data(), outputBytes.data(), outputBytes.size());

    REQUIRE(results.size() == 5);
    const double essQ0 = results[0].ess;
    const double essQ1 = results[1].ess;
    const double essQ2 = results[2].ess;
    const double essQ3 = results[3].ess;
    const double essQ4 = results[4].ess;

    // --- (1) Non-tautological physical checks on Ess itself ------------
    // Near-mirror (alpha=0.02): minimal single-scatter energy loss.
    CHECK(essQ0 == doctest::Approx(1.0).epsilon(0.01));
    // Monotonically decreasing as roughness grows (NoV=1 sweep) -- the
    // well-documented GGX/Smith single-scatter energy-loss trend.
    CHECK(essQ0 > essQ1);
    CHECK(essQ1 > essQ2);
    CHECK(essQ2 > essQ3);
    // Measurable deficit at max roughness (research-session cross-check:
    // ~0.307 at NoV=1, alpha=1.0 -- see task-07-report.md).
    CHECK(essQ3 < 0.5);
    // Off-normal view (Q4) also shows a real, non-trivial deficit (not an
    // NoV=1-only artifact).
    CHECK(essQ4 < 0.9);

    // --- (2) energyCompensation() itself is wired correctly -------------
    // Algebraically exact (Ess * (1/Ess) == 1) up to fp32 rounding only --
    // see this file's own header comment for why this is a real check on
    // the FORMULA's implementation, not on Ess's own correctness.
    for (const auto& r : results) {
        CHECK(static_cast<double>(r.compensated) == doctest::Approx(1.0).epsilon(1e-3));
    }

    // --- ON-vs-OFF discrimination [ticket's own acceptance line] --------
    // The uncompensated (OFF) response IS Ess itself; the compensated (ON)
    // response is ~1.0 per the check above. At high roughness the gap is
    // large; at near-mirror roughness it is small -- both asserted here.
    CHECK(std::abs(static_cast<double>(results[3].compensated) - essQ3) > 0.1);  // Q3: large gap.
    CHECK(std::abs(static_cast<double>(results[0].compensated) - essQ0) < 0.05); // Q0: small gap.

    vkDeviceWaitIdle(fixture->device.device());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}
