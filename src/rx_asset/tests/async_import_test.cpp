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
    while (!ioThreadIdKnown.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    // [Issue #30] Single deterministic assertion after the wait, instead of a
    // REQUIRE evaluated once per spin above -- the spin count is itself
    // wall-clock/scheduler dependent, so a per-iteration REQUIRE made the
    // doctest ASSERTION COUNT a function of live timing (issue #30's defect
    // class). A timeout still fails this REQUIRE exactly like the
    // per-iteration form it replaces did.
    REQUIRE(ioThreadIdKnown.load(std::memory_order_acquire));
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
    bool rankRegressed = false;
    bool itemsRegressed = false;
    bool sawTerminal = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!sawTerminal && std::chrono::steady_clock::now() < deadline) {
        scheduler->pumpMain();
        ImportProgress p = registry.importProgress(handle);
        if (rank(p.stage) < lastRank) {
            rankRegressed = true;
        }
        lastRank = rank(p.stage);
        if (p.itemsCompleted < lastItemsCompleted) {
            itemsRegressed = true;
        }
        lastItemsCompleted = p.itemsCompleted;
        if (p.stage == ImportStage::Done || p.stage == ImportStage::Failed) {
            sawTerminal = true;
        }
    }
    // [Issue #30] Deterministic assertion count: the loop above may run a
    // wall-clock/scheduling-dependent number of times, but the CLAIMS --
    // stage rank and itemsCompleted never regress across ANY observed tick,
    // and a terminal stage is reached before the deadline -- are now exactly
    // 3 fixed assertions regardless of iteration count, instead of up to 2
    // CHECKs + 1 REQUIRE PER iteration. A regression on any single tick still
    // flips one of these flags permanently (same discriminating power as the
    // per-iteration CHECKs they replace).
    REQUIRE(sawTerminal);
    CHECK_FALSE(rankRegressed);
    CHECK_FALSE(itemsRegressed);
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
// [Issue #30, round 2 -- reviewer-found, pre-existing, out of the six-site
// rework's scope] `pumpUntilTerminal()` (this file's shared helper, defined
// above, unmodified by this fix) polls `registry.importProgress(handle)`
// and returns as soon as `stage` reads Done/Failed/cancelled. Callers
// (every TEST_CASE in this file, and any real host application following
// the same documented pattern) then assume the paired completion callback
// has ALREADY fired by that point -- true for every terminal transition
// EXCEPT one: a compute-phase PARSE failure (malformed/corrupt document
// bytes) used to call setStage(stageOut, ImportStage::Failed) DIRECTLY on
// the WORKER thread, inside computeGltfImport() (import_gltf.cpp, the
// dataBuffer-wrap and parser.loadGltf early-return paths) -- a store fully
// decoupled from, and racing ahead of, the completion callback, which only
// fires later when that same worker thread's OWN postToMain(finishAsync-
// ImportCompute) closure is actually drained by a pumpMain() call on the
// main thread. Every OTHER terminal transition (Done via
// pollAsyncImportUploads(), Failed via finishAsyncImportCompute()'s own
// compute.error branch, Failed via the three IO-thread-read-failure
// closures in Registry::importGltfAsync(path, ...)) sets `stage` and fires
// the callback in the SAME synchronous call, so no such window exists
// there -- this was narrow (worker-thread instruction count between the
// store and its own postToMain() call, typically far under a microsecond)
// and reviewer-reproduced only 2/40 full-suite runs under real GPU
// contention (a competing process widening the scheduling window). Own
// reproduction during this fix, on a quiet host: 0/5000 naturally (the
// window is too narrow to hit without contention here), 31/5000 once an
// artificial 500us delay was inserted right after the direct store (a
// TEMPORARY, since-removed diagnostic -- see the fix's own report
// addendum) -- confirming both the mechanism and that this loop detects
// it. Fixed in import_gltf.cpp by deleting both redundant direct stores --
// finishAsyncImportCompute() (registry.cpp) already authoritatively sets
// `stage = Failed` in the exact same call as firing the callback, mirroring
// every other terminal-transition site, which makes the race structurally
// impossible rather than merely less likely. Fixed number of iterations
// (not wall-clock-derived), so this loop carries no issue-#30-class
// assertion-count variance of its own; the loop itself performs no
// REQUIRE/CHECK, only the single aggregated REQUIRE after it.
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: [race regression] repeated garbage-bytes imports never observe a terminal stage "
          "before the paired completion callback has fired") {
    auto fixture = makeFixture("async_garbage_race_stress");
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

    // [Issue #30 round 2] 2000, not the 5000 used during this fix's own
    // diagnosis: the fix (see import_gltf.cpp's own comment at both
    // deleted call sites) makes the race STRUCTURALLY impossible, not
    // merely less likely -- `job->stage` now has exactly one write site for
    // a compute-phase failure (finishAsyncImportCompute(), registry.cpp),
    // always in the same synchronous call as firing the completion
    // callback -- so this loop's only remaining job is guarding against a
    // FUTURE regression reintroducing a similarly racy direct store
    // elsewhere. On this quiet host the ORIGINAL (pre-fix) bug did not
    // reproduce naturally at 5000 iterations either (0/5000; it only
    // reproduced once an artificial delay was inserted to widen the
    // window, matching the reviewer's own finding that this needs real
    // scheduling contention to surface) -- so a higher count buys little
    // real-world guard strength against ambient noise; 2000 is cheap
    // insurance, not a probability play.
    constexpr int kIterations = 2000;
    int terminalTimeouts = 0;
    int raceHits = 0;
    for (int i = 0; i < kIterations; ++i) {
        Registry registry;
        bool done = false;
        AsyncImportHandle handle = registry.importGltfAsync(bytes, source, *fixture->pool, *scheduler, nullptr,
                                                              [&](ImportResult) { done = true; });
        const bool reachedTerminal =
            pumpUntilTerminal(*scheduler, registry, handle, std::chrono::milliseconds(2000));
        if (!reachedTerminal) {
            ++terminalTimeouts;  // should never happen either -- counted separately for diagnosis
            continue;
        }
        if (!done) {
            ++raceHits;
        }
    }
    MESSAGE("race regression: ", raceHits, " / ", kIterations,
            " iterations observed stage-terminal before the completion callback fired (", terminalTimeouts,
            " separate timeout(s))");
    REQUIRE(terminalTimeouts == 0);
    REQUIRE(raceHits == 0);
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

    // [Fix round 1, IMPORTANT item] Direct wait-calls==0 baseline -- both
    // counters are added ONLY to rx_asset's own GeometryPool/TextureCache
    // (rx_rhi_vk is off-limits this round, see this file's own commit
    // history), incremented at the ONE call site in each class that ever
    // calls Uploader::wait() (the synchronous upload()/loadFromBytes()
    // paths only -- never uploadDeferred()/registerDecoded(), the two the
    // async prepare-step above exclusively uses). A defect that
    // reintroduced a blocking wait() anywhere on the async path would tick
    // one of these counters upward; this test's whole point is that
    // neither ever does.
    const size_t poolWaitCallsBefore = fixture->pool->waitCallCountForTesting();
    const size_t textureWaitCallsBefore = fixture->textures->waitCallCountForTesting();
    // [Fix round 1, IMPORTANT item] Direct ring/pool no-exhaustion check --
    // a fresh pool starts at zero blocks/zero bytes used; the assertion
    // after the loop below is that the import's own geometry upload
    // succeeded WITHIN a single block's capacity (real progress, no silent
    // stall/overflow), not that the pool never grows at all.
    const PoolStats poolStatsBefore = fixture->pool->stats();
    REQUIRE(poolStatsBefore.blockCount == 0);
    REQUIRE(poolStatsBefore.vertexBytesUsed == 0);

    // [RC6 recalibration, coordinator-ruled, CI incident 32180630087 --
    // see wine-segfault-fix-report.md's own addendum for the full
    // measurement writeup this comment summarizes] The CI-tier threshold
    // below used to be a fixed 10ms constant, calibrated against fast-
    // dev-hardware noise. CI's own 2-core + single-thread-lavapipe
    // environment legitimately reached 12.6ms on a single real texture
    // registration during the incident this citation names -- a real,
    // slow-but-correct call, not the pre-D25 blocking-defect class RC6's
    // stall detector exists to catch. A fixed number recalibrated against
    // this one incident would still be exactly as fragile the next time
    // CI's own throughput drifts, so this tier is now SELF-CALIBRATING
    // instead: it times ONE real TextureCache::registerDecoded() call,
    // right here, against a synthetic payload sized like this fixture's
    // own real textures (2048x2048 RGBA8 -- every one of DamagedHelmet's
    // 5 JPGs decodes to exactly this size; the calibration op's cost is
    // otherwise dominated by payload-size-dependent memcpy/copy-recording
    // cost, not fixed per-call overhead -- an earlier 4x4 probe measured
    // only that fixed overhead and was two orders of magnitude too small
    // to be a representative baseline, see the report), on THIS machine,
    // THIS run, immediately before the real gate below -- then derives
    // the CI-tier ceiling as a multiple of that live measurement, so it
    // scales automatically with whatever hardware this test happens to
    // run on instead of encoding one machine's snapshot forever.
    //
    // MULTIPLIER DERIVATION (measured, not guessed -- 3 measurement sets,
    // 22 total runs, constrained repro: taskset -c 0,1 + LP_NUM_THREADS=1
    // + forced lavapipe, matching CI's own 2-vCPU/software-rasterizer
    // class; full per-run numbers in wine-segfault-fix-report.md's own
    // addendum):
    //   1. LEGIT max (this exact gate, 22 runs across two batches -- 12
    //      WALL-CLOCK-only + 10 full-suite): in-loop max pumpMain()
    //      duration 9166-10288us (worst observed: 10288us).
    //   2. CALIBRATION baseline (this exact probe, same 22 runs):
    //      5634-6202us. Worst-case per-run ratio (legit max / calibration,
    //      SAME run): 1.771x -- i.e. a real single-texture registration on
    //      this class of hardware can cost up to ~1.77x this calibration
    //      op.
    //   3. BLOCKING-DEFECT signature (scratch worktree, pre-RC6 whole-
    //      batch shape reintroduced -- the same revert shape as Task
    //      11/15's own evidence: marshalGltfImportPrepareStep()'s early
    //      `return true` after ONE texture slot removed, so all 5 real
    //      textures register in a single call -- 5 runs): 39399-40812us
    //      (worst case toward a tight margin: 39399us, the SMALLEST
    //      observed).
    //   Total available separation (blocking / legit) on this hardware
    //   class is ~3.8-4.0x (39399/10288 = 3.83) -- NOT the order-of-
    //   magnitude RC6's original prose assumed, so a strict >=2x margin on
    //   BOTH sides simultaneously is only barely mathematically possible
    //   (2x * 2x = 4x) and leaves zero slack against the worst-case
    //   numbers above -- this is the coordinator-anticipated "bands
    //   overlap" outcome for a naive FIXED threshold in that band, which
    //   is exactly why this tier is self-calibrating rather than a
    //   constant. Multiplier = 4 (of the live calibration measurement)
    //   biases toward legit-side safety (avoiding a false-positive CI-red
    //   is the whole reason this recalibration exists) while keeping a
    //   real, meaningful margin toward the defect signature:
    //     - vs. legit max: 4 / 1.771(worst-case ratio) = 2.26x margin --
    //       clears the >=2x bar with room to spare.
    //     - vs. blocking signature: on this hardware, 4x calibration
    //       (~23-25ms) sits ~1.6-1.7x below the 39.4-40.8ms blocking
    //       signature -- short of a full 2x margin (the ~3.8-4x total
    //       separation does not allow both margins to be a full 2x with
    //       any slack), but still a clean, unambiguous fail for the
    //       defect class this tier exists to catch, not a coin flip.
    //   A floor of kLocalBudget below prevents a degenerate near-zero
    //   threshold if calibration ever reads implausibly small.
    constexpr int64_t kCiStallMultiplier = 4;

    TextureDecodeResult calibrationPayload;
    calibrationPayload.outcome = TextureDecodeResult::Outcome::Ready;
    calibrationPayload.format = VK_FORMAT_R8G8B8A8_UNORM;
    calibrationPayload.width = 2048;
    calibrationPayload.height = 2048;
    DecodedTextureLevel calibrationLevel0;
    calibrationLevel0.level = 0;
    calibrationLevel0.width = 2048;
    calibrationLevel0.height = 2048;
    calibrationLevel0.bytes.assign(static_cast<size_t>(2048) * 2048 * 4, std::byte{0xFF});
    calibrationPayload.levels.push_back(std::move(calibrationLevel0));
    const auto calibStart = std::chrono::steady_clock::now();
    TextureHandle calibrationHandle = fixture->textures->registerDecoded(calibrationPayload, "wallclock-gate-calibration-probe");
    const auto calibDuration =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - calibStart);
    REQUIRE(calibrationHandle.isValid());
    MESSAGE("wall-clock gate: calibration probe (2048x2048 synthetic registerDecoded()) took ", calibDuration.count(),
            " us on this run -- CI-tier threshold derived as ", kCiStallMultiplier, "x this value");

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

    constexpr auto kLocalBudget = std::chrono::microseconds(2000);   // 2 ms, published/trend-tracked -- unchanged
    const auto kCiStallDetector = std::max(kLocalBudget, kCiStallMultiplier * calibDuration);  // self-calibrated (see above)

    uint64_t framesWhileInFlight = 0;
    std::chrono::microseconds maxPumpDuration{0};
    uint32_t overLocalBudgetCount = 0;

    // [Issue #30] The loop below no longer asserts inside the loop body. It
    // used to REQUIRE(pumpDuration < kCiStallDetector) AND
    // REQUIRE(now < testDeadline) on EVERY iteration -- but how many
    // iterations run before `done` flips true is itself a direct function of
    // live wall-clock/scheduler timing (this is deliberately a real,
    // overlapping async import against real GPU work), so the doctest
    // ASSERTION COUNT for this one test case varied run to run (confirmed:
    // baseline 10-run/4-core measurement for this whole binary showed
    // 8203599 vs 8153987 total assertions across otherwise-identical 57/57
    // passes -- this loop is the dominant contributor). Aggregating the
    // per-iteration measurement into `maxPumpDuration` and asserting ONCE
    // after the loop (below) makes the assertion count for this claim fixed
    // (exactly 1) regardless of iteration count, while preserving the exact
    // same discriminating power: a reintroduced blocking wait() still shows
    // up as an outlier this max captures, clearing kCiStallDetector by the
    // same margin the multiplier-derivation comment above already
    // establishes (a spike on any single iteration cannot be diluted by
    // however many fast iterations happen to run alongside it, since max()
    // ignores everything but the worst observed value).
    const auto testDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done && std::chrono::steady_clock::now() < testDeadline) {
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
    }

    // The timeout safety net now surfaces here (done stays false if
    // testDeadline was hit) instead of via a per-iteration REQUIRE inside
    // the loop -- same failure outcome, fixed assertion count.
    REQUIRE(done);
    REQUIRE(result.ok());
    // >= 300 rendered frames while genuinely in flight -- proves real
    // overlap (the import did not simply complete on the first pump).
    CHECK(framesWhileInFlight >= 300);

    // The CI stall-detector tier (RC6, self-calibrated -- see
    // kCiStallDetector's own comment above): THE load-bearing assertion for
    // this whole test case, asserted once, hard, against the worst pumpMain()
    // observed across the entire run -- see the [Issue #30] comment above the
    // loop for why this replaced a per-iteration REQUIRE.
    REQUIRE(maxPumpDuration < kCiStallDetector);

    // Published number [D18/RC6]: the 2ms figure is a TREND metric,
    // explicitly "never CI-blocking" (D18's own wording, amended by RC6
    // only for the SEPARATE, now self-calibrated stall-detector tier
    // asserted above via REQUIRE) -- reported via MESSAGE only,
    // deliberately never a CHECK/REQUIRE, so a real but small overage
    // (this task's own dev-machine run: DamagedHelmet's largest single
    // JPEG texture, ~1.3MB decoded to RGBA8, occasionally costs a few ms
    // of real memcpy+copy-recording time for ONE texture's own
    // registerDecoded() step -- see task-15-report.md's own
    // performance-numbers section) is visible in the log without flaking
    // CI, exactly matching D18's own posture for every other wall-clock
    // number in this codebase.
    MESSAGE("wall-clock gate: max single pumpMain() call = ", maxPumpDuration.count(), " us across ",
            framesWhileInFlight, " in-flight frame(s); ", overLocalBudgetCount,
            " call(s) exceeded the 2ms local/published budget (trend-tracked, never CI-blocking per D18/RC6) -- "
            "self-calibrated CI-tier ceiling this run was ", kCiStallDetector.count(),
            " us (REQUIRE'd once above, aggregated over every call, see kCiStallDetector's own comment)");

    // [Fix round 1, IMPORTANT item] D25 direct proof, not just a wall-clock
    // proxy: the async path never called Uploader::wait() through EITHER
    // GeometryPool or TextureCache -- both counters must be unchanged from
    // their pre-import baseline.
    CHECK(fixture->pool->waitCallCountForTesting() == poolWaitCallsBefore);
    CHECK(fixture->textures->waitCallCountForTesting() == textureWaitCallsBefore);

    // [Fix round 1, IMPORTANT item] Direct ring/pool no-exhaustion check:
    // the import's combined geometry landed in exactly one block, used
    // real (non-zero) capacity, and stayed within that block's own
    // capacity the whole time -- real progress, not a silent
    // stall/overflow that the wall-clock bound alone would only catch
    // indirectly (an actually-exhausted/blocking ring reclaim would also
    // violate the 10ms ceiling above, but this is the direct proof the
    // brief asked for, not an inference from timing).
    const PoolStats poolStatsAfter = fixture->pool->stats();
    CHECK(poolStatsAfter.blockCount >= 1);
    CHECK(poolStatsAfter.vertexBytesUsed > 0);
    CHECK(poolStatsAfter.vertexBytesUsed <= poolStatsAfter.vertexBytesCapacity);
    CHECK(poolStatsAfter.indexBytesUsed > 0);
    CHECK(poolStatsAfter.indexBytesUsed <= poolStatsAfter.indexBytesCapacity);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ---------------------------------------------------------------------
// Cancel-mid-upload rollback [Fix round 1, mandatory]: cancelling AFTER
// real GPU resources are already registered -- both a texture AND a
// geometry range, not just one -- must roll BOTH back through
// marshalGltfImportRollback(), not merely stop future work.
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: cancelImport() after >=1 real GPU resource (texture AND geometry) is already "
          "registered rolls BOTH back -- geometry range freed, non-fallback texture released, no registry "
          "mutation, callback never fires") {
    if (!std::filesystem::exists(damagedHelmetPath())) {
        MESSAGE("DamagedHelmet not fetched (run tools/fetch_assets.sh) -- skipping the cancel-mid-upload rollback "
                "test");
        return;
    }
    auto fixture = makeFixture("async_cancel_mid_upload");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    // A real TextureCache is load-bearing here for the same reason as the
    // WALL-CLOCK GATE test above: this is the ONLY combination this file
    // has already proven, empirically, decodes and registers real GPU
    // resources through the async prepare-step path.
    attachTextureCache(*fixture);
    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);

    // Slow decode (same mechanism as the WALL-CLOCK GATE test): widens the
    // in-flight window so this test's own per-tick pump loop below has
    // ample room to observe the mid-upload state deterministically,
    // without racing a real completion.
    SlowRecordingByteSource source(std::filesystem::path(damagedHelmetPath()).parent_path(),
                                    std::chrono::milliseconds(60));
    std::vector<std::byte> documentBytes = readFileBytes(damagedHelmetPath());

    Registry registry;
    const size_t liveTexturesBefore = fixture->textures->liveTextureCountForTesting();
    const PoolStats poolStatsBefore = fixture->pool->stats();
    REQUIRE(poolStatsBefore.blockCount == 0);

    bool done = false;
    AsyncImportHandle handle = registry.importGltfAsync(documentBytes, source, *fixture->pool, *scheduler,
                                                            fixture->textures.get(), [&](ImportResult) { done = true; });
    REQUIRE(handle.isValid());

    // Pump ONE bounded step at a time (Scheduler::pumpMain()'s own
    // swap-then-run semantics -- a closure posted mid-drain is only ever
    // picked up by the NEXT call, scheduler.cpp's own comment) until the
    // geometry step has run -- observed directly via GeometryPool::stats(),
    // a real GPU-facing signal, not an internal cursor this test cannot
    // see. marshalGltfImportPrepareStep()'s own ordering registers EVERY
    // texture slot before the geometry branch ever runs (texturesPrepared
    // must already be true first), so by the time this loop observes
    // non-zero geometry usage, every one of DamagedHelmet's real textures
    // is ALSO already registered -- both real GPU resource classes are
    // live at the exact instant this loop calls cancelImport() below, and
    // finalize() (the only step that would mutate the Registry) is still
    // strictly ahead of this point (prepare -> poll -> finalize, in that
    // order, per registry.cpp's own postToMain chain).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool geometryRegistered = false;
    bool finalizedBeforeGeometryObserved = false;
    while (std::chrono::steady_clock::now() < deadline) {
        scheduler->pumpMain();
        if (fixture->pool->stats().blockCount > 0) {
            geometryRegistered = true;
            break;
        }
        if (done) {
            finalizedBeforeGeometryObserved = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(geometryRegistered);
    // [Issue #30] Aggregated to a single deterministic assertion -- this was
    // a REQUIRE_FALSE(done) evaluated once per loop spin above (an iteration
    // count that is itself wall-clock/scheduling-dependent). Same
    // discriminating power: the flag latches permanently the first time
    // `done` is observed true before the mid-upload window is caught.
    // [Round 2, LOW finding] REQUIRE, not CHECK -- restores the original
    // abort-on-violation semantics: a caught violation here means the rest
    // of this test case's own state (post-cancel mesh/material/pool
    // assertions below) can no longer be trusted, so it must not keep
    // running against data the violation may have already invalidated.
    REQUIRE_FALSE(finalizedBeforeGeometryObserved);

    REQUIRE(fixture->textures->liveTextureCountForTesting() > liveTexturesBefore);
    const PoolStats poolStatsMidUpload = fixture->pool->stats();
    REQUIRE(poolStatsMidUpload.vertexBytesUsed > 0);
    REQUIRE(poolStatsMidUpload.indexBytesUsed > 0);
    const size_t meshesBeforeCancel = registry.meshCountForTesting();
    const size_t materialsBeforeCancel = registry.materialCountForTesting();

    registry.cancelImport(handle);

    // Drain the rollback -- marshalGltfImportRollback() runs on the very
    // next runAsyncImportPrepareStep()/pollAsyncImportUploads() tick that
    // observes `cancelled` (both check it first, before touching pool/
    // textures further) -- bounded latency, same contract as the
    // immediate-cancel test above.
    const auto rollbackDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < rollbackDeadline) {
        scheduler->pumpMain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (registry.importProgress(handle).cancelled && fixture->pool->stats().vertexBytesUsed == 0) {
            break;
        }
    }

    // Abandon semantics: no callback, no registry mutation.
    CHECK_FALSE(done);
    CHECK(registry.meshCountForTesting() == meshesBeforeCancel);
    CHECK(registry.materialCountForTesting() == materialsBeforeCancel);

    // GEOMETRY rollback: the block itself persists (GeometryPool never
    // deallocates a whole block, D9's own no-defragmentation posture) but
    // its used-bytes accounting must return to exactly zero -- the
    // suballocation was freed back to the block's own TLSF metadata
    // (pool.free(), vmaVirtualFree -- synchronous, CPU-side, no
    // DeletionQueue involved for geometry).
    const PoolStats poolStatsAfterRollback = fixture->pool->stats();
    CHECK(poolStatsAfterRollback.vertexBytesUsed == 0);
    CHECK(poolStatsAfterRollback.indexBytesUsed == 0);

    // TEXTURE rollback: releaseUnpublished() only marks the entry
    // non-resident immediately and defers the real HandlePool reclaim into
    // DeletionQueue [D24 clause (c)] -- liveTextureCountForTesting() only
    // reflects it once the deletion queue's own fence-gated pass runs.
    //
    // [Fix round 2, ITEM 2] Deliberately NO vkDeviceWaitIdle() before this
    // call -- round 1's own version had one here, and independent review
    // proved it was masking the very race this test exists to catch:
    // reverting registry.cpp's rollbackAsyncImportWhenSafe() back to an
    // unconditional, immediate marshalGltfImportRollback() call still
    // passed 8/8 with the wait in place (the wait, all on its own, drains
    // every in-flight GPU submission -- including the still-racing old
    // copy -- before onFrameFenceSignaled() ever gets a chance to destroy
    // anything, regardless of whether the production code itself waited
    // for anything). THIS is now the actual discriminating step: with
    // rollbackAsyncImportWhenSafe() intact, every upload ticket
    // prepare() issued is already confirmed complete (via
    // marshalGltfImportUploadsComplete()'s own non-blocking poll) by the
    // time marshalGltfImportRollback() ever runs, so destroying
    // immediately afterward, with no additional wait of any kind, is
    // safe. See task-15-report.md's round-2 section for the fresh
    // revert-evidence pair (fails reverted / passes intact) this
    // restructuring was verified against. Ordinary fixture teardown
    // (Device::~Device()'s own unconditional vkDeviceWaitIdle) remains
    // the safety net for whatever this call does NOT reclaim -- nothing
    // in this test relies on an extra manual wait for that.
    fixture->deletionQueue->onFrameFenceSignaled(0);
    CHECK(fixture->textures->liveTextureCountForTesting() == liveTexturesBefore);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ---------------------------------------------------------------------
// Cancel mid-registration [Fix round 2, ITEM 1, CRITICAL]: a cancellation
// caught between two texture slots -- some already marshalled, some
// not -- must never touch the ones marshal has not reached yet, whose
// TextureRef::handle still holds the COMPUTE-time shared checkerboard
// placeholder (import_gltf.cpp's own fillRef() lambda), not a handle this
// import owns.
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: cancelImport() after EXACTLY 1 of N texture slots has been registered does NOT "
          "release the shared checkerboard fallback -- liveTextureCountForTesting() never drops below the "
          "pre-import baseline, checkerboard stays resolvable") {
    if (!std::filesystem::exists(damagedHelmetPath())) {
        MESSAGE("DamagedHelmet not fetched (run tools/fetch_assets.sh) -- skipping the checkerboard-safety test");
        return;
    }
    auto fixture = makeFixture("async_cancel_checkerboard_safety");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    // DamagedHelmet has 5 present, real (Ready-outcome) texture slots
    // (baseColor/metallicRoughness/normal/occlusion/emissive) -- N > 1 is
    // load-bearing here: this test needs marshal to still have slots left
    // to reach AFTER the point it cancels, which is exactly the window
    // the bug this test targets lives in.
    attachTextureCache(*fixture);
    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);

    FilesystemByteSource source(std::filesystem::path(damagedHelmetPath()).parent_path());
    std::vector<std::byte> documentBytes = readFileBytes(damagedHelmetPath());

    Registry registry;
    const size_t liveTexturesBefore = fixture->textures->liveTextureCountForTesting();
    const TextureHandle checkerboard = fixture->textures->checkerboardHandle();
    REQUIRE(fixture->textures->resolve(checkerboard).width == 4);  // sanity: this IS the checkerboard, its own fixed dims

    bool done = false;
    AsyncImportHandle handle = registry.importGltfAsync(documentBytes, source, *fixture->pool, *scheduler,
                                                            fixture->textures.get(), [&](ImportResult) { done = true; });
    REQUIRE(handle.isValid());

    // Pump ONE bounded step at a time (same swap-then-run reasoning as the
    // cancel-mid-upload test above) until liveTextureCountForTesting()
    // first becomes EXACTLY baseline+1 -- marshalGltfImportPrepareStep()
    // registers at most one real texture slot per call (RC6 time-slicing),
    // and a Ready-outcome registration is the ONLY thing that increments
    // liveCount (a Failed/Checkerboard-outcome slot's own registerDecoded()
    // call returns a SHARED fallback handle, never a new pool acquisition
    // -- see TextureCache::applyDecodeResult()), so this is a precise,
    // non-guessed "exactly 1 of N slots marshalled, N-1 still ahead"
    // checkpoint, not an approximation.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool oneRegistered = false;
    size_t minLiveObservedDuringDrive = liveTexturesBefore;
    bool finalizedBeforeOneRegistered = false;
    bool overshotPastOne = false;
    while (std::chrono::steady_clock::now() < deadline) {
        scheduler->pumpMain();
        const size_t live = fixture->textures->liveTextureCountForTesting();
        minLiveObservedDuringDrive = std::min(minLiveObservedDuringDrive, live);
        if (live == liveTexturesBefore + 1) {
            oneRegistered = true;
            break;
        }
        if (done) {
            finalizedBeforeOneRegistered = true;
        }
        if (live > liveTexturesBefore + 1) {
            overshotPastOne = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(oneRegistered);
    // [Issue #30] Both claims aggregated to single deterministic assertions --
    // previously a REQUIRE_FALSE(done)/REQUIRE(live<=+1) pair evaluated once
    // per loop spin (a wall-clock/scheduling-dependent count). Same
    // discriminating power: either flag latches permanently on the first
    // violating tick. [Round 2, LOW finding] REQUIRE, not CHECK -- restores
    // the original abort-on-violation semantics (a caught violation here
    // means the rest of this test case's own state can no longer be
    // trusted).
    REQUIRE_FALSE(finalizedBeforeOneRegistered);  // must not have finalized before this loop caught the 1-of-N window
    REQUIRE_FALSE(overshotPastOne);  // never overshoots past exactly 1
    // Never dropped below baseline on the way here either (the checkerboard
    // and every role fallback are built once at TextureCache::create() and
    // must never be touched by anything this import's own marshal does).
    REQUIRE(minLiveObservedDuringDrive >= liveTexturesBefore);

    registry.cancelImport(handle);

    // Drain the rollback, watching liveTextureCountForTesting() on EVERY
    // tick (not just at the end) -- the bug this test targets is a
    // TRANSIENT-if-unwatched drop (releaseUnpublished() on the shared
    // checkerboard handle marks it non-resident immediately, then
    // DeletionQueue's own later reclaim is what would actually destroy
    // the VkImage; watching every tick catches the non-resident marking
    // half too, not merely the eventual destroy).
    //
    // Deliberately NOT an early-exit-on-first-observed-state loop like
    // the geometry-side cancel-mid-upload test above -- GeometryPool::
    // stats().vertexBytesUsed dropping to zero is a synchronous,
    // reliable "pool.free() has now actually run" signal, but there is
    // no equivalent synchronous signal on the texture side:
    // liveTextureCountForTesting() does not change at all until
    // onFrameFenceSignaled() below runs, so "cancelled == true" alone
    // says nothing about whether rollbackAsyncImportWhenSafe()'s own
    // ticket-completion poll (marshalGltfImportEnsureRollbackTicketed(),
    // ITEM 3) has finished yet. Instead: drive a fixed, generous settle
    // window of pumpMain() calls AFTER cancellation is first observed,
    // long enough for that poll loop to genuinely converge (the
    // underlying GPU work is one small JPEG decode's worth of bytes --
    // real completion happens within a handful of frames on any GPU this
    // test suite runs on).
    size_t minLiveObservedDuringRollback = fixture->textures->liveTextureCountForTesting();
    const auto rollbackDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool cancelledObserved = false;
    int settleTicksRemaining = 200;
    while (std::chrono::steady_clock::now() < rollbackDeadline && settleTicksRemaining > 0) {
        scheduler->pumpMain();
        minLiveObservedDuringRollback =
            std::min(minLiveObservedDuringRollback, fixture->textures->liveTextureCountForTesting());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (registry.importProgress(handle).cancelled) {
            cancelledObserved = true;
        }
        if (cancelledObserved) {
            --settleTicksRemaining;
        }
    }
    REQUIRE(cancelledObserved);
    CHECK_FALSE(done);
    CHECK(minLiveObservedDuringRollback >= liveTexturesBefore);

    // Real reclaim -- deliberately no vkDeviceWaitIdle() here either, same
    // reasoning as ITEM 2's own restructuring above: the production fix
    // (rollbackAsyncImportWhenSafe(), plus this round's own `registered`
    // gate in marshalGltfImportRollback()) is what must make this safe.
    fixture->deletionQueue->onFrameFenceSignaled(0);
    CHECK(fixture->textures->liveTextureCountForTesting() == liveTexturesBefore);

    // The direct proof: the shared checkerboard itself is still exactly
    // what it always was -- same dims, still a resident fallback record --
    // not a destroyed/dangling handle silently substituted or corrupted.
    const TextureRecord& cb = fixture->textures->resolve(checkerboard);
    CHECK(cb.isFallback);
    CHECK(cb.resident);
    CHECK(cb.width == 4);
    CHECK(cb.height == 4);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ---------------------------------------------------------------------
// Concurrent imports: isolated, correct, and D24-safe to resolve DURING a
// genuine, actively-driven overlap window with another in-flight import
// (not merely two imports that happen to be started close together).
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: a resolve-heavy loop against an ALREADY-COMPLETED import's handles runs correctly "
          "and repeatedly WHILE a second, independent async import is PROVABLY still in flight [D24, "
          "mandatory literal overlap -- not a reworded/inferred overlap]") {
    auto fixture = makeFixture("async_concurrent");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);
    Registry registry;

    // --- Import A: driven fully to completion FIRST (pumpUntilTerminal),
    // so its handles are genuine, already-published registry state before
    // B ever starts -- the resolve loop below targets THIS import's
    // handles, never its own still-in-flight ones.
    ImportResult resultA;
    bool doneA = false;
    AsyncImportHandle handleA = registry.importGltfAsync(
        testAssetDir() + "/cube_textured.gltf", *fixture->pool, *scheduler, nullptr,
        [&](ImportResult r) {
            resultA = std::move(r);
            doneA = true;
        });
    REQUIRE(handleA.isValid());
    REQUIRE(pumpUntilTerminal(*scheduler, registry, handleA));
    REQUIRE(doneA);
    REQUIRE(resultA.ok());
    REQUIRE(resultA.meshes.size() == 1);
    REQUIRE(registry.mesh(resultA.meshes[0]).submeshes.size() == 1);
    const MeshRange expectedRangeA = registry.mesh(resultA.meshes[0]).submeshes[0].range;
    const AABB expectedBoundsA = registry.mesh(resultA.meshes[0]).submeshes[0].bounds;

    // --- Import B: held open via a slow, THREAD-RECORDING byte source --
    // provably in flight (progress polled fresh on every loop iteration
    // below, never assumed from timing alone), wide enough that the
    // resolve loop against A gets MANY iterations while B is still
    // running, satisfying the mandatory "literally overlap, not reworded"
    // requirement.
    SlowRecordingByteSource sourceB(std::filesystem::path(testAssetDir()), std::chrono::milliseconds(40));
    std::vector<std::byte> documentBytesB = readFileBytes(testAssetDir() + "/cube_multi_primitive.gltf");

    ImportResult resultB;
    bool doneB = false;
    AsyncImportHandle handleB = registry.importGltfAsync(
        documentBytesB, sourceB, *fixture->pool, *scheduler, nullptr,
        [&](ImportResult r) {
            resultB = std::move(r);
            doneB = true;
        });
    REQUIRE(handleB.isValid());
    CHECK_FALSE(handleA == handleB);

    // The overlap window itself: pump B forward one tick at a time; on
    // EVERY tick where B's own freshly-polled progress proves it has not
    // yet reached a terminal stage, immediately resolve A's already-
    // published handle again and check its data is still exactly correct
    // -- a stale/torn/corrupted-by-B resolve would show up right here, in
    // the same instant B is mutating its OWN in-flight compute/upload
    // state on other threads.
    uint64_t overlapResolves = 0;
    bool submeshCountWrong = false;
    bool dataCorrupted = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!doneB && std::chrono::steady_clock::now() < deadline) {
        const ImportProgress progressB = registry.importProgress(handleB);
        const bool bStillInFlight =
            progressB.stage != ImportStage::Done && progressB.stage != ImportStage::Failed && !progressB.cancelled;
        if (bStillInFlight) {
            const MeshAsset& meshA = registry.mesh(resultA.meshes[0]);
            if (meshA.submeshes.size() != 1) {
                submeshCountWrong = true;
            } else {
                const Submesh& subA = meshA.submeshes[0];
                const bool matches = subA.range.blockId == expectedRangeA.blockId &&
                                      subA.range.firstIndex == expectedRangeA.firstIndex &&
                                      subA.range.vertexOffset == expectedRangeA.vertexOffset &&
                                      subA.range.indexCount == expectedRangeA.indexCount &&
                                      subA.bounds.min.x == doctest::Approx(expectedBoundsA.min.x) &&
                                      subA.bounds.max.x == doctest::Approx(expectedBoundsA.max.x);
                if (!matches) {
                    dataCorrupted = true;
                }
            }
            ++overlapResolves;
        }
        scheduler->pumpMain();
    }

    // [Issue #30] Deterministic assertion count: the per-iteration REQUIRE +
    // 6x CHECK block above (however many ticks the wall-clock/scheduling
    // race happened to land while B was in flight -- itself timing-
    // dependent) is now 2 fixed assertions, aggregated across the whole
    // overlap window. Same discriminating power: a stale/torn/corrupted
    // resolve on ANY tick still latches dataCorrupted (or submeshCountWrong)
    // permanently.
    REQUIRE_FALSE(submeshCountWrong);
    CHECK_FALSE(dataCorrupted);

    // Proof this was a genuine, sustained overlap, not one lucky
    // iteration: many resolve-loop passes against A landed while B's own
    // polled progress was demonstrably non-terminal.
    CHECK(overlapResolves >= 20);

    REQUIRE(doneB);
    REQUIRE(resultB.ok());
    REQUIRE(resultB.meshes.size() == 1);
    CHECK(registry.mesh(resultB.meshes[0]).submeshes.size() == 2);

    // D24: resolving one import's handles is unaffected by the other's,
    // including throughout the just-exercised genuinely-overlapping
    // window -- distinct HandlePool acquisitions, disjoint handle spaces.
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

// ---------------------------------------------------------------------
// [Wine/CI SIGSEGV regression, CI run 32180630087] Abandoning a job that
// has already reached the MARSHAL/UPLOAD phase (>=1 real GPU resource
// registered), with NO explicit cancelImport() ever called and no further
// pumpMain() call -- registry.cpp's own ~Registry() must roll it back
// synchronously (drainAndRollbackAbandonedAsyncJob()), not merely flag it
// cancelled and walk away. The two teardown-race tests above never
// exercised this: both pass textures == nullptr, so their jobs never
// leave the compute phase and never have anything GPU-side to roll back.
// This is exactly the state the WALL-CLOCK GATE test's own CI stall-
// detector REQUIRE (line 748) landed in when it fired on CI's slower
// hardware -- doctest's exception unwind skipped straight to local
// destructors mid-upload, which is indistinguishable, from this
// production code's own point of view, from any other reason a host
// application might stop pumping mid-import (an unrelated fatal error, an
// early shutdown path). Before the fix: a real
// VUID-vkEndCommandBuffer-commandBuffer-00059 validation error followed by
// SIGSEGV. After the fix: clean teardown, zero validation errors.
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: abandoning a job genuinely mid-UPLOAD (>=1 real GPU resource already registered, no "
          "explicit cancelImport(), no further pumpMain()) does not crash and raises no validation errors "
          "[Wine/CI SIGSEGV regression, CI run 32180630087]") {
    if (!std::filesystem::exists(damagedHelmetPath())) {
        MESSAGE("DamagedHelmet not fetched (run tools/fetch_assets.sh) -- skipping the abandon-mid-upload "
                "regression test");
        return;
    }
    auto fixture = makeFixture("async_abandon_mid_upload");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    // A real TextureCache is load-bearing here -- same reason as the
    // WALL-CLOCK GATE and cancel-mid-upload tests above: this is what
    // actually gets this job into the marshal/upload phase with a real
    // VkImage registered, the exact state the root cause needs to exist.
    attachTextureCache(*fixture);
    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);

    SlowRecordingByteSource source(std::filesystem::path(damagedHelmetPath()).parent_path(),
                                    std::chrono::milliseconds(60));
    std::vector<std::byte> documentBytes = readFileBytes(damagedHelmetPath());

    // Registry wrapped in optional so it can be destroyed explicitly,
    // in-scope, BEFORE `scheduler` -- letting this test check
    // `fixture->context.hasValidationErrors()` afterward, while `fixture`
    // (and its Context) is still alive, exactly like every other test in
    // this file does.
    std::optional<Registry> registryOpt{std::in_place};
    Registry& registry = *registryOpt;
    const size_t liveTexturesBefore = fixture->textures->liveTextureCountForTesting();

    bool fired = false;
    AsyncImportHandle handle = registry.importGltfAsync(documentBytes, source, *fixture->pool, *scheduler,
                                                            fixture->textures.get(), [&](ImportResult) { fired = true; });
    REQUIRE(handle.isValid());

    // Pump until AT LEAST ONE real texture slot has been registered -- a
    // genuine GPU resource (a real VkImage with an upload copy command
    // already recorded via TextureCache::registerDecoded()) -- the same
    // observable signal the checkerboard-safety test above uses.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool oneRegistered = false;
    while (std::chrono::steady_clock::now() < deadline) {
        scheduler->pumpMain();
        if (fixture->textures->liveTextureCountForTesting() > liveTexturesBefore) {
            oneRegistered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(oneRegistered);
    REQUIRE_FALSE(fired);  // must not have finalized before this test caught the mid-upload window

    // Deliberately NO cancelImport() call -- mirrors the WALL-CLOCK GATE
    // test's own REQUIRE-failure unwind: the job is abandoned genuinely
    // mid-upload, with nothing more ever pumping it. Registry destructs
    // first (registry.cpp's own ~Registry() -- this is where
    // drainAndRollbackAbandonedAsyncJob() now runs, synchronously, while
    // fixture->pool/fixture->textures are still fully valid), then
    // Scheduler (drains/joins its own threads; any still-queued closure
    // referencing this job is simply dropped, now safe because Registry's
    // destructor already released the job's real GPU resources).
    registryOpt.reset();
    scheduler.reset();

    // Reclaim the deferred-destroy texture drainAndRollbackAbandonedAsyncJob()'s
    // own releaseUnpublished() call handed to the DeletionQueue -- the SAME
    // cleanup step the explicit cancel-mid-upload rollback test above
    // performs after its own rollback (D24's own eviction contract: a
    // caller must eventually flush a non-empty DeletionQueue; Registry has
    // no DeletionQueue reference of its own to do this automatically, by
    // design -- see that class's own comment).
    fixture->deletionQueue->onFrameFenceSignaled(0);

    CHECK_FALSE(fired);

    // [Root-cause-accurate check] The corrupting event this regression
    // targets -- Uploader::~Uploader()'s own auto-flush calling
    // vkEndCommandBuffer() on a command buffer whose bound VkImage was
    // ALREADY destroyed -- only happens once BOTH (a) the abandoned
    // texture's VkImage has been destroyed (TextureCache's own pool_
    // member unconditionally destroys every still-"resident" entry when
    // TextureCache itself is destroyed) AND (b) Uploader's own recording
    // command buffer (which still references that image, if it was never
    // safely released) gets flushed/ended afterward. Reproducing that
    // exact sequence explicitly -- rather than only implicitly, via
    // `fixture`'s natural end-of-scope teardown -- keeps `fixture->context`
    // alive afterward so this test can actually OBSERVE the result via
    // hasValidationErrors(), instead of only proving "the process didn't
    // crash" (empirically, revert-testing this fix: native lavapipe on
    // this dev machine tolerates the resulting corrupted command-buffer
    // state without crashing -- only CI's own environment, both the
    // linux-native and windows-cross-zig/Wine jobs, actually segfaults on
    // it, CI run 32180630087 -- so a bare "did it crash" check is too weak
    // a discriminator locally; the validation error itself is the
    // reliable, portable signal both before and after the fix).
    //
    // Before this fix: `textures.reset()` destroys a TextureCache that
    // still holds the abandoned texture as a live, never-released entry
    // (nothing ever called releaseUnpublished() for it) -- destroying it
    // here, then flushing Uploader explicitly right after, reproduces
    // CI's exact VUID-vkEndCommandBuffer-commandBuffer-00059 validation
    // error deterministically (confirmed via this fix's own revert-test:
    // 3/3 runs against the pre-fix registry.cpp). After this fix:
    // Registry::~Registry() (via drainAndRollbackAbandonedAsyncJob(),
    // registry.cpp) already safely released it through
    // TextureCache::releaseUnpublished() + the DeletionQueue's own
    // fence-gated reclaim (the onFrameFenceSignaled() call above), so by
    // the time `textures.reset()` runs here, there is nothing left to
    // corrupt.
    fixture->textures.reset();
    fixture->uploader.flush();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ---------------------------------------------------------------------
// [Wine/CI SIGSEGV regression, fix round 2, CRITICAL -- reviewer-found,
// reproduced 3/3 against fix round 1] The round-1 fix above
// (drainAndRollbackAbandonedAsyncJob(), registry.cpp) only helps a job
// that is STILL TRACKED in Registry's own asyncJobs_ map at ~Registry()
// time. reapFinishedAsyncJobs() (called at the top of every
// importGltfAsync()/cancelImport() call, pre-existing Task 15 code) used
// to erase a job the instant `cancelled` flipped true, REGARDLESS of
// whether its `pending` had actually been drained yet. Calling
// cancelImport() on the SAME handle TWICE, with no pumpMain() call
// between them, hit exactly this: the second call's own
// reapFinishedAsyncJobs() erased the still-`pending` job from the map
// right then -- making it INVISIBLE to ~Registry()'s own drain loop
// (which only ever walks asyncJobs_) -- reproducing the identical
// corrupted-command-buffer SIGSEGV round 1 was supposed to close, via a
// different path into the same gap. registry.cpp's reapFinishedAsyncJobs()
// now gates the cancelled case on `pending == nullptr` too -- this test
// is the reviewer's own probe, shipped as a permanent regression test.
// ---------------------------------------------------------------------

TEST_CASE("importGltfAsync: cancelImport() called TWICE on a job genuinely mid-UPLOAD, then destroyed with no "
          "pumpMain() between any of the three steps, does not crash and raises no validation errors "
          "[Wine/CI SIGSEGV regression round 2, reapFinishedAsyncJobs() pending-aware gate]") {
    if (!std::filesystem::exists(damagedHelmetPath())) {
        MESSAGE("DamagedHelmet not fetched (run tools/fetch_assets.sh) -- skipping the double-cancel regression "
                "test");
        return;
    }
    auto fixture = makeFixture("async_double_cancel_mid_upload");
    if (!fixture) {
        return;
    }
    attachPool(*fixture);
    attachTextureCache(*fixture);
    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);

    SlowRecordingByteSource source(std::filesystem::path(damagedHelmetPath()).parent_path(),
                                    std::chrono::milliseconds(60));
    std::vector<std::byte> documentBytes = readFileBytes(damagedHelmetPath());

    std::optional<Registry> registryOpt{std::in_place};
    Registry& registry = *registryOpt;
    const size_t liveTexturesBefore = fixture->textures->liveTextureCountForTesting();

    bool fired = false;
    AsyncImportHandle handle = registry.importGltfAsync(documentBytes, source, *fixture->pool, *scheduler,
                                                            fixture->textures.get(), [&](ImportResult) { fired = true; });
    REQUIRE(handle.isValid());

    // Pump until AT LEAST ONE real texture slot has been registered --
    // the job is now genuinely mid-upload (`pending != nullptr`), same
    // signal the single-cancel regression test above uses.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool oneRegistered = false;
    while (std::chrono::steady_clock::now() < deadline) {
        scheduler->pumpMain();
        if (fixture->textures->liveTextureCountForTesting() > liveTexturesBefore) {
            oneRegistered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(oneRegistered);
    REQUIRE_FALSE(fired);

    // THE reviewer's probe: cancelImport() TWICE, back to back, with NO
    // pumpMain() call between them (or after) -- the second call's own
    // reapFinishedAsyncJobs() is where the pre-fix bug erased the
    // still-pending job from asyncJobs_. Also proves cancelImport() is
    // safely idempotent for an already-cancelled, not-yet-drained job
    // (registry.h's own "documented no-op" contract for an
    // already-cancelled job, exercised here for the FIRST time against a
    // job still holding real GPU resources).
    registry.cancelImport(handle);
    registry.cancelImport(handle);

    // Still no pumpMain() call anywhere -- Registry, then Scheduler,
    // destruct with the job exactly as double-cancellation left it.
    registryOpt.reset();
    scheduler.reset();

    fixture->deletionQueue->onFrameFenceSignaled(0);
    CHECK_FALSE(fired);

    // Same root-cause-accurate check as the single-cancel regression test
    // above (see that test's own comment for why `fixture` must stay
    // alive to observe this via hasValidationErrors() instead of only
    // "did it crash"): force the exact corrupting sequence explicitly.
    // Before this round's fix: the second cancelImport() call's own reap
    // already erased the job, so ~Registry() drained nothing, and
    // `textures.reset()` here destroys a TextureCache that still holds
    // the abandoned texture as a live, never-released entry -- reproduces
    // CI's exact VUID-vkEndCommandBuffer-commandBuffer-00059 validation
    // error deterministically (confirmed via this fix's own revert-test:
    // 3/3 runs with reapFinishedAsyncJobs()'s pending-aware gate reverted).
    // After this round's fix: the second cancelImport() call's reap
    // leaves the job tracked (pending != nullptr), so ~Registry() still
    // finds and drains it -- nothing left to corrupt here.
    fixture->textures.reset();
    fixture->uploader.flush();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}
