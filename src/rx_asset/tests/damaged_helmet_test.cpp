#include <doctest/doctest.h>
#include <rx_asset/geometry_pool.h>
#include <rx_asset/registry.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/upload.h>
#include <rx_platform/window.h>
#include <rx_task/scheduler.h>
#include <filesystem>

// damaged_helmet_test.cpp -- DamagedHelmet integration test [D16, matrix-
// issue02 row 14: "DamagedHelmet integration test (counts/submeshes/
// skin-preservation assertions)"]. Fetched content, NOT committed to the
// repo -- see tools/fetch_assets.sh (mandatory, run before this binary in
// CI). Gracefully skips (MESSAGE + early return, matching every other
// "environment-dependent" skip guard in this codebase -- see
// import_gltf_gpu_test.cpp's own makeFixture()) when the asset has not
// been fetched locally, so a developer who never ran fetch_assets.sh does
// not see a spurious failure; CI always runs the fetch step first, so
// this assertion battery is NOT optional there.

using namespace rx::asset;

namespace {

std::string damagedHelmetPath() {
    return std::string(RX_ASSET_ROOT_DIR) + "/assets/fetched/DamagedHelmet/glTF/DamagedHelmet.gltf";
}

}  // namespace

TEST_CASE("importGltf: DamagedHelmet (fetched, real-world PBR asset) integration") {
    if (!std::filesystem::exists(damagedHelmetPath())) {
        MESSAGE("DamagedHelmet not fetched (run tools/fetch_assets.sh) -- skipping integration test");
        return;
    }

    auto window = rx::platform::Window::create("damaged_helmet", 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping DamagedHelmet test");
        return;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions, skipping DamagedHelmet test");
        return;
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
    auto pool = rx::asset::GeometryPool::create(*allocator, *device, *uploader);
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);

    Registry registry;
    ImportResult result = registry.importGltf(damagedHelmetPath(), *pool, *scheduler);
    REQUIRE(result.ok());

    REQUIRE(result.meshes.size() == 1);
    const MeshAsset& mesh = registry.mesh(result.meshes[0]);
    REQUIRE(mesh.submeshes.size() == 1);

    const Submesh& sub = mesh.submeshes[0];
    // The source POSITION/index accessors are u16 (46356 indices,
    // max index 14555) -- widened to u32 by the accessor-tools pipeline;
    // this asserts the WHOLE file's real index count survived intact.
    CHECK(sub.range.indexCount == 46356);
    CHECK(sub.range.indexCount % 3 == 0);
    CHECK(sub.bounds.isValid());

    // Skin-preservation assertion [matrix row 14]: DamagedHelmet has NO
    // skin in the source file -- asserting `present == false` (rather
    // than omitting the check) is itself the proof this importer does
    // not fabricate skin data for an unskinned asset.
    CHECK_FALSE(mesh.skin.present);

    REQUIRE(result.materials.size() == 1);
    const MaterialAsset& material = registry.material(result.materials[0]);
    CHECK(material.name == "Material_MR");
    CHECK(material.disposition == MaterialDisposition::StandardPBR);
    CHECK(material.baseColorTexture.present);
    CHECK(material.metallicRoughnessTexture.present);
    CHECK(material.normalTexture.present);
    CHECK(material.occlusionTexture.present);
    CHECK(material.emissiveTexture.present);

    CHECK(result.scene.instances.size() == 1);

    CHECK_FALSE(context->hasValidationErrors());
}
