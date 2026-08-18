#include <doctest/doctest.h>
#include <rx_asset/geometry_pool.h>
#include <rx_asset/registry.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/upload.h>
#include <rx_platform/window.h>
#include <rx_task/scheduler.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// import_gltf_gpu_test.cpp -- end-to-end coverage for
// rx::asset::Registry::importGltf() [Phase 4 Stage 1 Task 13, spec D7/
// D11/D12/D16/D24/D25, gate matrix-issue02 as amended by
// gate/rulings-2026-08-18.md #2]. GPU tests only -- device-free is
// impossible for the real behavior here: every successful import ends in
// a real GeometryPool::upload() (see gltf_pipeline_test.cpp for the
// device-free per-primitive pipeline coverage this file does NOT
// duplicate).

using namespace rx::asset;

namespace {

std::string testAssetDir() { return std::string(RX_ASSET_ROOT_DIR) + "/assets/test"; }

struct GpTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    rx::rhi::Uploader uploader;
    std::unique_ptr<rx::asset::GeometryPool> pool;
    std::unique_ptr<rx::task::Scheduler> scheduler;
};

std::optional<GpTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping glTF import test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping glTF import test");
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

    // pool/scheduler are attached separately, by attachPoolAndScheduler()
    // below, AFTER this optional has been returned to and dereferenced
    // by the caller -- NOT here. GeometryPool::create() stores
    // REFERENCES to the Allocator/Device/Uploader arguments it is given
    // (matching every other Allocator-derived RAII type's documented
    // lifetime discipline in this codebase); constructing it against
    // THIS function's local `fixture` before returning it by value would
    // capture references into a temporary that this function's return
    // statement then MOVES from -- a dangling-reference-after-move bug
    // (reproduced directly this task: a null VmaAllocator SIGSEGV inside
    // the very first GeometryPool::upload() call). Every reference-
    // capturing member must be constructed against the CALLER's own
    // stable storage, exactly like geometry_pool_test.cpp's own fixture
    // never stores a GeometryPool inside GpTestFixture either.
    return GpTestFixture{std::move(*window), std::move(*context), std::move(*device), std::move(*allocator),
                          std::move(*uploader), nullptr, nullptr};
}

void attachPoolAndScheduler(GpTestFixture& fixture) {
    fixture.pool = rx::asset::GeometryPool::create(fixture.allocator, fixture.device, fixture.uploader);
    fixture.scheduler = rx::task::Scheduler::create(2);
    REQUIRE(fixture.scheduler != nullptr);
}

// Copies `count` PoolVertex elements starting at `firstVertex` out of
// `blockId`'s device-local vertex buffer -- same readback pattern
// geometry_pool_test.cpp's own readBackBuffer() establishes.
std::vector<rx::asset::PoolVertex> readBackVertices(GpTestFixture& fixture, uint32_t blockId, int32_t firstVertex,
                                                      uint32_t count) {
    VkBuffer src = fixture.pool->vertexBufferHandle(blockId);
    const VkDeviceSize offset = static_cast<VkDeviceSize>(firstVertex) * sizeof(rx::asset::PoolVertex);
    const VkDeviceSize size = static_cast<VkDeviceSize>(count) * sizeof(rx::asset::PoolVertex);

    auto readback = fixture.allocator.createHostVisibleBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());
    auto cmdCtx = rx::rhi::CommandContext::create(fixture.device.device(), fixture.device.graphicsQueue(),
                                                    fixture.device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferCopy region{offset, 0, size};
        vkCmdCopyBuffer(cmd, src, readback->handle(), 1, &region);
    });
    readback->invalidate();

    std::vector<rx::asset::PoolVertex> result(count);
    std::memcpy(result.data(), readback->mappedData(), static_cast<size_t>(size));
    return result;
}

std::vector<std::byte> readFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.is_open());
    const auto size = f.tellg();
    f.seekg(0);
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

}  // namespace

TEST_CASE("importGltf: committed cube_textured.gltf imports with correct counts, AABB, and tangent presence") {
    auto fixture = makeFixture("import_cube");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    Registry registry;
    ImportResult result =
        registry.importGltf(testAssetDir() + "/cube_textured.gltf", *fixture->pool, *fixture->scheduler);

    REQUIRE(result.ok());
    REQUIRE(result.meshes.size() == 1);
    const MeshAsset& mesh = registry.mesh(result.meshes[0]);
    REQUIRE(mesh.submeshes.size() == 1);
    const Submesh& sub = mesh.submeshes[0];
    CHECK(sub.range.indexCount == 36);
    CHECK(sub.range.indexCount % 3 == 0);

    CHECK(sub.bounds.isValid());
    CHECK(sub.bounds.min.x == doctest::Approx(-0.5F));
    CHECK(sub.bounds.max.x == doctest::Approx(0.5F));
    CHECK(sub.bounds.min.y == doctest::Approx(-0.5F));
    CHECK(sub.bounds.max.y == doctest::Approx(0.5F));
    CHECK(sub.bounds.min.z == doctest::Approx(-0.5F));
    CHECK(sub.bounds.max.z == doctest::Approx(0.5F));

    REQUIRE(result.materials.size() == 1);
    const MaterialAsset& mat = registry.material(result.materials[0]);
    CHECK(mat.baseColorFactor.r == doctest::Approx(0.8F));
    CHECK(mat.baseColorFactor.g == doctest::Approx(0.2F));
    CHECK(mat.metallicFactor == doctest::Approx(0.1F));
    CHECK(mat.roughnessFactor == doctest::Approx(0.6F));
    CHECK(sub.material == result.materials[0]);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: sync import issues at most ONE blocking upload wait for a multi-primitive file") {
    auto fixture = makeFixture("import_wait_count");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    Registry registry;
    const uint32_t waitsBefore = fixture->uploader.blockingRingWaitCount();
    ImportResult result =
        registry.importGltf(testAssetDir() + "/cube_textured.gltf", *fixture->pool, *fixture->scheduler);
    REQUIRE(result.ok());
    // GeometryPool::upload() itself does exactly one flush()+wait() per
    // call [Task 12's own contract], and this importer batches EVERY
    // submesh in the whole file into ONE combined upload() call
    // (import_gltf.cpp's own "ONE combined GeometryPool upload" section)
    // specifically so a multi-primitive file never waits more than once
    // -- see that file's comment for why this is the chosen design given
    // GeometryPool's landed per-call-wait contract [matrix-issue02 row
    // 12].
    CHECK(fixture->uploader.blockingRingWaitCount() <= waitsBefore + 1);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: byte-source spy proves ZERO direct filesystem reads for external-URI content") {
    auto fixture = makeFixture("import_spy");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);

    // Load the committed cube's real bytes ONCE, here in the test
    // harness (not the importer) -- this is legitimate setup, not a
    // filesystem access the importer itself performs.
    const std::string dir = testAssetDir();
    std::vector<std::byte> documentBytes = readFileBytes(dir + "/cube_textured.gltf");
    std::vector<std::byte> binBytes = readFileBytes(dir + "/cube_textured.bin");

    // A spy ByteSource backed ENTIRELY by an in-memory map -- constructed
    // pointing at NO real directory at all, so if the importer ever fell
    // back to a direct std::ifstream/std::filesystem call somewhere
    // instead of going through this interface, that call would have
    // nothing on disk to find (this test never creates
    // "cube_textured.bin" anywhere on the real filesystem) and the
    // import would fail outright rather than silently succeeding via a
    // bypassed path.
    class SpyByteSource final : public ByteSource {
    public:
        std::unordered_map<std::string, std::vector<std::byte>> entries;
        int callCount = 0;
        std::optional<std::vector<std::byte>> read(const std::string& uri) override {
            ++callCount;
            auto it = entries.find(uri);
            if (it == entries.end()) {
                return std::nullopt;  // fails on miss -- see this TEST_CASE's own name
            }
            return it->second;
        }
    };

    SpyByteSource spy;
    spy.entries["cube_textured.bin"] = binBytes;

    Registry registry;
    ImportResult result = registry.importGltf(documentBytes, spy, *fixture->pool, *fixture->scheduler);

    REQUIRE(result.ok());
    CHECK(spy.callCount >= 1);  // the external .bin WAS requested through the spy
    REQUIRE(result.meshes.size() == 1);
    CHECK(registry.mesh(result.meshes[0]).submeshes.size() == 1);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: in-memory .glb import (no file on disk anywhere) succeeds") {
    auto fixture = makeFixture("import_glb_memory");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    std::vector<std::byte> glbBytes = readFileBytes(testAssetDir() + "/cube.glb");

    class NeverCalledSource final : public ByteSource {
    public:
        std::optional<std::vector<std::byte>> read(const std::string&) override {
            FAIL("byte source read() called for a self-contained GLB -- every buffer is embedded in the BIN chunk");
            return std::nullopt;
        }
    };
    NeverCalledSource source;

    Registry registry;
    ImportResult result = registry.importGltf(glbBytes, source, *fixture->pool, *fixture->scheduler);
    REQUIRE(result.ok());
    REQUIRE(result.meshes.size() == 1);
    CHECK(registry.mesh(result.meshes[0]).submeshes.size() == 1);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: three same-cube source-variant packagings deep-equal") {
    auto fixture = makeFixture("import_source_variants");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    const std::string dir = testAssetDir();

    Registry registry;
    ImportResult external = registry.importGltf(dir + "/cube_textured.gltf", *fixture->pool, *fixture->scheduler);
    ImportResult dataUri = registry.importGltf(dir + "/cube_datauri.gltf", *fixture->pool, *fixture->scheduler);
    ImportResult glb = registry.importGltf(dir + "/cube.glb", *fixture->pool, *fixture->scheduler);

    REQUIRE(external.ok());
    REQUIRE(dataUri.ok());
    REQUIRE(glb.ok());

    auto submeshOf = [&](const ImportResult& r) -> const Submesh& { return registry.mesh(r.meshes[0]).submeshes[0]; };
    const Submesh& a = submeshOf(external);
    const Submesh& b = submeshOf(dataUri);
    const Submesh& c = submeshOf(glb);

    CHECK(a.range.indexCount == b.range.indexCount);
    CHECK(a.range.indexCount == c.range.indexCount);
    CHECK(a.bounds.min.x == doctest::Approx(b.bounds.min.x));
    CHECK(a.bounds.min.x == doctest::Approx(c.bounds.min.x));
    CHECK(a.bounds.max.x == doctest::Approx(b.bounds.max.x));
    CHECK(a.bounds.max.x == doctest::Approx(c.bounds.max.x));
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: malformed-file battery -- named error, zero partial registry mutation, no crash") {
    auto fixture = makeFixture("import_malformed");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);

    class MemorySource final : public ByteSource {
    public:
        std::optional<std::vector<std::byte>> read(const std::string&) override { return std::nullopt; }
    };
    MemorySource source;

    const auto toBytes = [](const std::string& s) {
        std::vector<std::byte> b(s.size());
        std::memcpy(b.data(), s.data(), s.size());
        return b;
    };

    // Every SUBCASE below checks BOTH r.meshes/materials.empty() (what
    // THIS call reported back) AND registry.meshCountForTesting()/
    // materialCountForTesting() (the registry's OWN internal live-entry
    // count, unaffected by whatever a call chooses to report) against
    // its pre-import baseline -- see registry.h's own comment on why
    // these are a DIFFERENT property: a hypothetical bug that silently
    // registered an asset without adding its handle to ImportResult
    // would satisfy the first check while failing the second. This
    // exact distinction was caught empirically during this task's own
    // scratch-worktree revert testing (see the task report) -- the
    // r.meshes.empty()-only version of this test did NOT discriminate a
    // premature-registration injection; this version does.
    SUBCASE("not-JSON garbage") {
        Registry registry;
        const size_t meshesBefore = registry.meshCountForTesting();
        const size_t materialsBefore = registry.materialCountForTesting();
        auto bytes = toBytes("this is not json at all { [ garbage");
        ImportResult r = registry.importGltf(bytes, source, *fixture->pool, *fixture->scheduler);
        CHECK_FALSE(r.ok());
        CHECK(r.meshes.empty());
        CHECK(registry.meshCountForTesting() == meshesBefore);
        CHECK(registry.materialCountForTesting() == materialsBefore);
    }
    SUBCASE("valid JSON, invalid glTF") {
        Registry registry;
        const size_t meshesBefore = registry.meshCountForTesting();
        const size_t materialsBefore = registry.materialCountForTesting();
        auto bytes = toBytes(R"({"foo": "bar"})");
        ImportResult r = registry.importGltf(bytes, source, *fixture->pool, *fixture->scheduler);
        CHECK_FALSE(r.ok());
        CHECK(r.meshes.empty());
        CHECK(registry.meshCountForTesting() == meshesBefore);
        CHECK(registry.materialCountForTesting() == materialsBefore);
    }
    SUBCASE("truncated GLB") {
        Registry registry;
        const size_t meshesBefore = registry.meshCountForTesting();
        const size_t materialsBefore = registry.materialCountForTesting();
        std::vector<std::byte> bytes = readFileBytes(testAssetDir() + "/cube.glb");
        bytes.resize(bytes.size() / 2);
        ImportResult r = registry.importGltf(bytes, source, *fixture->pool, *fixture->scheduler);
        CHECK_FALSE(r.ok());
        CHECK(r.meshes.empty());
        CHECK(registry.meshCountForTesting() == meshesBefore);
        CHECK(registry.materialCountForTesting() == materialsBefore);
    }
    SUBCASE("wrong-magic GLB") {
        Registry registry;
        const size_t meshesBefore = registry.meshCountForTesting();
        const size_t materialsBefore = registry.materialCountForTesting();
        std::vector<std::byte> bytes = readFileBytes(testAssetDir() + "/cube.glb");
        bytes[0] = std::byte{0x00};
        ImportResult r = registry.importGltf(bytes, source, *fixture->pool, *fixture->scheduler);
        CHECK_FALSE(r.ok());
        CHECK(r.meshes.empty());
        CHECK(registry.meshCountForTesting() == meshesBefore);
        CHECK(registry.materialCountForTesting() == materialsBefore);
    }
    SUBCASE("unsupported version") {
        Registry registry;
        const size_t meshesBefore = registry.meshCountForTesting();
        const size_t materialsBefore = registry.materialCountForTesting();
        auto bytes = toBytes(R"({"asset":{"version":"1.0"},"scenes":[{"nodes":[]}],"scene":0})");
        ImportResult r = registry.importGltf(bytes, source, *fixture->pool, *fixture->scheduler);
        CHECK_FALSE(r.ok());
        CHECK(r.meshes.empty());
        CHECK(registry.meshCountForTesting() == meshesBefore);
        CHECK(registry.materialCountForTesting() == materialsBefore);
    }
    SUBCASE("missing file (path overload)") {
        Registry registry;
        const size_t meshesBefore = registry.meshCountForTesting();
        const size_t materialsBefore = registry.materialCountForTesting();
        ImportResult r = registry.importGltf(std::filesystem::path("/nonexistent/path/does_not_exist.gltf"), *fixture->pool,
                                               *fixture->scheduler);
        CHECK_FALSE(r.ok());
        CHECK(r.error == ImportError::ByteSourceUnavailable);
        CHECK(r.meshes.empty());
        CHECK(registry.meshCountForTesting() == meshesBefore);
        CHECK(registry.materialCountForTesting() == materialsBefore);
    }
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: unknown required extension yields a named error, unknown used-only extension never fails") {
    auto fixture = makeFixture("import_extensions");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    class MemorySource final : public ByteSource {
    public:
        std::optional<std::vector<std::byte>> read(const std::string&) override { return std::nullopt; }
    };
    MemorySource source;
    const auto toBytes = [](const std::string& s) {
        std::vector<std::byte> b(s.size());
        std::memcpy(b.data(), s.data(), s.size());
        return b;
    };

    SUBCASE("required + unsupported -> error") {
        Registry registry;
        auto bytes = toBytes(
            R"({"asset":{"version":"2.0"},"extensionsRequired":["THIS_DOES_NOT_EXIST_ANYWHERE"],)"
            R"("extensionsUsed":["THIS_DOES_NOT_EXIST_ANYWHERE"],"scenes":[{"nodes":[]}],"scene":0})");
        ImportResult r = registry.importGltf(bytes, source, *fixture->pool, *fixture->scheduler);
        CHECK_FALSE(r.ok());
        CHECK(r.error == ImportError::UnknownRequiredExtension);
    }
    SUBCASE("used-only + unsupported -> import still succeeds") {
        Registry registry;
        auto bytes = toBytes(
            R"({"asset":{"version":"2.0"},"extensionsUsed":["THIS_DOES_NOT_EXIST_ANYWHERE"],)"
            R"("scenes":[{"nodes":[]}],"scene":0})");
        ImportResult r = registry.importGltf(bytes, source, *fixture->pool, *fixture->scheduler);
        CHECK(r.ok());
    }
}

TEST_CASE("importGltf: D24 eviction invariant -- evict, resolve fallback, reimport, resolve real again") {
    auto fixture = makeFixture("import_eviction");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    Registry registry;
    ImportResult first = registry.importGltf(testAssetDir() + "/cube_textured.gltf", *fixture->pool,
                                               *fixture->scheduler);
    REQUIRE(first.ok());
    MeshHandle handle = first.meshes[0];

    const MeshAsset& before = registry.mesh(handle);
    CHECK_FALSE(before.submeshes.empty());

    registry.evictForTesting(handle);
    const MeshAsset& evicted = registry.mesh(handle);
    CHECK(evicted.submeshes.empty());  // resolved to the D11 fallback -- never a null deref, never an assert

    ImportResult second = registry.importGltf(testAssetDir() + "/cube_textured.gltf", *fixture->pool,
                                                *fixture->scheduler);
    REQUIRE(second.ok());
    const MeshAsset& reimported = registry.mesh(second.meshes[0]);
    CHECK_FALSE(reimported.submeshes.empty());  // the NEW handle resolves to the real, freshly-imported asset

    // The OLD (evicted) handle is untouched by the reimport -- still
    // resolves to the fallback, proving eviction state is per-handle,
    // not a registry-wide flag the reimport happened to clear.
    CHECK(registry.mesh(handle).submeshes.empty());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: KHR_mesh_quantization dequantizes within tolerance of the unquantized values") {
    auto fixture = makeFixture("import_quantized");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    Registry registry;
    ImportResult result =
        registry.importGltf(testAssetDir() + "/cube_quantized.gltf", *fixture->pool, *fixture->scheduler);
    REQUIRE(result.ok());
    REQUIRE(result.meshes.size() == 1);
    const MeshAsset& mesh = registry.mesh(result.meshes[0]);
    REQUIRE(mesh.submeshes.size() == 1);
    // Quantized to normalized SHORT (1/32767 step); the quad spans
    // [-0.5,0.5] on X and Y -- bounds should land within a couple
    // quantization steps of the authored extent.
    const float tol = 4.0F / 32767.0F;
    CHECK(mesh.submeshes[0].bounds.min.x == doctest::Approx(-0.5F).epsilon(tol));
    CHECK(mesh.submeshes[0].bounds.max.x == doctest::Approx(0.5F).epsilon(tol));
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: sparse accessor substitution is applied") {
    auto fixture = makeFixture("import_sparse");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    Registry registry;
    ImportResult result =
        registry.importGltf(testAssetDir() + "/cube_sparse.gltf", *fixture->pool, *fixture->scheduler);
    REQUIRE(result.ok());
    REQUIRE(result.meshes.size() == 1);
    const MeshAsset& mesh = registry.mesh(result.meshes[0]);
    REQUIRE(mesh.submeshes.size() == 1);
    // Base data is implicit zero; sparse overrides corners 1,2 to
    // (1,0,0)/(0,1,0) -- the resulting bounds must reflect the
    // OVERRIDDEN values, not an all-zero degenerate point.
    CHECK(mesh.submeshes[0].bounds.max.x == doctest::Approx(1.0F));
    CHECK(mesh.submeshes[0].bounds.max.y == doctest::Approx(1.0F));
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: morph targets and default weights are preserved, unconsumed") {
    auto fixture = makeFixture("import_morph");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    Registry registry;
    ImportResult result =
        registry.importGltf(testAssetDir() + "/cube_morph.gltf", *fixture->pool, *fixture->scheduler);
    REQUIRE(result.ok());
    const MeshAsset& mesh = registry.mesh(result.meshes[0]);
    REQUIRE(mesh.morphWeights.size() == 1);
    CHECK(mesh.morphWeights[0] == doctest::Approx(0.5F));
    REQUIRE(mesh.submeshes.size() == 1);
    REQUIRE(mesh.submeshes[0].morphTargets.size() == 1);
    const MorphTarget& target = mesh.submeshes[0].morphTargets[0];
    REQUIRE(target.positionDeltas.size() == 3);
    CHECK(target.positionDeltas[1].z == doctest::Approx(1.0F));
    CHECK(target.positionDeltas[2].z == doctest::Approx(1.0F));
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: animation channels/samplers round-trip bit-exact, including CUBICSPLINE triplets") {
    auto fixture = makeFixture("import_anim");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    Registry registry;
    ImportResult result =
        registry.importGltf(testAssetDir() + "/cube_anim.gltf", *fixture->pool, *fixture->scheduler);
    REQUIRE(result.ok());
    REQUIRE(result.scene.animations.size() == 1);
    const AnimationClipData& clip = result.scene.animations[0];
    REQUIRE(clip.samplers.size() == 3);
    REQUIRE(clip.channels.size() == 3);

    const AnimationSamplerData& linear = clip.samplers[0];
    CHECK(linear.interpolation == AnimationInterpolation::Linear);
    REQUIRE(linear.outputFlat.size() == 6);
    CHECK(linear.outputFlat[3] == doctest::Approx(1.0F));
    CHECK(linear.outputFlat[4] == doctest::Approx(2.0F));
    CHECK(linear.outputFlat[5] == doctest::Approx(3.0F));

    const AnimationSamplerData& step = clip.samplers[1];
    CHECK(step.interpolation == AnimationInterpolation::Step);
    REQUIRE(step.outputFlat.size() == 8);
    CHECK(step.outputFlat[6] == doctest::Approx(0.70710678F));
    CHECK(step.outputFlat[7] == doctest::Approx(0.70710678F));

    const AnimationSamplerData& cubic = clip.samplers[2];
    CHECK(cubic.interpolation == AnimationInterpolation::CubicSpline);
    REQUIRE(cubic.outputFlat.size() == 18);  // 2 keyframes * 3 (in/value/out) * 3 components
    CHECK(cubic.outputFlat[4] == doctest::Approx(1.0F));   // keyframe 0 value.y
    CHECK(cubic.outputFlat[13] == doctest::Approx(2.0F));  // keyframe 1 value.y

    CHECK(clip.channels[0].path == AnimationPath::Translation);
    CHECK(clip.channels[1].path == AnimationPath::Rotation);
    CHECK(clip.channels[2].path == AnimationPath::Scale);
    for (const auto& ch : clip.channels) {
        REQUIRE(ch.targetNodeIndex.has_value());
        CHECK(*ch.targetNodeIndex == 0);
    }
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: skin (joints/IBM/JOINTS_0/WEIGHTS_0) round-trips into MeshAsset::skin, unconsumed") {
    auto fixture = makeFixture("import_skin");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    Registry registry;
    ImportResult result =
        registry.importGltf(testAssetDir() + "/cube_skin.gltf", *fixture->pool, *fixture->scheduler);
    REQUIRE(result.ok());
    const MeshAsset& mesh = registry.mesh(result.meshes[0]);
    CHECK(mesh.skin.present);
    REQUIRE(mesh.skin.jointNodeIndices.size() == 2);
    REQUIRE(mesh.skin.inverseBindMatrices.size() == 2);
    for (const auto& m : mesh.skin.inverseBindMatrices) {
        CHECK(m[0][0] == doctest::Approx(1.0F));  // identity IBMs in the fixture
    }
    REQUIRE(mesh.submeshes.size() == 1);
    REQUIRE(mesh.submeshes[0].skinVertices.joints.size() == 4);
    REQUIRE(mesh.submeshes[0].skinVertices.weights.size() == 4);
    CHECK(mesh.submeshes[0].skinVertices.joints[0].x == 0);
    CHECK(mesh.submeshes[0].skinVertices.joints[2].x == 1);
    CHECK(mesh.submeshes[0].skinVertices.weights[0].x == doctest::Approx(1.0F));
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: cameras (perspective infinite + orthographic) and all 3 light types round-trip") {
    auto fixture = makeFixture("import_lights_camera");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    Registry registry;
    ImportResult result = registry.importGltf(testAssetDir() + "/cube_lights_camera.gltf", *fixture->pool,
                                                *fixture->scheduler);
    REQUIRE(result.ok());
    REQUIRE(result.scene.cameras.size() == 2);
    const CameraData* persp = nullptr;
    const CameraData* ortho = nullptr;
    for (const auto& c : result.scene.cameras) {
        if (c.type == CameraData::Type::Perspective) persp = &c;
        if (c.type == CameraData::Type::Orthographic) ortho = &c;
    }
    REQUIRE(persp != nullptr);
    REQUIRE(ortho != nullptr);
    CHECK(persp->yfov == doctest::Approx(0.8F));
    CHECK_FALSE(persp->zfar.has_value());  // infinite-perspective (zfar omitted)
    CHECK(ortho->xmag == doctest::Approx(2.0F));
    CHECK(ortho->zfar == doctest::Approx(100.0F));

    REQUIRE(result.scene.lights.size() == 3);
    bool sawDirectional = false, sawPoint = false, sawSpot = false;
    for (const auto& l : result.scene.lights) {
        if (l.type == LightData::Type::Directional) {
            sawDirectional = true;
            CHECK(l.intensity == doctest::Approx(3.0F));
        }
        if (l.type == LightData::Type::Point) {
            sawPoint = true;
            REQUIRE(l.range.has_value());
            CHECK(*l.range == doctest::Approx(10.0F));
        }
        if (l.type == LightData::Type::Spot) {
            sawSpot = true;
            REQUIRE(l.innerConeAngle.has_value());
            REQUIRE(l.outerConeAngle.has_value());
            CHECK(*l.innerConeAngle == doctest::Approx(0.2F));
            CHECK(*l.outerConeAngle == doctest::Approx(0.6F));
        }
    }
    CHECK(sawDirectional);
    CHECK(sawPoint);
    CHECK(sawSpot);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: EXT_meshopt_compression (real meshoptimizer-encoded fixture) decodes correctly") {
    auto fixture = makeFixture("import_meshopt_compressed");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    Registry registry;
    ImportResult result =
        registry.importGltf(testAssetDir() + "/cube_meshopt.gltf", *fixture->pool, *fixture->scheduler);
    REQUIRE(result.ok());
    REQUIRE(result.meshes.size() == 1);
    const MeshAsset& mesh = registry.mesh(result.meshes[0]);
    REQUIRE(mesh.submeshes.size() == 1);
    CHECK(mesh.submeshes[0].range.indexCount == 6);
    CHECK(mesh.submeshes[0].bounds.min.x == doctest::Approx(0.0F));
    CHECK(mesh.submeshes[0].bounds.max.x == doctest::Approx(1.0F));
    CHECK(mesh.submeshes[0].bounds.max.y == doctest::Approx(1.0F));
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: KHR_draco_mesh_compression (real Draco-encoded fixture) decodes within quantization tolerance") {
    auto fixture = makeFixture("import_draco_compressed");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    Registry registry;
    ImportResult result =
        registry.importGltf(testAssetDir() + "/cube_draco.gltf", *fixture->pool, *fixture->scheduler);
    REQUIRE(result.ok());
    REQUIRE(result.meshes.size() == 1);
    const MeshAsset& mesh = registry.mesh(result.meshes[0]);
    REQUIRE(mesh.submeshes.size() == 1);
    CHECK(mesh.submeshes[0].range.indexCount == 6);
    // 16-bit position quantization over a unit-scale bounding box.
    const float tol = 4.0F / 65535.0F;
    CHECK(mesh.submeshes[0].bounds.min.x == doctest::Approx(0.0F).epsilon(tol));
    CHECK(mesh.submeshes[0].bounds.max.x == doctest::Approx(1.0F).epsilon(tol));
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: D12 node flattening -- matrix node, TRS node, 3-level nesting, negative-determinant flag") {
    auto fixture = makeFixture("import_flatten");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    class MemorySource final : public ByteSource {
    public:
        std::optional<std::vector<std::byte>> read(const std::string&) override { return std::nullopt; }
    };
    MemorySource source;
    const auto toBytes = [](const std::string& s) {
        std::vector<std::byte> b(s.size());
        std::memcpy(b.data(), s.data(), s.size());
        return b;
    };

    // node0: TRS translate(1,0,0); node1 (matrix, scale(-1,1,1) via explicit matrix), child node2: TRS translate(0,2,0).
    // A minimal triangle mesh is reused for node0/node1's mesh reference; node2 has no mesh (pure hierarchy leaf).
    const std::string json = R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [
            {"mesh": 0, "translation": [1, 0, 0], "name": "trs_node"},
            {"mesh": 0, "matrix": [-1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1], "children": [2], "name": "matrix_node"},
            {"translation": [0, 2, 0], "name": "leaf_child"}
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "mode": 4}]}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min":[0,0,0], "max":[1,1,0]}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
        "buffers": [{"uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAA==",
                     "byteLength": 36}]
    })";

    Registry registry;
    ImportResult result = registry.importGltf(toBytes(json), source, *fixture->pool, *fixture->scheduler);
    REQUIRE(result.ok());
    REQUIRE(result.scene.instances.size() == 2);  // node0 and node1 both have meshes; node2 (leaf, no mesh) contributes none

    const InstanceRecord* trsInstance = nullptr;
    const InstanceRecord* matrixInstance = nullptr;
    for (const auto& inst : result.scene.instances) {
        if (inst.sourceNodeIndex == 0) trsInstance = &inst;
        if (inst.sourceNodeIndex == 1) matrixInstance = &inst;
    }
    REQUIRE(trsInstance != nullptr);
    REQUIRE(matrixInstance != nullptr);

    CHECK(trsInstance->worldTransform[3][0] == doctest::Approx(1.0F));
    CHECK_FALSE(trsInstance->negativeDeterminant);

    CHECK(matrixInstance->negativeDeterminant);  // scale(-1,1,1) flips winding
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltf: zero-scene document imports with an empty ImportedScene, no crash") {
    auto fixture = makeFixture("import_zero_scene");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);
    class MemorySource final : public ByteSource {
    public:
        std::optional<std::vector<std::byte>> read(const std::string&) override { return std::nullopt; }
    };
    MemorySource source;
    const std::string json = R"({"asset": {"version": "2.0"}})";
    std::vector<std::byte> bytes(json.size());
    std::memcpy(bytes.data(), json.data(), json.size());

    Registry registry;
    ImportResult result = registry.importGltf(bytes, source, *fixture->pool, *fixture->scheduler);
    REQUIRE(result.ok());
    CHECK(result.scene.instances.empty());
}

TEST_CASE("importGltf: u8 index accessor widens to u32 with a VALUE round-trip, not just a count match") {
    auto fixture = makeFixture("import_u8_index_widening");
    if (!fixture) {
        return;
    }
    attachPoolAndScheduler(*fixture);

    Registry registry;
    ImportResult result =
        registry.importGltf(testAssetDir() + "/cube_u8_indices.gltf", *fixture->pool, *fixture->scheduler);
    REQUIRE(result.ok());
    REQUIRE(result.meshes.size() == 1);
    const MeshAsset& mesh = registry.mesh(result.meshes[0]);
    REQUIRE(mesh.submeshes.size() == 1);
    const Submesh& sub = mesh.submeshes[0];
    CHECK(sub.range.indexCount == 6);

    // Read the final (post-meshoptimizer) vertex buffer back and read the
    // final index buffer back too, then reconstruct which POSITIONS the
    // widened u32 indices actually address -- proving the u8->u32
    // widening carried the correct VALUES (not merely the correct count):
    // a widening bug that corrupted index values (e.g. truncation,
    // sign-extension, or an off-by-one in the conversion) would point
    // the same index COUNT at the WRONG vertices.
    auto vertices = readBackVertices(*fixture, sub.range.blockId, sub.range.vertexOffset, 6);

    VkBuffer indexBuf = fixture->pool->indexBufferHandle(sub.range.blockId);
    const VkDeviceSize idxOffset = static_cast<VkDeviceSize>(sub.range.firstIndex) * sizeof(uint32_t);
    const VkDeviceSize idxSize = static_cast<VkDeviceSize>(sub.range.indexCount) * sizeof(uint32_t);
    auto idxReadback = fixture->allocator.createHostVisibleBuffer(idxSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(idxReadback.has_value());
    auto cmdCtx = rx::rhi::CommandContext::create(fixture->device.device(), fixture->device.graphicsQueue(),
                                                    fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferCopy region{idxOffset, 0, idxSize};
        vkCmdCopyBuffer(cmd, indexBuf, idxReadback->handle(), 1, &region);
    });
    idxReadback->invalidate();
    std::vector<uint32_t> indices(sub.range.indexCount);
    std::memcpy(indices.data(), idxReadback->mappedData(), static_cast<size_t>(idxSize));

    std::vector<glm::vec3> addressedPositions;
    for (uint32_t idx : indices) {
        REQUIRE(idx < vertices.size());
        const auto& v = vertices[idx];
        addressedPositions.emplace_back(v.px, v.py, v.pz);
    }

    // The fixture's own 6 SOURCE positions (see assets/test/
    // cube_u8_indices.gltf's own generator) -- every one must appear
    // among the positions the widened indices actually address.
    const std::vector<glm::vec3> expected = {
        {0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {100, 0, 0}, {110, 0, 0}, {100, 10, 0},
    };
    for (const auto& e : expected) {
        const bool found = std::any_of(addressedPositions.begin(), addressedPositions.end(), [&](const glm::vec3& p) {
            return p.x == doctest::Approx(e.x) && p.y == doctest::Approx(e.y) && p.z == doctest::Approx(e.z);
        });
        CHECK_MESSAGE(found, "expected position not found among widened-index-addressed vertices: (", e.x, ",", e.y,
                      ",", e.z, ")");
    }
    CHECK_FALSE(fixture->context.hasValidationErrors());
}
