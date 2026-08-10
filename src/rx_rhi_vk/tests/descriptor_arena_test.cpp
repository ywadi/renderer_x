#include <doctest/doctest.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/descriptor_arena.h>
#include <VkBootstrap.h>
#include <utility>

// Pure headless test -- no rx::platform::Window, no VkSurfaceKHR, no
// swapchain anywhere in this file, exactly like clear_color_test.cpp/
// pipeline_layout_test.cpp/bindless_test.cpp/deletion_queue_test.cpp (safe
// to run in any order alongside the windowed Device/Buffer tests in this
// binary because tests/doctest_main.cpp warms vk-bootstrap's process-wide
// instance-function cache before any TEST_CASE runs -- see the comment
// there). DescriptorArena needs neither descriptor-indexing nor
// dynamic-rendering/sync2 device features (unlike bindless_test.cpp's own
// fixture) -- it only ever creates/resets plain VkDescriptorPools and
// allocates ordinary (non-update-after-bind) VkDescriptorSets -- so this
// file's fixture is deliberately the most minimal headless device in this
// binary: a bare Vulkan 1.3 device, no required-features struct at all.
namespace {

struct HeadlessDescriptorArenaFixture {
    rx::rhi::Context context;
    vkb::Device vkbDevice;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
};

HeadlessDescriptorArenaFixture makeFixture() {
    auto context = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    vkb::PhysicalDeviceSelector selector(context->vkbInstance());
    auto physResult = selector.set_minimum_version(1, 3).select();
    REQUIRE(physResult.has_value());

    auto deviceResult = vkb::DeviceBuilder(physResult.value()).build();
    REQUIRE(deviceResult.has_value());
    vkb::Device vkbDevice = deviceResult.value();
    volkLoadDevice(vkbDevice.device);

    return HeadlessDescriptorArenaFixture{std::move(*context), vkbDevice, vkbDevice.device,
                                           vkbDevice.physical_device.physical_device};
}

// One VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER binding at binding 0 -- the exact
// shape rx_material's own ParamArena allocates against (instance.h), and
// enough to exercise allocate() with a real, non-trivial layout rather than
// an empty (zero-binding) one.
VkDescriptorSetLayout makeUboSetLayout(VkDevice device) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings = &binding;

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) == VK_SUCCESS);
    return layout;
}

}  // namespace

TEST_CASE("DescriptorArena::allocate across 3 simulated frames reuses reset pools with zero validation errors") {
    HeadlessDescriptorArenaFixture fixture = makeFixture();

    {
        rx::rhi::DescriptorArena::Capacities capacities{/*maxSets=*/4, /*uniformBuffers=*/4};
        auto arena = rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/2, capacities);
        REQUIRE(arena.has_value());
        CHECK(arena->framesInFlight() == 2);

        VkDescriptorSetLayout layout = makeUboSetLayout(fixture.device);

        // Real host-visible buffer to write a genuine UBO descriptor
        // against, so the sets this test allocates are exercised as more
        // than opaque handles -- proves allocate()'s sets are real,
        // vkUpdateDescriptorSets-legal objects, not just non-null.
        auto allocator =
            rx::rhi::Allocator::createRaw(fixture.physicalDevice, fixture.device, fixture.context.instance());
        REQUIRE(allocator.has_value());
        auto buffer = allocator->createHostVisibleBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        REQUIRE(buffer.has_value());

        auto writeUbo = [&](VkDescriptorSet set) {
            VkDescriptorBufferInfo bufferInfo{buffer->handle(), 0, 256};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo = &bufferInfo;
            vkUpdateDescriptorSets(fixture.device, 1, &write, 0, nullptr);
        };

        // --- Simulated frame 0 (slot 0): allocate up to this arena's own
        // maxSets, then confirm exhaustion is a clean VK_NULL_HANDLE, not a
        // crash or a validation error.
        arena->beginFrame(0);
        for (uint32_t i = 0; i < capacities.maxSets; ++i) {
            VkDescriptorSet set = arena->allocate(layout);
            REQUIRE(set != VK_NULL_HANDLE);
            writeUbo(set);
        }
        CHECK(arena->allocate(layout) == VK_NULL_HANDLE);

        // --- Simulated frame 1 (slot 1): a DIFFERENT pool, so allocation
        // succeeds again immediately -- proves the two frame-in-flight
        // slots are genuinely independent, not one shared pool.
        arena->beginFrame(1);
        VkDescriptorSet frame1Set = arena->allocate(layout);
        REQUIRE(frame1Set != VK_NULL_HANDLE);
        writeUbo(frame1Set);

        // --- Simulated frame 2 (back to slot 0, frameIndex % 2 == 0):
        // beginFrame() resets slot 0's pool, reclaiming the capacity
        // frame 0 fully consumed above -- allocation must succeed again,
        // proving reset (not merely "a fresh arena") is what freed it.
        arena->beginFrame(2);
        VkDescriptorSet frame2Set = arena->allocate(layout);
        REQUIRE(frame2Set != VK_NULL_HANDLE);
        writeUbo(frame2Set);
        for (uint32_t i = 1; i < capacities.maxSets; ++i) {
            REQUIRE(arena->allocate(layout) != VK_NULL_HANDLE);
        }
        CHECK(arena->allocate(layout) == VK_NULL_HANDLE);

        vkDestroyDescriptorSetLayout(fixture.device, layout, nullptr);
        // `buffer`/`allocator`/`arena` (RAII) go out of scope at the end of
        // this block, before the device itself is destroyed below -- same
        // reverse-destruction discipline every other rx_rhi_vk GPU test in
        // this binary documents.
    }

    vkDeviceWaitIdle(fixture.device);
    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}

TEST_CASE("DescriptorArena::create rejects framesInFlight == 0 and zero capacities with a clean error, no crash") {
    HeadlessDescriptorArenaFixture fixture = makeFixture();

    CHECK_FALSE(rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/0).has_value());

    rx::rhi::DescriptorArena::Capacities zeroSets{/*maxSets=*/0, /*uniformBuffers=*/4};
    CHECK_FALSE(rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/2, zeroSets).has_value());

    rx::rhi::DescriptorArena::Capacities zeroUbos{/*maxSets=*/4, /*uniformBuffers=*/0};
    CHECK_FALSE(rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/2, zeroUbos).has_value());

    // A valid call still succeeds -- proves the rejections above are
    // specific to the bad inputs, not a broken fixture.
    CHECK(rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/2).has_value());

    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}
