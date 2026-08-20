#include <rx_asset/registry.h>
#include "fallbacks.h"
#include "import_gltf.h"
#include "import_pipeline.h"
#include <rx_core/debug_checks.h>
#include <rx_core/log.h>
#include <rx_task/scheduler.h>
#include <chrono>
#include <fstream>
#include <thread>
#include <utility>

namespace rx::asset {

namespace {

uint64_t packHandle(uint32_t index, uint32_t generation) { return (static_cast<uint64_t>(index) << 32) | generation; }

template <typename Tag>
uint64_t packHandle(rx::core::Handle<Tag> handle) {
    return packHandle(handle.index(), handle.generation());
}

}  // namespace

// ---------------------------------------------------------------------
// [Phase 4 Stage 1 Task 15] Async import job -- see registry.h's own
// "Async import" section for the full contract this implements.
// ---------------------------------------------------------------------

struct AsyncImportJob {
    // Config, set once at creation (importGltfAsync()'s own call, on the
    // main thread) and read-only from every thread afterward -- no
    // synchronization needed for these specifically (every WRITE happens
    // strictly before any closure that could read them is even posted).
    Registry* registry = nullptr;
    GeometryPool* pool = nullptr;
    TextureCache* textures = nullptr;
    rx::task::Scheduler* scheduler = nullptr;
    ImportCompletionFn onComplete;
    TextureHandle fallbackTextureHandleForUncached;
    // [Task 15 fix] Snapshotted ONCE, on the main thread, at
    // importGltfAsync()'s own call -- see TextureFallbackHandles' own
    // comment (import_pipeline.h) for why computeGltfImport() cannot call
    // TextureCache::checkerboardHandle()/fallbackHandle() itself (both are
    // RX_ASSERT_MAIN_THREAD-guarded; this job's compute phase runs on a
    // worker thread).
    TextureFallbackHandles textureFallbacks;

    // Byte-source config: EITHER the byte-span overload's caller-owned
    // span+ByteSource& (documented long-lived-caller-owned contract,
    // registry.h), OR the path overload's own owned buffer +
    // FilesystemByteSource (set on the IO thread, before computeGltfImport()
    // ever runs -- see importGltfAsync(path, ...) below).
    std::span<const std::byte> documentBytes;
    ByteSource* source = nullptr;
    std::vector<std::byte> ownedDocumentBytes;
    std::optional<FilesystemByteSource> ownedSource;

    // Cross-thread state -- written from IO/worker/main threads, polled
    // from main (importProgress()/cancelImport()).
    std::atomic<bool> cancelled{false};
    std::atomic<uint32_t> itemsCompleted{0};
    std::atomic<uint32_t> itemsTotal{0};
    std::atomic<ImportStage> stage{ImportStage::Reading};
    std::atomic<bool> completionFired{false};

    // Main-thread-only (only ever touched from postToMain()-drained
    // closures, all of which run on the same thread as importGltfAsync()
    // itself per this class's own D5 contract).
    std::unique_ptr<MarshalPendingImport> pending;
};

namespace {

// [Task 15] Runs on the main thread (a postToMain()-drained closure) --
// exactly-once completion delivery (a CAS guard, not merely a bool check:
// two closures racing to fire the SAME job's completion is not possible
// under this design (every step is strictly sequenced through postToMain's
// own FIFO-per-caller-thread + this Registry's single main thread), but
// the guard is cheap and makes the "exactly once" contract robust to any
// future change in that sequencing rather than merely true today).
void fireAsyncImportCompletion(const std::shared_ptr<AsyncImportJob>& job, ImportResult result) {
    bool expected = false;
    if (!job->completionFired.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (job->onComplete) {
        job->onComplete(std::move(result));
    }
}

// [Task 15, RC6 time-sliced marshalling] Runs ONE bounded unit of prepare
// work (marshalGltfImportPrepareStep()'s own contract: at most one real
// texture registration, or the one geometry upload) then either re-posts
// itself via postToMain() for the NEXT pumpMain() tick (more work left --
// this is what keeps a large texture batch from blowing the wall-clock
// budget in a single call, the exact regression this task's own
// wall-clock-gate test caught empirically) or hands off to
// pollAsyncImportUploads() once fully prepared.
void runAsyncImportPrepareStep(std::shared_ptr<AsyncImportJob> job);

void pollAsyncImportUploads(std::shared_ptr<AsyncImportJob> job);

// [Fix round 1] Abandon-path rollback, gated on outstanding upload
// tickets actually completing first -- NOT a direct call to
// marshalGltfImportRollback(). Found load-bearing by this task's own new
// cancel-mid-upload rollback test (async_import_test.cpp), which
// reproduced a genuine Vulkan validation error ("vkDestroyImage ... in
// use by a command buffer") the first time it exercised cancellation
// AFTER prepare() had already issued real upload tickets: freeing
// GeometryPool's suballocation makes that memory range immediately
// reusable by the very next upload (a write-after-write hazard against
// the still-in-flight old copy), and releasing a TextureCache handle
// whose DeletionQueue-tagged reclaim later runs risks destroying a
// VkImage a still-in-flight transfer command buffer is writing into --
// in BOTH cases, only once marshalGltfImportUploadsComplete() confirms
// every ticket prepare() actually issued has genuinely finished on the
// GPU is it safe to free/release. Never a blocking wait (D25's own "poll,
// never a blocking wait" invariant, unchanged even on this abandon path)
// -- re-posts itself via postToMain() exactly like the non-cancelled poll
// loop below, until safe.
//
// [Fix round 2, ITEM 3] marshalGltfImportEnsureRollbackTicketed() runs
// FIRST, every call (idempotent) -- closes the one gap
// marshalGltfImportUploadsComplete() cannot see on its own: a texture
// slot registered but not yet ticketed (registerDecoded() already ran,
// but marshalGltfImportPrepareStep() yielded before this batch's own
// flushPendingUploads() call). Without this, marshalGltfImportUploadsComplete()
// would report "nothing outstanding" for that slot (no ticket to poll)
// and let marshalGltfImportRollback() release a handle whose copy
// command has been recorded but never even submitted -- see that
// function's own comment (import_pipeline.h) for the full mechanism and
// task-15-report.md's round-2 section for the empirical reproduction
// this closes.
void rollbackAsyncImportWhenSafe(std::shared_ptr<AsyncImportJob> job) {
    if (job->pending) {
        marshalGltfImportEnsureRollbackTicketed(job->textures, *job->pending);
        if (!marshalGltfImportUploadsComplete(*job->pool, job->textures, *job->pending)) {
            rx::task::Scheduler* scheduler = job->scheduler;
            scheduler->postToMain([job = std::move(job)]() mutable { rollbackAsyncImportWhenSafe(std::move(job)); });
            return;
        }
    }
    marshalGltfImportRollback(*job->pool, job->textures, std::move(job->pending));
}

void runAsyncImportPrepareStep(std::shared_ptr<AsyncImportJob> job) {
    if (job->cancelled.load(std::memory_order_acquire) || job->registry == nullptr) {
        rollbackAsyncImportWhenSafe(std::move(job));
        return;
    }
    const bool more = marshalGltfImportPrepareStep(*job->pool, job->textures, *job->pending);
    rx::task::Scheduler* scheduler = job->scheduler;
    if (more) {
        scheduler->postToMain([job = std::move(job)]() mutable { runAsyncImportPrepareStep(std::move(job)); });
        return;
    }
    // [Fix round 1] Deliberately a SEPARATE postToMain() tick rather than
    // an inline call: keeps every pumpMain() call doing exactly one
    // bounded unit of work (prepare-step OR poll-check, never both in the
    // same call -- the same "never batch two units into one call" posture
    // the RC6 wall-clock budget already requires elsewhere), and gives a
    // deterministic, non-racy window (one pumpMain() tick wide) between
    // "every GPU resource this import needs is now registered" and "the
    // first upload-ticket completion check runs" -- load-bearing for this
    // task's own cancel-mid-upload rollback test (async_import_test.cpp),
    // which needs to observe "fully prepared" and call cancelImport()
    // before any poll/finalize has a chance to run.
    scheduler->postToMain([job = std::move(job)]() mutable { pollAsyncImportUploads(std::move(job)); });
}

// [Task 15] Poll step -- re-posts itself via postToMain() until every
// upload ticket marshalGltfImportPrepareStep() issued is complete, never
// blocking (D25's own "poll, never a blocking wait" invariant;
// isUploadComplete() is a pure semaphore-counter poll). Runs on the main
// thread throughout.
void pollAsyncImportUploads(std::shared_ptr<AsyncImportJob> job) {
    if (job->cancelled.load(std::memory_order_acquire) || job->registry == nullptr) {
        // Abandon semantics: no registry mutation, no callback -- release
        // whatever prepare() already registered GPU-side, once it is
        // actually safe to (see rollbackAsyncImportWhenSafe()'s own
        // comment above).
        rollbackAsyncImportWhenSafe(std::move(job));
        return;
    }
    if (!marshalGltfImportUploadsComplete(*job->pool, job->textures, *job->pending)) {
        rx::task::Scheduler* scheduler = job->scheduler;
        scheduler->postToMain([job = std::move(job)]() mutable { pollAsyncImportUploads(std::move(job)); });
        return;
    }

    ImportResult result = marshalGltfImportFinalize(*job->registry, std::move(job->pending));
    job->stage.store(result.ok() ? ImportStage::Done : ImportStage::Failed, std::memory_order_release);
    fireAsyncImportCompletion(job, std::move(result));
}

// [Task 15] Runs on the main thread (postToMain(), posted from the worker
// task that ran computeGltfImport()) -- the compute-to-marshal handoff.
void finishAsyncImportCompute(std::shared_ptr<AsyncImportJob> job, ImportComputeResult compute) {
    RX_ASSERT_MAIN_THREAD("Registry::importGltfAsync (marshal)");

    if (job->cancelled.load(std::memory_order_acquire) || job->registry == nullptr) {
        return;  // abandon semantics -- compute-only data, nothing GPU-side to roll back yet.
    }
    if (compute.error != ImportError::None) {
        job->stage.store(ImportStage::Failed, std::memory_order_release);
        ImportResult result;
        result.error = compute.error;
        fireAsyncImportCompletion(job, std::move(result));
        return;
    }

    job->stage.store(ImportStage::Uploading, std::memory_order_release);
    job->pending = marshalGltfImportBeginDeferred(std::move(compute));
    runAsyncImportPrepareStep(std::move(job));
}

// [Task 15] Runs on a background WORKER thread (Scheduler::runOnWorkerThread())
// -- never the dedicated IO thread, never main (see scheduler.h's own
// runOnWorkerThread() doc comment for exactly why this is legal AND why it
// is the only Scheduler primitive that satisfies both constraints at
// once). Every byte-source read this triggers (buffer/image resolution)
// therefore also runs on THIS thread, not the IO thread -- a documented
// scope simplification, see import_gltf.cpp's own ResolvedBuffers comment
// for the full rationale.
void runAsyncImportComputePhase(std::shared_ptr<AsyncImportJob> job) {
    if (job->cancelled.load(std::memory_order_acquire)) {
        return;
    }
    ImportComputeResult compute =
        computeGltfImport(job->documentBytes, *job->source, *job->scheduler, job->textures,
                           job->fallbackTextureHandleForUncached, job->textureFallbacks, &job->cancelled,
                           &job->itemsCompleted, &job->itemsTotal, &job->stage);
    rx::task::Scheduler* scheduler = job->scheduler;
    scheduler->postToMain(
        [job = std::move(job), compute = std::move(compute)]() mutable { finishAsyncImportCompute(std::move(job), std::move(compute)); });
}

void startAsyncImportComputePhase(std::shared_ptr<AsyncImportJob> job) {
    if (job->cancelled.load(std::memory_order_acquire)) {
        return;
    }
    rx::task::Scheduler* scheduler = job->scheduler;
    scheduler->runOnWorkerThread([job = std::move(job)]() mutable { runAsyncImportComputePhase(std::move(job)); });
}

// [Wine/CI SIGSEGV fix] Synchronous, teardown-time counterpart to
// rollbackAsyncImportWhenSafe() above -- closes the one abandonment path
// that function's own postToMain()-repost design cannot cover: nobody is
// guaranteed to ever call pumpMain() again once a job has entered the
// marshal/upload phase (>=1 real GPU resource already registered via
// TextureCache::registerDecoded()/GeometryPool::uploadDeferred()) and its
// owning Registry is being destroyed. registry.h's own "POOL/TEXTURES/
// SCHEDULER LIFETIME" contract already REQUIRES `job->pool`/`job->textures`
// to outlive the job (hence outlive Registry, which may still hold a live
// job referencing them at its own destruction), so it is always safe for
// ~Registry() to use them directly here -- this function does not depend
// on anything ~Registry() was not already contractually guaranteed.
//
// Root cause this closes: if a job's `pending` (MarshalPendingImport) is
// destroyed by ordinary C++ member-wise destruction -- e.g. because the
// shared_ptr<AsyncImportJob> keeping it alive was captured inside a
// postToMain()/runOnWorkerThread() closure that Scheduler::~Scheduler()
// later drops unexecuted (scheduler.h's own documented "postToMain() work
// still queued when pumpMain() is never called again is simply dropped"
// behavior) -- NOTHING ever calls TextureCache::releaseUnpublished()/
// GeometryPool::free() for whatever it already registered. The real
// VkImage/VkBuffer stays "resident" inside TextureCache's/GeometryPool's
// own bookkeeping, with its upload copy command possibly still sitting
// RECORDED-but-not-yet-submitted (or submitted-but-not-yet-complete) on
// Uploader's own batched command buffer. Whenever that texture/geometry
// range is LATER destroyed by ordinary object teardown (TextureCache's own
// pool_ member destructing, GeometryPool's blocks destructing) -- which,
// in the exact fixture/member order this project's own async tests use,
// happens BEFORE Uploader's destructor gets a chance to end/submit that
// still-recording command buffer -- Uploader::~Uploader()'s own auto-flush
// (upload.cpp) calls vkEndCommandBuffer() on a command buffer that still
// references an already-destroyed VkImage: a real
// VUID-vkEndCommandBuffer-commandBuffer-00059 validation error, observed
// (CI run 32180630087, both the linux-native and windows-cross-zig/Wine
// jobs) immediately followed by SIGSEGV as the driver processes the
// corrupted command buffer state. This is a genuine PRODUCTION-code gap in
// the async import pipeline's exception/lifetime safety, not a test-only
// artifact: any host that tears down its Registry (for any reason -- a
// thrown exception, an early return, an unrelated fatal error) while an
// async import is genuinely mid-upload, without first explicitly calling
// cancelImport() and then continuing to pumpMain() until the rollback is
// confirmed safe, hits this same corruption. The two existing "destroy
// while in flight" regression tests (async_import_test.cpp) never
// exercised this because both use `textures == nullptr` -- their jobs
// never leave the compute phase, so they never have anything registered
// to roll back in the first place.
//
// Fix: block here (a destructor/teardown-only exception to D25's "poll,
// never block" invariant on the LIVE async path -- the exact same
// trade-off Device::~Device()'s own unconditional vkDeviceWaitIdle()
// already makes, and the one buildFallbackTextures() already makes via its
// own uploader_.wait(uploader_.flush()) call for this identical failure
// signature, texture_cache.cpp) until every upload ticket this job's
// `pending` issued is confirmed complete, THEN run the exact same
// marshalGltfImportRollback() the live cancellation path uses. Bounded: if
// the GPU genuinely never completes (a hang/device-loss class of failure,
// not anything this fix can control), give up after a generous timeout and
// deliberately LEAK `pending` rather than risk destroying a VkImage/
// VkBuffer a still-incomplete command buffer might reference -- a resource
// leak is always recoverable at process exit; a use-after-free is not.
void drainAndRollbackAbandonedAsyncJob(const std::shared_ptr<AsyncImportJob>& job) {
    if (!job->pending) {
        return;  // still in the compute phase (or already finalized/rolled back) -- nothing GPU-side to release.
    }
    marshalGltfImportEnsureRollbackTicketed(job->textures, *job->pending);

    constexpr auto kAbandonDrainTimeout = std::chrono::seconds(5);
    const auto deadline = std::chrono::steady_clock::now() + kAbandonDrainTimeout;
    bool safe = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (marshalGltfImportUploadsComplete(*job->pool, job->textures, *job->pending)) {
            safe = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!safe) {
        RX_LOG_ERROR(
            "rx_asset: Registry::~Registry(): an abandoned async import's upload ticket(s) did not complete within "
            "{}s at teardown -- leaking its GPU resources rather than risk destroying a still-in-flight VkImage/"
            "VkBuffer (a genuine GPU hang/device-loss condition, not something this rollback can safely resolve)",
            kAbandonDrainTimeout.count());
        return;
    }
    marshalGltfImportRollback(*job->pool, job->textures, std::move(job->pending));
}

}  // namespace

Registry::Registry() {
    fallbackMesh_ = meshes_.acquire(makeFallbackMeshAsset());
    fallbackMaterial_ = materials_.acquire(makeFallbackMaterialAsset());
    fallbackTexture_ = textures_.acquire(makeFallbackTextureAsset());
}

Registry::~Registry() {
    // [Wine/CI SIGSEGV fix round 2] Every other mutating entry point on
    // this class carries this guard (importGltfAsync()/cancelImport()/
    // importProgress() below); this destructor now also does blocking,
    // cross-thread-sensitive teardown work (drainAndRollbackAbandonedAsyncJob()
    // polls real GPU-fence state), so it gets the same guard for the same
    // reason -- Registry's own single-main-thread contract applies to its
    // destructor exactly as much as to every other member function.
    RX_ASSERT_MAIN_THREAD("Registry::~Registry");

    // [Phase 4 Stage 1 Task 15] Teardown-with-import-in-flight: defined
    // behavior. Every outstanding job is cancelled HERE (synchronously,
    // on this Registry's own main thread -- matching every other
    // Registry-mutating call's thread-affinity) so any closure that runs
    // LATER (a worker/IO task already in flight, or an already-queued
    // postToMain() closure this destructor cannot un-post) observes
    // `cancelled == true` at its very first check and never touches this
    // Registry again -- `registry = nullptr` is defense-in-depth on top
    // of that, not the primary mechanism (see AsyncImportJob's own
    // pollAsyncImportUploads()/finishAsyncImportCompute() call sites,
    // which check cancelled first, before ever dereferencing `registry`).
    // No callback fires for any of these jobs (the SAME single documented
    // outcome as an explicit cancelImport() call -- registry.h's own
    // class comment). Each job's own shared_ptr keeps it alive for as
    // long as any in-flight closure still references it, independent of
    // this Registry's own destruction completing first.
    //
    // [Wine/CI SIGSEGV fix] drainAndRollbackAbandonedAsyncJob() (above)
    // additionally, and synchronously, releases any REAL GPU resource a
    // job already registered (>=1 texture/geometry range, i.e. it has
    // reached the marshal/upload phase) BEFORE this loop returns --
    // closing the gap where cancelling alone left that job's `pending`
    // to be destroyed later by ordinary C++ teardown (no `pumpMain()`
    // call ever guaranteed to run again once this Registry is gone),
    // which never releases GPU-owned handles through the safe,
    // ticket-aware path and corrupts Vulkan object lifetime (see that
    // function's own comment for the full mechanism and the CI evidence
    // this closes). A job still purely in the compute phase has
    // `pending == nullptr` here and this call is a no-op for it, exactly
    // as before this fix.
    for (auto& [id, job] : asyncJobs_) {
        job->cancelled.store(true, std::memory_order_release);
        drainAndRollbackAbandonedAsyncJob(job);
        job->registry = nullptr;
    }
    asyncJobs_.clear();
}

void Registry::reapFinishedAsyncJobs() {
    // [Wine/CI SIGSEGV fix round 2, CRITICAL -- reviewer-found, empirically
    // reproduced 3/3] A cancelled job is only ACTUALLY terminal once its
    // `pending` (if any) has been released through the safe, ticket-aware
    // path (rollbackAsyncImportWhenSafe() during normal pumpMain()-driven
    // operation, or drainAndRollbackAbandonedAsyncJob() at ~Registry()
    // time). Reaping the instant `cancelled` flips true, regardless of
    // `pending`, let a job that had already reached the marshal/upload
    // phase (>=1 real GPU resource registered) become INVISIBLE to
    // ~Registry()'s own drain loop -- which only ever walks asyncJobs_ --
    // the moment a LATER importGltfAsync()/cancelImport() call's own
    // reapFinishedAsyncJobs() ran before anything ever pumped it. Concretely:
    // `cancelImport(h); cancelImport(h); ~Registry();` with no pumpMain()
    // call between any of those three -- the SECOND cancelImport() call's
    // own reap (at its top, per this method's own call sites) erases the
    // still-`pending` job from the map right then, so ~Registry()'s drain
    // loop never sees it again. This reproduces the EXACT same corrupted-
    // command-buffer SIGSEGV drainAndRollbackAbandonedAsyncJob() (registry.cpp,
    // above) exists to close, just via a different path INTO the same
    // underlying gap -- a realistic pattern (cancel-everything loops,
    // cancel-then-replace) this task's own original fix did not cover
    // despite its commit message's claim to. Fix: gate the cancelled case
    // on `pending == nullptr` too. This does NOT change when a job
    // actually gets drained (that remains pumpMain()'s/
    // rollbackAsyncImportWhenSafe()'s/drainAndRollbackAbandonedAsyncJob()'s
    // job, entirely unchanged by this) -- it only stops this method from
    // LYING about a still-undrained job's liveness. The Done/Failed cases
    // are unaffected in practice: both are only ever set AFTER `pending`
    // is already moved-from (see pollAsyncImportUploads()'s own ordering,
    // and finishAsyncImportCompute()'s error branch, which sets Failed
    // while `pending` was never assigned in the first place) -- `pending
    // == nullptr` already holds whenever stage is Done or Failed, so this
    // tightens ONLY the cancelled-without-pending-cleared case.
    for (auto it = asyncJobs_.begin(); it != asyncJobs_.end();) {
        const ImportStage stage = it->second->stage.load(std::memory_order_acquire);
        const bool cancelled = it->second->cancelled.load(std::memory_order_acquire);
        const bool cancelledAndDrained = cancelled && it->second->pending == nullptr;
        const bool terminal = stage == ImportStage::Done || stage == ImportStage::Failed || cancelledAndDrained;
        if (terminal) {
            it = asyncJobs_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t Registry::asyncImportJobCountForTesting() const { return asyncJobs_.size(); }

AsyncImportHandle Registry::importGltfAsync(std::span<const std::byte> documentBytes, ByteSource& source, GeometryPool& pool,
                                             rx::task::Scheduler& scheduler, TextureCache* textures,
                                             ImportCompletionFn onComplete) {
    RX_ASSERT_MAIN_THREAD("Registry::importGltfAsync");
    reapFinishedAsyncJobs();

    auto job = std::make_shared<AsyncImportJob>();
    job->registry = this;
    job->pool = &pool;
    job->textures = textures;
    job->scheduler = &scheduler;
    job->onComplete = std::move(onComplete);
    job->fallbackTextureHandleForUncached = fallbackTexture_;
    job->textureFallbacks = snapshotTextureFallbackHandles(textures);
    job->documentBytes = documentBytes;
    job->source = &source;

    const uint64_t id = nextAsyncImportId_++;
    AsyncImportHandle handle(id);
    asyncJobs_.emplace(id, job);

    // No IO-thread hop needed here -- `documentBytes` is already in
    // memory (the caller's own responsibility per this overload's
    // documented byte-source lifetime contract, registry.h) -- straight
    // to the compute phase on a worker thread.
    startAsyncImportComputePhase(std::move(job));
    return handle;
}

AsyncImportHandle Registry::importGltfAsync(const std::filesystem::path& path, GeometryPool& pool, rx::task::Scheduler& scheduler,
                                             TextureCache* textures, ImportCompletionFn onComplete) {
    RX_ASSERT_MAIN_THREAD("Registry::importGltfAsync");
    reapFinishedAsyncJobs();

    auto job = std::make_shared<AsyncImportJob>();
    job->registry = this;
    job->pool = &pool;
    job->textures = textures;
    job->scheduler = &scheduler;
    job->onComplete = std::move(onComplete);
    job->fallbackTextureHandleForUncached = fallbackTexture_;
    job->textureFallbacks = snapshotTextureFallbackHandles(textures);

    const uint64_t id = nextAsyncImportId_++;
    AsyncImportHandle handle(id);
    asyncJobs_.emplace(id, job);

    const std::filesystem::path parentDir = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    rx::task::Scheduler* schedulerPtr = &scheduler;

    // The one REAL dedicated-IO-thread byte-source read every async
    // import needs before computeGltfImport() can run at all: the main
    // document file itself (mirrors importGltf(path, ...)'s own
    // synchronous std::ifstream read, byte-for-byte, just off the main
    // thread).
    schedulerPtr->runOnIoThread([job, path, parentDir, schedulerPtr]() mutable {
        if (job->cancelled.load(std::memory_order_acquire)) {
            return;
        }

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            RX_LOG_ERROR("rx_asset: Registry::importGltfAsync: failed to open '{}'", path.string());
            schedulerPtr->postToMain([job]() mutable {
                ImportResult result;
                result.error = ImportError::ByteSourceUnavailable;
                job->stage.store(ImportStage::Failed, std::memory_order_release);
                fireAsyncImportCompletion(job, std::move(result));
            });
            return;
        }
        const auto size = file.tellg();
        if (size < 0) {
            RX_LOG_ERROR("rx_asset: Registry::importGltfAsync: failed to determine size of '{}'", path.string());
            schedulerPtr->postToMain([job]() mutable {
                ImportResult result;
                result.error = ImportError::ByteSourceUnavailable;
                job->stage.store(ImportStage::Failed, std::memory_order_release);
                fireAsyncImportCompletion(job, std::move(result));
            });
            return;
        }
        file.seekg(0);

        job->ownedDocumentBytes.resize(static_cast<size_t>(size));
        if (!job->ownedDocumentBytes.empty() &&
            !file.read(reinterpret_cast<char*>(job->ownedDocumentBytes.data()),
                       static_cast<std::streamsize>(job->ownedDocumentBytes.size()))) {
            RX_LOG_ERROR("rx_asset: Registry::importGltfAsync: failed to read '{}'", path.string());
            schedulerPtr->postToMain([job]() mutable {
                ImportResult result;
                result.error = ImportError::ByteSourceUnavailable;
                job->stage.store(ImportStage::Failed, std::memory_order_release);
                fireAsyncImportCompletion(job, std::move(result));
            });
            return;
        }

        job->ownedSource.emplace(parentDir);
        job->documentBytes = std::span<const std::byte>(job->ownedDocumentBytes);
        job->source = &*job->ownedSource;

        if (job->cancelled.load(std::memory_order_acquire)) {
            return;
        }
        startAsyncImportComputePhase(std::move(job));
    });

    return handle;
}

void Registry::cancelImport(AsyncImportHandle handle) {
    RX_ASSERT_MAIN_THREAD("Registry::cancelImport");
    reapFinishedAsyncJobs();
    auto it = asyncJobs_.find(handle.id_);
    if (it == asyncJobs_.end()) {
        return;  // unknown/already-reclaimed handle -- documented no-op
    }
    it->second->cancelled.store(true, std::memory_order_release);
}

ImportProgress Registry::importProgress(AsyncImportHandle handle) const {
    RX_ASSERT_MAIN_THREAD("Registry::importProgress");
    auto it = asyncJobs_.find(handle.id_);
    if (it == asyncJobs_.end()) {
        ImportProgress terminal;
        terminal.stage = ImportStage::Done;
        return terminal;
    }
    const AsyncImportJob& job = *it->second;
    ImportProgress progress;
    progress.stage = job.stage.load(std::memory_order_acquire);
    progress.itemsCompleted = job.itemsCompleted.load(std::memory_order_acquire);
    progress.itemsTotal = job.itemsTotal.load(std::memory_order_acquire);
    progress.cancelled = job.cancelled.load(std::memory_order_acquire);
    return progress;
}

ImportResult Registry::importGltf(std::span<const std::byte> documentBytes, ByteSource& source, GeometryPool& pool,
                                   rx::task::Scheduler& scheduler, TextureCache* textures) {
    // [Phase 4 exit fix wave, in-round pre-existing-defect closure] Was
    // documented main-thread-only (registry.h's own top comment) but never
    // actually enforced anywhere in this call's chain
    // (importGltfPipeline() -> computeGltfImport()/marshalGltfImportSync()
    // -> registerMesh()/registerMaterial(), none guarded) -- discovered
    // while writing I4's threading.md text, closed here per standing
    // no-deferred-fixes policy (no prerequisite blocks it). Matches every
    // other main-thread-only Registry mutator's guard convention.
    RX_ASSERT_MAIN_THREAD("Registry::importGltf");
    return importGltfPipeline(*this, documentBytes, source, pool, scheduler, textures);
}

ImportResult Registry::importGltf(const std::filesystem::path& path, GeometryPool& pool, rx::task::Scheduler& scheduler,
                                   TextureCache* textures) {
    // [Phase 4 exit fix wave] See the byte-span overload's own comment
    // above -- this overload also reaches it (below), so this guard is
    // technically redundant with that one, but it fires here FIRST (before
    // this overload's own direct filesystem read), matching this
    // codebase's own established "guard every public entry point, even
    // when an inner call is separately guarded too" convention (e.g.
    // MaterialSystem::bindInstance() and the getPipeline() it calls
    // internally both carry their own guard).
    RX_ASSERT_MAIN_THREAD("Registry::importGltf");
    // The one sanctioned direct-filesystem read in this whole library
    // [byte_source.h's own top comment]: reading the MAIN document
    // itself for this convenience overload. Every OTHER byte (every
    // external buffer/image URI the document goes on to reference)
    // still resolves exclusively through the FilesystemByteSource this
    // wraps below.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        RX_LOG_ERROR("rx_asset: Registry::importGltf: failed to open '{}'", path.string());
        ImportResult result;
        result.error = ImportError::ByteSourceUnavailable;
        return result;
    }

    const auto size = file.tellg();
    if (size < 0) {
        RX_LOG_ERROR("rx_asset: Registry::importGltf: failed to determine size of '{}'", path.string());
        ImportResult result;
        result.error = ImportError::ByteSourceUnavailable;
        return result;
    }
    file.seekg(0);

    std::vector<std::byte> bytes(static_cast<size_t>(size));
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        RX_LOG_ERROR("rx_asset: Registry::importGltf: failed to read '{}'", path.string());
        ImportResult result;
        result.error = ImportError::ByteSourceUnavailable;
        return result;
    }

    FilesystemByteSource fsSource(path.has_parent_path() ? path.parent_path() : std::filesystem::path("."));
    return importGltf(bytes, fsSource, pool, scheduler, textures);
}

const MeshAsset& Registry::mesh(MeshHandle handle) const {
    // [Phase 4 exit fix wave, M2] Task 12's review ruling narrowed
    // GeometryPool's read accessors to guarded main-thread-only precisely
    // because unguarded reads against unlocked mutable state invite silent
    // misuse -- Registry's identical-posture reads (this class holds no
    // internal lock either, registry.h's own top comment) never got the
    // same treatment until now.
    RX_ASSERT_MAIN_THREAD("Registry::mesh");
    if (nonresidentMesh_.find(packHandle(handle)) == nonresidentMesh_.end()) {
        if (const MeshAsset* asset = meshes_.get(handle)) {
            return *asset;
        }
    }
    const MeshAsset* fallback = meshes_.get(fallbackMesh_);
    return *fallback;  // always resident -- created in the constructor, never evicted/released
}

const MaterialAsset& Registry::material(MaterialHandle handle) const {
    // [Phase 4 exit fix wave, M2] See mesh()'s own comment above.
    RX_ASSERT_MAIN_THREAD("Registry::material");
    if (nonresidentMaterial_.find(packHandle(handle)) == nonresidentMaterial_.end()) {
        if (const MaterialAsset* asset = materials_.get(handle)) {
            return *asset;
        }
    }
    const MaterialAsset* fallback = materials_.get(fallbackMaterial_);
    return *fallback;
}

const TextureAsset& Registry::texture(TextureHandle handle) const {
    // [Phase 4 exit fix wave, M2] See mesh()'s own comment above.
    RX_ASSERT_MAIN_THREAD("Registry::texture");
    if (const TextureAsset* asset = textures_.get(handle)) {
        return *asset;
    }
    const TextureAsset* fallback = textures_.get(fallbackTexture_);
    return *fallback;
}

void Registry::evictForTesting(MeshHandle handle) {
    // [Phase 4 exit fix wave, in-round pre-existing-defect closure] Same
    // rationale as importGltf() above -- documented main-thread-only,
    // previously unguarded.
    RX_ASSERT_MAIN_THREAD("Registry::evictForTesting");
    nonresidentMesh_.insert(packHandle(handle));
}

void Registry::evictForTesting(MaterialHandle handle) {
    // [Phase 4 exit fix wave] See the MeshHandle overload's own comment
    // above.
    RX_ASSERT_MAIN_THREAD("Registry::evictForTesting");
    nonresidentMaterial_.insert(packHandle(handle));
}

}  // namespace rx::asset
