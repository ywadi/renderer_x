// rx_ibl/tests/test_ibl_energy_compensation_gpu.cpp -- Phase 5 Stage 1
// Task 9 [#45]: the ticket's own "energy-compensation activation test"
// acceptance line -- "T8's variant with the REAL LUT (white-furnace-with-
// multiscatter now closes to ~1.0 where single-scatter loses energy)".
//
// Task 7 (test_brdf_white_furnace_gpu.cpp) and Task 8
// (test_standard_pbr_energy_compensation_gpu.cpp) already proved the
// Kulla-Conty identity `compensated = dfgY * energyCompensation(f0=1,
// dfgY).x == 1` (algebraically exact for f0=1, ANY dfgY) against a
// LOCALLY-COMPUTED dfgY -- brdf.slang's own energyCompensation() had no
// real backed source until this task. This test's job is narrower and
// different: prove the REAL PRODUCTION `energyCompensation()` function
// [shaders/material/brdf.slang], fed THIS TASK'S OWN REAL BAKED DFG LUT
// value (not a synthetic/local one), still closes the SAME identity --
// i.e. the bake chain's output is genuinely CONSUMABLE by the waiting
// production function, activating it with a backed source for the first
// time. Numerical correctness of the baked dfgY VALUE itself is proved
// separately (test_ibl_analytic_gpu.cpp's DFG closed-form test, which
// DOES discriminate against a wrong value); this test proves the WIRING.
//
// A ROUGH, high-F0 (metal) probe is used specifically -- the DFG LUT's
// SMOOTHEST row (linearRoughness~0) has dfgY~1 (see the analytic test's
// own derivation: single-scattering already conserves ~all energy at low
// roughness), which would make "single-scatter loses energy" vacuously
// true-by-near-1 rather than a real, measurable loss -- the ROUGHEST row
// (linearRoughness~1) is where the Ess deficit this mechanism exists to
// correct is actually substantial.
#include "ibl_gpu_fixture.h"

#include <doctest/doctest.h>
#include <rx_ibl/bake.h>
#include <rx_rhi_vk/compute_pipeline.h>
#include <rx_shader/reflection.h>

#include <slang-com-ptr.h>
#include <slang.h>

#include <array>
#include <cstring>
#include <vector>

using namespace rx::ibl_test;

namespace {

// Minimal duplicate of rx_material/tests/brdf_test_harness.h's own
// createBrdfSession()/compileBrdfComputeModule() -- this codebase's own
// established per-file-duplicated-helper idiom (that header's own comment
// cites the same precedent for why it exists rather than being shared).
// rx_shader::Compiler (already linked by this binary) sets no session
// searchPaths, so it cannot resolve `import brdf;` -- a session built
// directly against the raw Slang API, with `searchPaths` pointed at
// RX_MATERIAL_SHADER_DIR, is required instead.
//
// LIFETIME -- read before changing this shape: BrdfSession (and this
// struct) keeps BOTH the global and per-test session alive TOGETHER, and
// the CALLER must keep the returned struct alive for as long as it calls
// rx::shader::reflect() against a CompileResult built from it. Found
// empirically THIS session (not from documentation): an earlier version
// of this file built the session as a purely LOCAL variable inside the
// compile helper, returning only the CompileResult -- the session (and
// the slang::IGlobalSession beneath it) was destroyed at that function's
// return, and the SUBSEQUENT rx::shader::reflect() call segfaulted deep
// inside Slang's own RefObject::releaseReference()/
// spReflectionVariable_GetName (gdb backtrace, this session) touching
// already-freed session-owned memory -- the SAME class of vendored-Slang
// fragility Task 2's own compiler.cpp header comment documents (a second
// getLayout() call on a compute-only linked program can crash), but a
// DIFFERENT trigger (session lifetime, not a redundant getLayout() call
// -- this file already populates CompileResult::cachedLayout correctly).
struct EnergyCompSession {
    Slang::ComPtr<slang::IGlobalSession> globalSession;
    Slang::ComPtr<slang::ISession> session;
};

std::optional<EnergyCompSession> createEnergyCompSession(const std::string& shaderDir) {
    EnergyCompSession result;
    if (SLANG_FAILED(slang::createGlobalSession(result.globalSession.writeRef())) ||
        result.globalSession.get() == nullptr) {
        return std::nullopt;
    }

    slang::TargetDesc targetDesc{};
    targetDesc.format = SLANG_SPIRV;
    targetDesc.profile = result.globalSession->findProfile("sm_6_0");

    std::array<slang::CompilerOptionEntry, 2> entries{};
    uint32_t entryCount = 0;
    SlangCapabilityID spirvFloor = result.globalSession->findCapability("spirv_1_3");
    if (spirvFloor != SLANG_CAPABILITY_UNKNOWN) {
        slang::CompilerOptionEntry e{};
        e.name = slang::CompilerOptionName::Capability;
        e.value.kind = slang::CompilerOptionValueKind::Int;
        e.value.intValue0 = static_cast<int32_t>(spirvFloor);
        entries[entryCount++] = e;
    }
    slang::CompilerOptionEntry profileEntry{};
    profileEntry.name = slang::CompilerOptionName::Profile;
    profileEntry.value.kind = slang::CompilerOptionValueKind::Int;
    profileEntry.value.intValue0 = static_cast<int32_t>(targetDesc.profile);
    entries[entryCount++] = profileEntry;
    targetDesc.compilerOptionEntries = entries.data();
    targetDesc.compilerOptionEntryCount = entryCount;

    const char* searchPath = shaderDir.c_str();
    slang::SessionDesc sessionDesc{};
    sessionDesc.targets = &targetDesc;
    sessionDesc.targetCount = 1;
    sessionDesc.searchPaths = &searchPath;
    sessionDesc.searchPathCount = 1;

    if (SLANG_FAILED(result.globalSession->createSession(sessionDesc, result.session.writeRef())) ||
        result.session.get() == nullptr) {
        return std::nullopt;
    }
    return result;
}

std::optional<rx::shader::CompileResult> compileEnergyCompensationProbe(slang::ISession* session) {
    // Real production function under test: brdf.slang's own
    // energyCompensation(f0, dfgY) [Task 7/8], fed values captured from
    // THIS test's own real, baked DFG LUT via push constants -- not
    // reimplemented or approximated here.
    static const char* kSource = R"(
import brdf;
struct PushConstants { float f0; float dfgY; };
[[vk::push_constant]] ConstantBuffer<PushConstants> gPush;
[[vk::binding(0, 1)]] RWStructuredBuffer<float2> gOut;
[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id: SV_DispatchThreadID) {
    float3 comp = energyCompensation(float3(gPush.f0, gPush.f0, gPush.f0), gPush.dfgY);
    // gOut[0].x = the UNCOMPENSATED single-scatter response at f0=1
    // (Er_single(f0=1) = (1-1)*dfgX + 1*dfgY = dfgY exactly -- see this
    // file's own header comment for why f0=1 makes this reduction exact).
    // gOut[0].y = the COMPENSATED response (dfgY * comp.x).
    gOut[0] = float2(gPush.dfgY, gPush.dfgY * comp.x);
}
)";

    Slang::ComPtr<slang::IBlob> sourceBlob(Slang::INIT_ATTACH, slang_createBlob(kSource, std::strlen(kSource)));
    Slang::ComPtr<slang::IBlob> loadDiag;
    slang::IModule* module =
        session->loadModuleFromSource("RxIblEnergyCompProbe", "RxIblEnergyCompProbe.slang", sourceBlob, loadDiag.writeRef());
    if (module == nullptr) {
        return std::nullopt;
    }
    Slang::ComPtr<slang::IEntryPoint> entryPoint;
    if (SLANG_FAILED(module->findEntryPointByName("csMain", entryPoint.writeRef())) || entryPoint.get() == nullptr) {
        return std::nullopt;
    }

    std::vector<slang::IComponentType*> parts{module, entryPoint.get()};
    Slang::ComPtr<slang::IComponentType> composite;
    Slang::ComPtr<slang::IBlob> composeDiag;
    if (SLANG_FAILED(session->createCompositeComponentType(parts.data(), static_cast<SlangInt>(parts.size()),
                                                              composite.writeRef(), composeDiag.writeRef())) ||
        composite.get() == nullptr) {
        return std::nullopt;
    }
    Slang::ComPtr<slang::IComponentType> linked;
    Slang::ComPtr<slang::IBlob> linkDiag;
    if (SLANG_FAILED(composite->link(linked.writeRef(), linkDiag.writeRef())) || linked.get() == nullptr) {
        return std::nullopt;
    }

    rx::shader::CompileResult result;
    Slang::ComPtr<slang::IBlob> layoutDiag;
    result.cachedLayout = linked->getLayout(0, layoutDiag.writeRef());
    Slang::ComPtr<slang::IBlob> codeDiag;
    Slang::ComPtr<slang::IBlob> codeBlob;
    if (SLANG_FAILED(linked->getEntryPointCode(0, 0, codeBlob.writeRef(), codeDiag.writeRef())) ||
        codeBlob.get() == nullptr) {
        return std::nullopt;
    }
    rx::shader::SpirvBlob blob;
    blob.entryPointName = "csMain";
    blob.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    const auto* words = static_cast<const uint32_t*>(codeBlob->getBufferPointer());
    blob.code.assign(words, words + codeBlob->getBufferSize() / sizeof(uint32_t));
    result.ok = true;
    result.entryPointCode.push_back(std::move(blob));
    result.linkedProgram = linked;
    return result;
}

}  // namespace

TEST_CASE("IBL Task 9: energy-compensation activation -- production energyCompensation() fed the REAL baked DFG "
          "LUT closes the white-furnace identity at a rough, high-F0 probe where single-scatter measurably loses "
          "energy") {
    auto fx = makeIblFixture("rx_ibl_energy_comp");
    if (!fx.has_value()) {
        return;
    }

    std::array<std::array<float, 3>, 6> colors{};
    for (auto& c : colors) {
        c = {0.4F, 0.4F, 0.4F};
    }
    auto pixels = [&] {
        std::vector<float> px(static_cast<size_t>(8) * 8 * 4 * 6);
        for (uint32_t face = 0; face < 6; ++face) {
            for (uint32_t i = 0; i < 64; ++i) {
                size_t idx = (static_cast<size_t>(face) * 64 + i) * 4;
                px[idx + 0] = colors[face][0];
                px[idx + 1] = colors[face][1];
                px[idx + 2] = colors[face][2];
                px[idx + 3] = 1.0F;
            }
        }
        return px;
    }();
    auto source = uploadTestSource(*fx, 8, 8, 6, pixels);
    REQUIRE(source.has_value());

    rx::ibl::BakeParams params;
    params.baseCubemapFaceSize = 8;
    params.irradianceFaceSize = 4;
    params.irradianceSamples = 64;
    params.prefilteredMipCount = 2;
    params.prefilteredBaseFaceSize = 8;
    params.specularSamples = 32;
    params.dfgLutSize = 64;
    params.dfgSamples = 2048;  // generous -- roughest-row Ess needs to be a real, low-noise number here.

    auto result = rx::ibl::bakeEnvironment(fx->device, fx->allocator, *fx->cmdCtx, *fx->scheduler, *source, true,
                                             RX_IBL_SHADER_DIR, params, nullptr, "test_energy_compensation");
    REQUIRE(result.has_value());

    // Roughest row (y=0, linearRoughness~1.0), a mid/high NoV column
    // (x=3*width/4) -- away from the NoV~0 grazing edge, where the
    // DFV_Multiscatter Monte-Carlo estimator's own visibility term is
    // best-conditioned.
    const uint32_t x = params.dfgLutSize * 3 / 4;
    auto raw = readbackRaw(*fx, result->dfgLut, 0, 0, params.dfgLutSize, params.dfgLutSize, 4);
    const uint16_t* half = reinterpret_cast<const uint16_t*>(raw.data());
    size_t base = (static_cast<size_t>(0) * params.dfgLutSize + x) * 2;
    const float dfgY = halfToFloat(half[base + 1]);
    INFO("baked dfgY at roughest row, x=", x, ": ", dfgY);
    // Sanity: single-scatter DOES measurably lose energy at high
    // roughness (if this failed, the activation test below would be
    // vacuous -- proves this isn't a degenerate near-1 probe point).
    REQUIRE(dfgY < 0.95);
    REQUIRE(dfgY > 0.1);  // sanity floor -- a near-zero Ess would indicate a baking bug, not real physics here.

    // Kept alive for this TEST_CASE's own remaining scope -- see
    // EnergyCompSession's own header comment for why (rx::shader::
    // reflect() below touches session-owned memory).
    auto slangSession = createEnergyCompSession(RX_MATERIAL_SHADER_DIR);
    REQUIRE(slangSession.has_value());
    auto compiled = compileEnergyCompensationProbe(slangSession->session.get());
    REQUIRE(compiled.has_value());
    auto layoutInfo = rx::shader::reflect(*compiled);
    REQUIRE(layoutInfo.has_value());

    auto cache = rx::rhi::ComputePipelineCache::create(fx->device.device(),
                                                          std::filesystem::temp_directory_path() / "rx_ibl_energy_comp.cache");
    REQUIRE(cache.has_value());
    auto pso = cache->getOrCreate(compiled->entryPointCode[0].code, *layoutInfo);
    REQUIRE(pso.has_value());
    REQUIRE_FALSE(layoutInfo->pushRanges.empty());

    auto outBuf =
        fx->allocator.createHostVisibleBuffer(2 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(outBuf.has_value());

    std::array<VkDescriptorPoolSize, 1> poolSizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorPool(fx->device.device(), &poolInfo, nullptr, &pool) == VK_SUCCESS);

    VkDescriptorSetAllocateInfo emptyAlloc{};
    emptyAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    emptyAlloc.descriptorPool = pool;
    emptyAlloc.descriptorSetCount = 1;
    emptyAlloc.pSetLayouts = &pso->setLayouts[0];
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    REQUIRE(vkAllocateDescriptorSets(fx->device.device(), &emptyAlloc, &set0) == VK_SUCCESS);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &pso->setLayouts[1];
    VkDescriptorSet set1 = VK_NULL_HANDLE;
    REQUIRE(vkAllocateDescriptorSets(fx->device.device(), &allocInfo, &set1) == VK_SUCCESS);

    VkDescriptorBufferInfo bufInfo{outBuf->handle(), 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set1;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(fx->device.device(), 1, &write, 0, nullptr);

    fx->cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso->pipeline);
        std::array<VkDescriptorSet, 2> sets{set0, set1};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso->layout, 0, static_cast<uint32_t>(sets.size()),
                                 sets.data(), 0, nullptr);
        struct {
            float f0;
            float dfgY;
        } push{1.0F, dfgY};
        const auto& range = layoutInfo->pushRanges[0];
        vkCmdPushConstants(cmd, pso->layout, range.stages, range.offset, range.size, &push);
        vkCmdDispatch(cmd, 1, 1, 1);
    });
    outBuf->invalidate();
    std::array<float, 2> outVals{};
    std::memcpy(outVals.data(), outBuf->mappedData(), sizeof(outVals));

    INFO("uncompensated (=dfgY)=", outVals[0], " compensated=", outVals[1]);
    // The Kulla-Conty identity: dfgY*(1+1*(1/dfgY-1)) == dfgY+1-dfgY == 1
    // exactly (fp32 rounding only) at f0=1 -- proven by Task 7/8 already;
    // this assertion is proof that THIS task's REAL baked LUT value
    // survives that same identity through the REAL production function.
    CHECK(static_cast<double>(outVals[1]) == doctest::Approx(1.0).epsilon(1e-3));
    // Uncompensated is the REAL, measured energy loss this mechanism
    // exists to fix -- must be meaningfully below 1 (matches the REQUIRE
    // above on the raw baked dfgY, restated here on the value the
    // production kernel itself received).
    CHECK(static_cast<double>(outVals[0]) < 0.95);

    vkDeviceWaitIdle(fx->device.device());
    vkDestroyDescriptorPool(fx->device.device(), pool, nullptr);
    CHECK_FALSE(fx->context.hasValidationErrors());
}
