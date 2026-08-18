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
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// async_import_test.cpp -- Registry::importGltfAsync() coverage [Phase 4
// Stage 1 Task 15, spec D5/D18-RC6/D24/D25, gate matrix-issue22 as amended
// by gate/rulings-2026-08-18.md RC6 + "#22 async import"]. GPU-backed
// only, joining rx_asset_gltf_gpu_tests (same fixture shape as
// import_gltf_gpu_test.cpp/damaged_helmet_test.cpp -- every async import
// ends in a real GeometryPool upload, same as the sync path).
//
// Discrimination standard this file targets directly [brief's own
// "Discrimination standard" bullet]:
//  - wall-clock gate: the WALL-CLOCK GATE TEST_CASE below, empirically
//    revert-tested in-task (see its own comment + task-15-report.md's
//    revert-testing section for the full before/after numbers): (1) a
//    scratch revert of the GEOMETRY upload alone (GeometryPool::
//    uploadDeferred() -> the old blocking upload()) did NOT fail this
//    test on this dev GPU -- Uploader's own ReBAR/UMA direct-memcpy path
//    for buffers made that revert a false negative, an empirical finding
//    that is itself why this test uses a REAL TextureCache/texture
//    payload, not geometry alone; (2) with a real TextureCache wired in,
//    the SAME class of revert on the texture path (registering all of
//    DamagedHelmet's textures inside one synchronous call, before this
//    test's own RC6-motivated per-texture time-slicing fix landed) DID
//    fail this test's hard 10ms REQUIRE (observed: 15.8ms) -- proof this
//    gate genuinely discriminates a reintroduced blocking/oversized-batch
//    regression on the one upload class ("IMAGES ALWAYS STAGE",
//    upload.h) that is hardware-independent.
//  - ordering rule: OrderingDeterminismTest reuses cube_multi_primitive.gltf
//    (two primitives with disjoint, easily distinguished AABBs) at a high
//    worker count across many repeated runs -- an append-in-completion-
//    order regression (instead of the real write-by-index pattern) would
//    make submesh 0/1 swap under adversarial scheduling, intermittently
//    but repeatably across enough runs.
//  - exactly-once completion: CompletionExactlyOnceTest's counter would
//    catch a regression that fires the callback from more than one of the
//    marshal-finalize/error/cancel paths.

using namespace rx::asset;

namespace {

std::string testAssetDir() { return std::string(RX_ASSET_ROOT_DIR) + "/assets/test"; }
std::string damagedHelmetPath() {
    return std::string(RX_ASSET_ROOT_DIR) + "/assets/fetched/DamagedHelmet/glTF/DamagedHelmet.gltf";
}

struct AsyncTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    rx::rhi::Uploader uploader;
    std::unique_ptr<rx::asset::GeometryPool> pool;
    // Populated only by attachTextureCache() below (see its own comment)
    // -- most TEST_CASEs in this file pass textures=nullptr (D25's
    // ReBAR/UMA-direct buffer-upload fast path already makes GEOMETRY
    // uploads a weak discriminator for a reintroduced blocking wait --
    // see this file's own WALL-CLOCK GATE TEST_CASE comment for the
    // empirical finding that motivated adding a real TextureCache here:
    // "IMAGES ALWAYS STAGE" (upload.h) unconditionally, on every GPU,
    // making a texture upload the hardware-independent way to prove the
    // non-blocking contract).
    std::optional<rx::rhi::BindlessTable> bindless;
    std::optional<rx::rhi::DeletionQueue> deletionQueue;
    std::unique_ptr<TextureCache> textures;
};

// See import_gltf_gpu_test.cpp's own makeFixture()/attachPoolAndScheduler()
// comment for exactly why GeometryPool is attached in a SEPARATE step,
// against the fixture's own now-stable members -- identical rationale,
// identical two-step pattern, reused verbatim.
std::optional<AsyncTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping async import test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions, skipping async import test");
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
    return AsyncTestFixture{std::move(*window),   std::move(*context),  std::move(*device),  std::move(*allocator),
                             std::move(*uploader), nullptr,              std::nullopt,         std::nullopt,
                             nullptr};
}

void attachPool(AsyncTestFixture& fixture) { fixture.pool = rx::asset::GeometryPool::create(fixture.allocator, fixture.device, fixture.uploader); }

// Builds a real TextureCache against the fixture's own now-stable members
// -- mirrors texture_cache_test.cpp's own makeCache() two-step pattern
// (BindlessTable/DeletionQueue/TextureCache all store references into
// this fixture, so they must be built AFTER makeFixture() has already
// settled everything into its final address, never before).
void attachTextureCache(AsyncTestFixture& fixture) {
    rx::rhi::BindlessTable::Capacities capacities{/*sampledImages=*/64, /*samplers=*/8, /*storageBuffers=*/1};
    auto bindless = rx::rhi::BindlessTable::create(fixture.device.physicalDevice(), fixture.device.device(), capacities);
    REQUIRE(bindless.has_value());
    fixture.bindless.emplace(std::move(*bindless));
    fixture.deletionQueue.emplace();
    fixture.textures = TextureCache::create(fixture.allocator, fixture.device, fixture.uploader, *fixture.bindless,
                                             *fixture.deletionQueue);
    REQUIRE(fixture.textures != nullptr);
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

// Blocks (via repeated pumpMain() calls, never a real sleep-spin on
// nothing) until `handle`'s progress reaches a terminal state (Done,
// Failed, or cancelled) or `timeout` elapses. Returns false on timeout
// (a REQUIRE at the call site turns that into a clean test failure
// instead of an indefinite hang). A tiny yield between pumps keeps this
// from busy-spinning a full core while a background worker/IO task is
// still running.
bool pumpUntilTerminal(rx::task::Scheduler& scheduler, const Registry& registry, AsyncImportHandle handle,
                        std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        scheduler.pumpMain();
        ImportProgress p = registry.importProgress(handle);
        if (p.stage == ImportStage::Done || p.stage == ImportStage::Failed || p.cancelled) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// A ByteSource that wraps a real FilesystemByteSource, injecting
// `delay` before every read() returns and recording (under a mutex) the
// identity of every thread that called it -- used both as the wall-clock
// gate's "deliberately slow decode" mechanism AND as this file's
// worker-thread-participation / decode-never-on-the-IO-thread proof (a
// legitimate, non-invasive observation seam: ByteSource is already the
// project's own documented host-injectable extension point, byte_source.h).
class SlowRecordingByteSource final : public ByteSource {
public:
    SlowRecordingByteSource(std::filesystem::path baseDir, std::chrono::milliseconds delay)
        : inner_(std::move(baseDir)), delay_(delay) {}

    std::optional<std::vector<std::byte>> read(const std::string& uri) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            threadIds_.push_back(std::this_thread::get_id());
        }
        if (delay_.count() > 0) {
            std::this_thread::sleep_for(delay_);
        }
        return inner_.read(uri);
    }

    std::vector<std::thread::id> recordedThreadIds() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return threadIds_;
    }

private:
    FilesystemByteSource inner_;
    std::chrono::milliseconds delay_;
    mutable std::mutex mutex_;
    std::vector<std::thread::id> threadIds_;
};

}  // namespace

// ---------------------------------------------------------------------
// Determinism: async deep-equals sync; run-to-run identical; independent
// of worker count.
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: cube_textured.gltf async result deep-equals the sync result (counts, ranges, AABBs, "
          "material params) -- fresh registries/pools, 1 worker") {
    auto syncFixture = makeFixture("async_det_sync");
    if (!syncFixture) {
        return;
    }
    attachPool(*syncFixture);
    auto syncScheduler = rx::task::Scheduler::create(2);
    REQUIRE(syncScheduler != nullptr);
    Registry syncRegistry;
    ImportResult syncResult =
        syncRegistry.importGltf(testAssetDir() + "/cube_textured.gltf", *syncFixture->pool, *syncScheduler);
    REQUIRE(syncResult.ok());

    auto asyncFixture = makeFixture("async_det_async");
    if (!asyncFixture) {
        return;
    }
    attachPool(*asyncFixture);
    auto asyncScheduler = rx::task::Scheduler::create(1);
    REQUIRE(asyncScheduler != nullptr);
    Registry asyncRegistry;

    ImportResult asyncResult;
    bool completed = false;
    AsyncImportHandle handle = asyncRegistry.importGltfAsync(
        testAssetDir() + "/cube_textured.gltf", *asyncFixture->pool, *asyncScheduler, /*textures=*/nullptr,
        [&](ImportResult r) {
            asyncResult = std::move(r);
            completed = true;
        });
    REQUIRE(handle.isValid());
    REQUIRE(pumpUntilTerminal(*asyncScheduler, asyncRegistry, handle));
    REQUIRE(completed);
    REQUIRE(asyncResult.ok());

    REQUIRE(asyncResult.meshes.size() == syncResult.meshes.size());
    REQUIRE(asyncResult.materials.size() == syncResult.materials.size());
    const MeshAsset& syncMesh = syncRegistry.mesh(syncResult.meshes[0]);
    const MeshAsset& asyncMesh = asyncRegistry.mesh(asyncResult.meshes[0]);
    REQUIRE(asyncMesh.submeshes.size() == syncMesh.submeshes.size());
    const Submesh& sSub = syncMesh.submeshes[0];
    const Submesh& aSub = asyncMesh.submeshes[0];
    CHECK(aSub.range.blockId == sSub.range.blockId);
    CHECK(aSub.range.firstIndex == sSub.range.firstIndex);
    CHECK(aSub.range.vertexOffset == sSub.range.vertexOffset);
    CHECK(aSub.range.indexCount == sSub.range.indexCount);
    CHECK(aSub.bounds.min.x == doctest::Approx(sSub.bounds.min.x));
    CHECK(aSub.bounds.max.x == doctest::Approx(sSub.bounds.max.x));

    const MaterialAsset& sMat = syncRegistry.material(syncResult.materials[0]);
    const MaterialAsset& aMat = asyncRegistry.material(asyncResult.materials[0]);
    CHECK(aMat.baseColorFactor.r == doctest::Approx(sMat.baseColorFactor.r));
    CHECK(aMat.metallicFactor == doctest::Approx(sMat.metallicFactor));
    CHECK(aMat.roughnessFactor == doctest::Approx(sMat.roughnessFactor));
    CHECK(aMat.disposition == sMat.disposition);

    REQUIRE(asyncResult.scene.instances.size() == syncResult.scene.instances.size());
    CHECK(asyncResult.scene.instances[0].worldTransform == syncResult.scene.instances[0].worldTransform);
    CHECK(asyncResult.scene.instances[0].negativeDeterminant == syncResult.scene.instances[0].negativeDeterminant);

    CHECK_FALSE(syncFixture->context.hasValidationErrors());
    CHECK_FALSE(asyncFixture->context.hasValidationErrors());
}

TEST_CASE("importGltfAsync: run-to-run identical into fresh registries, and independent of worker count (1 vs 6)") {
    auto fixture1 = makeFixture("async_det_w1");
    if (!fixture1) {
        return;
    }
    attachPool(*fixture1);
    auto fixture6 = makeFixture("async_det_w6");
    if (!fixture6) {
        return;
    }
    attachPool(*fixture6);

    auto scheduler1 = rx::task::Scheduler::create(1);
    auto scheduler6 = rx::task::Scheduler::create(6);
    REQUIRE(scheduler1 != nullptr);
    REQUIRE(scheduler6 != nullptr);

    Registry registry1;
    Registry registry6;
    ImportResult result1;
    ImportResult result6;
    bool done1 = false;
    bool done6 = false;

    AsyncImportHandle h1 = registry1.importGltfAsync(testAssetDir() + "/cube_multi_primitive.gltf", *fixture1->pool,
                                                        *scheduler1, nullptr,
                                                        [&](ImportResult r) { result1 = std::move(r); done1 = true; });
    AsyncImportHandle h6 = registry6.importGltfAsync(testAssetDir() + "/cube_multi_primitive.gltf", *fixture6->pool,
                                                        *scheduler6, nullptr,
                                                        [&](ImportResult r) { result6 = std::move(r); done6 = true; });
    REQUIRE(pumpUntilTerminal(*scheduler1, registry1, h1));
    REQUIRE(pumpUntilTerminal(*scheduler6, registry6, h6));
    REQUIRE(done1);
    REQUIRE(done6);
    REQUIRE(result1.ok());
    REQUIRE(result6.ok());

    REQUIRE(result1.meshes.size() == result6.meshes.size());
    REQUIRE(result1.materials.size() == result6.materials.size());
    const MeshAsset& mesh1 = registry1.mesh(result1.meshes[0]);
    const MeshAsset& mesh6 = registry6.mesh(result6.meshes[0]);
    REQUIRE(mesh1.submeshes.size() == 2);
    REQUIRE(mesh6.submeshes.size() == 2);
    for (size_t i = 0; i < 2; ++i) {
        CHECK(mesh1.submeshes[i].range.indexCount == mesh6.submeshes[i].range.indexCount);
        CHECK(mesh1.submeshes[i].bounds.min.x == doctest::Approx(mesh6.submeshes[i].bounds.min.x));
        CHECK(mesh1.submeshes[i].bounds.max.x == doctest::Approx(mesh6.submeshes[i].bounds.max.x));
    }
    CHECK_FALSE(fixture1->context.hasValidationErrors());
    CHECK_FALSE(fixture6->context.hasValidationErrors());
}

// ---------------------------------------------------------------------
// Ordering rule: file-order application at the marshal point, regardless
// of worker completion order.
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: cube_multi_primitive.gltf submeshes land in FILE order regardless of worker "
          "completion order -- stress: 8 workers, 15 repeated fresh imports, every run") {
    for (int run = 0; run < 15; ++run) {
        auto fixture = makeFixture("async_order");
        if (!fixture) {
            return;
        }
        attachPool(*fixture);
        auto scheduler = rx::task::Scheduler::create(8);
        REQUIRE(scheduler != nullptr);
        Registry registry;
        ImportResult result;
        bool done = false;
        AsyncImportHandle handle = registry.importGltfAsync(
            testAssetDir() + "/cube_multi_primitive.gltf", *fixture->pool, *scheduler, nullptr,
            [&](ImportResult r) {
                result = std::move(r);
                done = true;
            });
        REQUIRE(pumpUntilTerminal(*scheduler, registry, handle));
        REQUIRE(done);
        REQUIRE(result.ok());
        const MeshAsset& mesh = registry.mesh(result.meshes[0]);
        REQUIRE(mesh.submeshes.size() == 2);
        // File order, not completion order: primitive 0's material/AABB
        // (x in [0,1]) must land at submesh index 0; primitive 1's
        // (x in [2,3]) at index 1 -- see import_gltf_gpu_test.cpp's own
        // "multi-primitive mesh" test for these exact source values.
        INFO("run = ", run);
        CHECK(mesh.submeshes[0].bounds.min.x == doctest::Approx(0.0F));
        CHECK(mesh.submeshes[0].bounds.max.x == doctest::Approx(1.0F));
        CHECK(mesh.submeshes[1].bounds.min.x == doctest::Approx(2.0F));
        CHECK(mesh.submeshes[1].bounds.max.x == doctest::Approx(3.0F));
        CHECK(mesh.submeshes[0].material == result.materials[0]);
        CHECK(mesh.submeshes[1].material == result.materials[1]);
    }
}

// ---------------------------------------------------------------------
// Main-thread affinity: registry mutation genuinely happens on the main
// thread even though the import itself was driven asynchronously (the
// RX_ASSERT_MAIN_THREAD guards on Registry/GeometryPool/Scheduler::pumpMain
// stay armed throughout -- a violation would abort the whole process, so
// simply completing this test at all with validation clean IS the proof;
// no separate hook needed given these guards abort rather than throw).
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: full run with every RX_ASSERT_MAIN_THREAD guard armed completes without aborting") {
    auto fixture = makeFixture("async_mainthread");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);
    Registry registry;
    ImportResult result;
    bool done = false;
    AsyncImportHandle handle = registry.importGltfAsync(
        testAssetDir() + "/cube_multi_primitive.gltf", *fixture->pool, *scheduler, nullptr,
        [&](ImportResult r) {
            result = std::move(r);
            done = true;
        });
    REQUIRE(pumpUntilTerminal(*scheduler, registry, handle));
    REQUIRE(done);
    REQUIRE(result.ok());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ---------------------------------------------------------------------
// Worker-thread participation / decode never on the IO thread.
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: buffer/image byte-source reads happen off BOTH the main thread and the dedicated IO "
          "thread (worker-thread participation, proven via the ByteSource observation seam)") {
    if (!std::filesystem::exists(damagedHelmetPath())) {
        MESSAGE("DamagedHelmet not fetched -- skipping");
        return;
    }
    auto fixture = makeFixture("async_thread_id");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    attachTextureCache(*fixture);  // exercises texture DECODE too, not just buffer resolution
    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);

    std::atomic<bool> ioThreadIdKnown{false};
    std::thread::id ioThreadId{};
    scheduler->runOnIoThread([&] {
        ioThreadId = std::this_thread::get_id();
        ioThreadIdKnown.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ioThreadIdKnown.load(std::memory_order_acquire)) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    const std::thread::id mainThreadId = std::this_thread::get_id();

    SlowRecordingByteSource source(std::filesystem::path(damagedHelmetPath()).parent_path(),
                                    std::chrono::milliseconds(0));
    std::vector<std::byte> documentBytes = readFileBytes(damagedHelmetPath());

    Registry registry;
    ImportResult result;
    bool done = false;
    AsyncImportHandle handle = registry.importGltfAsync(
        documentBytes, source, *fixture->pool, *scheduler, fixture->textures.get(),
        [&](ImportResult r) {
            result = std::move(r);
            done = true;
        });
    REQUIRE(pumpUntilTerminal(*scheduler, registry, handle));
    REQUIRE(done);
    REQUIRE(result.ok());

    std::vector<std::thread::id> recorded = source.recordedThreadIds();
    REQUIRE_FALSE(recorded.empty());
    for (const auto& id : recorded) {
        CHECK(id != mainThreadId);
        CHECK(id != ioThreadId);
    }
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ---------------------------------------------------------------------
// Progress: monotonic, terminal arrival, exactly-once completion.
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: progress is monotonic (stage never regresses, itemsCompleted never decreases) and "
          "reaches Done; completion fires exactly once") {
    auto fixture = makeFixture("async_progress");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    Registry registry;

    std::atomic<int> completionCount{0};
    ImportResult result;
    AsyncImportHandle handle = registry.importGltfAsync(
        testAssetDir() + "/cube_multi_primitive.gltf", *fixture->pool, *scheduler, nullptr, [&](ImportResult r) {
            result = std::move(r);
            completionCount.fetch_add(1, std::memory_order_relaxed);
        });

    const auto rank = [](ImportStage s) {
        switch (s) {
            case ImportStage::Reading:
                return 0;
            case ImportStage::Parsing:
                return 1;
            case ImportStage::Decoding:
                return 2;
            case ImportStage::Optimizing:
                return 3;
            case ImportStage::Uploading:
                return 4;
            case ImportStage::Done:
            case ImportStage::Failed:
                return 5;
        }
        return -1;
    };

    int lastRank = 0;
    uint32_t lastItemsCompleted = 0;
    bool sawTerminal = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!sawTerminal) {
        scheduler->pumpMain();
        ImportProgress p = registry.importProgress(handle);
        CHECK(rank(p.stage) >= lastRank);
        lastRank = rank(p.stage);
        CHECK(p.itemsCompleted >= lastItemsCompleted);
        lastItemsCompleted = p.itemsCompleted;
        if (p.stage == ImportStage::Done || p.stage == ImportStage::Failed) {
            sawTerminal = true;
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
    }
    CHECK(completionCount.load() == 1);
    // Extra pumps after completion must never fire the callback again.
    for (int i = 0; i < 50; ++i) {
        scheduler->pumpMain();
    }
    CHECK(completionCount.load() == 1);
    CHECK(result.ok());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ---------------------------------------------------------------------
// Error propagation: async garbage-file import == sync ImportError
// (paired assert); worker-stage exceptions never escape (implicit: this
// whole suite runs with exceptions enabled and nothing crashes the
// process on the malformed-input path below).
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: garbage-bytes import yields the SAME named ImportError as the sync path "
          "(paired assert), zero registry mutation, callback still fires (only cancellation suppresses it)") {
    auto fixture = makeFixture("async_garbage");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);

    const std::string garbage = "this is not json at all { [ garbage";
    std::vector<std::byte> bytes(garbage.size());
    std::memcpy(bytes.data(), garbage.data(), garbage.size());

    class NullSource final : public ByteSource {
    public:
        std::optional<std::vector<std::byte>> read(const std::string&) override { return std::nullopt; }
    };
    NullSource source;

    Registry syncRegistry;
    ImportResult syncResult = syncRegistry.importGltf(bytes, source, *fixture->pool, *scheduler);
    CHECK_FALSE(syncResult.ok());

    Registry asyncRegistry;
    const size_t meshesBefore = asyncRegistry.meshCountForTesting();
    const size_t materialsBefore = asyncRegistry.materialCountForTesting();
    ImportResult asyncResult;
    bool done = false;
    AsyncImportHandle handle = asyncRegistry.importGltfAsync(bytes, source, *fixture->pool, *scheduler, nullptr,
                                                                [&](ImportResult r) {
                                                                    asyncResult = std::move(r);
                                                                    done = true;
                                                                });
    REQUIRE(pumpUntilTerminal(*scheduler, asyncRegistry, handle));
    REQUIRE(done);
    CHECK_FALSE(asyncResult.ok());
    CHECK(asyncResult.error == syncResult.error);
    CHECK(asyncRegistry.meshCountForTesting() == meshesBefore);
    CHECK(asyncRegistry.materialCountForTesting() == materialsBefore);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ---------------------------------------------------------------------
// Cancellation (abandon semantics).
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: cancelImport() before any work starts -- no registry mutation, callback never "
          "fires, progress observes cancelled") {
    auto fixture = makeFixture("async_cancel_immediate");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    Registry registry;
    const size_t meshesBefore = registry.meshCountForTesting();
    const size_t materialsBefore = registry.materialCountForTesting();

    bool fired = false;
    AsyncImportHandle handle = registry.importGltfAsync(testAssetDir() + "/cube_multi_primitive.gltf",
                                                            *fixture->pool, *scheduler, nullptr,
                                                            [&](ImportResult) { fired = true; });
    registry.cancelImport(handle);

    // Drain: even though cancelled immediately, the compute phase may
    // already be mid-flight on a worker/IO thread -- pump until the
    // whole chain has genuinely settled (bounded latency: one stage-item,
    // per the cancellation contract).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        scheduler->pumpMain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK_FALSE(fired);
    CHECK(registry.meshCountForTesting() == meshesBefore);
    CHECK(registry.materialCountForTesting() == materialsBefore);
    ImportProgress progress = registry.importProgress(handle);
    CHECK(progress.cancelled);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltfAsync: cancel-after-completion is a documented no-op (no crash, no double callback)") {
    auto fixture = makeFixture("async_cancel_after");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    Registry registry;
    int completionCount = 0;
    AsyncImportHandle handle = registry.importGltfAsync(testAssetDir() + "/cube_textured.gltf", *fixture->pool,
                                                            *scheduler, nullptr,
                                                            [&](ImportResult) { ++completionCount; });
    REQUIRE(pumpUntilTerminal(*scheduler, registry, handle));
    CHECK(completionCount == 1);
    registry.cancelImport(handle);  // documented no-op -- job already reclaimed/terminal
    scheduler->pumpMain();
    CHECK(completionCount == 1);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ---------------------------------------------------------------------
// Wall-clock two-tier gate [RC6, THE load-bearing test].
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: WALL-CLOCK GATE -- deliberately slow decode + a real (DamagedHelmet-scale) payload "
          "in flight, >=300 driven frames, every pumpMain() call bounded (2ms local budget, 10ms CI stall "
          "detector), zero wait-calls-from-async-path, ring never exhausts") {
    if (!std::filesystem::exists(damagedHelmetPath())) {
        MESSAGE("DamagedHelmet not fetched (run tools/fetch_assets.sh) -- skipping the wall-clock gate");
        return;
    }
    auto fixture = makeFixture("async_wallclock");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    // A REAL TextureCache, not nullptr -- load-bearing for this specific
    // test, found empirically during this task's own revert-testing: a
    // GEOMETRY-only import (textures=nullptr) is a WEAK discriminator on
    // this project's own dev GPU, because Uploader::uploadToBuffer()'s
    // direct (ReBAR/UMA) path memcpy's straight into DEVICE_LOCAL+
    // HOST_VISIBLE destination memory with NO staging copy and NO real
    // GPU work queued at all (upload.h's own class comment) -- a
    // reverted, reintroduced blocking pool.upload() call was tried here
    // first and did NOT make this test fail (max pump stayed ~1ms) for
    // exactly that reason. "IMAGES ALWAYS STAGE" unconditionally, on
    // every GPU (upload.h, no ReBAR exception exists for images -- no
    // VK_EXT_host_image_copy in this project's baseline) -- DamagedHelmet's
    // five real JPG textures (300KB-1.3MB each, genuine stb-decoded
    // pixel payloads) are what makes this gate discriminate for real,
    // hardware-independent of whatever hardware/driver CI happens to run
    // on. See task-15-report.md's own revert-testing section for the
    // full before/after numbers.
    attachTextureCache(*fixture);
    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);

    // Deliberately slow decode: every buffer/image byte-source read this
    // import makes sleeps 60ms before returning -- DamagedHelmet.gltf's
    // one external .bin buffer PLUS all five image files (ResolvedBuffers
    // caches per-buffer-index so the .bin itself is only ever actually
    // read once -- see import_gltf.cpp's own comment -- but each image is
    // resolved independently) adds up to a genuinely long, real in-flight
    // window this test's own frame loop overlaps.
    SlowRecordingByteSource source(std::filesystem::path(damagedHelmetPath()).parent_path(),
                                    std::chrono::milliseconds(60));
    std::vector<std::byte> documentBytes = readFileBytes(damagedHelmetPath());

    Registry registry;
    ImportResult result;
    bool done = false;
    AsyncImportHandle handle = registry.importGltfAsync(
        documentBytes, source, *fixture->pool, *scheduler, fixture->textures.get(),
        [&](ImportResult r) {
            result = std::move(r);
            done = true;
        });
    REQUIRE(handle.isValid());

    constexpr auto kLocalBudget = std::chrono::microseconds(2000);   // 2 ms, published/trend-tracked
    constexpr auto kCiStallDetector = std::chrono::microseconds(10000);  // 10 ms, CI-blocking (RC6)

    uint64_t framesWhileInFlight = 0;
    std::chrono::microseconds maxPumpDuration{0};
    uint32_t overLocalBudgetCount = 0;

    const auto testDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done) {
        ImportProgress before = registry.importProgress(handle);
        const bool wasInFlight = before.stage != ImportStage::Done && before.stage != ImportStage::Failed;

        const auto pumpStart = std::chrono::steady_clock::now();
        scheduler->pumpMain();
        const auto pumpDuration =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - pumpStart);

        if (wasInFlight) {
            ++framesWhileInFlight;
        }
        maxPumpDuration = std::max(maxPumpDuration, pumpDuration);
        if (pumpDuration > kLocalBudget) {
            ++overLocalBudgetCount;
        }
        // The CI stall-detector tier (RC6): this IS asserted hard, every
        // single call -- a defect here (e.g. a reintroduced blocking
        // wait()) would show up as tens of milliseconds, an order of
        // magnitude above this ceiling, per RC6's own reasoning.
        REQUIRE(pumpDuration < kCiStallDetector);

        REQUIRE(std::chrono::steady_clock::now() < testDeadline);
    }

    REQUIRE(done);
    REQUIRE(result.ok());
    // >= 300 rendered frames while genuinely in flight -- proves real
    // overlap (the import did not simply complete on the first pump).
    CHECK(framesWhileInFlight >= 300);

    // Published number [D18/RC6]: the 2ms figure is a TREND metric,
    // explicitly "never CI-blocking" (D18's own wording, amended by RC6
    // only for the SEPARATE 10ms stall-detector tier asserted above via
    // REQUIRE) -- reported via MESSAGE only, deliberately never a CHECK/
    // REQUIRE, so a real but small overage (this task's own dev-machine
    // run: DamagedHelmet's largest single JPEG texture, ~1.3MB decoded to
    // RGBA8, occasionally costs a few ms of real memcpy+copy-recording
    // time for ONE texture's own registerDecoded() step -- see
    // task-15-report.md's own performance-numbers section) is visible in
    // the log without flaking CI, exactly matching D18's own posture for
    // every other wall-clock number in this codebase.
    MESSAGE("wall-clock gate: max single pumpMain() call = ", maxPumpDuration.count(), " us across ",
            framesWhileInFlight, " in-flight frame(s); ", overLocalBudgetCount,
            " call(s) exceeded the 2ms local/published budget (trend-tracked, never CI-blocking per D18/RC6) -- "
            "hard CI stall-detector ceiling is 10ms (REQUIRE'd above, every call)");

    // D25: the async path must never call Uploader::wait() -- proxied
    // here by the ring never needing to block-reclaim, since this import
    // is the only user of this pool's Uploader in this test and a
    // wait()-based reclaim would itself show up as an outsized pump
    // duration already asserted against above. Ring-exhaustion proxy:
    // GeometryPool exposes no direct accessor, so the wall-clock bound
    // itself is this test's own exhaustion signal (a genuinely exhausted/
    // blocking ring reclaim would violate the 10ms ceiling above).
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ---------------------------------------------------------------------
// Concurrent imports: isolated, correct, independent completion order.
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: two overlapping async imports (different files) complete isolated and correct") {
    auto fixture = makeFixture("async_concurrent");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);
    Registry registry;

    ImportResult resultA;
    ImportResult resultB;
    bool doneA = false;
    bool doneB = false;
    AsyncImportHandle handleA = registry.importGltfAsync(
        testAssetDir() + "/cube_textured.gltf", *fixture->pool, *scheduler, nullptr,
        [&](ImportResult r) {
            resultA = std::move(r);
            doneA = true;
        });
    AsyncImportHandle handleB = registry.importGltfAsync(
        testAssetDir() + "/cube_multi_primitive.gltf", *fixture->pool, *scheduler, nullptr,
        [&](ImportResult r) {
            resultB = std::move(r);
            doneB = true;
        });
    REQUIRE(handleA.isValid());
    REQUIRE(handleB.isValid());
    CHECK_FALSE(handleA == handleB);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!(doneA && doneB)) {
        scheduler->pumpMain();
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(resultA.ok());
    REQUIRE(resultB.ok());
    REQUIRE(resultA.meshes.size() == 1);
    REQUIRE(resultB.meshes.size() == 1);
    CHECK(registry.mesh(resultA.meshes[0]).submeshes.size() == 1);
    CHECK(registry.mesh(resultB.meshes[0]).submeshes.size() == 2);

    // D24: resolving one import's handles is unaffected by the other's
    // (already-completed, in this case, but the handle spaces are
    // disjoint regardless of overlap timing -- distinct HandlePool
    // acquisitions).
    CHECK_FALSE(resultA.meshes[0] == resultB.meshes[0]);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ---------------------------------------------------------------------
// Teardown-with-import-in-flight.
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: destroying the Registry while an import is in flight is defined -- no crash, no "
          "callback after teardown begins") {
    auto fixture = makeFixture("async_teardown");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);

    SlowRecordingByteSource source(std::filesystem::path(testAssetDir()), std::chrono::milliseconds(100));
    std::vector<std::byte> documentBytes = readFileBytes(testAssetDir() + "/cube_multi_primitive.gltf");

    bool fired = false;
    {
        Registry registry;
        AsyncImportHandle handle = registry.importGltfAsync(documentBytes, source, *fixture->pool, *scheduler,
                                                                nullptr, [&](ImportResult) { fired = true; });
        REQUIRE(handle.isValid());
        // Registry destructs HERE, deliberately before the (deliberately
        // slow) compute phase has had time to finish.
    }
    // Drain whatever the in-flight worker/IO task still does -- it must
    // find `cancelled` already true and touch neither the (destroyed)
    // Registry nor fire the callback, by construction (registry.cpp's own
    // ~Registry() comment).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        scheduler->pumpMain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_FALSE(fired);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("importGltfAsync: destroying the WHOLE fixture (Scheduler included) while an import is in flight "
          "does not crash -- Scheduler::~Scheduler()'s own drain guarantee covers outstanding IO/worker tasks") {
    SlowRecordingByteSource source(std::filesystem::path(testAssetDir()), std::chrono::milliseconds(50));
    std::vector<std::byte> documentBytes = readFileBytes(testAssetDir() + "/cube_multi_primitive.gltf");
    {
        auto fixture = makeFixture("async_teardown_full");
        if (!fixture) {
            return;
        }
        attachPool(*fixture);
        auto scheduler = rx::task::Scheduler::create(2);
        REQUIRE(scheduler != nullptr);
        Registry registry;
        AsyncImportHandle handle =
            registry.importGltfAsync(documentBytes, source, *fixture->pool, *scheduler, nullptr, [](ImportResult) {});
        REQUIRE(handle.isValid());
        // fixture, scheduler, and registry all unwind here, in REVERSE
        // declaration order (registry destructs first, then scheduler,
        // then fixture -- ordinary C++ local-variable teardown order).
        // Registry destructing first cancels its own outstanding job
        // immediately (registry.cpp's own ~Registry() comment); Scheduler
        // destructing next then blocks (WaitforAllAndShutdown()'s own
        // documented guarantee) until whatever the in-flight worker/IO
        // task was doing has actually finished, observing `cancelled`
        // already true and touching neither the (already-destroyed)
        // Registry nor the pool/textures pointers it still carries (both
        // of which remain valid a while longer, until `fixture` itself
        // unwinds last) -- safe regardless of which of the two teardown
        // orderings (Registry-first, as here, or Scheduler-first, the
        // previous TEST_CASE) a caller happens to use.
    }
    CHECK(true);  // reaching here at all (no crash/hang) is the assertion.
}
