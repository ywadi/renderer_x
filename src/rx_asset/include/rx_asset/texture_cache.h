#pragma once

// rx_asset/texture_cache.h -- Thread-affinity (D5, Phase 4): GPU-object
// mutation (Texture2D creation, Uploader submission, BindlessTable
// registration/release, VkSampler creation) is MAIN-THREAD-ONLY --
// load()/loadFromBytes()/getOrCreateSampler()/evictForTesting() all carry
// an RX_ASSERT_MAIN_THREAD guard, matching GeometryPool/Registry/Uploader/
// BindlessTable's identical convention (docs/threading.md). Decode/
// transcode (the CPU-heavy part of load()) is pure, worker-eligible CPU
// work -- see texture_decode.h, which this class calls into -- but this
// task keeps the whole load() call synchronous on the calling thread
// (Task 15 parallelizes the decode half across rx_task workers). Read
// accessors (resolve()/stats()/fallbackHandle()) share the same main-
// thread-only posture as GeometryPool/Registry's own read accessors (no
// internal lock -- see those headers' own top comments for the
// rationale).
//
// [Phase 4 Stage 1 Task 14, spec D10/D11/D24/D25, gate matrix-issue03 as
// amended by gate/rulings-2026-08-18.md #3] KTX2/Basis compressed texture
// pipeline + sampler cache. Owns its OWN TextureHandle space (a separate
// rx::core::HandlePool<TextureTag, TextureRecord> instance from
// Registry's -- see this header's own TextureRecord comment for why the
// two are deliberately not the same pool) and creates its own D11
// fallback/utility GPU textures at create() time (Registry cannot: it has
// no Device/Allocator/Uploader/BindlessTable to build a REAL GPU resource
// with -- see registry.cpp's own trivial-placeholder fallbackTexture_).

#include <rx_asset/byte_source.h>
#include <rx_asset/mesh_asset.h>
#include <rx_asset/texture_decode.h>
#include <rx_core/handle.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/memory_report.h>
#include <rx_rhi_vk/texture.h>
#include <rx_rhi_vk/upload.h>
#include <volk.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rx::rhi {
class Allocator;
class Device;
class Uploader;
class BindlessTable;
class DeletionQueue;
}  // namespace rx::rhi

namespace rx::asset {

// GPU-backed, real texture data -- deliberately DISTINCT from Registry's
// own (Task 13) `TextureAsset` (mesh_asset.h, `{bool isFallback}` only):
// Registry has no Device/Allocator at construction time (its constructor
// takes none), so it structurally cannot back a handle with a real
// VkImage -- its own `textures_` pool exists only to give
// `TextureRef::handle` SOMETHING to resolve to before this class exists
// (see mesh_asset.h's own TextureRef comment). For a REAL (non-fallback)
// texture reference produced by an import that supplied a TextureCache,
// `TextureRef::handle` is a handle into THIS class's own handle space --
// resolve it through the SAME TextureCache instance the import used, via
// resolve() below, never through `Registry::texture()` (mirrors exactly
// how a GeometryPool `MeshRange::blockId` is resolved through the SAME
// GeometryPool the mesh was uploaded into, never through Registry).
struct TextureRecord {
    bool isFallback = false;
    TextureRole role = TextureRole::GenericData;
    // BindlessTable sampled-image slot index -- ALWAYS valid (points at
    // the checkerboard/utility fallback's own slot while nonresident/
    // evicted, D24 -- never a stale or dangling index).
    uint32_t bindlessIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    // [D24] false between evictForTesting() and DeletionQueue's actual
    // reclaim -- resolve() substitutes the checkerboard's record (still
    // isFallback=true, resident=true) whenever this is false.
    bool resident = true;
};

// `TextureHandle` itself is mesh_asset.h's own `rx::core::Handle<struct
// TextureTag>` alias (already visible here via the `#include
// <rx_asset/mesh_asset.h>` above) -- deliberately NOT re-aliased in this
// header. See this header's own top-of-class comment on TextureRecord for
// why the same C++ TYPE is reused across two logically-separate handle
// SPACES (Registry's own vs. this class's own).

// Bytes-by-role + count [FG9 issue comment: "TextureCache exposes
// resident-memory stats (bytes by role, count) mirroring GeometryPool's
// stats"]. Balances to zero across load/evict/teardown -- the Task 10
// accounting test pattern (src/rx_asset/tests/accounting_test.cpp,
// GeometryPool's own equivalent) applied here in
// texture_cache_accounting_test.cpp.
struct TextureCacheStats {
    struct RoleStats {
        uint64_t bytes = 0;
        uint32_t count = 0;
    };
    // Indexed by static_cast<size_t>(TextureRole) -- 6 entries, one per
    // TextureRole enumerator (texture_decode.h).
    std::array<RoleStats, 6> byRole{};
    uint64_t totalBytes = 0;
    uint32_t totalCount = 0;
};

class TextureCache {
public:
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;
    TextureCache(TextureCache&&) = delete;
    TextureCache& operator=(TextureCache&&) = delete;
    ~TextureCache();

    // Builds this cache's D11 fallback/utility GPU textures (checkerboard
    // + white/flat-normal/neutral-MR, all real, bindless-registered
    // images -- see this header's own top comment for why Registry
    // cannot do this itself) and returns nullopt (logged) on any failure
    // building them. `allocator`/`device`/`uploader`/`bindless`/
    // `deletionQueue` are stored BY REFERENCE -- this TextureCache must
    // not outlive any of them (same lifetime discipline as
    // GeometryPool::create(), geometry_pool.h's own comment).
    static std::unique_ptr<TextureCache> create(rx::rhi::Allocator& allocator, rx::rhi::Device& device,
                                                 rx::rhi::Uploader& uploader, rx::rhi::BindlessTable& bindless,
                                                 rx::rhi::DeletionQueue& deletionQueue);

    // Primary entry point [D10, the IO-source abstraction invariant]:
    // resolves `uri` through `source` (mirrors Registry::importGltf's own
    // ByteSource pattern -- byte_source.h), then routes to
    // loadFromBytes() below. ANY failure (byte-source miss, corrupt
    // container, unsupported layout, transcode failure) logs exactly ONCE
    // (D11 dedup -- no per-frame spam) and returns checkerboardHandle()
    // -- never a dead/invalid handle, never a crash.
    //
    // Main-thread-only (D5).
    [[nodiscard]] TextureHandle load(ByteSource& source, const std::string& uri, TextureRole role);

    // For callers that already hold bytes in memory (import_gltf.cpp's
    // own KHR_texture_basisu / data-URI / GLB-bufferView resolution --
    // see that file's own image-source resolution helper -- and this
    // class's own in-memory tests). `debugName` is whatever the caller
    // has on hand for the ONE log line a failure produces (a URI,
    // "material 'X' baseColor texture", ...) -- purely cosmetic, never
    // consulted for any decision (role inference happens at the CALL
    // SITE, slot-driven, never from this string -- matrix's own
    // misleading-filename test targets exactly this).
    //
    // Main-thread-only (D5).
    [[nodiscard]] TextureHandle loadFromBytes(std::span<const std::byte> bytes, TextureRole role,
                                               std::string_view debugName);

    // [Phase 4 Stage 1 Task 15] Thread-affinity: NONE -- worker-safe (see
    // texture_decode.h's own decodeTextureForUpload() comment, which this
    // is a thin wrapper over, supplying this cache's own
    // maxImageDimension2D_/isFormatSupported() as the two device-specific
    // parameters that pure function needs). Touches no mutable
    // TextureCache state: isFormatSupported() is a read-only physical-
    // device query (Vulkan spec permits this from any thread with no
    // external synchronization) and maxImageDimension2D_ is set once at
    // create() and never mutated again. This is the "decode" half of
    // loadFromBytes()'s own two-step contract, split out so the async
    // import pipeline can run it on a compute worker
    // (Scheduler::parallelFor()) while registerDecoded() below stays
    // main-thread-only for the GPU-facing half -- the SAME decode
    // function loadFromBytes() itself now calls, not a fork.
    [[nodiscard]] TextureDecodeResult decodeForUpload(std::span<const std::byte> bytes, TextureRole role) const;

    // [Task 15] Main-thread-only: registers the GPU-side result of a PRIOR
    // decodeForUpload() call (run on any thread, including a worker) --
    // the second half of loadFromBytes()'s own contract. Applies the same
    // once-per-(debugName,category) deferred-warning log dedup
    // loadFromBytes() itself applies (see TextureDecodeResult::warnings'
    // own comment), then, for a Ready decode, does exactly what
    // loadFromBytes() does GPU-side (Texture2D creation, Uploader
    // submission, BindlessTable registration) -- EXCEPT it does NOT call
    // flush()/wait() itself: the caller batches via flushPendingUploads()
    // below and polls isUploadComplete() (D25's "poll, never a blocking
    // wait" invariant -- the async import path must never reach this
    // class's own wait()-based loadFromBytes()/load()).
    //
    // Main-thread-only (D5).
    [[nodiscard]] TextureHandle registerDecoded(const TextureDecodeResult& decoded, std::string_view debugName);

    // [Task 15] Forwards to this cache's own Uploader::flush() -- submits
    // every registerDecoded() batch recorded since the last flush and
    // returns a pollable UploadTicket (D25).
    //
    // Main-thread-only (D5).
    rx::rhi::UploadTicket flushPendingUploads();

    // [Task 15] Forwards to this cache's own Uploader::isComplete() --
    // pure poll, never blocks.
    //
    // Main-thread-only (D5).
    [[nodiscard]] bool isUploadComplete(rx::rhi::UploadTicket ticket) const;

    // [Fix round 1] Test/diagnostic-only: how many times THIS cache has
    // ever called Uploader::wait() -- i.e. how many times loadFromBytes()
    // (the blocking convenience) has actually blocked. The async import
    // pipeline exclusively uses registerDecoded(), so this counter staying
    // at its pre-import baseline throughout an async import is a DIRECT
    // proof of "wait-calls-from-async-path == 0" (matrix-issue22), not
    // merely inferred from wall-clock timing.
    //
    // Main-thread-only (D5).
    [[nodiscard]] size_t waitCallCountForTesting() const;

    // [Task 15] Production-named release path for a texture
    // registerDecoded() created whose OWNING async import was cancelled
    // (or failed) before the registry ever exposed a handle referencing
    // it -- e.g. abandon-style cancel() observed between registerDecoded()
    // and the import's own final registry-commit step (no registry
    // mutation applied after observation -- the cancellation contract).
    // Same underlying mechanism as evictForTesting() below (D24: mark
    // nonresident immediately, retire the real GPU resource -- Texture2D
    // destruction + BindlessTable slot release -- into DeletionQueue
    // tagged `frameIndex`) -- given a non-test name here because a
    // cancelled async import is a genuine PRODUCTION caller of this exact
    // release path, unlike evictForTesting()'s own test/diagnostic-only
    // callers.
    //
    // Main-thread-only (D5).
    void releaseUnpublished(TextureHandle handle, uint64_t frameIndex = 0);

    // Convenience: filesystem-backed, wraps a FilesystemByteSource (same
    // pattern as Registry::importGltf's path overload) -- zero
    // std::filesystem/fopen touched in texture_cache.cpp ITSELF (grep-
    // enforced review criterion; FilesystemByteSource's own
    // implementation lives in byte_source.cpp, a different translation
    // unit).
    //
    // Main-thread-only (D5).
    [[nodiscard]] TextureHandle load(const std::filesystem::path& path, TextureRole role);

    // [D24] Residency-tolerant resolve: a dead (never valid, or since-
    // reclaimed) or live-but-nonresident (evictForTesting(), not yet
    // reclaimed) handle resolves to checkerboardHandle()'s own record --
    // never a null dereference, never UB. Main-thread-only (D5) -- see
    // GeometryPool/Registry's own read-accessor comment for why a "read"
    // still carries the guard in this codebase.
    [[nodiscard]] const TextureRecord& resolve(TextureHandle handle) const;

    // [D11] Role-appropriate UTILITY texture for an UNBOUND material slot
    // (the material carries no texture reference for this role AT ALL --
    // never called for a present-but-failed-to-load reference, which
    // instead flows through load()/loadFromBytes()'s own checkerboard
    // fallback path above; see this header's own class comment and
    // task-14-report.md for the exact matrix wording this split is taken
    // from verbatim). baseColor/emissive/genericData -> white;
    // normal -> flat-normal (never checkerboard -- "a magenta normal map
    // would shade garbage"); metallicRoughness/occlusion -> neutral-MR.
    [[nodiscard]] TextureHandle fallbackHandle(TextureRole role) const;

    // [D11] The 4x4 magenta/black checkerboard -- the fallback for every
    // load()/loadFromBytes() FAILURE mode (missing bytes, corrupt
    // container, transcode failure, unsupported layout, unsupported MIME/
    // decode failure), role-independent by design (a deliberately loud,
    // "something is wrong here" visual, distinct from the unbound-slot
    // utility textures above).
    [[nodiscard]] TextureHandle checkerboardHandle() const;

    // ---------------------------------------------------------------
    // Sampler cache [G6]
    // ---------------------------------------------------------------
    // glTF sampler -> VkSampler, deduplicated by the CONVERTED Vulkan
    // state (wrapS, wrapT, magFilter, minFilter, mipmapMode,
    // maxAnisotropy) -- two glTF samplers that differ only in a raw
    // enum spelling that maps to the SAME Vulkan state (e.g.
    // magFilter==0 "unspecified" vs magFilter==9729 "LINEAR", both -> VK_
    // FILTER_LINEAR) correctly collapse to ONE VkSampler, which is
    // strictly the more useful dedup granularity than a raw-glTF-bytes
    // key would give. `desc` fields left at SamplerDesc's own defaults
    // (mesh_asset.h) reproduce the glTF spec's documented sampler
    // defaults (REPEAT wrap, auto/linear filtering) -- see
    // texture_cache.cpp's own glTF-enum -> Vk mapping table for the full
    // canonical (VkFilter, VkSamplerMipmapMode) pairing per minFilter
    // value.
    //
    // Anisotropy: 8x requested when the device's VkPhysicalDeviceFeatures
    // ::samplerAnisotropy is supported (clamped to
    // VkPhysicalDeviceLimits::maxSamplerAnisotropy), cleanly OFF
    // (anisotropyEnable=VK_FALSE) otherwise -- verified in-task against
    // the CI driver alongside the BC format-support dump (task-14-report.
    // md).
    //
    // Main-thread-only (D5) -- vkCreateSampler is GPU-object mutation.
    [[nodiscard]] VkSampler getOrCreateSampler(const SamplerDesc& desc);

    // [Fix round, sampler-wrap P0] Bindless-INDEX counterpart to
    // getOrCreateSampler() above -- the actual real fix for the shipped
    // sampler-wrap defect (helmet-sampler-fix-brief.md's own root-cause
    // account): every material texture slot used to sample through ONE
    // hardcoded process-wide CLAMP_TO_EDGE sampler regardless of that
    // texture's own glTF wrap mode, silently breaking any asset relying on
    // glTF-default REPEAT for UVs outside [0,1] (DamagedHelmet's own
    // TEXCOORD_0 V, wholly in [1.0005,1.9987]) -- this class ALREADY built
    // the correct per-texture VkSampler (getOrCreateSampler() above,
    // honoring `desc`'s real glTF wrap/filter state) but never registered
    // it against the real bindless SAMPLER table, so no shader could ever
    // reach it. This method closes exactly that gap: gets-or-creates the
    // deduplicated VkSampler (via getOrCreateSampler() -- same dedup
    // granularity, same cache), then gets-or-creates its OWN bindless
    // registration (a second, VkSampler-keyed cache -- one BindlessTable
    // registration per unique dedup'd VkSampler, not per call), and
    // returns the real, shader-visible bindless index. `desc` left at its
    // own default-constructed values (SamplerDesc's own REPEAT-wrap/
    // auto-filter defaults, mesh_asset.h) reproduces the glTF spec's own
    // "sampler unspecified" default -- correct to pass verbatim for BOTH
    // an absent texture slot (samples a role-neutral D11 fallback texture,
    // wrap mode irrelevant) and a present slot whose glTF texture
    // referenced no sampler object at all (spec REPEAT/auto is the exactly
    // correct answer, not a fallback-of-last-resort).
    //
    // Returns std::nullopt (logged) on a genuine failure (vkCreateSampler
    // failure inside getOrCreateSampler(), or BindlessTable::
    // registerSampler() capacity exhaustion) -- callers fall back to
    // whatever process-wide default sampler they already carry (belt-and-
    // suspenders, mirroring resolveTextureIndex()'s own fallback
    // philosophy in samples/08_gltf_viewer/main.cpp), never a crash or a
    // silently-wrong index.
    //
    // Main-thread-only (D5) -- registerSampler() is GPU-object mutation.
    [[nodiscard]] std::optional<uint32_t> getOrCreateSamplerBindlessIndex(const SamplerDesc& desc);

    // [FG9] Resident bytes/count by role -- balances to zero across
    // load/evict/teardown (Task 10 accounting-test pattern). Main-thread-
    // only (D5), matching every other read accessor on this class.
    [[nodiscard]] TextureCacheStats stats() const;

    // [D24] Test-only deferred-eviction path: immediately marks
    // `handle`'s record non-resident (resolve() substitutes the
    // checkerboard from this instant), then retires the REAL GPU
    // resource's destructor (Texture2D destruction + BindlessTable slot
    // release) into this cache's DeletionQueue tagged `frameIndex` --
    // only reclaimed (slot freed, handle generation bumped) once the
    // caller itself calls `deletionQueue.onFrameFenceSignaled(frameIndex)`
    // (the SAME DeletionQueue reference passed to create(); this method
    // does not own or drive it). Mirrors
    // src/rx_rhi_vk/tests/eviction_contract_test.cpp's own synthetic
    // evict()/clause-(c) sequence, but over a REAL rx::rhi::Texture2D +
    // rx::rhi::BindlessTable instead of a synthetic stand-in. Production
    // code has no need for this (Phase 4 has no automatic eviction
    // POLICY, D24) -- test/diagnostic-only, matching Registry::
    // evictForTesting()'s identical naming/scope.
    //
    // Main-thread-only (D5).
    void evictForTesting(TextureHandle handle, uint64_t frameIndex = 0);

    // Test/diagnostic accessors -- production code has no need for them.
    // Main-thread-only (D5), matching every other accessor on this class.
    [[nodiscard]] size_t samplerCountForTesting() const;
    [[nodiscard]] size_t liveTextureCountForTesting() const;

private:
    struct Entry {
        TextureRecord record;
        std::optional<rx::rhi::Texture2D> image;
        // The FULL BindlessHandle (kind+index+generation) -- release()
        // needs this, not just record.bindlessIndex (a raw index alone
        // cannot express BindlessTable's own generational free-list
        // safety).
        rx::rhi::BindlessHandle bindlessHandle;
    };

    TextureCache(rx::rhi::Allocator& allocator, rx::rhi::Device& device, rx::rhi::Uploader& uploader,
                 rx::rhi::BindlessTable& bindless, rx::rhi::DeletionQueue& deletionQueue);

    // Shared registration tail: creates a Texture2D via
    // Texture2D::createForPresuppliedMips(), uploads every entry in
    // `uploadLevels` via Uploader::uploadImageMips(), registers the
    // result in `bindless_`, and inserts a new pool Entry -- the ONE
    // place a real (non-fallback) TextureRecord is minted, shared by
    // load()/loadFromBytes()'s libktx path, the stb path (a single
    // `uploadLevels` entry, mipLevel 0), and buildFallbackTextures()'s own
    // solid-color/checkerboard textures below (also single-entry).
    // Returns an invalid (default-constructed) TextureHandle -- NEVER
    // itself falling back to checkerboardHandle(), since that fallback
    // does not exist yet the first time this runs (buildFallbackTextures()
    // IS this function's own first caller) -- on any GPU-side failure;
    // callers translate that into whatever fallback is appropriate for
    // their own call site.
    [[nodiscard]] TextureHandle registerRealTexture(TextureRole role, VkFormat format, uint32_t width,
                                                     uint32_t height, uint32_t mipLevels,
                                                     std::span<const rx::rhi::Uploader::ImageMipLevel> uploadLevels);

    // Logs the FAILURE `reason` (RX_LOG_ERROR, once per (debugName,
    // "failed") pair -- see shouldLogOnce() below) and returns
    // checkerboardHandle() -- the ONE place every load()/loadFromBytes()
    // failure branch funnels through.
    [[nodiscard]] TextureHandle failureFallback(std::string_view debugName, std::string_view reason);

    // [D11 "ONE log per asset (dedup criterion -- no per-frame spam)"]
    // True the FIRST time this exact (debugName, category) pair is seen;
    // false (and no further bookkeeping) on every later call with the
    // same pair. `category` distinguishes independent warning CLASSES for
    // the SAME debugName (e.g. "failed" vs "no-mips" vs "colorspace") so
    // dedup on one never accidentally suppresses a genuinely different
    // warning about the same asset.
    [[nodiscard]] bool shouldLogOnce(std::string_view debugName, std::string_view category);

    // [Task 15] Shared tail for loadFromBytes() (sync) and
    // registerDecoded() (async, no forked logic) -- applies `decoded`'s
    // deferred warnings through shouldLogOnce(), then either falls back
    // (Failed/Checkerboard) or performs the real GPU registration
    // (registerRealTexture()) for a Ready decode. Never flushes/waits --
    // that stays each caller's own job (loadFromBytes() flushes+waits
    // unconditionally; registerDecoded() never does).
    [[nodiscard]] TextureHandle applyDecodeResult(const TextureDecodeResult& decoded, std::string_view debugName);

    void releaseInternal(TextureHandle handle, uint64_t frameIndex);

    bool buildFallbackTextures();
    [[nodiscard]] std::optional<TextureHandle> uploadSolidColor(TextureRole role, std::array<uint8_t, 4> rgba,
                                                                  VkFormat format, std::string_view debugName);
    [[nodiscard]] std::optional<TextureHandle> uploadCheckerboard();

    [[nodiscard]] bool isFormatSupported(VkFormat format) const;

    rx::rhi::Allocator& allocator_;
    rx::rhi::Device& device_;
    rx::rhi::Uploader& uploader_;
    rx::rhi::BindlessTable& bindless_;
    rx::rhi::DeletionQueue& deletionQueue_;

    rx::core::HandlePool<struct TextureTag, Entry> pool_;

    TextureHandle checkerboard_;
    std::array<TextureHandle, 6> roleFallback_{};  // indexed by static_cast<size_t>(TextureRole)

    struct SamplerKey {
        VkSamplerAddressMode wrapS;
        VkSamplerAddressMode wrapT;
        VkFilter magFilter;
        VkFilter minFilter;
        VkSamplerMipmapMode mipmapMode;
        int32_t anisotropyTimes100;  // maxAnisotropy*100, rounded -- exact float equality is not a safe map key
        bool operator==(const SamplerKey&) const = default;
    };
    struct SamplerKeyHash {
        size_t operator()(const SamplerKey& key) const;
    };
    std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> samplerCache_;

    // [Fix round, sampler-wrap P0] getOrCreateSamplerBindlessIndex()'s own
    // second-level cache -- keyed by the ALREADY-DEDUPLICATED VkSampler
    // handle from samplerCache_ above (not a second SamplerKey lookup: a
    // VkSampler is trivially hashable, and every unique SamplerKey maps to
    // exactly one VkSampler, so keying on the sampler itself avoids
    // recomputing/duplicating SamplerKey's own glTF-enum -> Vulkan mapping
    // logic here) -- one BindlessTable registration per unique dedup'd
    // sampler, never one per call. Released (bindless_.release()) in
    // ~TextureCache(), symmetric with the vkDestroySampler loop just below
    // this member in that destructor.
    std::unordered_map<VkSampler, rx::rhi::BindlessHandle> samplerBindlessCache_;

    bool anisotropySupported_ = false;
    float maxAnisotropy_ = 1.0F;
    uint32_t maxImageDimension2D_ = 0;
    size_t waitCallCountForTesting_ = 0;

    // [FG9] Incrementally maintained by registerRealTexture() (+1) and
    // evictForTesting()'s deferred reclaim callback (-1) -- HandlePool
    // deliberately exposes no by-index iteration (rx_core/handle.h's own
    // class comment), so stats() cannot walk `pool_` itself; this is the
    // cache's own running ledger instead, mirroring GeometryPool::stats()
    // 's identical "plain snapshot, caller feeds Tracy" posture.
    TextureCacheStats stats_;

    // Per-asset (per debugName) failure-log dedup [D11 "ONE log per asset"]
    // -- a set of debug names already warned about once, so a repeated
    // load() of the SAME failing texture (e.g. re-imported scenes sharing
    // one broken KTX2 file) never spams past the first occurrence.
    std::unordered_set<std::string> loggedFailures_;
};

}  // namespace rx::asset
