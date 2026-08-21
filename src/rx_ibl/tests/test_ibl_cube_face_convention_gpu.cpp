// rx_ibl/tests/test_ibl_cube_face_convention_gpu.cpp -- Phase 5 Stage 1
// Task 9 [#45, gate matrix-p5t09-ibl-bake-chain.md Open Question: "Cube-
// typed storage image views vs 2D-ARRAY views for compute WRITES...
// needs verification-in-task... before the coordinator treats it as
// settled"]. Proves the per-face WRITE convention every shader in
// shaders/ibl/*.slang shares (getDirectionForFace(): face 0..5 =
// +X,-X,+Y,-Y,+Z,-Z, Filament's own Cubemap::getDirectionFor() re-derived
// byte-for-byte) is genuinely consistent with REAL HARDWARE cube
// addressing (VK_IMAGE_VIEW_TYPE_CUBE sampling) -- NOT merely asserted by
// citation. This is the ONE test in this suite the "uniform environment"
// analytic tests (test_ibl_analytic_gpu.cpp) are architecturally BLIND to
// (a spatially uniform environment produces the same output regardless of
// any face-order/handedness bug, since every face sees identical input);
// this test is the one that would actually catch such a bug.
//
// Method: writes each of the 6 faces to a FLAT color encoding that face's
// own CENTER direction (u=v=0.5) -- which, for getDirectionForFace()'s
// exact formula, is precisely the canonical axis (+X,-X,+Y,-Y,+Z,-Z) for
// faces 0..5 respectively (cx=cy=0 at the face center collapses every
// face's own formula to its single nonzero +-1 component). Then samples
// the resulting REAL VK_IMAGE_VIEW_TYPE_CUBE view via hardware
// TextureCube.SampleLevel() at those SAME 6 canonical directions and
// checks each returns the color written to the correspondingly-INDEXED
// face -- a direct, GPU-verified round trip through real cube hardware,
// not a re-assertion of the same CPU-side formula.
#include "ibl_gpu_fixture.h"

#include <doctest/doctest.h>
#include <rx_rhi_vk/compute_pipeline.h>
#include <rx_rhi_vk/storage_image.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>

#include <array>
#include <cmath>
#include <cstring>

using namespace rx::ibl_test;

namespace {

constexpr const char* kWriteFaceSource = R"(
struct PushConstants { uint faceIndex; };
[[vk::push_constant]] ConstantBuffer<PushConstants> gPush;
[[vk::binding(0, 1)]] RWTexture2D<float4> gOutFace;

float3 getDirectionForFace(uint face, float u, float v) {
    float cx = u * 2.0 - 1.0;
    float cy = 1.0 - v * 2.0;
    float3 dir;
    if (face == 0u) dir = float3(1.0, cy, -cx);
    else if (face == 1u) dir = float3(-1.0, cy, cx);
    else if (face == 2u) dir = float3(cx, 1.0, -cy);
    else if (face == 3u) dir = float3(cx, -1.0, cy);
    else if (face == 4u) dir = float3(cx, cy, 1.0);
    else dir = float3(-cx, cy, -1.0);
    return normalize(dir);
}

[shader("compute")]
[numthreads(4, 4, 1)]
void csMain(uint3 id: SV_DispatchThreadID) {
    float3 dir = getDirectionForFace(gPush.faceIndex, 0.5, 0.5);
    gOutFace[id.xy] = float4(dir * 0.5 + 0.5, 1.0);
}
)";

constexpr const char* kSampleProbeSource = R"(
[[vk::binding(0, 1)]] TextureCube<float4> gProbe;
[[vk::binding(1, 1)]] SamplerState gSampler;
[[vk::binding(2, 1)]] RWStructuredBuffer<float4> gResults;

static const float3 kQueryDirs[6] = {
    float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0),
    float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1),
};

[shader("compute")]
[numthreads(6, 1, 1)]
void csMain(uint3 id: SV_DispatchThreadID) {
    gResults[id.x] = gProbe.SampleLevel(gSampler, kQueryDirs[id.x], 0.0);
}
)";

struct Kernel {
    rx::rhi::ComputePipelineCache::Pipeline pso;
    rx::shader::ShaderLayoutInfo layoutInfo;
    VkDescriptorSet emptySet0 = VK_NULL_HANDLE;
};

std::optional<Kernel> buildKernel(rx::shader::Compiler& compiler, rx::rhi::ComputePipelineCache& cache, VkDevice device,
                                    VkDescriptorPool pool, const char* moduleName, const char* source) {
    auto compiled = compiler.compileFromSource(moduleName, source, {"csMain"});
    if (!compiled.ok) {
        MESSAGE("compile failed: ", compiled.diagnostics);
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
    return Kernel{*pso, *layoutInfo, emptySet0};
}

}  // namespace

TEST_CASE("IBL Task 9: cube face-index<->direction convention matches real hardware TextureCube addressing") {
    auto fx = makeIblFixture("rx_ibl_cube_face_convention");
    if (!fx.has_value()) {
        return;
    }
    VkDevice device = fx->device.device();
    VkPhysicalDevice physicalDevice = fx->device.physicalDevice();

    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());
    auto cache = rx::rhi::ComputePipelineCache::create(
        device, std::filesystem::temp_directory_path() / "rx_ibl_cube_face_convention.cache");
    REQUIRE(cache.has_value());

    std::array<VkDescriptorPoolSize, 3> poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 4},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
    };
    // gResults is a StructuredBuffer, allocated separately below since
    // this pool's sizes above only cover image-kind descriptors.
    std::array<VkDescriptorPoolSize, 4> poolSizesFull{
        poolSizes[0], poolSizes[1], poolSizes[2],
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 16;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizesFull.size());
    poolInfo.pPoolSizes = poolSizesFull.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) == VK_SUCCESS);

    auto writeKernel = buildKernel(*compiler, *cache, device, pool, "RxIblFaceWrite", kWriteFaceSource);
    REQUIRE(writeKernel.has_value());
    auto sampleKernel = buildKernel(*compiler, *cache, device, pool, "RxIblFaceSample", kSampleProbeSource);
    REQUIRE(sampleKernel.has_value());

    constexpr uint32_t kDim = 4;
    // SAMPLED_BIT explicitly requested (STORAGE_BIT is auto-added by
    // StorageImage::create()) -- this probe image is both compute-written
    // AND, afterward, read via a genuine hardware TextureCube sampled
    // view, all within this one test.
    auto probe = rx::rhi::StorageImage::create(physicalDevice, device, fx->allocator, VkExtent2D{kDim, kDim},
                                                 VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT, 1, 6, true);
    REQUIRE(probe.has_value());

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    REQUIRE(vkCreateSampler(device, &samplerInfo, nullptr, &sampler) == VK_SUCCESS);

    auto readback = fx->allocator.createHostVisibleBuffer(6 * sizeof(float) * 4,
                                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());

    fx->cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        // A freshly-created StorageImage starts in VK_IMAGE_LAYOUT_UNDEFINED
        // (its creation-time initial layout) -- the render graph normally
        // handles this transition automatically (TransientPool/Executor's
        // own layout tracking); this test dispatches by hand, so it must
        // transition to GENERAL itself before the first compute write
        // (every descriptor write below claims GENERAL).
        VkImageMemoryBarrier2 toGeneral{};
        toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toGeneral.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        toGeneral.srcAccessMask = VK_ACCESS_2_NONE;
        toGeneral.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = probe->image();
        toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        VkDependencyInfo depInit{};
        depInit.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        depInit.imageMemoryBarrierCount = 1;
        depInit.pImageMemoryBarriers = &toGeneral;
        vkCmdPipelineBarrier2(cmd, &depInit);

        // --- write pass: each face gets a flat color encoding its own
        // center direction. -------------------------------------------
        for (uint32_t face = 0; face < 6; ++face) {
            VkImageView faceView = probe->viewForSubresource(0, 1, face, 1);
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = pool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &writeKernel->pso.setLayouts[1];
            VkDescriptorSet set = VK_NULL_HANDLE;
            REQUIRE(vkAllocateDescriptorSets(device, &allocInfo, &set) == VK_SUCCESS);
            VkDescriptorImageInfo imgInfo{VK_NULL_HANDLE, faceView, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write.pImageInfo = &imgInfo;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, writeKernel->pso.pipeline);
            std::array<VkDescriptorSet, 2> sets{writeKernel->emptySet0, set};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, writeKernel->pso.layout, 0,
                                     static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
            const auto& range = writeKernel->layoutInfo.pushRanges[0];
            struct { uint32_t faceIndex; } push{face};
            vkCmdPushConstants(cmd, writeKernel->pso.layout, range.stages, range.offset, range.size, &push);
            vkCmdDispatch(cmd, 1, 1, 1);
        }

        // --- transition to SHADER_READ_ONLY_OPTIMAL for the sample pass.
        // Per-face barriers (not one call spanning all 6 layers) --
        // matches this module's own bake.cpp precedent (see that file's
        // "capture pass" comment) and each write dispatch above used its
        // OWN per-face VkImageView (viewForSubresource), not the whole-
        // cube fullView() this transition prepares for -- one barrier
        // per subresource range that was ACTUALLY written keeps the
        // barrier's own coverage exactly matched to what preceded it. ---
        std::array<VkImageMemoryBarrier2, 6> toRead{};
        for (uint32_t face = 0; face < 6; ++face) {
            toRead[face].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toRead[face].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            toRead[face].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            toRead[face].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            toRead[face].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            toRead[face].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toRead[face].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toRead[face].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead[face].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead[face].image = probe->image();
            toRead[face].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, face, 1};
        }
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(toRead.size());
        dep.pImageMemoryBarriers = toRead.data();
        vkCmdPipelineBarrier2(cmd, &dep);

        // --- sample pass: read the REAL cube view at the 6 canonical
        // axis directions. ------------------------------------------
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &sampleKernel->pso.setLayouts[1];
        VkDescriptorSet set = VK_NULL_HANDLE;
        REQUIRE(vkAllocateDescriptorSets(device, &allocInfo, &set) == VK_SUCCESS);

        VkDescriptorImageInfo cubeInfo{sampler, probe->fullView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo samplerInfoWrite{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkDescriptorBufferInfo bufInfo{readback->handle(), 0, VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[0].pImageInfo = &cubeInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[1].pImageInfo = &samplerInfoWrite;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = set;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sampleKernel->pso.pipeline);
        std::array<VkDescriptorSet, 2> sets{sampleKernel->emptySet0, set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sampleKernel->pso.layout, 0,
                                 static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);

        VkMemoryBarrier2 toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        toHost.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toHost.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep2{};
        dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep2.memoryBarrierCount = 1;
        dep2.pMemoryBarriers = &toHost;
        vkCmdPipelineBarrier2(cmd, &dep2);
    });

    readback->invalidate();
    std::array<float, 24> results{};
    std::memcpy(results.data(), readback->mappedData(), results.size() * sizeof(float));

    // Expected: sampling at canonical direction N returns the color
    // written to face N (encode(dir)=dir*0.5+0.5; decode back by
    // *2-1). kQueryDirs[N] IS getDirectionForFace(N, 0.5, 0.5) by
    // construction (both are the canonical axis for N) -- so this
    // checks the round trip exactly, not a coincidence of matching
    // encodings.
    constexpr std::array<std::array<float, 3>, 6> kExpectedDir{{
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    }};
    for (uint32_t i = 0; i < 6; ++i) {
        float r = results[i * 4 + 0] * 2.0F - 1.0F;
        float g = results[i * 4 + 1] * 2.0F - 1.0F;
        float b = results[i * 4 + 2] * 2.0F - 1.0F;
        INFO("query dir index ", i, " decoded=(", r, ",", g, ",", b, ") expected=(", kExpectedDir[i][0], ",",
             kExpectedDir[i][1], ",", kExpectedDir[i][2], ")");
        CHECK(r == doctest::Approx(kExpectedDir[i][0]).epsilon(0.02));
        CHECK(g == doctest::Approx(kExpectedDir[i][1]).epsilon(0.02));
        CHECK(b == doctest::Approx(kExpectedDir[i][2]).epsilon(0.02));
    }

    vkDeviceWaitIdle(device);
    vkDestroySampler(device, sampler, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    CHECK_FALSE(fx->context.hasValidationErrors());
}
