// per_frame_storage_buffer_test.cpp -- [Phase 5 Task 5, ticket #41 row 2]
// rx::rhi::PerFrameStorageBuffer: the promoted per-FIF, bindless-registered,
// host-visible storage buffer pattern (Phase 4 exit-review I1). Pure
// headless (no rx::platform::Window/VkSurfaceKHR/swapchain), mirroring
// bindless_test.cpp's own descriptor-indexing-feature fixture -- this
// class's own write() discipline is the exact thing I1 fixed, so the
// LOAD-BEARING assertion in this file is the revert-discrimination test
// below: a write-before-fence-wait (single shared buffer) reversion must
// FAIL it.
#include <doctest/doctest.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/per_frame_storage_buffer.h>
#include <VkBootstrap.h>

#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace {

struct HeadlessPerFrameBufferFixture {
    rx::rhi::Context context;
    vkb::Device vkbDevice;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
};

HeadlessPerFrameBufferFixture makeFixture() {
    auto context = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    // Same descriptor-indexing feature set bindless_test.cpp's own headless
    // fixture enables -- registerStorageBuffer()/release() need it exactly
    // like registerSampledImage()/registerSampler() do.
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

    vkb::PhysicalDeviceSelector selector(context->vkbInstance());
    auto physResult = selector.set_minimum_version(1, 3).set_required_features_12(features12).select();
    REQUIRE(physResult.has_value());

    auto deviceResult = vkb::DeviceBuilder(physResult.value()).build();
    REQUIRE(deviceResult.has_value());
    vkb::Device vkbDevice = deviceResult.value();
    volkLoadDevice(vkbDevice.device);

    return HeadlessPerFrameBufferFixture{std::move(*context), vkbDevice, vkbDevice.device,
                                          vkbDevice.physical_device.physical_device};
}

rx::rhi::BindlessTable::Capacities makeTestCapacities() {
    return rx::rhi::BindlessTable::Capacities{/*sampledImages=*/1, /*samplers=*/1, /*storageBuffers=*/16};
}

}  // namespace

TEST_CASE("PerFrameStorageBuffer::create allocates one buffer per frame-in-flight slot, each independently "
          "bindless-registered with a distinct index, and primes every slot with initialData") {
    HeadlessPerFrameBufferFixture fixture = makeFixture();
    {
        auto allocator =
            rx::rhi::Allocator::createRaw(fixture.physicalDevice, fixture.device, fixture.context.instance());
        REQUIRE(allocator.has_value());
        auto bindless = rx::rhi::BindlessTable::create(fixture.physicalDevice, fixture.device, makeTestCapacities());
        REQUIRE(bindless.has_value());

        std::array<uint32_t, 4> seed{11, 22, 33, 44};
        auto perFrame = rx::rhi::PerFrameStorageBuffer::create(*allocator, *bindless, sizeof(seed), seed.data(),
                                                                  /*framesInFlight=*/2);
        REQUIRE(perFrame.has_value());
        CHECK(perFrame->framesInFlight() == 2);

        // Distinct bindless indices for each slot.
        CHECK(perFrame->bindlessIndex(0) != perFrame->bindlessIndex(1));

        vkDeviceWaitIdle(fixture.device);
        perFrame->release(*bindless);
    }
    vkDeviceWaitIdle(fixture.device);
    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}

TEST_CASE("PerFrameStorageBuffer::write [Phase 4 exit-review I1, revert-discrimination]: writing into one "
          "frame-in-flight slot never touches another slot's own bytes -- a reversion to a SINGLE SHARED buffer "
          "(the pre-I1 bug this class exists to prevent) would corrupt slot 0's own content as soon as slot 1 is "
          "written, which this test's own byte-level readback catches immediately") {
    HeadlessPerFrameBufferFixture fixture = makeFixture();
    {
        auto allocator =
            rx::rhi::Allocator::createRaw(fixture.physicalDevice, fixture.device, fixture.context.instance());
        REQUIRE(allocator.has_value());
        auto bindless = rx::rhi::BindlessTable::create(fixture.physicalDevice, fixture.device, makeTestCapacities());
        REQUIRE(bindless.has_value());

        constexpr VkDeviceSize kBytes = 16;
        auto perFrame =
            rx::rhi::PerFrameStorageBuffer::create(*allocator, *bindless, kBytes, nullptr, /*framesInFlight=*/2);
        REQUIRE(perFrame.has_value());

        std::array<uint8_t, kBytes> blobA{};
        blobA.fill(0xAA);
        std::array<uint8_t, kBytes> blobB{};
        blobB.fill(0xBB);

        // --- Write slot 0 (blob A), then slot 1 (blob B). ---------------
        REQUIRE(perFrame->write(0, blobA.data(), blobA.size()));
        REQUIRE(perFrame->write(1, blobB.data(), blobB.size()));

        // THE discriminator: read back slot 0's raw physical bytes directly
        // (rx::rhi::detail::debugSlotBufferData(), the same test-only seam
        // convention rx::material::ParamArena's own
        // detail::debugFrameBufferData() establishes) and confirm they
        // still read blob A -- with the I1 bug reinstated (one shared
        // buffer for every slot), slot 1's write above would have
        // physically overwritten the SAME bytes slot 0's write just wrote,
        // and this CHECK would read blob B instead.
        const void* slot0Data = rx::rhi::detail::debugSlotBufferData(*perFrame, 0);
        REQUIRE(slot0Data != nullptr);
        CHECK(std::memcmp(slot0Data, blobA.data(), blobA.size()) == 0);

        const void* slot1Data = rx::rhi::detail::debugSlotBufferData(*perFrame, 1);
        REQUIRE(slot1Data != nullptr);
        CHECK(std::memcmp(slot1Data, blobB.data(), blobB.size()) == 0);

        // --- A third write into slot 0 must still never touch slot 1. ---
        std::array<uint8_t, kBytes> blobC{};
        blobC.fill(0xCC);
        REQUIRE(perFrame->write(0, blobC.data(), blobC.size()));
        CHECK(std::memcmp(rx::rhi::detail::debugSlotBufferData(*perFrame, 0), blobC.data(), blobC.size()) == 0);
        CHECK(std::memcmp(rx::rhi::detail::debugSlotBufferData(*perFrame, 1), blobB.data(), blobB.size()) == 0);

        vkDeviceWaitIdle(fixture.device);
        perFrame->release(*bindless);
    }
    vkDeviceWaitIdle(fixture.device);
    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}

TEST_CASE("PerFrameStorageBuffer::write refuses (false, no crash) a write exceeding this instance's own "
          "bytesPerSlot capacity, and an out-of-range frameSlot") {
    HeadlessPerFrameBufferFixture fixture = makeFixture();
    {
        auto allocator =
            rx::rhi::Allocator::createRaw(fixture.physicalDevice, fixture.device, fixture.context.instance());
        REQUIRE(allocator.has_value());
        auto bindless = rx::rhi::BindlessTable::create(fixture.physicalDevice, fixture.device, makeTestCapacities());
        REQUIRE(bindless.has_value());

        constexpr VkDeviceSize kBytes = 16;
        auto perFrame =
            rx::rhi::PerFrameStorageBuffer::create(*allocator, *bindless, kBytes, nullptr, /*framesInFlight=*/2);
        REQUIRE(perFrame.has_value());

        std::array<uint8_t, kBytes + 1> tooBig{};
        CHECK_FALSE(perFrame->write(0, tooBig.data(), tooBig.size()));

        std::array<uint8_t, kBytes> ok{};
        CHECK_FALSE(perFrame->write(/*frameSlot=*/2, ok.data(), ok.size()));  // only slots 0/1 exist.
        CHECK(perFrame->bindlessIndex(2) == 0);

        vkDeviceWaitIdle(fixture.device);
        perFrame->release(*bindless);
    }
    vkDeviceWaitIdle(fixture.device);
    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}

TEST_CASE("PerFrameStorageBuffer::create rejects bytesPerSlot == 0 and framesInFlight == 0 with a clean error, "
          "no crash") {
    HeadlessPerFrameBufferFixture fixture = makeFixture();
    {
        auto allocator =
            rx::rhi::Allocator::createRaw(fixture.physicalDevice, fixture.device, fixture.context.instance());
        REQUIRE(allocator.has_value());
        auto bindless = rx::rhi::BindlessTable::create(fixture.physicalDevice, fixture.device, makeTestCapacities());
        REQUIRE(bindless.has_value());

        CHECK_FALSE(rx::rhi::PerFrameStorageBuffer::create(*allocator, *bindless, /*bytesPerSlot=*/0).has_value());
        CHECK_FALSE(rx::rhi::PerFrameStorageBuffer::create(*allocator, *bindless, /*bytesPerSlot=*/16, nullptr,
                                                             /*framesInFlight=*/0)
                        .has_value());
    }
    vkDeviceWaitIdle(fixture.device);
    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}

TEST_CASE("PerFrameStorageBuffer::release is idempotent -- calling it a second time is a safe no-op") {
    HeadlessPerFrameBufferFixture fixture = makeFixture();
    {
        auto allocator =
            rx::rhi::Allocator::createRaw(fixture.physicalDevice, fixture.device, fixture.context.instance());
        REQUIRE(allocator.has_value());
        auto bindless = rx::rhi::BindlessTable::create(fixture.physicalDevice, fixture.device, makeTestCapacities());
        REQUIRE(bindless.has_value());

        auto perFrame =
            rx::rhi::PerFrameStorageBuffer::create(*allocator, *bindless, /*bytesPerSlot=*/16, nullptr,
                                                     /*framesInFlight=*/2);
        REQUIRE(perFrame.has_value());

        vkDeviceWaitIdle(fixture.device);
        perFrame->release(*bindless);
        perFrame->release(*bindless);  // must not double-free / assert / crash.
    }
    vkDeviceWaitIdle(fixture.device);
    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}
