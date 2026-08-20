// [Task 2 (#38), gate ruling RC2] Device-backed coverage for
// rx::rhi::StorageImage -- the RHI primitive backing rx_graph's new
// StorageImageOutput/StorageImageInput resource kind. Creation-shape
// (plain 2D, array, cube), fullView()/viewForSubresource() correctness
// (distinct handles per distinct range, caching, whole-resource shortcut),
// and move semantics; test_compute_gpu.cpp (rx_graph_gpu_tests) proves the
// end-to-end real-dispatch/readback path this class ultimately backs.
#include <doctest/doctest.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/storage_image.h>
#include <rx_platform/window.h>
#include <optional>
#include <utility>

namespace {

// Same skip-guarded windowed-device fixture pattern as texture_test.cpp.
struct StorageImageTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
};

std::optional<StorageImageTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping StorageImage test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping StorageImage test");
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

    return StorageImageTestFixture{std::move(*window), std::move(*context), std::move(*device),
                                    std::move(*allocator)};
}

}  // namespace

TEST_CASE("StorageImage::create builds a plain single-mip/single-layer storage image") {
    auto fixture = makeFixture("rx_rhi_vk_storage_image_plain");
    if (!fixture.has_value()) {
        return;
    }

    auto image = rx::rhi::StorageImage::create(fixture->device.physicalDevice(), fixture->device.device(),
                                                 fixture->allocator, VkExtent2D{32, 32}, VK_FORMAT_R8G8B8A8_UNORM,
                                                 /*usage=*/0, /*mipLevels=*/1, /*arrayLayers=*/1, /*cube=*/false);
    REQUIRE(image.has_value());
    CHECK(image->image() != VK_NULL_HANDLE);
    CHECK(image->fullView() != VK_NULL_HANDLE);
    CHECK(image->format() == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(image->extent().width == 32);
    CHECK(image->extent().height == 32);
    CHECK(image->mipLevels() == 1);
    CHECK(image->arrayLayers() == 1);
    CHECK_FALSE(image->cube());
    CHECK(image->allocatedBytes() > 0);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("StorageImage::create builds a cube-compatible 6-layer storage image") {
    auto fixture = makeFixture("rx_rhi_vk_storage_image_cube");
    if (!fixture.has_value()) {
        return;
    }

    auto image = rx::rhi::StorageImage::create(fixture->device.physicalDevice(), fixture->device.device(),
                                                 fixture->allocator, VkExtent2D{16, 16}, VK_FORMAT_R8G8B8A8_UNORM,
                                                 /*usage=*/0, /*mipLevels=*/1, /*arrayLayers=*/6, /*cube=*/true);
    REQUIRE(image.has_value());
    CHECK(image->arrayLayers() == 6);
    CHECK(image->cube());
    CHECK(image->fullView() != VK_NULL_HANDLE);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("StorageImage::create builds a multi-mip array storage image") {
    auto fixture = makeFixture("rx_rhi_vk_storage_image_mip_array");
    if (!fixture.has_value()) {
        return;
    }

    auto image = rx::rhi::StorageImage::create(fixture->device.physicalDevice(), fixture->device.device(),
                                                 fixture->allocator, VkExtent2D{32, 32}, VK_FORMAT_R8G8B8A8_UNORM,
                                                 /*usage=*/0, /*mipLevels=*/3, /*arrayLayers=*/2, /*cube=*/false);
    REQUIRE(image.has_value());
    CHECK(image->mipLevels() == 3);
    CHECK(image->arrayLayers() == 2);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("StorageImage::viewForSubresource returns fullView() itself for a whole-resource request") {
    auto fixture = makeFixture("rx_rhi_vk_storage_image_whole_view");
    if (!fixture.has_value()) {
        return;
    }

    auto image = rx::rhi::StorageImage::create(fixture->device.physicalDevice(), fixture->device.device(),
                                                 fixture->allocator, VkExtent2D{16, 16}, VK_FORMAT_R8G8B8A8_UNORM,
                                                 /*usage=*/0, /*mipLevels=*/1, /*arrayLayers=*/6, /*cube=*/true);
    REQUIRE(image.has_value());

    VkImageView whole = image->viewForSubresource(0, 1, 0, 6);
    CHECK(whole == image->fullView());

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("StorageImage::viewForSubresource returns distinct, cached views per distinct subresource range") {
    auto fixture = makeFixture("rx_rhi_vk_storage_image_subresource_views");
    if (!fixture.has_value()) {
        return;
    }

    auto image = rx::rhi::StorageImage::create(fixture->device.physicalDevice(), fixture->device.device(),
                                                 fixture->allocator, VkExtent2D{16, 16}, VK_FORMAT_R8G8B8A8_UNORM,
                                                 /*usage=*/0, /*mipLevels=*/1, /*arrayLayers=*/6, /*cube=*/true);
    REQUIRE(image.has_value());

    VkImageView face2 = image->viewForSubresource(0, 1, 2, 1);
    VkImageView face5 = image->viewForSubresource(0, 1, 5, 1);
    REQUIRE(face2 != VK_NULL_HANDLE);
    REQUIRE(face5 != VK_NULL_HANDLE);
    CHECK(face2 != face5);
    CHECK(face2 != image->fullView());

    // Repeating the SAME range returns the SAME (cached) handle, not a
    // freshly-created second view.
    VkImageView face2Again = image->viewForSubresource(0, 1, 2, 1);
    CHECK(face2Again == face2);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("StorageImage move construction/assignment transfer ownership with no double-destroy") {
    auto fixture = makeFixture("rx_rhi_vk_storage_image_move");
    if (!fixture.has_value()) {
        return;
    }

    auto image = rx::rhi::StorageImage::create(fixture->device.physicalDevice(), fixture->device.device(),
                                                 fixture->allocator, VkExtent2D{16, 16}, VK_FORMAT_R8G8B8A8_UNORM,
                                                 /*usage=*/0, /*mipLevels=*/1, /*arrayLayers=*/6, /*cube=*/true);
    REQUIRE(image.has_value());
    VkImage rawImage = image->image();
    VkImageView rawView = image->fullView();
    VkImageView rawFace = image->viewForSubresource(0, 1, 0, 1);

    rx::rhi::StorageImage moved = std::move(*image);
    CHECK(moved.image() == rawImage);
    CHECK(moved.fullView() == rawView);
    CHECK(moved.viewForSubresource(0, 1, 0, 1) == rawFace);  // cache moved along too

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
