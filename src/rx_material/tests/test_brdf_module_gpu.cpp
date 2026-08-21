// src/rx_material/tests/test_brdf_module_gpu.cpp -- Phase 5 Stage 1 Task 7
// [#43]: the ticket's own primary port-parity gate. Every reference value
// below is computed by an INDEPENDENTLY-TYPED fp64 C++ function (a fresh
// transcription of the exact pinned Filament formula, never brdf.slang's
// own source text, never copy-pasted), compared against brdf.slang's own
// compiled-and-dispatched fp32 GPU output -- see brdf.slang's own header
// comment for the full google/filament v1.75.0 (commit
// 0e58877c09afb1aacd09ff640f74d2adcd2a7e80, Apache-2.0) provenance.
//
// TEST_CASE 1 needs no GPU device at all (Slang compilation is a host-side
// process) -- proves the ticket's own "modules compile standalone through
// the existing Slang path" acceptance line directly, via rx_shader::
// Compiler (brdf.slang imports nothing, so no custom search-path session is
// needed for this one case).
//
// TEST_CASE 2 is the port-parity table: D_GGX/V_SmithGGXCorrelated/
// F_Schlick(implicit f90)/fresnelDefault(computed f90)/Fd_Burley/Fd_Lambert
// at a 7-point table (values hand-verified against a Python transcription
// of the SAME formulas during this task's own research), all in ONE
// compute dispatch, exact-tolerance asserts.
//
// TEST_CASE 3 is the gate matrix's own literal ask (row 2, "Discrimination:
// swap in the UN-height-correlated (separable) Smith form at one table
// point and confirm the test fails"): a SEPARATE compute module, NOT
// importing brdf.slang at all, computing the textbook separable
// Smith-GGX visibility (G1(NoV)*G1(NoL)/(4*NoV*NoL)) at table point P3
// (alpha=1.0, NoV=0.1, NoL=0.2 -- the table's own most grazing/roughest
// point, where the two forms diverge by ~0.91, five orders of magnitude
// past the parity tolerance) -- proving the harness actually distinguishes
// the CORRELATED term, not just "some visibility function ran".
//
// TEST_CASE 4 is the computed-f90 discrimination case (table point P6,
// f0=0.01 grazing VoH=0.02 -- see brdf.slang's own fresnelDefault() header
// comment for why this LOW-f0 point, not a metal, is where the two Fresnel
// forms actually diverge for this exact pinned formula): asserts
// F_Schlick(f0,VoH) [implicit f90=1] and fresnelDefault(f0,VoH) [computed
// f90] differ by far more than the parity tolerance at that same point,
// using TEST_CASE 2's own dispatch output (no extra GPU work needed).
#include <doctest/doctest.h>

#include "brdf_gpu_fixture.h"
#include "brdf_test_harness.h"

#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>
#include <rx_rhi_vk/compute_pipeline.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace rx::brdf_test;

namespace {

constexpr double kPiRef = 3.14159265358979323846;

// --- Independent fp64 reference formulas [see this file's header comment]
double refDGgx(double alpha, double NoH) {
    double oneMinusNoHSq = 1.0 - NoH * NoH;
    double a = NoH * alpha;
    double k = alpha / (oneMinusNoHSq + a * a);
    return k * k * (1.0 / kPiRef);
}

double refVSmithCorrelated(double alpha, double NoV, double NoL) {
    double a2 = alpha * alpha;
    double lambdaV = NoL * std::sqrt((NoV - a2 * NoV) * NoV + a2);
    double lambdaL = NoV * std::sqrt((NoL - a2 * NoL) * NoL + a2);
    return 0.5 / std::max(lambdaV + lambdaL, 1e-5);
}

double refFSchlickImplicit(double f0, double VoH) {
    double f = std::pow(std::clamp(1.0 - VoH, 0.0, 1.0), 5.0);
    return f + f0 * (1.0 - f);
}

double refFresnelComputed(double f0, double VoH) {
    double f90 = std::clamp(49.5 * f0, 0.0, 1.0);  // dot((f0,f0,f0),(16.5,16.5,16.5)) = 49.5*f0.
    double p5 = std::pow(std::clamp(1.0 - VoH, 0.0, 1.0), 5.0);
    return f0 + (f90 - f0) * p5;
}

double refFdBurley(double alpha, double NoV, double NoL, double LoH) {
    double f90 = 0.5 + 2.0 * alpha * LoH * LoH;
    auto schlickScalar = [&](double VoX) {
        double p5 = std::pow(std::clamp(1.0 - VoX, 0.0, 1.0), 5.0);
        return 1.0 + (f90 - 1.0) * p5;
    };
    return schlickScalar(NoL) * schlickScalar(NoV) * (1.0 / kPiRef);
}

// Textbook SEPARABLE (un-height-correlated) Smith-GGX visibility -- the
// deliberately WRONG formula TEST_CASE 3 dispatches, per the matrix's own
// discrimination requirement. G1 is the standard Smith masking/shadowing
// term for a single direction; the separable combination is simply
// G1(NoV)*G1(NoL), UNLIKE V_SmithGGXCorrelated's height-correlated joint
// term above.
double refVSeparable(double alpha, double NoV, double NoL) {
    double a2 = alpha * alpha;
    auto g1 = [&](double NoX) { return 2.0 * NoX / (NoX + std::sqrt(a2 + (1.0 - a2) * NoX * NoX)); };
    return (g1(NoV) * g1(NoL)) / (4.0 * NoV * NoL);
}

struct TestPointGpu {
    float alpha;
    float NoV;
    float NoL;
    float NoH;
    float VoH;
    float f0;
};

struct TestResultGpu {
    float d;
    float v;
    float fSchlickImplicit;
    float fresnelComputed;
    float fdBurley;
    float fdLambert;
};

// 7-point table -- see this file's header comment for P3 (index 3, Smith
// discrimination) and P6 (index 6, f90 discrimination)'s special roles.
// Values hand-verified via an independent Python transcription of these
// same formulas during this task's own research (not committed -- the
// C++ refXxx() functions above ARE the checked-in reference, computed
// fresh at test-run time, not a hardcoded literal table).
const std::vector<TestPointGpu>& testPoints() {
    static const std::vector<TestPointGpu> kPoints = {
        {0.01F, 0.9F, 0.8F, 0.95F, 0.85F, 0.04F},  // P0: near-mirror, moderate angles.
        {0.09F, 0.6F, 0.5F, 0.7F, 0.5F, 0.5F},     // P1: light roughness, mid F0.
        {0.36F, 0.3F, 0.7F, 0.4F, 0.3F, 0.9F},     // P2: mid roughness, high F0 (metal-like).
        {1.00F, 0.1F, 0.2F, 0.15F, 0.1F, 0.02F},   // P3: max roughness, grazing, asymmetric NoV/NoL.
        {0.16F, 1.0F, 1.0F, 1.0F, 1.0F, 0.04F},    // P4: normal incidence sanity (VoH=1 -> F==f0 exactly).
        {0.25F, 0.5F, 0.5F, 0.99F, 0.05F, 0.08F},  // P5: near-grazing VoH, symmetric NoV/NoL.
        {0.16F, 0.5F, 0.5F, 0.5F, 0.02F, 0.01F},   // P6: LOW f0, grazing VoH -- f90 discrimination.
    };
    return kPoints;
}

constexpr const char* kParityComputeSource = R"(
import brdf;

struct TestPointGpu { float alpha; float NoV; float NoL; float NoH; float VoH; float f0; };
struct TestResultGpu { float d; float v; float fSchlickImplicit; float fresnelComputed; float fdBurley; float fdLambert; };

[[vk::binding(0, 1)]]
StructuredBuffer<TestPointGpu> gPoints;

[[vk::binding(1, 1)]]
RWStructuredBuffer<TestResultGpu> gResults;

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID) {
    TestPointGpu p = gPoints[id.x];
    float3 f0v = float3(p.f0, p.f0, p.f0);

    TestResultGpu r;
    r.d = D_GGX(p.alpha, p.NoH);
    r.v = V_SmithGGXCorrelated(p.alpha, p.NoV, p.NoL);
    r.fSchlickImplicit = F_Schlick(f0v, p.VoH).x;
    r.fresnelComputed = fresnelDefault(f0v, p.VoH).x;
    r.fdBurley = Fd_Burley(p.alpha, p.NoV, p.NoL, p.VoH);
    r.fdLambert = Fd_Lambert();
    gResults[id.x] = r;
}
)";

// Deliberately WRONG shader for TEST_CASE 3 -- separable Smith, NOT
// importing brdf.slang at all (this is the "swap in the wrong formula"
// discrimination case, not a brdf.slang code path).
constexpr const char* kSeparableSmithComputeSource = R"(
struct TestPointGpu { float alpha; float NoV; float NoL; float NoH; float VoH; float f0; };
struct TestResultGpu { float d; float v; float fSchlickImplicit; float fresnelComputed; float fdBurley; float fdLambert; };

[[vk::binding(0, 1)]]
StructuredBuffer<TestPointGpu> gPoints;

[[vk::binding(1, 1)]]
RWStructuredBuffer<TestResultGpu> gResults;

float g1Separable(float NoX, float a2) {
    return 2.0 * NoX / (NoX + sqrt(a2 + (1.0 - a2) * NoX * NoX));
}

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID) {
    TestPointGpu p = gPoints[id.x];
    float a2 = p.alpha * p.alpha;
    TestResultGpu r;
    r.v = (g1Separable(p.NoV, a2) * g1Separable(p.NoL, a2)) / (4.0 * p.NoV * p.NoL);
    r.d = 0.0;
    r.fSchlickImplicit = 0.0;
    r.fresnelComputed = 0.0;
    r.fdBurley = 0.0;
    r.fdLambert = 0.0;
    gResults[id.x] = r;
}
)";

std::optional<rx::rhi::ComputePipelineCache::Pipeline> buildPso(VkDevice device, rx::rhi::ComputePipelineCache& cache,
                                                                  const rx::shader::CompileResult& compiled) {
    if (!compiled.ok || compiled.entryPointCode.empty()) {
        return std::nullopt;
    }
    auto layoutInfo = rx::shader::reflect(compiled);
    if (!layoutInfo.has_value()) {
        return std::nullopt;
    }
    return cache.getOrCreate(compiled.entryPointCode[0].code, *layoutInfo);
}

// Runs the parity dispatch (used by both TEST_CASE 2 and TEST_CASE 4, which
// only needs its output) once per test process -- returns the raw
// TestResultGpu vector, one entry per testPoints() index.
std::optional<std::vector<TestResultGpu>> runParityDispatch(BrdfGpuFixture& fixture) {
    auto session = createBrdfSession(RX_MATERIAL_SHADER_DIR);
    if (!session.has_value()) {
        return std::nullopt;
    }
    auto compiled = compileBrdfComputeModule(session->session.get(), "RxBrdfParity", kParityComputeSource, "csMain");
    if (!compiled.ok) {
        MESSAGE("brdf parity compute compile failed: ", compiled.diagnostics);
        return std::nullopt;
    }

    auto cache = rx::rhi::ComputePipelineCache::create(fixture.device.device(),
                                                         std::filesystem::temp_directory_path() / "rx_brdf_parity.cache");
    if (!cache.has_value()) {
        return std::nullopt;
    }
    auto pso = buildPso(fixture.device.device(), *cache, compiled);
    if (!pso.has_value()) {
        return std::nullopt;
    }

    const auto& points = testPoints();
    std::vector<uint8_t> inputBytes(points.size() * sizeof(TestPointGpu));
    std::memcpy(inputBytes.data(), points.data(), inputBytes.size());

    auto outputBytes = dispatchOneInOneOut(fixture, *pso, inputBytes, points.size() * sizeof(TestResultGpu),
                                            static_cast<uint32_t>(points.size()));

    std::vector<TestResultGpu> results(points.size());
    std::memcpy(results.data(), outputBytes.data(), outputBytes.size());

    vkDeviceWaitIdle(fixture.device.device());
    CHECK_FALSE(fixture.context.hasValidationErrors());
    return results;
}

}  // namespace

TEST_CASE("brdf.slang compiles standalone -- no material.slang import, zero entry points requested") {
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());

    std::string path = std::string(RX_MATERIAL_SHADER_DIR) + "/brdf.slang";
    auto result = compiler->compileFromFile(path, {});
    INFO(result.diagnostics);
    CHECK(result.ok);
}

TEST_CASE("brdf.slang port-parity: D_GGX/V_SmithGGXCorrelated/F_Schlick/fresnelDefault/Fd_Burley/Fd_Lambert match "
          "independent fp64 references at a 7-point table") {
    auto fixture = makeBrdfGpuFixture("rx_brdf_parity");
    if (!fixture.has_value()) {
        return;
    }
    auto results = runParityDispatch(*fixture);
    REQUIRE(results.has_value());

    const auto& points = testPoints();
    REQUIRE(results->size() == points.size());

    constexpr double kEps = 1e-4;  // relative-ish tolerance, doctest::Approx's own semantics -- see this file's header.
    for (size_t i = 0; i < points.size(); ++i) {
        const auto& p = (*results)[i];
        const auto& in = points[i];
        CAPTURE(i);
        CHECK(static_cast<double>(p.d) == doctest::Approx(refDGgx(in.alpha, in.NoH)).epsilon(kEps));
        CHECK(static_cast<double>(p.v) == doctest::Approx(refVSmithCorrelated(in.alpha, in.NoV, in.NoL)).epsilon(kEps));
        CHECK(static_cast<double>(p.fSchlickImplicit) == doctest::Approx(refFSchlickImplicit(in.f0, in.VoH)).epsilon(kEps));
        CHECK(static_cast<double>(p.fresnelComputed) == doctest::Approx(refFresnelComputed(in.f0, in.VoH)).epsilon(kEps));
        CHECK(static_cast<double>(p.fdBurley) == doctest::Approx(refFdBurley(in.alpha, in.NoV, in.NoL, in.VoH)).epsilon(kEps));
        CHECK(static_cast<double>(p.fdLambert) == doctest::Approx(1.0 / kPiRef).epsilon(kEps));
    }
}

TEST_CASE("brdf.slang V_SmithGGXCorrelated discrimination: the un-height-correlated (separable) Smith form "
          "diverges measurably at P3 -- proves the harness distinguishes the correlated term") {
    auto fixture = makeBrdfGpuFixture("rx_brdf_smith_discrimination");
    if (!fixture.has_value()) {
        return;
    }

    const auto& points = testPoints();
    const TestPointGpu& p3 = points[3];  // alpha=1.0, NoV=0.1, NoL=0.2 -- most grazing/roughest point.

    auto session = createBrdfSession(RX_MATERIAL_SHADER_DIR);
    REQUIRE(session.has_value());
    auto compiled = compileBrdfComputeModule(session->session.get(), "RxBrdfSeparableSmith",
                                              kSeparableSmithComputeSource, "csMain");
    REQUIRE(compiled.ok);

    auto cache = rx::rhi::ComputePipelineCache::create(
        fixture->device.device(), std::filesystem::temp_directory_path() / "rx_brdf_separable_smith.cache");
    REQUIRE(cache.has_value());
    auto pso = buildPso(fixture->device.device(), *cache, compiled);
    REQUIRE(pso.has_value());

    std::vector<uint8_t> inputBytes(sizeof(TestPointGpu));
    std::memcpy(inputBytes.data(), &p3, sizeof(TestPointGpu));
    auto outputBytes = dispatchOneInOneOut(*fixture, *pso, inputBytes, sizeof(TestResultGpu), 1);
    TestResultGpu result{};
    std::memcpy(&result, outputBytes.data(), sizeof(TestResultGpu));

    double correlatedRef = refVSmithCorrelated(p3.alpha, p3.NoV, p3.NoL);
    double separableGpu = static_cast<double>(result.v);
    double separableRef = refVSeparable(p3.alpha, p3.NoV, p3.NoL);

    // The wrong shader's own GPU output matches ITS OWN (also wrong)
    // reference, confirming this is a real, sane computation, not a NaN/
    // garbage artifact that would trivially "differ" from anything.
    CHECK(separableGpu == doctest::Approx(separableRef).epsilon(1e-4));
    // ...and diverges from the CORRELATED reference by far more than the
    // parity tolerance above (5+ orders of magnitude here: ~0.91 vs 1e-4).
    CHECK(std::abs(separableGpu - correlatedRef) > 0.05);

    vkDeviceWaitIdle(fixture->device.device());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("brdf.slang fresnelDefault() (computed f90) discrimination: diverges measurably from the implicit-f90=1 "
          "form at a low-f0, grazing-angle point (P6)") {
    auto fixture = makeBrdfGpuFixture("rx_brdf_f90_discrimination");
    if (!fixture.has_value()) {
        return;
    }
    auto results = runParityDispatch(*fixture);
    REQUIRE(results.has_value());

    const auto& p6 = (*results)[6];
    double implicitGpu = static_cast<double>(p6.fSchlickImplicit);
    double computedGpu = static_cast<double>(p6.fresnelComputed);

    // Both already independently matched their own fp64 references in
    // TEST_CASE 2's own general parity loop -- this case's own job is
    // proving they diverge from EACH OTHER by far more than that shared
    // parity tolerance (1e-4): expected diff here is ~0.456.
    CHECK(std::abs(implicitGpu - computedGpu) > 0.05);
}
