#pragma once

// rx_asset/registry.h -- Thread-affinity (D5): Registry MUTATION
// (importGltf(), evictForTesting()) is main-thread-only, matching every
// other GPU-object-mutation entry point in this codebase (BindlessTable/
// Uploader/DeletionQueue/GeometryPool -- docs/threading.md). The read
// accessors (mesh()/material()/texture()) are declared const and are
// safe to call from the main thread at any point after a prior
// importGltf() call has returned; they are NOT safe to call concurrently
// with a mutating call on another thread (this class holds no internal
// lock -- same posture as GeometryPool, D5's whole point being that
// GPU-object-owning types stay single-threaded rather than growing
// locks).
//
// D24 EVICTION INVARIANT: mesh()/material() are RESIDENCY-TOLERANT --
// a handle that is still "live" (passes the generational check) but has
// been marked nonresident (evictForTesting(), the one deferred-eviction
// path this task exercises; Phase 4 has no automatic eviction POLICY,
// only this invariant) resolves to the registry's own D11 fallback
// asset, never a null dereference and never an assert. No public method
// anywhere on this class returns a raw pointer or index that could
// outlive a later mutation -- every return is a value handle or a
// const reference whose documented lifetime is "until the next
// mutating call on this Registry".

#include <rx_asset/byte_source.h>
#include <rx_asset/import_error.h>
#include <rx_asset/mesh_asset.h>
#include <rx_core/handle.h>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace rx::task {
class Scheduler;
}  // namespace rx::task

namespace rx::asset {

class GeometryPool;
// [Phase 4 Stage 1 Task 15] Forward-declared only -- full definitions live
// in the PRIVATE import_pipeline.h (import_gltf.cpp's own compute/marshal
// split), never included from this public header. See the friend
// declarations below for the only two places this class needs them.
struct ImportComputeResult;
struct MarshalPendingImport;

// [Phase 4 Stage 1 Task 15] Forward-declared only, defined (at this SAME
// namespace scope, deliberately never nested inside Registry) in
// registry.cpp -- an async import job's cross-thread bookkeeping. Kept
// namespace-scope rather than a private nested type so registry.cpp's own
// free-function async orchestration helpers (which are not, and must not
// need to be, Registry members or friends) can name
// `std::shared_ptr<AsyncImportJob>` in their own signatures without
// widening Registry's friend surface any further than the two
// marshalGltfImport*() functions above already do.
struct AsyncImportJob;

// Forward-declared only -- Task 14's deliverable. Registry::importGltf()
// accepts a pointer per the plan's own interface note ("Stage-1 Task 14
// adds"); Task 13 never dereferences a non-null one (texture payloads
// are preserved in the material parameter set for Task 14 to resolve,
// per D7/matrix-issue02's texture rows -- see import_gltf.cpp).
class TextureCache;

struct ImportResult {
    ImportError error = ImportError::None;
    [[nodiscard]] bool ok() const { return error == ImportError::None; }

    ImportedScene scene;                    // flattened instances [D12] + preserved cameras/lights/animations
    std::vector<MeshHandle> meshes;          // newly created handles, in source Mesh-array order
    std::vector<MaterialHandle> materials;   // newly created handles, in source Material-array order

    // Test/diagnostic-only [fix round 1, matrix-issue02 row 12]: how many
    // times THIS import called `GeometryPool::upload()`. GeometryPool's
    // own landed (Task 12) contract is exactly one flush()+wait() per
    // upload() call, so this is a direct, precise proxy for "how many
    // times did sync import block on GPU upload completion" -- a
    // correct one, unlike `Uploader::blockingRingWaitCount()` (which
    // this task's own fix-round testing found measures ring-RECLAMATION
    // blocking only, never incrementing at all for small test payloads
    // regardless of how many separate upload() calls were made -- see
    // the task report's fix-round section). Production code has no need
    // for this field.
    uint32_t poolUploadCallCountForTesting = 0;
};

// ---------------------------------------------------------------------
// [Phase 4 Stage 1 Task 15, spec D5/D18-RC6/D24/D25] Async import --
// importGltfAsync() below shares the EXACT SAME import_pipeline.h compute/
// marshal split importGltf() itself now uses (no forked logic): parse,
// per-primitive processing, and per-material texture decode run on
// rx::task::Scheduler workers (never the dedicated IO thread -- debug-
// asserted, see import_gltf.cpp); GPU uploads and every registry mutation
// are marshalled to the main thread via postToMain()/pumpMain(). Thread-
// affinity (D5): importGltfAsync()/cancelImport()/importProgress() are
// main-thread-only, matching importGltf() itself -- this Registry's "main"
// thread is whichever thread also calls `scheduler.pumpMain()` each frame,
// since that pump loop is what actually advances an in-flight import (no
// separate per-frame call is needed on Registry itself).
//
// BYTE-SOURCE LIFETIME CONTRACT: unlike the synchronous importGltf()
// overload (which only needs `documentBytes`/`source` for the duration of
// one call), the byte-span overload of importGltfAsync() below holds onto
// its `documentBytes`/`source` references for the ENTIRE async job's
// lifetime (until its completion callback fires or it is cancelled) --
// the caller must keep both alive for at least that long. The
// filesystem-path overload has no such contract (it reads the whole file
// into a heap buffer this job owns itself, exactly like importGltf()'s
// own path overload does synchronously).
//
// POOL/TEXTURES/SCHEDULER LIFETIME: `pool`/`textures`/`scheduler` must
// also outlive the async job for the same reason (this Registry has no
// way to freeze or extend anyone else's lifetime) -- destroying any of
// them while an import that references them is in flight is a caller
// lifetime bug, same as the sync overload's own by-reference parameters,
// just stretched over a longer window. Registry ITSELF is the one
// exception with defined teardown behavior (see ~Registry()'s own
// comment): destroying the REGISTRY specifically while one of its own
// async imports is in flight is safe and defined.
//
// CANCELLATION (rulings N1, abandon semantics): cancelImport() sets one
// atomic flag, checked at STAGE boundaries (never mid-parallelFor-chunk --
// an in-flight stage's items always run to completion once started). No
// registry mutation is ever applied for a job once its cancellation is
// observed; any GPU resource already registered (texture/geometry) before
// that point is released (ASan-clean). The completion callback does NOT
// fire for a cancelled job (the chosen single documented outcome, per the
// matrix's "onCancelled path OR status-in-result -- pick one" instruction)
// -- instead, `ImportProgress::cancelled` observes it via polling.
// Cancellation latency is bounded by one stage-item (the in-flight
// parallelFor call, or the in-flight GPU upload ticket). A cancelImport()
// call after the job has already completed (or already been cancelled) is
// a documented no-op.
//
// PROGRESS (rulings N2, minimal): importProgress() returns a poll-able
// snapshot -- stage enum + items-completed/items-total, monotonic within
// one job. Granularity is at least per-primitive and per-texture (the
// worker fan-out's own unit). Internal C++ surface this phase (matrix N2:
// SDK-phase ABI projection is a separate, later question).
// ---------------------------------------------------------------------

enum class ImportStage : uint8_t { Reading, Parsing, Decoding, Optimizing, Uploading, Done, Failed };

struct ImportProgress {
    ImportStage stage = ImportStage::Reading;
    uint32_t itemsCompleted = 0;
    uint32_t itemsTotal = 0;
    bool cancelled = false;
};

// Opaque, comparable, generational-in-spirit handle to one importGltfAsync()
// call -- NOT a rx::core::Handle<Tag> (this is not a registry-owned
// ASSET, it is a transient job identity), just a plain monotonic id
// wrapper with the same "default-constructed == invalid" convention every
// other handle type in this codebase follows.
class AsyncImportHandle {
public:
    AsyncImportHandle() = default;
    bool operator==(const AsyncImportHandle&) const = default;
    [[nodiscard]] bool isValid() const { return id_ != 0; }

private:
    friend class Registry;
    explicit AsyncImportHandle(uint64_t id) : id_(id) {}
    uint64_t id_ = 0;
};

using ImportCompletionFn = std::function<void(ImportResult)>;

class Registry {
public:
    Registry();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) = delete;
    Registry& operator=(Registry&&) = delete;
    ~Registry();

    // Full control [matrix-issue02 row 2/3, the IO-source abstraction
    // invariant]: `documentBytes` is the main glTF/GLB document, already
    // in memory (fastgltf's own GltfDataBuffer::FromBytes consumes it
    // directly -- no filesystem access for the document itself, ever);
    // `source` resolves every external buffer/image URI the document
    // references. Never touches std::filesystem itself (byte_source.h's
    // own comment covers the one exception -- FilesystemByteSource is
    // POLICY the path-taking overload below opts into, not something
    // this overload does on its own).
    //
    // `scheduler`: sync import still parallelizes per-primitive CPU work
    // internally via `scheduler.parallelFor()` [D5/matrix-issue02 row 15
    // -- "parallelism is the engine default, not an async-only
    // property"]. Deviation from the plan's illustrative interface
    // sketch (which shows no Scheduler parameter): necessary, not
    // incidental -- there is no reasonable way to satisfy row 15 without
    // a live Scheduler reference, and spec D2 commits to no engine-wide
    // singleton one could reach for instead. See the task report for the
    // full rationale (mirrors Task 12's own GeometryPool::create()
    // Allocator& addition).
    //
    // Main-thread-only (D5): GPU-object mutation (the final GeometryPool
    // upload) happens on the calling thread; only the CPU-side per-
    // primitive work (parse-independent accessor reads, tangent
    // generation, meshoptimizer) is fanned out to workers.
    [[nodiscard]] ImportResult importGltf(std::span<const std::byte> documentBytes, ByteSource& source, GeometryPool& pool,
                                           rx::task::Scheduler& scheduler, TextureCache* textures = nullptr);

    // Convenience: filesystem-backed [matrix row 2: "a thin wrapper over
    // a filesystem-backed byte source"]. Reads `path` itself (plain
    // std::ifstream -- this ONE call site is where a filesystem read for
    // the main document is allowed to live) and wraps a
    // FilesystemByteSource rooted at `path.parent_path()` for every
    // external reference, then forwards to the overload above. Returns
    // ImportError::ByteSourceUnavailable if `path` cannot be opened at
    // all (D11: fallback + ERROR log naming the path; matrix row 13).
    [[nodiscard]] ImportResult importGltf(const std::filesystem::path& path, GeometryPool& pool, rx::task::Scheduler& scheduler,
                                           TextureCache* textures = nullptr);

    // [D24] Residency-tolerant resolve: yields the D11 fallback asset
    // for a handle that is either dead (never valid, or since-released)
    // or live-but-nonresident (evictForTesting() below). Never null,
    // never asserts.
    [[nodiscard]] const MeshAsset& mesh(MeshHandle handle) const;
    [[nodiscard]] const MaterialAsset& material(MaterialHandle handle) const;
    [[nodiscard]] const TextureAsset& texture(TextureHandle handle) const;

    // [D24] Test/diagnostic-only: marks `handle` nonresident WITHOUT
    // releasing it (it stays "live" per the generational check --
    // dead and nonresident are different states). mesh()/material()
    // resolve to the D11 fallback for a nonresident handle until either
    // a later import recreates one at the same index/generation
    // (impossible -- HandlePool never reissues a still-live handle) or,
    // more realistically, a caller simply starts using the NEW handle a
    // fresh import returns. This exists to exercise the eviction
    // INVARIANT this task commits to; Phase 4 has no automatic eviction
    // POLICY that would call this in production (D24).
    void evictForTesting(MeshHandle handle);
    void evictForTesting(MaterialHandle handle);

    [[nodiscard]] MeshHandle fallbackMeshHandle() const { return fallbackMesh_; }
    [[nodiscard]] MaterialHandle fallbackMaterialHandle() const { return fallbackMaterial_; }
    [[nodiscard]] TextureHandle fallbackTextureHandle() const { return fallbackTexture_; }

    // Test/diagnostic-only: the registry's OWN internal live-entry count
    // (rx::core::HandlePool::liveCount()) -- as opposed to inspecting
    // what a specific importGltf() call happened to report back via
    // ImportResult::meshes/materials. This is what a "zero registry
    // mutation on error" test needs to assert against: an import that
    // silently registered something WITHOUT reporting its handle back
    // would pass a check against ImportResult::meshes.empty() while
    // still failing this one -- see the task report's revert-testing
    // section for the real bug this distinction caught in-task.
    [[nodiscard]] size_t meshCountForTesting() const { return meshes_.liveCount(); }
    [[nodiscard]] size_t materialCountForTesting() const { return materials_.liveCount(); }

    // [Phase 4 Stage 1 Task 15] See this header's own "Async import"
    // section above for the full contract (byte-source/pool/textures/
    // scheduler lifetime, cancellation, progress, teardown). `onComplete`
    // fires on the main thread (thread-id asserted in debug builds),
    // exactly once (a counter/flag-guarded call), strictly after every
    // registry mutation this import makes is visible -- unless the job is
    // cancelled first, in which case it never fires at all.
    //
    // Main-thread-only (D5).
    [[nodiscard]] AsyncImportHandle importGltfAsync(std::span<const std::byte> documentBytes, ByteSource& source,
                                                       GeometryPool& pool, rx::task::Scheduler& scheduler,
                                                       TextureCache* textures, ImportCompletionFn onComplete);

    // Convenience: filesystem-backed, mirroring importGltf()'s own path
    // overload -- the file is read via runOnIoThread() (the one real,
    // dedicated-IO-thread byte-source read every async import needs
    // before computeGltfImport() can run at all). A file that cannot be
    // opened/read completes with ImportError::ByteSourceUnavailable,
    // delivered through `onComplete` exactly like every other import
    // failure (never silently dropped).
    //
    // Main-thread-only (D5).
    [[nodiscard]] AsyncImportHandle importGltfAsync(const std::filesystem::path& path, GeometryPool& pool,
                                                       rx::task::Scheduler& scheduler, TextureCache* textures,
                                                       ImportCompletionFn onComplete);

    // Abandon-style cancel -- see this header's own "Async import" section.
    //
    // Main-thread-only (D5).
    void cancelImport(AsyncImportHandle handle);

    // Poll-able progress snapshot -- see this header's own "Async import"
    // section. An unknown or already-reclaimed handle (a completed job's
    // bookkeeping is reclaimed opportunistically -- see
    // reapFinishedAsyncJobs()'s own comment) reports a TERMINAL-looking
    // snapshot (stage == Done, 0/0 items, cancelled == false) rather than
    // one that could be mistaken for "just starting" -- never asserts,
    // never throws.
    //
    // Main-thread-only (D5).
    [[nodiscard]] ImportProgress importProgress(AsyncImportHandle handle) const;

    // Test/diagnostic-only: how many async jobs this Registry is
    // currently tracking (in flight, or completed-but-not-yet-reclaimed --
    // a finished job's bookkeeping is reclaimed the next time ANY
    // importGltfAsync()/cancelImport()/importProgress() call runs, not
    // synchronously the instant it finishes). Production code has no need
    // for this.
    [[nodiscard]] size_t asyncImportJobCountForTesting() const;

private:
    // import_gltf.cpp's orchestration functions need direct access to
    // registerMesh()/registerMaterial() below (and, transitively, this
    // class's HandlePools) to insert what it parses -- declared as named
    // friends rather than widening any of the methods above to public, so
    // the ONLY way to mutate this registry from outside its own two .cpp
    // files is through the public importGltf()/importGltfAsync()/
    // evictForTesting() surface.
    friend ImportResult importGltfPipeline(Registry& registry, std::span<const std::byte> documentBytes, ByteSource& source,
                                            GeometryPool& pool, rx::task::Scheduler& scheduler, TextureCache* textures);

    // [Phase 4 Stage 1 Task 15] The compute/marshal split's own two
    // registry-mutating entry points (import_pipeline.h/import_gltf.cpp)
    // -- ImportComputeResult/MarshalPendingImport are forward-declared
    // just below (registry.h stays free of import_pipeline.h, a PRIVATE
    // header, per this project's public/private header-boundary
    // convention -- a reference/unique_ptr parameter only needs a
    // forward declaration at a friend-declaration site; the full type is
    // visible where these functions are actually DEFINED, in
    // import_gltf.cpp, which does include that header).
    friend ImportResult marshalGltfImportSync(Registry& registry, GeometryPool& pool, TextureCache* textures,
                                                ImportComputeResult&& compute);
    friend ImportResult marshalGltfImportFinalize(Registry& registry, std::unique_ptr<MarshalPendingImport> pending);

    MeshHandle registerMesh(MeshAsset asset) { return meshes_.acquire(std::move(asset)); }
    MaterialHandle registerMaterial(MaterialAsset asset) { return materials_.acquire(std::move(asset)); }

    rx::core::HandlePool<MeshTag, MeshAsset> meshes_;
    rx::core::HandlePool<MatTag, MaterialAsset> materials_;
    rx::core::HandlePool<TextureTag, TextureAsset> textures_;

    // D24 residency tracking: packed (index,generation) keys of handles
    // marked nonresident by evictForTesting(). A released (dead) handle
    // does not need an entry here -- HandlePool's own generational check
    // already makes get() fail for it; this set only needs to cover the
    // "still generationally live, but treat as absent" case nothing else
    // in HandlePool expresses.
    std::unordered_set<uint64_t> nonresidentMesh_;
    std::unordered_set<uint64_t> nonresidentMaterial_;

    MeshHandle fallbackMesh_;
    MaterialHandle fallbackMaterial_;
    TextureHandle fallbackTexture_;

    // [Phase 4 Stage 1 Task 15] Async import job tracking -- AsyncImportJob
    // itself (forward-declared above, defined in registry.cpp) holds every
    // cross-thread atomic (cancelled/stage/itemsCompleted/completionFired)
    // plus the main-thread-only MarshalPendingImport this job is carrying
    // once its compute phase finishes. shared_ptr (not unique_ptr):
    // every worker/IO/main closure this job's async chain posts captures
    // its OWN copy of this shared_ptr (never a raw Registry*/job* it
    // would have to reason about outliving) -- the job object itself
    // stays alive for as long as ANY of them still reference it, even
    // after ~Registry() has already removed Registry's own entry from
    // asyncJobs_ below (see ~Registry()'s own comment).
    std::unordered_map<uint64_t, std::shared_ptr<AsyncImportJob>> asyncJobs_;
    uint64_t nextAsyncImportId_ = 1;

    // Drops every FINISHED (completed or cancelled) job's entry from
    // asyncJobs_ -- called opportunistically at the top of every public
    // async-surface call (mirrors rx_task::Scheduler::runOnWorkerThread()'s
    // own opportunistic-reap convention). A job's OWN shared_ptr may still
    // be kept alive elsewhere (an in-flight closure that captured it) --
    // this only ever drops REGISTRY's reference, never forces destruction.
    void reapFinishedAsyncJobs();
};

}  // namespace rx::asset
