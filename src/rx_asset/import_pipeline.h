#pragma once

// import_pipeline.h -- PRIVATE (not installed). [Phase 4 Stage 1 Task 15]
// The compute/marshal split that makes Registry::importGltf() (sync) and
// Registry::importGltfAsync() (async) ONE SHARED PIPELINE with two
// completion styles (D5/matrix-issue22 row 15's "no forked logic" rule):
//
//   computeGltfImport()  -- pure CPU/IO work (parse, byte-source reads,
//                            per-primitive processing via parallelFor,
//                            per-material texture DECODE via parallelFor,
//                            node-graph flattening) -- NEVER touches
//                            Registry/GeometryPool/TextureCache mutation.
//                            Safe to call from any thread (the async path
//                            calls it from a Scheduler::runOnWorkerThread()
//                            task; the sync path calls it inline, on
//                            whichever thread called importGltf()).
//
//   marshalGltfImport()  -- the main-thread-only GPU/registry mutation
//                            tail: GeometryPool upload, TextureCache
//                            texture registration, Registry mesh/material
//                            insertion, instance handle patching.
//
// import_gltf.cpp's own importGltfPipeline() (the existing sync entry
// point, unchanged signature) is now a thin two-line wrapper: compute(),
// then marshal() with the BLOCKING primitives (matching its pre-Task-15
// behavior exactly -- see marshalGltfImport()'s own `deferred` parameter).
// registry.cpp's async orchestration calls the two halves separately,
// across several threads and several pumpMain() ticks, using the
// NON-blocking primitives instead.

#include <rx_asset/byte_source.h>
#include <rx_asset/import_error.h>
#include <rx_asset/mesh_asset.h>
#include <rx_asset/registry.h>
#include <rx_asset/texture_decode.h>
#include <rx_rhi_vk/upload.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace rx::task {
class Scheduler;
}  // namespace rx::task

namespace rx::asset {

class GeometryPool;
class TextureCache;
class Registry;

// One glTF primitive's fully-processed geometry, still in FILE order
// (indexed by work-item position, never by worker-completion order --
// the ordering-rule determinism criterion) and still LOCAL to this
// import's own combined vertex/index arrays (vertexStart/indexStart are
// offsets into ImportComputeResult::combinedVertices/combinedIndices, NOT
// yet a real GeometryPool MeshRange -- marshalGltfImport() adds the
// combined upload's own base offset once it lands).
struct PendingSubmeshCompute {
    size_t meshIndex = 0;
    size_t vertexStart = 0, vertexCount = 0;
    size_t indexStart = 0, indexCount = 0;
    AABB bounds;
    size_t materialIndex = SIZE_MAX;  // index into ImportComputeResult::materials; SIZE_MAX == no material (D11 fallback)
    SkinVertexData skinVertices;
    std::vector<MorphTarget> morphTargets;
    bool ok = false;
};

// One material-texture-slot's DECODE result, worker-computed -- registered
// (GPU-side) only at marshal time. `attempted == false` means this slot
// never needed a real load (unbound, or textures == nullptr) and
// `MaterialAsset`'s own TextureRef::handle already holds its final D11
// fallback/utility value from compute time -- marshal leaves it untouched.
//
// [Fix round 2, CRITICAL] `registered` -- true only once
// marshalGltfImportPrepareStep() has actually called registerDecoded()
// for THIS slot (main thread, marshal time). Until then, even an
// `attempted` slot's own TextureRef::handle still holds the COMPUTE-time
// placeholder (import_gltf.cpp's own fillRef() lambda pre-sets every
// present slot to the SHARED checkerboard fallback, overwritten with a
// real/failure handle only when this slot's marshal turn arrives) --
// marshalGltfImportRollback() MUST gate on this flag, not `attempted`
// alone, or a cancellation landing mid-registration (some slots
// marshalled, some not yet) releases the shared checkerboard itself
// (TextureCache::releaseUnpublished() on every other still-live import's
// fallback handle -- confirmed empirically, see task-15-report.md's
// round-2 section).
struct PendingTextureLoad {
    bool attempted = false;
    bool registered = false;
    TextureRole role = TextureRole::GenericData;
    std::string debugName;
    TextureDecodeResult decoded;
};

// Fixed slot order -- mirrors MaterialAsset's own 5 TextureRef fields
// (mesh_asset.h) exactly; textureRefForSlot() (import_gltf.cpp) is the one
// place that mapping is spelled out.
enum class MaterialTextureSlot : uint8_t { BaseColor, MetallicRoughness, Normal, Occlusion, Emissive, Count };

struct PendingMaterialCompute {
    MaterialAsset asset;
    std::array<PendingTextureLoad, static_cast<size_t>(MaterialTextureSlot::Count)> textureSlots;
};

// One flattened glTF-node instance [D12], LOCAL to this import (meshIndex
// indexes ImportComputeResult::meshSkins/meshMorphWeights and, via the
// submesh->mesh grouping, the eventual registered MeshHandle array --
// marshalGltfImport() patches `mesh` in place once mesh registration
// produces real handles).
struct PendingInstance {
    size_t meshIndex = SIZE_MAX;
    glm::mat4 worldTransform{1.0F};
    AABB worldBounds;
    bool negativeDeterminant = false;
    int32_t sourceNodeIndex = -1;
};

// ImportStage is PUBLIC API (registry.h -- included below): computeGltfImport()
// only ever moves it through Reading->Parsing->Decoding->Optimizing
// (Uploading/Done/Failed are marshal-side transitions the async
// orchestration in registry.cpp applies itself); the sync path never
// surfaces this at all (no observer to poll it).

// The full result of the compute phase -- everything marshalGltfImport()
// needs, with NO registry/pool/texture-cache mutation anywhere upstream of
// it. A default-constructed instance with `error != ImportError::None` (or
// `cancelled == true`) carries no further usable data; every OTHER field
// is only meaningful when both are false/None.
struct ImportComputeResult {
    ImportError error = ImportError::None;
    bool cancelled = false;

    std::vector<PoolVertex> combinedVertices;
    std::vector<uint32_t> combinedIndices;
    std::vector<PendingSubmeshCompute> submeshes;  // one per WORK ITEM (primitive), file order

    size_t meshCount = 0;                              // == source asset.meshes.size()
    std::vector<SkinData> meshSkins;                    // size == meshCount, index == source mesh index
    std::vector<std::vector<float>> meshMorphWeights;   // size == meshCount

    std::vector<PendingMaterialCompute> materials;  // file (source Material-array) order

    std::vector<PendingInstance> instances;
    std::vector<CameraData> cameras;
    std::vector<LightData> lights;
    std::vector<AnimationClipData> animations;

    uint32_t itemsTotal = 0;  // primitives + attempted texture loads -- progress denominator
};

// Pure CPU/IO stage -- see this header's own top comment. `source` is read
// through synchronously on WHATEVER thread this function itself runs on
// (the async orchestration in registry.cpp is what routes the actual
// byte-source reads onto the dedicated IO thread specifically, via a
// SEPARATE call before this function is invoked at all on a worker
// thread -- see that file's own comment); this function does not itself
// pick a thread for anything, it just must never be called from the
// dedicated IO thread when a caller cares about the D5 "decode never runs
// on the IO thread" invariant (debug-asserted by the caller, not here --
// this function has no way to learn "which thread is IO" on its own).
//
// `cancelled`: polled at STAGE boundaries only (never mid-parallelFor-chunk
// -- in-flight stage items always run to completion, per the abandon-
// cancellation contract) -- nullptr is a legal "never cancel" default (the
// sync path passes nullptr). `itemsCompleted`/`stage`: written as work
// progresses, for a caller to poll concurrently -- both nullptr is legal
// (the sync path passes nullptr for both; nothing polls it).
// [Task 15, fix -- found empirically by this task's own wall-clock-gate
// test once it started exercising a real TextureCache: TextureCache::
// checkerboardHandle()/fallbackHandle() are BOTH RX_ASSERT_MAIN_THREAD-
// guarded (D5 -- every TextureCache method carries the guard, matching
// this project's own "the whole class stays main-thread-only" convention,
// texture_cache.h's own top comment), so calling either from inside the
// compute phase (a worker thread, for the async path) aborts the process
// immediately. Both values are genuinely immutable after
// TextureCache::create() (set once, in buildFallbackTextures(), never
// mutated again) -- reading them is not itself a real hazard, but this
// function still has no legal (unguarded) way to reach them through
// TextureCache's own main-thread-only surface. Fix: the caller (which DOES
// run on the main thread at the point it decides to kick off an import)
// snapshots every fallback handle ONCE, up front, into this plain POD,
// and this function reads ONLY the snapshot -- never TextureCache itself
// -- for these lookups.] `byRole` is indexed by
// `static_cast<size_t>(TextureRole)` (texture_decode.h), matching
// TextureCache's own `roleFallback_` indexing exactly.
struct TextureFallbackHandles {
    TextureHandle checkerboard;
    std::array<TextureHandle, 6> byRole{};
};

// Main-thread-only (calls straight into TextureCache's own guarded
// accessors) -- called once, by whichever caller is about to kick off an
// import (importGltfPipeline() below, or registry.cpp's async
// orchestration), never from computeGltfImport() itself. `textures ==
// nullptr` returns a default-constructed (all-invalid-handle) snapshot --
// computeGltfImport() never actually reads it in that case anyway (the
// `textures != nullptr` branch is what consults it).
[[nodiscard]] TextureFallbackHandles snapshotTextureFallbackHandles(TextureCache* textures);

// `fallbackTextureHandleForUncached`: the value an unbound-but-present
// texture reference resolves to ONLY when `textures == nullptr` (the
// no-TextureCache Task-13 baseline contract) -- this function has no
// Registry reference of its own (the whole point of the compute/marshal
// split), so the one Registry-owned value it still needs
// (Registry::fallbackTextureHandle()) is passed in explicitly by whichever
// caller DOES have a Registry (both importGltfPipeline() below and
// registry.cpp's async orchestration snapshot it once, up front).
// `textureFallbacks`: see TextureFallbackHandles' own comment just above
// -- the checkerboard/role-fallback handles this function needs when
// `textures != nullptr`, pre-fetched on the main thread.
// `itemsTotalOut`: set ONCE, as soon as the total is known (before the
// per-primitive/per-texture fan-out begins) -- lets a concurrent poller
// see a meaningful items-completed/items-total FRACTION while the
// Decoding/Optimizing stages are still in flight, not just at the very
// end. nullptr is legal (the sync path passes nullptr for every one of
// these four instrumentation pointers; nothing polls it).
[[nodiscard]] ImportComputeResult computeGltfImport(std::span<const std::byte> documentBytes, ByteSource& source,
                                                      rx::task::Scheduler& scheduler, TextureCache* textures,
                                                      TextureHandle fallbackTextureHandleForUncached,
                                                      const TextureFallbackHandles& textureFallbacks,
                                                      const std::atomic<bool>* cancelled,
                                                      std::atomic<uint32_t>* itemsCompleted,
                                                      std::atomic<uint32_t>* itemsTotalOut,
                                                      std::atomic<ImportStage>* stageOut);

// Main-thread-only GPU/registry mutation tail -- takes ownership of
// `compute` (moved from). `deferred == false` (the sync path): uses
// GeometryPool::upload()/TextureCache::registerDecoded()+flush+wait --
// blocks until every GPU upload this import needs is complete, then
// performs every registry mutation and returns the finished ImportResult
// immediately, matching importGltf()'s pre-Task-15 synchronous contract
// byte-for-byte. `deferred == true` (the async path): uses
// GeometryPool::uploadDeferred()/TextureCache::registerDecoded() (still
// called here, main-thread, but WITHOUT ever calling wait()) and returns
// as soon as every upload has been RECORDED -- the caller (registry.cpp's
// async orchestration) is responsible for polling the returned tickets to
// completion and THEN calling finalizeGltfImport() below to perform the
// actual registry mutation; `outPending` receives everything
// finalizeGltfImport() needs plus every UploadTicket to poll. Passing
// `deferred == true` with `outPending == nullptr` is a caller error
// (asserted).
// Everything the deferred (async) marshal path needs to carry between
// prepareDeferred() (right after compute, before any upload ticket is
// confirmed) and finalizeGltfImport()/rollback() (once it is, or once
// cancellation is observed first). registry.cpp's own AsyncImportJob holds
// this behind a std::unique_ptr -- FULLY defined here (not merely
// forward-declared) because that unique_ptr is a plain DATA MEMBER of a
// struct registry.cpp itself defines, and a unique_ptr<T> data member
// needs T's complete type wherever its OWN enclosing type's destructor is
// generated (the classic reason a pImpl forward declaration alone does not
// suffice once a unique_ptr to it becomes a member elsewhere, not just a
// function parameter/return value) -- registry.cpp is still not expected
// to reach into these fields directly (by convention, not by enforced
// opacity): it only ever passes this struct, as a whole, through the four
// functions below.
struct MarshalPendingImport {
    ImportComputeResult compute;

    MeshRange combinedRange;
    bool hasGeometry = false;
    rx::rhi::UploadTicket poolTicket;
    bool poolTicketIssued = false;

    bool hasTextureUploads = false;
    rx::rhi::UploadTicket textureTicket;
    bool textureTicketIssued = false;

    // [Task 15, RC6 "time-sliced marshalling" fix] Incremental-prepare
    // cursor -- found LOAD-BEARING by this task's own wall-clock-gate
    // test: registering DamagedHelmet's five real JPG textures (up to
    // ~1.3 MB of decoded RGBA8 pixels each) in ONE synchronous call
    // measured 15.8 ms, nearly 8x the 2 ms local budget and above the
    // 10 ms CI stall detector -- each registerDecoded() call's own
    // memcpy-into-the-staging-ring cost is real CPU time, not something
    // "poll, never block" alone fixes (that invariant is about GPU
    // COMPLETION, not the CPU cost of RECORDING the copy in the first
    // place). Fix: marshalGltfImportPrepareStep() below registers AT MOST
    // ONE texture slot per call, so a caller can spread the whole batch
    // across as many pumpMain() ticks as it has slots -- "never block
    // longer for a big asset, more frames instead" (plan Task 15's own
    // wording), never a single oversized posted closure.
    size_t nextMaterialIndex = 0;
    size_t nextSlotIndex = 0;
    bool texturesPrepared = false;
    bool geometryPrepared = false;
};

// Both marshalGltfImportSync() and marshalGltfImportFinalize() below call
// Registry::registerMesh()/registerMaterial() directly -- registry.h's own
// friend declaration names these two functions specifically (alongside
// the legacy importGltfPipeline()) so this is the only widening beyond
// the pre-Task-15 friend surface.
[[nodiscard]] ImportResult marshalGltfImportSync(Registry& registry, GeometryPool& pool, TextureCache* textures,
                                                    ImportComputeResult&& compute);

// Wraps `compute` for the deferred (async) marshal path -- does NO GPU
// work itself (a plain allocation + move), so it never needs its own
// time-slicing. Follow with repeated marshalGltfImportPrepareStep() calls
// (one per pumpMain() tick) until it returns false.
[[nodiscard]] std::unique_ptr<MarshalPendingImport> marshalGltfImportBeginDeferred(ImportComputeResult&& compute);

// Performs ONE bounded unit of prepare work -- registers AT MOST ONE
// still-unregistered texture slot (in (material, slot) file order) via
// TextureCache::registerDecoded(), OR, once every texture slot is done,
// the ONE combined geometry upload (GeometryPool::uploadDeferred()) plus
// both flush()es -- and returns true iff there is more prepare work left
// to do (the caller should re-post via postToMain() for the NEXT
// pumpMain() tick rather than looping further within this same call).
// Never calls wait() (D25's own "poll, never a blocking wait" invariant)
// and never registers more than one texture per call (the fix this
// struct's own comment documents). No-op (returns false) once already
// fully prepared, or if `pending.compute` carries an error/cancellation.
[[nodiscard]] bool marshalGltfImportPrepareStep(GeometryPool& pool, TextureCache* textures, MarshalPendingImport& pending);

// True once every UploadTicket marshalGltfImportPrepareStep() issued has
// completed (pure poll, GeometryPool/TextureCache's own isUploadComplete()
// -- never blocks). Only meaningful once marshalGltfImportPrepareStep()
// has returned false (fully prepared) at least once.
[[nodiscard]] bool marshalGltfImportUploadsComplete(const GeometryPool& pool, const TextureCache* textures,
                                                       const MarshalPendingImport& pending);

// The final registry-mutation step -- ONLY safe to call once
// marshalGltfImportUploadsComplete() has returned true (asserted in debug
// builds). Performs every Registry::registerMesh()/registerMaterial() call
// (file order) and returns the finished ImportResult.
[[nodiscard]] ImportResult marshalGltfImportFinalize(Registry& registry, std::unique_ptr<MarshalPendingImport> pending);

// [Fix round 2, ITEM 3] Closes the one gap marshalGltfImportUploadsComplete()
// cannot see on its own: a texture slot can be REGISTERED
// (TextureCache::registerDecoded() already called, real GPU-side copy
// command recorded) without yet having a TICKET (TextureCache::
// flushPendingUploads() not yet called -- marshalGltfImportPrepareStep()
// only issues the texture ticket once EVERY slot is done, but yields
// after each individual registration, so a cancellation observed between
// two slot registrations lands `pending` in exactly this state:
// `hasTextureUploads == true`, `textureTicketIssued == false`). With no
// ticket issued, marshalGltfImportUploadsComplete() has nothing to poll
// and (incorrectly, on its own) reports "nothing outstanding" --
// releasing that slot's handle in that state destroys an image whose
// copy command has been recorded but never even submitted, corrupting
// whatever command buffer that recording lives in once anything LATER
// ends/flushes it (reproduced empirically as a real
// `UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-VkImage`
// validation error; see task-15-report.md's round-2 section). Call this
// BEFORE marshalGltfImportUploadsComplete() on the abandon/rollback path,
// every time -- idempotent (a no-op once `textureTicketIssued` is
// already true, or if nothing was ever registered), forces a REAL ticket
// to exist for whatever has been recorded so far by calling
// TextureCache::flushPendingUploads() early (safe: flush()'s own
// contract is "submit whatever is recorded so far", not "only once, at
// the very end" -- nothing about calling it earlier than usual is
// unsound). Never calls wait() (D25's own invariant, unchanged).
//
// GEOMETRY has no equivalent gap: GeometryPool::uploadDeferred() and its
// own flushPendingUploads() call are issued together, unconditionally,
// inside the SAME marshalGltfImportPrepareStep() call with no yield in
// between (see that function's own geometry branch) -- `pending.hasGeometry
// == true` and `pending.poolTicketIssued == false` can never coexist once
// prepare() has run at all. marshalGltfImportRollback() itself asserts
// this invariant (debug builds) rather than merely documenting it, so any
// future change that breaks the atomicity fails loudly instead of
// silently reopening this same class of bug on the geometry side.
void marshalGltfImportEnsureRollbackTicketed(TextureCache* textures, MarshalPendingImport& pending);

// Releases every GPU resource marshalGltfImportPrepareDeferred() already
// registered (TextureCache::releaseUnpublished()/GeometryPool::free()) --
// the cancellation/teardown rollback path for an import observed cancelled
// AFTER prepare() but BEFORE finalize(). Safe to call on a
// default/moved-from MarshalPendingImport (no-op). REQUIRES
// marshalGltfImportEnsureRollbackTicketed() to have already been called
// (see that function's own comment) -- callers on the abandon path (this
// library's own registry.cpp) always call it first; direct callers of
// this function must too.
void marshalGltfImportRollback(GeometryPool& pool, TextureCache* textures, std::unique_ptr<MarshalPendingImport> pending);

}  // namespace rx::asset
