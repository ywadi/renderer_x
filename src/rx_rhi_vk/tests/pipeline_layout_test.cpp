#include <doctest/doctest.h>
#include <rx_rhi_vk/bindless.h>
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
//
// `physicalDevice` was added this task (Task 6) alongside the two
// external-set-0 tests below, which need a real rx::rhi::BindlessTable
// (BindlessTable::create() takes a VkPhysicalDevice) -- the two
// pre-existing tests above never needed it.
struct HeadlessDescriptorIndexingDevice {
    rx::rhi::Context context;
    vkb::Device vkbDevice;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
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

    return HeadlessDescriptorIndexingDevice{std::move(*context), vkbDevice, vkbDevice.device,
                                             vkbDevice.physical_device.physical_device};
}

rx::rhi::BindlessTable::Capacities makeTestBindlessCapacities() {
    return rx::rhi::BindlessTable::Capacities{/*sampledImages=*/1024, /*samplers=*/16, /*storageBuffers=*/256};
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

// --- External set-0 substitution (Task 6) -----------------------------
//
// Closes the gap Task 3's review found: BindlessTable's real set-0 layout
// (capacities-sized, VARIABLE_DESCRIPTOR_COUNT on its last binding) and
// this builder's own from-scratch set-0 layout (always
// kUnboundedArrayDescriptorCapacity, no VARIABLE_DESCRIPTOR_COUNT flag) are
// NOT compatible pipeline layouts "by construction" -- build()'s new
// `externalSet0` parameter lets a caller substitute the real handle in
// directly instead. These two tests are the "focused unit test (mismatched-
// type rejection + happy path)" the Task 6 brief requires.

TEST_CASE("PipelineLayoutBuilder::build with an external set-0 layout: happy path reuses the caller's exact "
          "VkDescriptorSetLayout for set 0 and validates a subset-compatible reflected shape") {
    HeadlessDescriptorIndexingDevice headless = makeHeadlessDescriptorIndexingDevice();

    {
        auto table = rx::rhi::BindlessTable::create(headless.physicalDevice, headless.device,
                                                      makeTestBindlessCapacities());
        REQUIRE(table.has_value());

        // A reflected shape using two of BindlessTable's three fixed slots
        // (images + samplers) -- deliberately a STRICT SUBSET, not an
        // exact match, per the brief's "subset-compatible shape" wording:
        // a shader need not touch every bindless resource class to be
        // substitution-compatible with the table that provides all three.
        rx::shader::ShaderLayoutInfo info;

        rx::shader::ShaderLayoutInfo::Binding images;
        images.set = 0;
        images.binding = rx::rhi::BindlessTable::kSampledImageBinding;
        images.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        images.stages = VK_SHADER_STAGE_FRAGMENT_BIT;
        images.unboundedArray = true;
        info.bindings.push_back(images);

        rx::shader::ShaderLayoutInfo::Binding samplers;
        samplers.set = 0;
        samplers.binding = rx::rhi::BindlessTable::kSamplerBinding;
        samplers.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        samplers.stages = VK_SHADER_STAGE_FRAGMENT_BIT;
        samplers.unboundedArray = true;
        info.bindings.push_back(samplers);

        // A per-draw push range alongside the substituted set 0 -- proves
        // push constants keep working unmodified.
        rx::shader::ShaderLayoutInfo::PushRange pushRange;
        pushRange.stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = 12;
        info.pushRanges.push_back(pushRange);

        auto bundle = rx::rhi::PipelineLayoutBuilder::build(headless.device, info, table->descriptorSetLayout());
        REQUIRE(bundle.has_value());
        REQUIRE(bundle->setLayouts.size() == 1);
        // The exact handle, not a lookalike this builder created itself --
        // the entire point of the substitution.
        CHECK(bundle->setLayouts[0] == table->descriptorSetLayout());
        CHECK(bundle->layout != VK_NULL_HANDLE);
    }

    // `table`'s descriptor set layout must still be alive when `bundle`
    // above is destroyed (bundle must not touch it -- see
    // PipelineLayoutBundle's ownership comment); it goes out of scope
    // here, after `bundle` already has (reverse declaration order), so
    // this also exercises that destroyAll() genuinely skipped it.
    vkDeviceWaitIdle(headless.device);
    vkb::destroy_device(headless.vkbDevice);
    CHECK_FALSE(headless.context.hasValidationErrors());
}

TEST_CASE("PipelineLayoutBuilder::build with an external set-0 layout: rejects a reflected set-0 binding whose "
          "descriptor type doesn't match the external bindless-table layout's shape") {
    HeadlessDescriptorIndexingDevice headless = makeHeadlessDescriptorIndexingDevice();

    {
        auto table = rx::rhi::BindlessTable::create(headless.physicalDevice, headless.device,
                                                      makeTestBindlessCapacities());
        REQUIRE(table.has_value());

        // Binding number 0 matches BindlessTable::kSampledImageBinding, but
        // the declared type (UNIFORM_BUFFER) does not match that slot's
        // real type (SAMPLED_IMAGE) -- the mismatched-type case build()
        // must reject before ever touching `externalSet0`.
        rx::shader::ShaderLayoutInfo info;
        rx::shader::ShaderLayoutInfo::Binding wrongType;
        wrongType.set = 0;
        wrongType.binding = rx::rhi::BindlessTable::kSampledImageBinding;
        wrongType.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wrongType.count = 1;
        wrongType.stages = VK_SHADER_STAGE_FRAGMENT_BIT;
        info.bindings.push_back(wrongType);

        auto bundle = rx::rhi::PipelineLayoutBuilder::build(headless.device, info, table->descriptorSetLayout());
        CHECK_FALSE(bundle.has_value());
    }

    vkDeviceWaitIdle(headless.device);
    vkb::destroy_device(headless.vkbDevice);
    CHECK_FALSE(headless.context.hasValidationErrors());
}

TEST_CASE("PipelineLayoutBuilder::build with an external set-0 layout: rejects a bounded set-0 binding count "
          "exceeding this builder's generic capacity ceiling") {
    HeadlessDescriptorIndexingDevice headless = makeHeadlessDescriptorIndexingDevice();

    {
        auto table = rx::rhi::BindlessTable::create(headless.physicalDevice, headless.device,
                                                      makeTestBindlessCapacities());
        REQUIRE(table.has_value());

        // Binding number and type both match BindlessTable's real
        // sampled-image slot exactly, but `unboundedArray` is false with a
        // bounded `count` one past kUnboundedArrayDescriptorCapacity --
        // the "counts within capacity" half of the shape check (a
        // reflected finite-size array declared implausibly large for this
        // builder's own generic ceiling, independent of whatever the real
        // external layout's actual capacity happens to be -- see
        // pipeline_layout.h's comment on build() for why this builder has
        // no way to see that real capacity through an opaque
        // VkDescriptorSetLayout handle at all).
        rx::shader::ShaderLayoutInfo info;
        rx::shader::ShaderLayoutInfo::Binding oversizedBounded;
        oversizedBounded.set = 0;
        oversizedBounded.binding = rx::rhi::BindlessTable::kSampledImageBinding;
        oversizedBounded.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        oversizedBounded.unboundedArray = false;
        oversizedBounded.count = rx::rhi::PipelineLayoutBuilder::kUnboundedArrayDescriptorCapacity + 1;
        oversizedBounded.stages = VK_SHADER_STAGE_FRAGMENT_BIT;
        info.bindings.push_back(oversizedBounded);

        auto bundle = rx::rhi::PipelineLayoutBuilder::build(headless.device, info, table->descriptorSetLayout());
        CHECK_FALSE(bundle.has_value());
    }

    vkDeviceWaitIdle(headless.device);
    vkb::destroy_device(headless.vkbDevice);
    CHECK_FALSE(headless.context.hasValidationErrors());
}

TEST_CASE("PipelineLayoutBuilder::build with an external set-0 layout: rejects a set-0 binding number with no "
          "counterpart in the bindless-table shape") {
    HeadlessDescriptorIndexingDevice headless = makeHeadlessDescriptorIndexingDevice();

    {
        auto table = rx::rhi::BindlessTable::create(headless.physicalDevice, headless.device,
                                                      makeTestBindlessCapacities());
        REQUIRE(table.has_value());

        // Binding 7 has no counterpart in BindlessTable's fixed 0/1/2
        // scheme at all, regardless of its declared type.
        rx::shader::ShaderLayoutInfo info;
        rx::shader::ShaderLayoutInfo::Binding unknownBinding;
        unknownBinding.set = 0;
        unknownBinding.binding = 7;
        unknownBinding.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        unknownBinding.unboundedArray = true;
        info.bindings.push_back(unknownBinding);

        auto bundle = rx::rhi::PipelineLayoutBuilder::build(headless.device, info, table->descriptorSetLayout());
        CHECK_FALSE(bundle.has_value());
    }

    vkDeviceWaitIdle(headless.device);
    vkb::destroy_device(headless.vkbDevice);
    CHECK_FALSE(headless.context.hasValidationErrors());
}

TEST_CASE("PipelineLayoutBuilder::build with an external set-0 layout: wires set 0 to the external handle even "
          "when the shader reflects zero set-0 bindings") {
    HeadlessDescriptorIndexingDevice headless = makeHeadlessDescriptorIndexingDevice();

    {
        auto table = rx::rhi::BindlessTable::create(headless.physicalDevice, headless.device,
                                                      makeTestBindlessCapacities());
        REQUIRE(table.has_value());

        // A shader that declares no descriptor bindings at all (e.g. one
        // that only ever indexes the bindless table via a push-constant
        // index into a set it happens to share globally) is still a valid,
        // if degenerate, subset -- setCount must still reach 1 so set 0
        // gets wired to `externalSet0`.
        rx::shader::ShaderLayoutInfo info;

        auto bundle = rx::rhi::PipelineLayoutBuilder::build(headless.device, info, table->descriptorSetLayout());
        REQUIRE(bundle.has_value());
        REQUIRE(bundle->setLayouts.size() == 1);
        CHECK(bundle->setLayouts[0] == table->descriptorSetLayout());
    }

    vkDeviceWaitIdle(headless.device);
    vkb::destroy_device(headless.vkbDevice);
    CHECK_FALSE(headless.context.hasValidationErrors());
}
