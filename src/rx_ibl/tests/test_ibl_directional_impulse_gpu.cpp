// rx_ibl/tests/test_ibl_directional_impulse_gpu.cpp -- Phase 5 Stage 1
// Task 9 [#45, gate matrix-p5t09-ibl-bake-chain.md acceptance sketch].
//
// REVIEW-ROUND CORRECTION (task-09-review.md, Verdict 1 / OR-adjudication):
// the plan (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:384-386`)
// and the ticket (`gh issue view 45`) both read: "SH/irradiance VALUES
// asserted against analytic ground truth (uniform white environment ->
// known coefficients/irradiance; directional impulse -> known lobe)." The
// task-09-report.md's original "Scope note" characterized this as an
// OR-alternative ("uniform white environment... OR directional impulse")
// and treated the uniform-environment proof (test_ibl_analytic_gpu.cpp)
// as satisfying the whole line. That characterization was WRONG: the
// clause uses ";", the same semicolon-list convention the very next
// acceptance-sketch bullet uses for three items that are unambiguously
// ALL required (never rescued by an "or" reading), and nothing in the T9
// per-ticket ruling narrows this line. Both proofs are required. This
// file is the second one -- see task-09-report.md's own "Addendum"
// section (appended this round) for the corrected framing and the
// reviewer's own empirically-proven reason the uniform-environment proof
// alone cannot stand in for it.
//
// WHY THE UNIFORM-ENVIRONMENT PROOF CANNOT SUBSTITUTE (reviewer's own
// finding, re-derived here): roughnessFilter()'s estimator is
// `result = sum(L(dir_i)*w_i) / sum(w_i)` for ANY single per-sample
// weight function w_i (here, w_i = NoL_i) -- for a spatially UNIFORM
// L(dir)=L0, this collapses to EXACTLY L0 regardless of what w_i
// actually is, correct or buggy, AS LONG AS THE SAME w_i APPEARS IN BOTH
// SUMS. A bug that changes the EFFECTIVE per-sample weighting (e.g. the
// reviewer's own reproduced sabotage, `weight += noL*noL` instead of
// `weight += noL` -- an asymmetric numerator/denominator NoL exponent)
// changes the SHAPE of the resulting lobe (its roughness-to-angular-width
// mapping) while leaving per-mip brightness MONOTONICITY intact (rougher
// still blurs more, in whatever now-wrong way) and leaving the
// uniform-environment integral's own algebra untouched (a uniform
// environment cannot distinguish "weighted by NoL" from "weighted by
// NoL squared" -- both still integrate to exactly L0). Only a test that
// measures the ANGULAR PROFILE of a NON-uniform, spatially LOCALIZED
// source can catch this class of bug. This file is exactly that test.
//
// METHOD: a small (8 degree half-angle) bright "impulse" patch on the
// BASE cubemap, centered at a KNOWN direction L0=(0,0,1) (+Z, the SAME
// canonical axis test_ibl_cube_face_convention_gpu.cpp already proved
// this codebase's own face convention resolves correctly for -- this
// file therefore does not need to re-litigate face/direction convention,
// only the ANGULAR WIDTH of the resulting prefiltered lobe). Prefiltered
// at 3 mip levels (skipping mip 0's own literal-passthrough special
// case, already covered exactly by test_ibl_analytic_gpu.cpp's own
// mip-0-vs-source proof). The REAL, shipped prefilteredCubemap is probed
// via genuine hardware TextureCube sampling (same idiom as
// test_ibl_cube_face_convention_gpu.cpp's own probe kernel) at several
// angular offsets from L0 along one meridian, at each mip. Each probed
// value is compared against an INDEPENDENT, DOUBLE-PRECISION, high-
// sample-count (200,000) CPU re-implementation of EXACTLY roughnessFilter()'s
// own estimator (hammersley()/hemisphereImportanceSampleDggx()/tangent-
// basis/reflect, re-derived from the SAME pinned Filament v1.75.0
// CubemapIBL.cpp citations already established in this module's other
// kernels -- written fresh here, not copy-pasted from the shader file,
// matching this codebase's own established "independent CPU oracle"
// methodology (T4's exposure formula tables, T7's white-furnace
// Monte-Carlo, T8's ior/specular furnace test all use exactly this
// shape: re-implement the SAME pinned-source formula independently,
// never trust the file under test as its own oracle).
#include "ibl_gpu_fixture.h"

#include <doctest/doctest.h>
#include <rx_ibl/bake.h>
#include <rx_rhi_vk/compute_pipeline.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

using namespace rx::ibl_test;

namespace {

constexpr double kPiD = 3.14159265358979323846;

struct Double3 {
    double x = 0.0, y = 0.0, z = 0.0;
    Double3 operator+(const Double3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Double3 operator-(const Double3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Double3 operator*(double s) const { return {x * s, y * s, z * s}; }
};
double dot(const Double3& a, const Double3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Double3 cross(const Double3& a, const Double3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Double3 normalize(const Double3& v) {
    double len = std::sqrt(dot(v, v));
    return len > 0.0 ? Double3{v.x / len, v.y / len, v.z / len} : v;
}

// [CubemapIBL.cpp, utilities.h] hammersley(), re-derived independently
// (bit-for-bit identical algorithm to every Slang kernel's own copy, but
// written fresh in C++ double precision here -- not copy-pasted).
std::pair<double, double> hammersley(uint32_t i, double invN) {
    uint32_t bits = i;
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return {static_cast<double>(i) * invN, static_cast<double>(bits) * (1.0 / 4294967296.0)};
}

// [CubemapIBL.cpp:50-57] hemisphereImportanceSampleDggx(), re-derived
// independently in double precision.
Double3 hemisphereImportanceSampleDggx(double u1, double u2, double a) {
    double phi = 2.0 * kPiD * u1;
    double cosTheta2 = (1.0 - u2) / (1.0 + (a + 1.0) * ((a - 1.0) * u2));
    cosTheta2 = std::max(0.0, cosTheta2);
    double cosTheta = std::sqrt(cosTheta2);
    double sinTheta = std::sqrt(std::max(0.0, 1.0 - cosTheta2));
    return {sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
}

// Independent double-precision re-implementation of
// CubemapIBL::roughnessFilter()'s own per-texel estimator
// (CubemapIBL.cpp:344-465, the linearRoughness>0 branch: online
// accum/weight running sum, tangent basis, H->L reflection about N==V)
// -- for a caller-supplied environment function `envFn` (here, the
// impulse-patch membership test), NOT the GPU shader file, evaluated at
// a much larger sample count than the GPU's own per-pixel budget for a
// low-noise reference.
template <typename EnvFn>
double referenceRoughnessFilter(const Double3& n, double linearRoughness, uint32_t numSamples, EnvFn envFn) {
    Double3 up = std::abs(n.z) < 0.999 ? Double3{0, 0, 1} : Double3{1, 0, 0};
    Double3 tangentX = normalize(cross(up, n));
    Double3 tangentY = cross(n, tangentX);

    double accum = 0.0;
    double weight = 0.0;
    const double invN = 1.0 / static_cast<double>(numSamples);
    for (uint32_t i = 0; i < numSamples; ++i) {
        auto [u1, u2] = hammersley(i, invN);
        Double3 hs = hemisphereImportanceSampleDggx(u1, u2, linearRoughness);
        Double3 h = tangentX * hs.x + tangentY * hs.y + n * hs.z;
        double noH = dot(n, h);
        Double3 l = h * (2.0 * noH) - n;
        double noL = dot(n, l);
        if (noL > 0.0) {
            accum += envFn(l) * noL;
            weight += noL;
        }
    }
    return weight > 0.0 ? accum / weight : envFn(n);
}

// Impulse-patch environment: bright inside `patchHalfAngleRad` of `l0`,
// dark elsewhere.
struct ImpulsePatchEnv {
    Double3 l0;
    double patchHalfAngleRad;
    double brightValue;

    double operator()(const Double3& dir) const {
        double cosAngle = dot(dir, l0);
        double cosHalf = std::cos(patchHalfAngleRad);
        return cosAngle >= cosHalf ? brightValue : 0.0;
    }
};

// Duplicated getDirectionForFace() -- see any other test file in this
// suite for why (this codebase's own established per-file-duplicated-
// helper idiom); double precision here since it feeds the CPU-side
// fixture builder, which needs to agree EXACTLY with the GPU's own fp32
// version only up to texel-membership (a boolean), not bit-for-bit.
Double3 getDirectionForFaceD(uint32_t face, double u, double v) {
    double cx = u * 2.0 - 1.0;
    double cy = 1.0 - v * 2.0;
    Double3 dir;
    if (face == 0) {
        dir = {1.0, cy, -cx};
    } else if (face == 1) {
        dir = {-1.0, cy, cx};
    } else if (face == 2) {
        dir = {cx, 1.0, -cy};
    } else if (face == 3) {
        dir = {cx, -1.0, cy};
    } else if (face == 4) {
        dir = {cx, cy, 1.0};
    } else {
        dir = {-cx, cy, -1.0};
    }
    return normalize(dir);
}

std::vector<float> buildImpulsePatchCubeFaces(uint32_t dim, const Double3& l0, double patchHalfAngleDeg,
                                                float brightValue) {
    const double patchHalfAngleRad = patchHalfAngleDeg * kPiD / 180.0;
    const double cosHalf = std::cos(patchHalfAngleRad);
    std::vector<float> px(static_cast<size_t>(dim) * dim * 4 * 6, 0.0F);
    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t y = 0; y < dim; ++y) {
            for (uint32_t x = 0; x < dim; ++x) {
                double u = (static_cast<double>(x) + 0.5) / dim;
                double v = (static_cast<double>(y) + 0.5) / dim;
                Double3 dir = getDirectionForFaceD(face, u, v);
                bool inPatch = dot(dir, l0) >= cosHalf;
                size_t idx = ((static_cast<size_t>(face) * dim + y) * dim + x) * 4;
                float val = inPatch ? brightValue : 0.0F;
                px[idx + 0] = val;
                px[idx + 1] = val;
                px[idx + 2] = val;
                px[idx + 3] = 1.0F;
            }
        }
    }
    return px;
}

// --- GPU probe: samples the REAL prefilteredCubemap via hardware
// TextureCube at explicit (direction, mip) query points -- same idiom as
// test_ibl_cube_face_convention_gpu.cpp's own probe kernel. -------------
constexpr const char* kProbeSource = R"(
[[vk::binding(0, 1)]] TextureCube<float4> gPrefiltered;
[[vk::binding(1, 1)]] SamplerState gSampler;
[[vk::binding(2, 1)]] StructuredBuffer<float4> gQueries; // xyz=direction, w=mip level
[[vk::binding(3, 1)]] RWStructuredBuffer<float4> gResults;

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id: SV_DispatchThreadID) {
    float4 q = gQueries[id.x];
    gResults[id.x] = gPrefiltered.SampleLevel(gSampler, q.xyz, q.w);
}
)";

struct ProbeKernel {
    rx::rhi::ComputePipelineCache::Pipeline pso;
    VkDescriptorSet emptySet0 = VK_NULL_HANDLE;
};

std::optional<ProbeKernel> buildProbeKernel(rx::shader::Compiler& compiler, rx::rhi::ComputePipelineCache& cache,
                                              VkDevice device, VkDescriptorPool pool) {
    auto compiled = compiler.compileFromSource("RxIblImpulseProbe", kProbeSource, {"csMain"});
    if (!compiled.ok) {
        MESSAGE("probe compile failed: ", compiled.diagnostics);
        return std::nullopt;
    }
    auto layoutInfo = rx::shader::reflect(compiled);
    if (!layoutInfo.has_value()) {
        return std::nullopt;
    }
    auto pso = cache.getOrCreate(compiled.entryPointCode[0].code, *layoutInfo);
    if (!pso.has_value() || pso->setLayouts.empty()) {
        return std::nullopt;
    }
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &pso->setLayouts[0];
    VkDescriptorSet emptySet0 = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &allocInfo, &emptySet0) != VK_SUCCESS) {
        return std::nullopt;
    }
    return ProbeKernel{*pso, emptySet0};
}

}  // namespace

TEST_CASE("IBL Task 9: directional impulse -> known GGX lobe (angular direction + roughness-dependent width) -- "
          "the plan/ticket's own SECOND required proof, not an alternative to the uniform-environment one") {
    auto fx = makeIblFixture("rx_ibl_directional_impulse");
    if (!fx.has_value()) {
        return;
    }

    constexpr Double3 kL0{0.0, 0.0, 1.0};  // +Z -- canonical axis, face convention already proven elsewhere.
    constexpr double kPatchHalfAngleDeg = 8.0;
    constexpr float kBrightValue = 1.0F;
    // 256, not this module's own more usual small (32-64) test
    // resolutions -- LOAD-BEARING, found empirically this round: at 64,
    // the impulse patch's own boundary is coarsely TEXEL-DISCRETIZED (a
    // ~9-texel-diameter jagged circle, not a smooth one), which makes the
    // GPU's actual baked input measurably DIFFERENT from the CPU
    // reference's idealized continuous-circle membership test (`dot(dir,
    // l0) >= cosHalf`) -- a discretization mismatch BETWEEN THIS TEST'S
    // OWN fixture and its own oracle, not a bug in the bake -- observed
    // directly as a systematic ~5-10% gap between GPU and CPU-reference
    // values that persisted even at specularSamples=4096 (ruling out
    // sampling noise as the cause) and vanished at this resolution
    // (residual gap dropped to <1%, matching genuine sampling noise).
    constexpr uint32_t kDim = 256;

    auto pixels = buildImpulsePatchCubeFaces(kDim, kL0, kPatchHalfAngleDeg, kBrightValue);
    auto source = uploadTestSource(*fx, kDim, kDim, 6, pixels);
    REQUIRE(source.has_value());

    rx::ibl::BakeParams params;
    params.baseCubemapFaceSize = kDim;
    params.irradianceFaceSize = 8;
    params.irradianceSamples = 64;
    params.prefilteredMipCount = 5;
    params.prefilteredBaseFaceSize = kDim;
    params.specularSamples = 4096;  // generous -- shrinks GPU-side MC noise enough to cleanly separate it from a
                                      // real formula bug's own systematic bias (tuned empirically this round -- see
                                      // task-09-report.md's own "Addendum" for the measured before/after numbers).
    params.dfgLutSize = 16;
    params.dfgSamples = 64;

    auto result = rx::ibl::bakeEnvironment(fx->device, fx->allocator, *fx->cmdCtx, *fx->scheduler, *source, true,
                                             RX_IBL_SHADER_DIR, params, nullptr, "test_directional_impulse");
    REQUIRE(result.has_value());

    // roughnessFilter's own mip<->roughness mapping (dfg_lut.slang/
    // bake.cpp's own comment: coord=mip/(N-1), linearRoughness=coord^2) --
    // duplicated here so this test's own CPU reference uses EXACTLY the
    // alpha the production bake used for each mip, not a re-guessed one.
    auto linearRoughnessForMip = [&](uint32_t mip) {
        double coord = params.prefilteredMipCount > 1
                           ? static_cast<double>(mip) / static_cast<double>(params.prefilteredMipCount - 1)
                           : 0.0;
        return coord * coord;
    };

    // Offset angles (degrees) from L0, probed along the +X meridian
    // (direction = (sin(theta), 0, cos(theta)) -- a rotation of L0
    // in the XZ-plane, so theta=0 IS exactly L0).
    constexpr std::array<double, 6> kOffsetDeg{0.0, 8.0, 16.0, 24.0, 35.0, 50.0};
    constexpr std::array<uint32_t, 3> kMips{1, 2, 3};

    // Build the query buffer: for each mip, for each offset angle.
    struct Query {
        double offsetDeg;
        uint32_t mip;
        Double3 dir;
    };
    std::vector<Query> queries;
    for (uint32_t mip : kMips) {
        for (double offDeg : kOffsetDeg) {
            double theta = offDeg * kPiD / 180.0;
            Double3 dir{std::sin(theta), 0.0, std::cos(theta)};
            queries.push_back({offDeg, mip, dir});
        }
    }

    // --- GPU probe: real hardware TextureCube.SampleLevel reads of the
    // REAL, shipped prefilteredCubemap. ---------------------------------
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());
    auto cache = rx::rhi::ComputePipelineCache::create(
        fx->device.device(), std::filesystem::temp_directory_path() / "rx_ibl" / "test_directional_impulse_probe.pipeline_cache");
    REQUIRE(cache.has_value());

    std::array<VkDescriptorPoolSize, 3> poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 2},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 4;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorPool(fx->device.device(), &poolInfo, nullptr, &pool) == VK_SUCCESS);

    auto probeKernel = buildProbeKernel(*compiler, *cache, fx->device.device(), pool);
    REQUIRE(probeKernel.has_value());

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0F;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    REQUIRE(vkCreateSampler(fx->device.device(), &samplerInfo, nullptr, &sampler) == VK_SUCCESS);

    std::vector<float> queryBuf(queries.size() * 4);
    for (size_t i = 0; i < queries.size(); ++i) {
        queryBuf[i * 4 + 0] = static_cast<float>(queries[i].dir.x);
        queryBuf[i * 4 + 1] = static_cast<float>(queries[i].dir.y);
        queryBuf[i * 4 + 2] = static_cast<float>(queries[i].dir.z);
        queryBuf[i * 4 + 3] = static_cast<float>(queries[i].mip);
    }
    auto queryStaging = fx->allocator.createHostVisibleBuffer(queryBuf.size() * sizeof(float),
                                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(queryStaging.has_value());
    std::memcpy(queryStaging->mappedData(), queryBuf.data(), queryBuf.size() * sizeof(float));
    queryStaging->flush();

    auto resultsBuf = fx->allocator.createHostVisibleBuffer(
        queries.size() * 4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(resultsBuf.has_value());

    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = pool;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &probeKernel->pso.setLayouts[1];
    VkDescriptorSet set1 = VK_NULL_HANDLE;
    REQUIRE(vkAllocateDescriptorSets(fx->device.device(), &setAlloc, &set1) == VK_SUCCESS);

    VkDescriptorImageInfo cubeInfo{sampler, result->prefilteredCubemap.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo samplerWrite{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkDescriptorBufferInfo queryInfo{queryStaging->handle(), 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo resultsInfo{resultsBuf->handle(), 0, VK_WHOLE_SIZE};
    std::array<VkWriteDescriptorSet, 4> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set1;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &cubeInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set1;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo = &samplerWrite;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set1;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &queryInfo;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set1;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &resultsInfo;
    vkUpdateDescriptorSets(fx->device.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    fx->cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probeKernel->pso.pipeline);
        std::array<VkDescriptorSet, 2> sets{probeKernel->emptySet0, set1};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probeKernel->pso.layout, 0,
                                 static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        vkCmdDispatch(cmd, static_cast<uint32_t>(queries.size()), 1, 1);

        VkMemoryBarrier2 toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        toHost.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toHost.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &toHost;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    resultsBuf->invalidate();
    std::vector<float> gpuResults(queries.size() * 4);
    std::memcpy(gpuResults.data(), resultsBuf->mappedData(), gpuResults.size() * sizeof(float));

    // --- CPU high-precision reference + comparison. ---------------------
    constexpr uint32_t kRefSamples = 2000000;
    ImpulsePatchEnv env{kL0, kPatchHalfAngleDeg * kPiD / 180.0, static_cast<double>(kBrightValue)};

    // Tolerances TUNED EMPIRICALLY this round (not guessed) -- see
    // task-09-report.md's own "Addendum" for the measured numbers this
    // was calibrated against: a first attempt (specularSamples=1024,
    // kAbsTol=0.05, kRelTol=0.25) passed even against the reviewer's own
    // reproduced sabotage (`weight += noL*noL`, an asymmetric NoL
    // exponent) because the sabotage's own systematic bias (measured
    // ~20-32% relative at the mid/high-roughness mips) fell inside that
    // tolerance's own absolute floor for the small reference values
    // involved. Raising specularSamples 1024->4096 and kRefSamples
    // 200000->2000000 shrinks GENUINE Monte-Carlo noise (measured
    // unsabotaged relative error dropped to a consistent low single-digit
    // percentage after this change -- see the report); this tolerance
    // pair sits cleanly between that reduced noise floor and the
    // sabotage's own unchanged (it is a formula bug, not sampling noise)
    // bias.
    constexpr double kAbsTol = 0.003;
    constexpr double kRelTol = 0.08;

    std::vector<double> peakAtMip(kMips.size(), -1.0);
    for (size_t qi = 0; qi < queries.size(); ++qi) {
        const auto& q = queries[qi];
        double linearRoughness = linearRoughnessForMip(q.mip);
        double reference = referenceRoughnessFilter(q.dir, linearRoughness, kRefSamples, env);
        double measured = static_cast<double>(gpuResults[qi * 4 + 0]);  // R channel (all channels identical here).

        double tol = std::max(kAbsTol, kRelTol * reference);
        double diff = std::abs(measured - reference);
        INFO("mip=", q.mip, " linearRoughness=", linearRoughness, " offset=", q.offsetDeg, "deg measured=", measured,
             " reference=", reference, " |diff|=", diff, " tol=", tol);
        CHECK(diff <= tol);

        for (size_t mi = 0; mi < kMips.size(); ++mi) {
            if (kMips[mi] == q.mip && q.offsetDeg == 0.0) {
                peakAtMip[mi] = measured;
            }
        }
    }

    // --- DIRECTION check (reviewer's own explicit ask): the lobe's peak
    // stays centered at L0 (offset 0 deg) -- for EVERY tested mip, the
    // offset=0 measured value is >= every other offset's measured value
    // at that SAME mip (a real, GPU-measured ordering property, not
    // merely asserted from the reference). ------------------------------
    for (uint32_t mip : kMips) {
        double atZero = -1.0;
        double maxOther = -1.0;
        for (size_t qi = 0; qi < queries.size(); ++qi) {
            if (queries[qi].mip != mip) {
                continue;
            }
            double v = static_cast<double>(gpuResults[qi * 4 + 0]);
            if (queries[qi].offsetDeg == 0.0) {
                atZero = v;
            } else {
                maxOther = std::max(maxOther, v);
            }
        }
        INFO("mip=", mip, " peak-at-L0=", atZero, " max-elsewhere=", maxOther);
        CHECK(atZero >= maxOther);
    }

    CHECK_FALSE(fx->context.hasValidationErrors());

    vkDeviceWaitIdle(fx->device.device());
    vkDestroySampler(fx->device.device(), sampler, nullptr);
    vkDestroyDescriptorPool(fx->device.device(), pool, nullptr);
}
