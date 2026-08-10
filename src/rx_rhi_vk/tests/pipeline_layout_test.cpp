#include <doctest/doctest.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <VkBootstrap.h>
#include <utility>

// Pure headless test -- no rx::platform::Window, no VkSurfaceKHR, no
// swapchain anywhere in this file, exactly like clear_color_test.cpp
// (safe to run in any order alongside the windowed Device/Buffer tests in
// this binary because tests/doctest_main.cpp warms vk-bootstrap's
// process-wide instance-function cache before any TEST_CASE runs -- see
// the comment there, and the Phase 2 plan's Global Constraints on the
// vk-bootstrap landmine, for why this test joins rx_rhi_vk_tests rather
// than a new executable).
namespace {

// A headless device with exactly the Vulkan 1.2 descriptor-indexing
// features Task 3's brief will make process-wide on rx::rhi::Device
// itself (chained the same way vk-bootstrap's set_required_features_12
// will there): descriptorIndexing, runtimeDescriptorArray,
// descriptorBindingPartiallyBound, descriptorBindingVariableDescriptorCount,
// descriptorBindingSampledImageUpdateAfterBind,
// descriptorBindingStorageImageUpdateAfterBind,
// descriptorBindingStorageBufferUpdateAfterBind,
// descriptorBindingUpdateUnusedWhilePending,
// shaderSampledImageArrayNonUniformIndexing,
// shaderStorageBufferArrayNonUniformIndexing. Enabled LOCALLY here (not via
// rx::rhi::Device::create(), which does not enable them yet -- that is
// Task 3's job) because building a VkDescriptorSetLayout with an
// UPDATE_AFTER_BIND_BIT | PARTIALLY_BOUND_BIT binding plus the set-level
// UPDATE_AFTER_BIND_POOL flag requires the matching device features to be
// enabled at device-creation time, or validation correctly rejects it.
// This exact feature set is confirmed present on both the Steam Deck's
// RADV driver [R:B2] and (checked directly on this development machine)
// both an NVIDIA RTX 2080 and llvmpipe/lavapipe -- treated as a hard
// requirement (REQUIRE, no skip-guard) rather than an environment-
// dependent capability, matching clear_color_test.cpp's own headless
// pattern rather than device_test.cpp's windowed-skip one.
struct HeadlessDescriptorIndexingDevice {
    rx::rhi::Context context;
    vkb::Device vkbDevice;
    VkDevice device;
};

HeadlessDescriptorIndexingDevice makeHeadlessDescriptorIndexingDevice() {
    auto context = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;

    // No set_surface()/defer_surface_initialization() call -- see
    // clear_color_test.cpp's comment on Context::create({}, ...) selection:
    // a genuinely headless instance already has require_present = false,
    // and deferring surface init would ask for VK_KHR_swapchain without the
    // prerequisite VK_KHR_surface instance extension a headless Context
    // never enables.
    vkb::PhysicalDeviceSelector selector(context->vkbInstance());
    auto physResult = selector.set_minimum_version(1, 3).set_required_features_12(features12).select();
    REQUIRE(physResult.has_value());

    auto deviceResult = vkb::DeviceBuilder(physResult.value()).build();
    REQUIRE(deviceResult.has_value());
    vkb::Device vkbDevice = deviceResult.value();
    volkLoadDevice(vkbDevice.device);

    return HeadlessDescriptorIndexingDevice{std::move(*context), vkbDevice, vkbDevice.device};
}

// Mirrors rx_shader's reflection_test.cpp `kLayoutTestSource` shape exactly
// (same set/binding/type/count/stage table that test's reflect() call
// asserts) -- hand-crafted here, not obtained by actually calling
// rx::shader::reflect(), since rx_rhi_vk deliberately never links rx_shader/
// slang::slang (see pipeline_layout.h's comment). This is the "build the
// pipeline layout on a headless device" half of Task 2's brief; the
// "reflect a shader with ... push constants" half lives in
// reflection_test.cpp.
rx::shader::ShaderLayoutInfo makeUnderBudgetLayoutInfo() {
    rx::shader::ShaderLayoutInfo info;

    rx::shader::ShaderLayoutInfo::Binding textures;
    textures.set = 0;
    textures.binding = 0;
    textures.count = 0;
    textures.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    textures.stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    textures.unboundedArray = true;
    info.bindings.push_back(textures);

    rx::shader::ShaderLayoutInfo::Binding sampler;
    sampler.set = 1;
    sampler.binding = 0;
    sampler.count = 1;
    sampler.type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sampler.stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    info.bindings.push_back(sampler);

    rx::shader::ShaderLayoutInfo::Binding frame;
    frame.set = 1;
    frame.binding = 1;
    frame.count = 1;
    frame.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    frame.stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    info.bindings.push_back(frame);

    rx::shader::ShaderLayoutInfo::PushRange pushRange;
    pushRange.stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = 80;
    info.pushRanges.push_back(pushRange);

    return info;
}

}  // namespace

TEST_CASE("PipelineLayoutBuilder::build produces non-null handles with zero validation errors for an unbounded "
          "array + sampler + uniform buffer + push constants") {
    HeadlessDescriptorIndexingDevice headless = makeHeadlessDescriptorIndexingDevice();

    {
        // Inner scope: the bundle (and every VkDescriptorSetLayout/
        // VkPipelineLayout it owns) must be destroyed against a still-live
        // device, before the device itself is torn down below -- same
        // reverse-destruction discipline clear_color_test.cpp documents.
        auto bundle = rx::rhi::PipelineLayoutBuilder::build(headless.device, makeUnderBudgetLayoutInfo());
        REQUIRE(bundle.has_value());

        CHECK(bundle->layout != VK_NULL_HANDLE);
        REQUIRE(bundle->setLayouts.size() == 2);
        CHECK(bundle->setLayouts[0] != VK_NULL_HANDLE);
        CHECK(bundle->setLayouts[1] != VK_NULL_HANDLE);
    }

    vkDeviceWaitIdle(headless.device);
    vkb::destroy_device(headless.vkbDevice);
    CHECK_FALSE(headless.context.hasValidationErrors());
}

TEST_CASE("PipelineLayoutBuilder::build rejects a >128-byte push constant footprint with a logged error") {
    HeadlessDescriptorIndexingDevice headless = makeHeadlessDescriptorIndexingDevice();

    // Matches reflection_test.cpp's oversized-push-constant case (three
    // float4x4 members, 192 std430-packed bytes) -- proving reflect()
    // itself never clamps/rejects it (see that test) while
    // PipelineLayoutBuilder, the actual budget enforcer, does.
    rx::shader::ShaderLayoutInfo oversized;
    rx::shader::ShaderLayoutInfo::PushRange pushRange;
    pushRange.stages = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = 192;
    oversized.pushRanges.push_back(pushRange);

    auto bundle = rx::rhi::PipelineLayoutBuilder::build(headless.device, oversized);
    CHECK_FALSE(bundle.has_value());

    vkDeviceWaitIdle(headless.device);
    vkb::destroy_device(headless.vkbDevice);
    CHECK_FALSE(headless.context.hasValidationErrors());
}
