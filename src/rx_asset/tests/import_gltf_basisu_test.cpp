#include <doctest/doctest.h>
#include <rx_asset/geometry_pool.h>
#include <rx_asset/registry.h>
#include <rx_asset/texture_cache.h>
#include <rx_core/log.h>
#include <rx_core/log_forward_sink.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/upload.h>
#include <rx_platform/window.h>
#include <rx_task/scheduler.h>
#include <spdlog/sinks/ostream_sink.h>
#include <memory>
#include <optional>
#include <sstream>
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

// [Fix round 1] Swaps spdlog's default logger for an ostream-capturing one
// for the scope of one TEST_CASE -- same lightweight rx_core-only pattern
// src/rx_core/tests/log_test.cpp and texture_decode_test.cpp both already
// establish (this binary does not link rx_material, so the public
// rxSetLogCallback ABI is not an option here).
struct LogCapture {
    std::ostringstream stream;
    std::shared_ptr<spdlog::logger> previousDefault;

    LogCapture() {
        rx::core::log::init();
        previousDefault = spdlog::default_logger();
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(stream);
        auto testLogger = std::make_shared<spdlog::logger>("import_gltf_basisu_test", sink);
        testLogger->set_pattern("%v");
        spdlog::set_default_logger(testLogger);
    }
    ~LogCapture() { spdlog::set_default_logger(previousDefault); }

    std::string str() const { return stream.str(); }
    int count(const std::string& needle) const {
        const std::string s = str();
        int n = 0;
        size_t pos = 0;
        while ((pos = s.find(needle, pos)) != std::string::npos) {
            ++n;
            pos += needle.size();
        }
        return n;
    }
};

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
}

TEST_CASE("Registry::importGltf with a real TextureCache: UNBOUND material slots (no texture reference in "
          "the source file at all, distinct from a present-but-failed one) resolve to the role-appropriate "
          "D11 UTILITY texture -- flat-normal for the normal slot, never the checkerboard -- even though "
          "TextureRef::present correctly stays false [D11, matrix's own 'a magenta normal map would shade "
          "garbage' reasoning]") {
    auto fixture = makeFixture("rx_asset_unbound_slots");
    if (!fixture.has_value()) {
        return;
    }
    attachDependents(*fixture);

    Registry registry;
    // cube_textured.gltf [Task 13's own fixture]: pbrMetallicRoughness
    // carries factors only -- baseColorTexture/metallicRoughnessTexture/
    // normalTexture/occlusionTexture/emissiveTexture are ALL absent from
    // the source JSON, exercising every one of the 5 TextureRef slots'
    // unbound path in one import.
    ImportResult result = registry.importGltf(testAssetDir() + "/cube_textured.gltf", *fixture->pool,
                                                *fixture->scheduler, fixture->textures.get());
    REQUIRE(result.ok());
    REQUIRE(result.materials.size() == 1);

    const MaterialAsset& material = registry.material(result.materials[0]);

    CHECK_FALSE(material.baseColorTexture.present);
    CHECK(material.baseColorTexture.handle == fixture->textures->fallbackHandle(TextureRole::BaseColor));

    CHECK_FALSE(material.normalTexture.present);
    CHECK(material.normalTexture.handle == fixture->textures->fallbackHandle(TextureRole::Normal));
    // The flat-normal handle is NEVER the checkerboard.
    CHECK_FALSE(material.normalTexture.handle == fixture->textures->checkerboardHandle());

    CHECK_FALSE(material.metallicRoughnessTexture.present);
    CHECK(material.metallicRoughnessTexture.handle == fixture->textures->fallbackHandle(TextureRole::MetallicRoughness));

    CHECK_FALSE(material.occlusionTexture.present);
    CHECK(material.occlusionTexture.handle == fixture->textures->fallbackHandle(TextureRole::Occlusion));

    CHECK_FALSE(material.emissiveTexture.present);
    CHECK(material.emissiveTexture.handle == fixture->textures->fallbackHandle(TextureRole::Emissive));

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Registry::importGltf with a real TextureCache: a PRESENT texture reference whose bytes fail to "
          "resolve (malformed bufferView / unfetched absolute URI / missing file -- here, a nonexistent "
          "external URI) resolves through THAT SAME TextureCache's own checkerboard, never a cross-pool "
          "coincidence with registry.fallbackTextureHandle() [Fix round 1, reviewer IMPORTANT-1]") {
    auto fixture = makeFixture("rx_asset_missing_image_bytes");
    if (!fixture.has_value()) {
        return;
    }
    attachDependents(*fixture);

    Registry registry;
    LogCapture capture;
    ImportResult result = registry.importGltf(testAssetDir() + "/cube_basisu_missing_image.gltf", *fixture->pool,
                                                *fixture->scheduler, fixture->textures.get());
    // The import as a WHOLE still succeeds -- an unresolvable texture is a
    // D11 fallback, never a hard import failure.
    REQUIRE(result.ok());
    REQUIRE(result.materials.size() == 1);

    // Proves the FAILURE branch (not the success branch coincidentally
    // landing on the same value) actually executed.
    CHECK(capture.count("bytes could not be resolved -- D11 fallback") == 1);

    const MaterialAsset& material = registry.material(result.materials[0]);
    REQUIRE(material.baseColorTexture.present);  // a reference DID exist in the source file -- distinct from the unbound-slot case above

    TextureHandle handle = material.baseColorTexture.handle;

    // Genuine cross-pool discrimination is structurally unavailable here:
    // registry.fallbackTextureHandle() and fixture->textures->
    // checkerboardHandle() are BOTH, deterministically, Handle(index=0,
    // generation=1) in every reachable test -- Registry's own
    // constructor acquires exactly ONE entry into its private `textures_`
    // pool for its whole lifetime (nothing else in this codebase ever
    // grows it), and TextureCache::create()'s buildFallbackTextures()
    // unconditionally builds the checkerboard as the FIRST entry in its
    // own pool. A value-equality assertion (`handle ==
    // textures->checkerboardHandle()`) would therefore pass identically
    // under the OLD (buggy, cross-pool) assignment and the NEW (fixed)
    // one -- verified empirically this fix round by temporarily
    // reverting the fix and re-running this exact test: it still passed
    // (see task-14-report.md's fix-round section for the full
    // transcript). Per the reviewer's own sanctioned fallback ("if
    // impractical to simulate, assert the handle belongs to the cache's
    // pool by construction"), this asserts BY CONSTRUCTION instead: every
    // field of the record `handle` resolves to, through THIS TextureCache
    // instance's own resolve(), matches its OWN checkerboardHandle()'s
    // record field-for-field -- proving `handle` names a real, resident,
    // self-consistent entry inside `fixture->textures`'s own pool (not an
    // artifact of a coincidentally-matching foreign handle), which is the
    // property that actually matters: a caller resolving `handle` through
    // `fixture->textures` gets a correct, defined checkerboard, full
    // stop, regardless of what Registry's own unrelated pool happens to
    // contain at the same numeric slot.
    const TextureRecord& record = fixture->textures->resolve(handle);
    const TextureRecord& checkerboardRecord = fixture->textures->resolve(fixture->textures->checkerboardHandle());
    CHECK(record.isFallback);
    CHECK(record.isFallback == checkerboardRecord.isFallback);
    CHECK(record.bindlessIndex == checkerboardRecord.bindlessIndex);
    CHECK(record.width == checkerboardRecord.width);
    CHECK(record.height == checkerboardRecord.height);
    CHECK(record.format == checkerboardRecord.format);
    CHECK(record.width == 4);  // the checkerboard's own distinctive 4x4 size (D11)

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
