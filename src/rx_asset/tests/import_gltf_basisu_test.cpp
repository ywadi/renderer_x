#include <doctest/doctest.h>
#include <rx_asset/geometry_pool.h>
#include <rx_asset/registry.h>
#include <rx_asset/texture_cache.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/upload.h>
#include <rx_platform/window.h>
#include <rx_task/scheduler.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>

// import_gltf_basisu_test.cpp -- end-to-end coverage for the KHR_texture_
// basisu wiring from Registry::importGltf() THROUGH a real
// rx::asset::TextureCache [Phase 4 Stage 1 Task 14, spec D10, gate
// matrix-issue03 "KHR_texture_basisu wiring from import" row]. Separate
// from import_gltf_gpu_test.cpp (Task 13's own end-to-end coverage,
// texture-free) and texture_cache_test.cpp (TextureCache exercised in
// isolation, never through a real glTF import) -- this file is the ONE
// place both halves are proven wired together: a glTF material's
// baseColorTexture reference resolves to a REAL, non-fallback texture via
// the SAME TextureCache instance the import was given, with its role
// inferred from the MATERIAL SLOT it came from, never the referenced
// image's own (deliberately misleading, here) filename.

using namespace rx::asset;

namespace {

std::string testAssetDir() { return std::string(RX_ASSET_ROOT_DIR) + "/assets/test"; }

// Mirrors import_gltf_gpu_test.cpp's own GpTestFixture two-phase
// construction EXACTLY (see that file's own header comment on
// attachPoolAndScheduler() for why: every reference-capturing member
// -- GeometryPool, TextureCache -- must be built against the CALLER's
// own stable storage, never inside makeFixture() itself, or it captures
// dangling references into a temporary this function's own return
// statement then moves from).
struct BasisuTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    rx::rhi::Uploader uploader;
    rx::rhi::BindlessTable bindless;
    rx::rhi::DeletionQueue deletionQueue;
    std::unique_ptr<rx::asset::GeometryPool> pool;
    std::unique_ptr<rx::task::Scheduler> scheduler;
    std::unique_ptr<TextureCache> textures;
};

std::optional<BasisuTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping KHR_texture_basisu import test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping test");
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
    auto uploader = rx::rhi::Uploader::create(*allocator, *device);
    REQUIRE(uploader.has_value());
    rx::rhi::BindlessTable::Capacities capacities{/*sampledImages=*/32, /*samplers=*/4, /*storageBuffers=*/1};
    auto bindless = rx::rhi::BindlessTable::create(device->physicalDevice(), device->device(), capacities);
    REQUIRE(bindless.has_value());
    rx::rhi::DeletionQueue deletionQueue;

    return BasisuTestFixture{std::move(*window),      std::move(*context),   std::move(*device),
                              std::move(*allocator),    std::move(*uploader),  std::move(*bindless),
                              std::move(deletionQueue), nullptr,               nullptr,
                              nullptr};
}

void attachDependents(BasisuTestFixture& fixture) {
    fixture.pool = rx::asset::GeometryPool::create(fixture.allocator, fixture.device, fixture.uploader);
    fixture.scheduler = rx::task::Scheduler::create(2);
    REQUIRE(fixture.scheduler != nullptr);
    fixture.textures = TextureCache::create(fixture.allocator, fixture.device, fixture.uploader, fixture.bindless,
                                             fixture.deletionQueue);
    REQUIRE(fixture.textures != nullptr);
}

}  // namespace

TEST_CASE("Registry::importGltf with a real TextureCache: a KHR_texture_basisu baseColorTexture resolves to a "
          "REAL (non-fallback) texture, with role inferred from the MATERIAL SLOT -- never the referenced "
          "image's own filename, which this fixture deliberately names misleadingly "
          "('cube_basisu_misleading_normal.ktx2' used as baseColor, not normal) [matrix-issue03]") {
    auto fixture = makeFixture("rx_asset_basisu_import");
    if (!fixture.has_value()) {
        return;
    }
    attachDependents(*fixture);

    Registry registry;
    ImportResult result = registry.importGltf(testAssetDir() + "/cube_basisu.gltf", *fixture->pool, *fixture->scheduler,
                                                fixture->textures.get());
    REQUIRE(result.ok());
    REQUIRE(result.materials.size() == 1);

    const MaterialAsset& material = registry.material(result.materials[0]);
    REQUIRE(material.baseColorTexture.present);

    TextureHandle texHandle = material.baseColorTexture.handle;
    CHECK_FALSE(texHandle == fixture->textures->checkerboardHandle());

    const TextureRecord& record = fixture->textures->resolve(texHandle);
    CHECK_FALSE(record.isFallback);
    // THE assertion this test exists for: role came from the SLOT
    // (baseColorTexture -> TextureRole::BaseColor), never from the
    // image's own filename (which says "misleading_normal").
    CHECK(record.role == TextureRole::BaseColor);
    CHECK(record.width == 4);
    CHECK(record.height == 4);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Registry::importGltf with textures=nullptr (Task 13's original, still-supported contract): a "
          "KHR_texture_basisu material reference falls back to the registry's own D11 placeholder, exactly "
          "as before this task -- byte-identical behavior when no TextureCache is supplied") {
    auto fixture = makeFixture("rx_asset_basisu_nullptr");
    if (!fixture.has_value()) {
        return;
    }
    attachDependents(*fixture);

    Registry registry;
    ImportResult result =
        registry.importGltf(testAssetDir() + "/cube_basisu.gltf", *fixture->pool, *fixture->scheduler, nullptr);
    REQUIRE(result.ok());
    REQUIRE(result.materials.size() == 1);

    const MaterialAsset& material = registry.material(result.materials[0]);
    REQUIRE(material.baseColorTexture.present);
    CHECK(material.baseColorTexture.handle == registry.fallbackTextureHandle());

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
