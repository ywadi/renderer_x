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
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace rx::task {
class Scheduler;
}  // namespace rx::task

namespace rx::asset {

class GeometryPool;

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
};

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

private:
    // import_gltf.cpp's orchestration function needs direct access to
    // registerMesh()/registerMaterial() below (and, transitively, this
    // class's HandlePools) to insert what it parses -- declared as a
    // named friend rather than widening any of the methods above to
    // public, so the ONLY way to mutate this registry from outside its
    // own two .cpp files is through the public importGltf()/
    // evictForTesting() surface.
    friend ImportResult importGltfPipeline(Registry& registry, std::span<const std::byte> documentBytes, ByteSource& source,
                                            GeometryPool& pool, rx::task::Scheduler& scheduler, TextureCache* textures);

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
};

}  // namespace rx::asset
