#pragma once
// src/rx_material/tests/brdf_gpu_fixture.h -- Phase 5 Stage 1 Task 7 [#43]:
// the skip-guarded windowed-device fixture every brdf.slang GPU test needs,
// plus one shared dispatch-and-readback helper for the common "one input
// storage buffer, one output storage buffer, N-wide dispatch" shape every
// table-driven test in this suite uses. Fixture shape copied from
// rx_rhi_vk/tests/compute_pipeline_test.cpp's own ComputePipelineTestFixture
// (this codebase's own established per-file-duplicated-helper idiom, not a
// shared header with that file), extended with an Allocator since these
// tests move real data, not just build pipeline objects.

#include <rx_platform/window.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/compute_pipeline.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace rx::brdf_test {

struct BrdfGpuFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
};

inline std::optional<BrdfGpuFixture> makeBrdfGpuFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping brdf module GPU test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping brdf module GPU test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    auto allocator = rx::rhi::Allocator::create(*context, *device);
    REQUIRE(allocator.has_value());

    return BrdfGpuFixture{std::move(*window), std::move(*context), std::move(*device), std::move(*allocator)};
}

// Dispatches `pso` with exactly one host-visible input StructuredBuffer
// (set 1, binding 0 -- populated from `inputBytes` before dispatch) and one
// host-visible output RWStructuredBuffer (set 1, binding 1 -- read back
// into a fresh `outputBytes`-sized buffer after dispatch). Set 0 is never
// bound -- every compute module in this test suite declares no set-0
// resource, matching rx_graph/tests/test_compute_gpu.cpp's own TEST_CASE 2
// precedent (`vkCmdBindDescriptorSets(..., /*firstSet=*/1, ...)`, skipping
// set 0 entirely). No explicit device->host memory barrier before
// `invalidate()` -- matching test_compute_gpu.cpp's own established,
// validation-clean precedent: `CommandContext::runOnce()`'s
// `vkQueueWaitIdle()` already makes every write from that submission
// available before this function returns.
inline std::vector<uint8_t> dispatchOneInOneOut(BrdfGpuFixture& fixture, const rx::rhi::ComputePipelineCache::Pipeline& pso,
                                                 const std::vector<uint8_t>& inputBytes, size_t outputBytes,
                                                 uint32_t groupCountX) {
    VkDevice device = fixture.device.device();

    auto inBuf = fixture.allocator.createHostVisibleBuffer(inputBytes.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(inBuf.has_value());
    std::memcpy(inBuf->mappedData(), inputBytes.data(), inputBytes.size());
    inBuf->flush();

    auto outBuf = fixture.allocator.createHostVisibleBuffer(outputBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(outBuf.has_value());

    REQUIRE(pso.setLayouts.size() >= 2);
    std::array<VkDescriptorPoolSize, 1> sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) == VK_SUCCESS);

    VkDescriptorSetLayout set1Layout = pso.setLayouts[1];
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = pool;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = &set1Layout;
    REQUIRE(vkAllocateDescriptorSets(device, &setAllocInfo, &set) == VK_SUCCESS);

    VkDescriptorBufferInfo inInfo{inBuf->handle(), 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo outInfo{outBuf->handle(), 0, VK_WHOLE_SIZE};
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &inInfo;
    writes[1] = writes[0];
    writes[1].dstBinding = 1;
    writes[1].pBufferInfo = &outInfo;
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    auto cmdCtx = rx::rhi::CommandContext::create(device, fixture.device.graphicsQueue(), fixture.device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso.layout, 1, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, groupCountX, 1, 1);
    });

    outBuf->invalidate();
    std::vector<uint8_t> result(outputBytes);
    std::memcpy(result.data(), outBuf->mappedData(), outputBytes);

    vkDestroyDescriptorPool(device, pool, nullptr);
    return result;
}

}  // namespace rx::brdf_test
