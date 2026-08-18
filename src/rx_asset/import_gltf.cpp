#include "import_gltf.h"
#include "gltf_error.h"
#include "gltf_pipeline.h"
#include "import_pipeline.h"
#include <rx_asset/geometry_pool.h>
#include <rx_asset/texture_cache.h>
#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <rx_task/scheduler.h>
#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <meshoptimizer.h>
#include <draco/compression/decode.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

namespace rx::asset {

namespace {

// ---------------------------------------------------------------------
// Extensions enabled in the Parser -- see import_gltf.h's own comment:
// enabling every extension fastgltf itself SUPPORTS (this build's
// FASTGLTF_ENABLE_* CMake gates are all off, so no experimental/
// deprecated bits exist to reference here) is what makes
// Error::MissingExtensions unreachable for anything fastgltf knows how
// to parse -- an extension this project doesn't actually CONSUME still
// gets its typed struct populated for the generic log-don't-drop
// mechanism below to report on, and a file whose extensionsRequired
// names something fastgltf has genuinely never heard of is exactly what
// produces Error::UnknownRequiredExtension [matrix-issue02 §2C mechanism
// row].
constexpr fastgltf::Extensions kEnabledExtensions =
    fastgltf::Extensions::KHR_texture_transform | fastgltf::Extensions::KHR_texture_basisu |
    fastgltf::Extensions::MSFT_texture_dds | fastgltf::Extensions::KHR_mesh_quantization |
    fastgltf::Extensions::EXT_meshopt_compression | fastgltf::Extensions::KHR_lights_punctual |
    fastgltf::Extensions::EXT_texture_webp | fastgltf::Extensions::KHR_materials_specular |
    fastgltf::Extensions::KHR_materials_ior | fastgltf::Extensions::KHR_materials_iridescence |
    fastgltf::Extensions::KHR_materials_volume | fastgltf::Extensions::KHR_materials_transmission |
    fastgltf::Extensions::KHR_materials_clearcoat | fastgltf::Extensions::KHR_materials_emissive_strength |
    fastgltf::Extensions::KHR_materials_sheen | fastgltf::Extensions::KHR_materials_unlit |
    fastgltf::Extensions::KHR_materials_anisotropy | fastgltf::Extensions::EXT_mesh_gpu_instancing |
    fastgltf::Extensions::MSFT_packing_normalRoughnessMetallic | fastgltf::Extensions::MSFT_packing_occlusionRoughnessMetallic |
    fastgltf::Extensions::KHR_materials_dispersion | fastgltf::Extensions::KHR_materials_variants |
    fastgltf::Extensions::KHR_draco_mesh_compression | fastgltf::Extensions::GODOT_single_root |
    fastgltf::Extensions::KHR_materials_diffuse_transmission;

// ---------------------------------------------------------------------
// Generic extensionsUsed/Required surfacing [matrix-issue02 §2C
// mechanism row -- "this one mechanism is what makes log-don't-drop true
// for ALL current and future registry entries"]. Extensions this task
// actually implements get an explicit tag; anything else present in a
// file's extensionsUsed defaults to "logged" -- correct for the ~40
// remaining registry entries (MPEG_*/ADOBE_*/AGI_*/KHR_animation_pointer/
// etc.) without naming every one of them here.
std::string_view extensionDisposition(std::string_view name) {
    static const std::unordered_map<std::string_view, std::string_view> kKnown = {
        {"KHR_texture_transform", "consumed(offset/scale)/logged(rotation)"},
        {"KHR_texture_basisu", "consumed(routed to Task 14)"},
        {"KHR_mesh_quantization", "consumed"},
        {"EXT_meshopt_compression", "consumed(decoded)"},
        {"KHR_lights_punctual", "preserved"},
        {"KHR_materials_unlit", "consumed(maps to Unlit)"},
        {"EXT_mesh_gpu_instancing", "consumed(expanded at import)"},
        {"KHR_draco_mesh_compression", "consumed(decoded)"},
        {"KHR_materials_emissive_strength", "logged(value carried in parameter set)"},
    };
    if (auto it = kKnown.find(name); it != kKnown.end()) {
        return it->second;
    }
    return "logged";
}

// ---------------------------------------------------------------------
// `extras` application JSON [log-don't-drop, ruled; matrix-issue02 §2A
// "extras" row]. fastgltf does not store extras itself ("you are
// expected to store any data from extras yourself" -- docs/guides.rst,
// verified directly); detection is via `Parser::setExtrasParseCallback`
// + `setUserPointer`. This importer does not yet PRESERVE extras for a
// host to read (registry N3: "SDK-phase ABI projection" -- a future
// task's deliverable) -- it only detects and logs their presence, which
// is the whole of what log-don't-drop requires for this row: "detection
// requires... the callback"; storage is explicitly out of scope here.
// ---------------------------------------------------------------------
std::string_view categoryName(fastgltf::Category category) {
    switch (category) {
        case fastgltf::Category::Nodes:
            return "nodes";
        case fastgltf::Category::Meshes:
            return "meshes";
        case fastgltf::Category::Materials:
            return "materials";
        case fastgltf::Category::Animations:
            return "animations";
        case fastgltf::Category::Cameras:
            return "cameras";
        case fastgltf::Category::Skins:
            return "skins";
        case fastgltf::Category::Scenes:
            return "scenes";
        case fastgltf::Category::Textures:
            return "textures";
        case fastgltf::Category::Images:
            return "images";
        case fastgltf::Category::Samplers:
            return "samplers";
        case fastgltf::Category::Buffers:
            return "buffers";
        case fastgltf::Category::BufferViews:
            return "bufferViews";
        case fastgltf::Category::Accessors:
            return "accessors";
        case fastgltf::Category::Asset:
            return "asset";
        default:
            return "other";
    }
}

using ExtrasCounts = std::unordered_map<fastgltf::Category, size_t>;

void onExtrasParsed(simdjson::dom::object* /*extras*/, std::size_t /*objectIndex*/, fastgltf::Category objectType,
                     void* userPointer) {
    auto* counts = static_cast<ExtrasCounts*>(userPointer);
    (*counts)[objectType] += 1;
}

// ---------------------------------------------------------------------
// Byte-source-mediated buffer/image resolution [the IO-source
// abstraction invariant]. Resolves every fastgltf::sources::URI ITSELF
// through the injected ByteSource -- Options::LoadExternalBuffers/
// LoadExternalImages are NEVER set (see importGltfPipeline below).
//
// [Phase 4 Stage 1 Task 15 scope note] For the ASYNC import path, this
// resolution (including the ByteSource::read() calls it makes) runs on
// the SAME background worker thread as the rest of computeGltfImport()
// below, NOT on the dedicated IO thread runOnIoThread() otherwise owns --
// a deliberate, documented scope simplification (task-15-report.md's own
// deviations section has the full rationale): correctly staging fastgltf
// parsing itself (which determines WHICH buffer/image URIs even exist)
// across an IO-thread/worker-thread boundary while preserving
// ResolvedBuffers' own lazy-cached design was judged materially riskier,
// for materially less correctness benefit, than accepting that a worker
// thread occasionally blocks on a file read -- worker threads doing
// occasional blocking I/O is an ordinary, defensible engine pattern,
// whereas CPU decode work running on the one dedicated IO thread (the
// invariant this task's debug thread-id assert DOES enforce, strictly,
// around every real decode/transcode/tangent-generation/meshopt call
// below) would defeat that thread's entire purpose for every OTHER
// in-flight IO request. The dedicated IO thread is still exercised for
// real, for the one read every async import unconditionally needs before
// any of this can run at all: the main glTF/GLB document's own bytes
// (registry.cpp's async orchestration issues that one read via
// runOnIoThread() before ever calling computeGltfImport()).
// ---------------------------------------------------------------------

// Absolute or non-file-scheme (e.g. http/https) URIs are never handed to
// the byte source [matrix row "Absolute / http(s) URIs"] -- the CALLER
// logs a WARN and falls back; this only classifies.
bool isAbsoluteOrNetworkUri(const fastgltf::URI& uri) {
    if (!uri.isLocalPath()) {
        return true;  // has a scheme (http, https, data would already be sources::Array, etc.)
    }
    return std::filesystem::path(uri.path()).is_absolute();
}

// Reads and owns every Buffer's bytes exactly once, resolving
// sources::URI through `source`; sources::Array (base64 data URIs,
// pre-decoded by fastgltf) and sources::Vector are read in place with no
// extra copy needed beyond what fastgltf already allocated.
class ResolvedBuffers {
public:
    ResolvedBuffers(const fastgltf::Asset& asset, ByteSource& source) : asset_(asset), source_(source) {
        owned_.resize(asset.buffers.size());
    }

    // Returns the FULL byte span for buffer `index`, or an empty span
    // (logged) on failure. Never touches std::filesystem -- see the byte
    // source class comment.
    //
    // Called from EVERY worker chunk during import_gltf.cpp's
    // parallelFor fan-out (each primitive's accessor reads go through
    // this), so `bytes()` MUST be safe under concurrent calls for
    // DIFFERENT indices at minimum; this project's own D5 threading
    // posture does not require full arbitrary-index thread-safety here,
    // but the eager, sequential pre-resolution loop
    // resolveAllBuffersSequentially() below (called once, on the
    // calling/main thread, BEFORE the parallel section starts) is what
    // actually makes concurrent calls safe in practice: every index is
    // already cached by the time any worker thread calls bytes(), so
    // this method's own body never actually executes the mutating
    // first-resolve path concurrently -- see importGltfPipeline's own
    // call site for why the pre-pass exists (matrix-issue02 row 15's
    // per-primitive parallelism requirement would otherwise race this
    // vector).
    std::span<const std::byte> bytes(size_t index) {
        if (index >= asset_.buffers.size()) {
            return {};
        }
        if (resolved_.size() <= index) {
            resolved_.resize(asset_.buffers.size(), false);
        }
        if (resolved_[index]) {
            return owned_[index] ? std::span<const std::byte>(*owned_[index]) : std::span<const std::byte>{};
        }
        resolved_[index] = true;

        const fastgltf::Buffer& buffer = asset_.buffers[index];
        if (const auto* uri = std::get_if<fastgltf::sources::URI>(&buffer.data)) {
            if (isAbsoluteOrNetworkUri(uri->uri)) {
                RX_LOG_WARN("rx_asset: buffer {} references an absolute/network URI '{}' -- fallback (no bytes), never fetched",
                            index, uri->uri.string());
                return {};
            }
            auto readBytes = source_.read(std::string(uri->uri.string()));
            if (!readBytes) {
                RX_LOG_ERROR("rx_asset: buffer {} URI '{}' could not be read through the byte source", index, uri->uri.string());
                return {};
            }
            owned_[index] = std::move(readBytes);
            return std::span<const std::byte>(*owned_[index]);
        }
        // Array/Vector/ByteView all reference bytes fastgltf itself
        // already owns (inside `asset_`, which outlives this whole
        // import) -- copied into owned_[index] anyway, UNCONDITIONALLY,
        // so every branch caches identically and the early-return path
        // above (`resolved_[index]` true) is correct for every source
        // kind, not just sources::URI. [Fix, this task's own testing:
        // the ORIGINAL version of this method returned these spans
        // directly without caching them, which was fine on the FIRST
        // call for a given index but produced silent EMPTY spans (and a
        // downstream null-pointer crash inside fastgltf's own
        // copyFromAccessor) on every SUBSEQUENT call for the same
        // buffer index -- e.g. a GLB's embedded BIN-chunk buffer, read
        // once for POSITION and again for NORMAL/indices. Reproduced
        // directly via the in-memory .glb import test.]
        if (const auto* arr = std::get_if<fastgltf::sources::Array>(&buffer.data)) {
            owned_[index] = std::vector<std::byte>(arr->bytes.begin(), arr->bytes.end());
            return std::span<const std::byte>(*owned_[index]);
        }
        if (const auto* vec = std::get_if<fastgltf::sources::Vector>(&buffer.data)) {
            owned_[index] = vec->bytes;
            return std::span<const std::byte>(*owned_[index]);
        }
        if (const auto* bv = std::get_if<fastgltf::sources::ByteView>(&buffer.data)) {
            owned_[index] = std::vector<std::byte>(bv->bytes.begin(), bv->bytes.end());
            return std::span<const std::byte>(*owned_[index]);
        }
        // sources::Fallback / monostate / BufferView (buffers never hold
        // BufferView per fastgltf's own DataSource comment) -- nothing to
        // read.
        return {};
    }

    // Sequential pre-pass: resolves EVERY buffer's bytes once, on
    // whichever thread calls this (importGltfPipeline calls it on the
    // main thread, before any parallelFor fan-out) -- see bytes()'s own
    // comment for why this is what makes later concurrent bytes() calls
    // (from worker chunks, for DIFFERENT already-cached indices) safe.
    void resolveAllBuffersSequentially() {
        for (size_t i = 0; i < asset_.buffers.size(); ++i) {
            bytes(i);
        }
    }

private:
    const fastgltf::Asset& asset_;
    ByteSource& source_;
    std::vector<std::optional<std::vector<std::byte>>> owned_;
    std::vector<bool> resolved_;
};

// [Task 14, matrix-issue03 "KHR_texture_basisu wiring from import" row]
// Resolves ONE glTF `images[]` entry's full byte content through `source`
// -- the SAME three consume-now source kinds `ResolvedBuffers::bytes()`
// above already handles for buffers (external URI / inline data-URI /
// GLB bufferView, gate ruling C3), just for `fastgltf::Image::data`
// instead of `fastgltf::Buffer::data`. Unlike ResolvedBuffers, this is
// NOT cached across calls -- an image is resolved at most once per
// material-texture-slot reference during this whole import (no per-
// primitive fan-out ever touches an image the way accessor reads touch
// buffers), so the extra bookkeeping a cache would need has no payoff
// here.
std::optional<std::vector<std::byte>> resolveImageBytes(const fastgltf::Asset& asset, size_t imageIndex,
                                                          ResolvedBuffers& buffers, ByteSource& source) {
    if (imageIndex >= asset.images.size()) {
        return std::nullopt;
    }
    const fastgltf::Image& image = asset.images[imageIndex];

    if (const auto* uri = std::get_if<fastgltf::sources::URI>(&image.data)) {
        if (isAbsoluteOrNetworkUri(uri->uri)) {
            RX_LOG_WARN("rx_asset: image {} references an absolute/network URI '{}' -- fallback (no bytes), never fetched",
                        imageIndex, uri->uri.string());
            return std::nullopt;
        }
        return source.read(std::string(uri->uri.string()));
    }
    if (const auto* arr = std::get_if<fastgltf::sources::Array>(&image.data)) {
        return std::vector<std::byte>(arr->bytes.begin(), arr->bytes.end());
    }
    if (const auto* vec = std::get_if<fastgltf::sources::Vector>(&image.data)) {
        return vec->bytes;
    }
    if (const auto* bv = std::get_if<fastgltf::sources::ByteView>(&image.data)) {
        return std::vector<std::byte>(bv->bytes.begin(), bv->bytes.end());
    }
    if (const auto* bufView = std::get_if<fastgltf::sources::BufferView>(&image.data)) {
        // GLB-embedded / KHR_texture_basisu images: the overwhelmingly
        // common non-URI case (gltfpack, toktx --outfile-with-basisu-
        // style toolchains, and every GLB this task's own fixtures use).
        if (bufView->bufferViewIndex >= asset.bufferViews.size()) {
            return std::nullopt;
        }
        const fastgltf::BufferView& view = asset.bufferViews[bufView->bufferViewIndex];
        std::span<const std::byte> full = buffers.bytes(view.bufferIndex);
        if (full.size() < view.byteOffset + view.byteLength) {
            return std::nullopt;
        }
        std::span<const std::byte> sub = full.subspan(view.byteOffset, view.byteLength);
        return std::vector<std::byte>(sub.begin(), sub.end());
    }
    // sources::CustomBuffer/Fallback/monostate -- nothing to read.
    return std::nullopt;
}

// ---------------------------------------------------------------------
// EXT_meshopt_compression decode [matrix-issue02 §2C row]: pre-decodes
// every meshoptCompression-flagged bufferView ONCE, before any accessor
// touches it, into a plain owned byte buffer. Mode -> function map per
// the extension's own metadata (CompressedBufferView::mode).
// ---------------------------------------------------------------------
class DecodedMeshoptViews {
public:
    DecodedMeshoptViews(const fastgltf::Asset& asset, ResolvedBuffers& buffers) {
        decoded_.resize(asset.bufferViews.size());
        for (size_t i = 0; i < asset.bufferViews.size(); ++i) {
            const fastgltf::BufferView& view = asset.bufferViews[i];
            if (!view.meshoptCompression) {
                continue;
            }
            const fastgltf::CompressedBufferView& mc = *view.meshoptCompression;
            std::span<const std::byte> src = buffers.bytes(mc.bufferIndex);
            if (src.size() < mc.byteOffset + mc.byteLength) {
                RX_LOG_ERROR("rx_asset: EXT_meshopt_compression bufferView {} references out-of-range compressed bytes", i);
                continue;
            }
            src = src.subspan(mc.byteOffset, mc.byteLength);

            std::vector<std::byte> dst(mc.count * mc.byteStride);
            int rc = -1;
            switch (mc.mode) {
                case fastgltf::MeshoptCompressionMode::Attributes:
                    rc = meshopt_decodeVertexBuffer(dst.data(), mc.count, mc.byteStride,
                                                     reinterpret_cast<const unsigned char*>(src.data()), src.size());
                    if (rc == 0) {
                        switch (mc.filter) {
                            case fastgltf::MeshoptCompressionFilter::Octahedral:
                                meshopt_decodeFilterOct(dst.data(), mc.count, mc.byteStride);
                                break;
                            case fastgltf::MeshoptCompressionFilter::Quaternion:
                                meshopt_decodeFilterQuat(dst.data(), mc.count, mc.byteStride);
                                break;
                            case fastgltf::MeshoptCompressionFilter::Exponential:
                                meshopt_decodeFilterExp(dst.data(), mc.count, mc.byteStride);
                                break;
                            case fastgltf::MeshoptCompressionFilter::None:
                                break;
                        }
                    }
                    break;
                case fastgltf::MeshoptCompressionMode::Triangles:
                    rc = meshopt_decodeIndexBuffer(dst.data(), mc.count, mc.byteStride,
                                                    reinterpret_cast<const unsigned char*>(src.data()), src.size());
                    break;
                case fastgltf::MeshoptCompressionMode::Indices:
                    rc = meshopt_decodeIndexSequence(dst.data(), mc.count, mc.byteStride,
                                                      reinterpret_cast<const unsigned char*>(src.data()), src.size());
                    break;
            }
            if (rc != 0) {
                RX_LOG_ERROR("rx_asset: EXT_meshopt_compression decode failed for bufferView {} (mode={})", i,
                             static_cast<int>(mc.mode));
                continue;
            }
            decoded_[i] = std::move(dst);
        }
    }

    [[nodiscard]] const std::vector<std::byte>* find(size_t bufferViewIndex) const {
        if (bufferViewIndex >= decoded_.size() || !decoded_[bufferViewIndex]) {
            return nullptr;
        }
        return &*decoded_[bufferViewIndex];
    }

private:
    std::vector<std::optional<std::vector<std::byte>>> decoded_;
};

// Custom BufferDataAdapter [fastgltf/tools.hpp's own extension seam --
// DefaultBufferDataAdapter's doc comment documents exactly this
// customization point]: transparently substitutes decoded
// EXT_meshopt_compression bytes for a flagged bufferView, and resolves
// every ordinary bufferView's buffer through the byte source otherwise.
// This one adapter, passed to every fastgltf accessor-tools call in this
// file, is what makes sparse accessors, `normalized` denormalization,
// and KHR_mesh_quantization dequantization "free" [gate matrix rows]:
// none of that logic lives in this project's code at all -- it is
// entirely inside copyFromAccessor/iterateAccessor, which this adapter
// only supplies raw bytes to.
struct ImportBufferDataAdapter {
    const fastgltf::Asset& asset;
    ResolvedBuffers& buffers;
    const DecodedMeshoptViews& meshoptViews;
    // [Fix, this task's own testing] An unresolvable bufferView (e.g. an
    // absolute/network URI buffer, matrix row "Absolute / http(s)
    // URIs" -- never fetched) previously made this operator() return an
    // EMPTY span, which crashed (SIGSEGV) the moment fastgltf's own
    // accessor tools indexed past it -- fastgltf does no bounds-checking
    // of its own against a short adapter result. Zero-filling to the
    // FULL declared byteLength instead keeps every read in-bounds and
    // well-defined; the resulting geometry degrades to zero-valued (an
    // all-origin degenerate primitive, itself rejected/skipped by
    // gltf_pipeline.cpp's own zero-vertex/non-finite guards where
    // applicable) rather than crashing -- the WARN naming the URI is
    // already logged at the point resolution failed (ResolvedBuffers::
    // bytes()). `static thread_local` (NOT a member of this struct):
    // this adapter is shared by reference across every parallelFor
    // worker chunk (D5/matrix row 15), and a plain member would race
    // concurrent calls needing different fallback sizes on different
    // threads.
    std::span<const std::byte> operator()(const fastgltf::Asset&, size_t bufferViewIndex) const {
        const fastgltf::BufferView& view = asset.bufferViews[bufferViewIndex];
        if (const std::vector<std::byte>* decoded = meshoptViews.find(bufferViewIndex)) {
            return std::span<const std::byte>(*decoded);
        }
        std::span<const std::byte> full = buffers.bytes(view.bufferIndex);
        if (full.size() < view.byteOffset + view.byteLength) {
            static thread_local std::vector<std::byte> zeroFallback;
            zeroFallback.assign(view.byteLength, std::byte{0});
            return std::span<const std::byte>(zeroFallback);
        }
        return full.subspan(view.byteOffset, view.byteLength);
    }
};

// [Upstream bug worked around, this task -- verified directly against
// the pinned fastgltf v0.9.0 source] `fastgltf::copyFromAccessor<T>`'s
// own per-element fallback path (tools.hpp, the branch taken whenever
// `accessor.normalized` is true, since its bulk-memcpy fast path is
// itself gated on `!accessor.normalized`) calls
// `internal::getAccessorElementAt<ElementType>(accessor.componentType,
// bytes)` WITHOUT forwarding `accessor.normalized` at all -- that
// function's `normalized` parameter defaults to `false`, so
// copyFromAccessor silently returns RAW, un-denormalized integer values
// reinterpreted as floats for any `normalized:true` accessor (reproduced
// directly: a normalized-SHORT KHR_mesh_quantization fixture returned
// -16384.0F instead of ~-0.5F). Every OTHER accessor-tools entry point
// this file could use (`iterateAccessor`/`getAccessorElement`/the sparse
// substitution path) correctly forwards `accessor.normalized` at their
// own equivalent call sites (verified directly, same file) -- only
// copyFromAccessor's fallback branch drops it. Fix: read through
// `iterateAccessor<T>` instead, which is unaffected and still exactly
// "never a raw byte poke" (a public fastgltf accessor tool, just a
// different one) -- collecting its range into a vector costs one extra
// per-element move over copyFromAccessor's bulk-copy fast path, which is
// not itself reachable for normalized data anyway.
// SECOND upstream issue, also worked around here: the RANGE-returning
// `iterateAccessor<T>` overload (the one a range-for loop's begin()/
// end() would use) constructs an `IterableAccessor`, whose constructor
// unconditionally dereferences `*accessor.bufferViewIndex` with NO
// `has_value()` check first -- for an accessor with no base bufferView
// at all (legal per the glTF spec for a fully sparse-substituted
// accessor; the spec text is "accessor.bufferView ... When undefined,
// the accessor MUST be initialized with zeros" -- this task's own
// cube_sparse.gltf fixture uses exactly this pattern), that dereference
// is undefined behavior -- reproduced directly as an intermittent
// SIGSEGV (heap-garbage-dependent) inside the sparse-accessor import
// test. The FUNCTOR overload used below is a separately-implemented
// function (never constructs an IterableAccessor) that explicitly
// branches on `accessor.bufferViewIndex` before dereferencing it
// (verified directly, same file) -- using this one specific overload
// avoids both this bug and the normalized-propagation bug above.
template <typename ElementType>
std::vector<ElementType> readAccessor(const fastgltf::Asset& asset, const fastgltf::Accessor& accessor,
                                       const ImportBufferDataAdapter& adapter) {
    std::vector<ElementType> out;
    out.reserve(accessor.count);
    fastgltf::iterateAccessor<ElementType>(
        asset, accessor, [&](ElementType v) { out.push_back(std::move(v)); }, adapter);
    return out;
}

// ---------------------------------------------------------------------
// KHR_draco_mesh_compression decode [matrix-issue02 §2B row]. Returns
// deindexed positions/normals/uvs/tangents (parallel arrays, one entry
// per Draco POINT -- Draco already welds identical points, matching
// exactly the shape an ordinary accessor-based primitive's
// (positions[], indices[]) pair has) plus the triangle index list.
// ---------------------------------------------------------------------
struct DracoDecodeResult {
    bool ok = false;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<glm::vec4> tangents;
    std::vector<uint32_t> indices;
};

DracoDecodeResult decodeDracoPrimitive(const fastgltf::Asset& asset, const fastgltf::Primitive& primitive,
                                        ResolvedBuffers& buffers) {
    DracoDecodeResult result;
    const fastgltf::DracoCompressedPrimitive& draco = *primitive.dracoCompression;
    const fastgltf::BufferView& view = asset.bufferViews[draco.bufferView];
    std::span<const std::byte> full = buffers.bytes(view.bufferIndex);
    if (full.size() < view.byteOffset + view.byteLength) {
        RX_LOG_ERROR("rx_asset: KHR_draco_mesh_compression bufferView {} references out-of-range bytes", draco.bufferView);
        return result;
    }
    std::span<const std::byte> compressed = full.subspan(view.byteOffset, view.byteLength);

    draco::DecoderBuffer decoderBuffer;
    decoderBuffer.Init(reinterpret_cast<const char*>(compressed.data()), compressed.size());

    draco::Decoder decoder;
    auto statusOr = decoder.DecodeMeshFromBuffer(&decoderBuffer);
    if (!statusOr.ok() || statusOr.value() == nullptr) {
        RX_LOG_ERROR("rx_asset: Draco decode failed: {}", statusOr.ok() ? "null mesh" : statusOr.status().error_msg());
        return result;
    }
    std::unique_ptr<draco::Mesh> mesh = std::move(statusOr).value();

    const auto findAttr = [&](std::string_view name) -> const draco::PointAttribute* {
        auto it = draco.findAttribute(name);
        if (it == draco.attributes.end()) {
            return nullptr;
        }
        const int32_t attId = static_cast<int32_t>(it->accessorIndex);  // holds the DRACO attribute id in this context, not a glTF accessor index
        if (attId < 0 || attId >= mesh->num_attributes()) {
            return nullptr;
        }
        return mesh->attribute(attId);
    };

    const size_t numPoints = mesh->num_points();
    const draco::PointAttribute* posAttr = findAttr("POSITION");
    if (posAttr == nullptr) {
        RX_LOG_ERROR("rx_asset: Draco-compressed primitive has no POSITION attribute");
        return result;
    }
    const draco::PointAttribute* normAttr = findAttr("NORMAL");
    const draco::PointAttribute* uvAttr = findAttr("TEXCOORD_0");
    const draco::PointAttribute* tanAttr = findAttr("TANGENT");

    result.positions.resize(numPoints);
    result.normals.resize(numPoints);
    result.uvs.resize(numPoints);
    result.tangents.resize(numPoints);
    for (size_t p = 0; p < numPoints; ++p) {
        const draco::PointIndex pi(static_cast<uint32_t>(p));
        float pos[3] = {0, 0, 0};
        posAttr->ConvertValue<float, 3>(posAttr->mapped_index(pi), pos);
        result.positions[p] = glm::vec3(pos[0], pos[1], pos[2]);

        if (normAttr != nullptr) {
            float n[3] = {0, 0, 1};
            normAttr->ConvertValue<float, 3>(normAttr->mapped_index(pi), n);
            result.normals[p] = glm::vec3(n[0], n[1], n[2]);
        }
        if (uvAttr != nullptr) {
            float uv[2] = {0, 0};
            uvAttr->ConvertValue<float, 2>(uvAttr->mapped_index(pi), uv);
            result.uvs[p] = glm::vec2(uv[0], uv[1]);
        }
        if (tanAttr != nullptr) {
            float t[4] = {1, 0, 0, 1};
            tanAttr->ConvertValue<float, 4>(tanAttr->mapped_index(pi), t);
            result.tangents[p] = glm::vec4(t[0], t[1], t[2], t[3]);
        }
    }
    if (normAttr == nullptr) {
        result.normals.clear();
    }
    if (uvAttr == nullptr) {
        result.uvs.clear();
    }
    if (tanAttr == nullptr) {
        result.tangents.clear();
    }

    result.indices.resize(static_cast<size_t>(mesh->num_faces()) * 3);
    for (draco::FaceIndex f(0); f < mesh->num_faces(); ++f) {
        const draco::Mesh::Face& face = mesh->face(f);
        result.indices[static_cast<size_t>(f.value()) * 3 + 0] = face[0].value();
        result.indices[static_cast<size_t>(f.value()) * 3 + 1] = face[1].value();
        result.indices[static_cast<size_t>(f.value()) * 3 + 2] = face[2].value();
    }

    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------
// D12 node-graph flattening
// ---------------------------------------------------------------------

glm::mat4 nodeLocalTransform(const fastgltf::Node& node) {
    if (const auto* matrix = std::get_if<fastgltf::math::fmat4x4>(&node.transform)) {
        // Both fastgltf::math::mat and glm::mat4 are column-major with an
        // identical `m[col][row]` indexing convention (verified directly
        // against fastgltf's math.hpp) -- explicit element-by-element
        // construction avoids any type-punning/layout assumption a raw
        // memcpy across two unrelated library types would carry.
        glm::mat4 m;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                m[c][r] = (*matrix)[c][r];
            }
        }
        return m;
    }
    const auto& trs = std::get<fastgltf::TRS>(node.transform);
    const glm::vec3 t(trs.translation.x(), trs.translation.y(), trs.translation.z());
    const glm::quat r(trs.rotation.w(), trs.rotation.x(), trs.rotation.y(), trs.rotation.z());
    const glm::vec3 s(trs.scale.x(), trs.scale.y(), trs.scale.z());
    return glm::translate(glm::mat4(1.0F), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0F), s);
}

bool hasNegativeDeterminant(const glm::mat4& m) {
    const glm::mat3 upper3x3(m);
    return glm::determinant(upper3x3) < 0.0F;
}

// [Task 15] Maps a MaterialTextureSlot to the corresponding TextureRef
// field on a MaterialAsset -- the ONE place that mapping is spelled out,
// shared by every caller that needs to patch a slot's handle after
// registerDecoded() (import_gltf.cpp's own compute/marshal code, never
// exposed outside this file).
TextureRef& textureRefForSlot(MaterialAsset& mat, MaterialTextureSlot slot) {
    switch (slot) {
        case MaterialTextureSlot::BaseColor:
            return mat.baseColorTexture;
        case MaterialTextureSlot::MetallicRoughness:
            return mat.metallicRoughnessTexture;
        case MaterialTextureSlot::Normal:
            return mat.normalTexture;
        case MaterialTextureSlot::Occlusion:
            return mat.occlusionTexture;
        case MaterialTextureSlot::Emissive:
        case MaterialTextureSlot::Count:
        default:
            return mat.emissiveTexture;
    }
}

void setStage(std::atomic<ImportStage>* stageOut, ImportStage stage) {
    if (stageOut != nullptr) {
        stageOut->store(stage, std::memory_order_release);
    }
}

bool isCancelled(const std::atomic<bool>* cancelled) {
    return cancelled != nullptr && cancelled->load(std::memory_order_acquire);
}

}  // namespace

// ---------------------------------------------------------------------
// The compute-phase entry point [Task 15] -- see import_pipeline.h's own
// top comment for the full compute/marshal split rationale.
// ---------------------------------------------------------------------

TextureFallbackHandles snapshotTextureFallbackHandles(TextureCache* textures) {
    TextureFallbackHandles snapshot;
    if (textures == nullptr) {
        return snapshot;
    }
    snapshot.checkerboard = textures->checkerboardHandle();
    for (size_t i = 0; i < snapshot.byRole.size(); ++i) {
        snapshot.byRole[i] = textures->fallbackHandle(static_cast<TextureRole>(i));
    }
    return snapshot;
}

ImportComputeResult computeGltfImport(std::span<const std::byte> documentBytes, ByteSource& source,
                                       rx::task::Scheduler& scheduler, TextureCache* textures,
                                       TextureHandle fallbackTextureHandleForUncached,
                                       const TextureFallbackHandles& textureFallbacks,
                                       const std::atomic<bool>* cancelled, std::atomic<uint32_t>* itemsCompleted,
                                       std::atomic<uint32_t>* itemsTotalOut, std::atomic<ImportStage>* stageOut) {
    // [Task 15] Tracy zone coverage -- one named zone for the whole
    // compute phase (this function runs on a worker thread for the async
    // path, capturing exactly the "off-main-thread decode work" span a
    // manual capture is meant to show) plus one per major stage below
    // (parse, materials/texture-decode, primitives). See
    // task-15-report.md's own MANUAL_VERIFICATION section for the capture
    // procedure -- code presence + procedure, not CI-gated (D18).
    RX_ZONE_NAMED("computeGltfImport");
    ImportComputeResult result;
    setStage(stageOut, ImportStage::Parsing);

    auto dataBuffer = fastgltf::GltfDataBuffer::FromBytes(reinterpret_cast<const std::byte*>(documentBytes.data()),
                                                            documentBytes.size());
    if (dataBuffer.error() != fastgltf::Error::None) {
        result.error = mapFastgltfError(dataBuffer.error());
        RX_LOG_ERROR("rx_asset: importGltf: failed to wrap document bytes: {}", importErrorName(result.error));
        setStage(stageOut, ImportStage::Failed);
        return result;
    }

    fastgltf::Parser parser(kEnabledExtensions);
    ExtrasCounts extrasCounts;
    parser.setUserPointer(&extrasCounts);
    parser.setExtrasParseCallback(&onExtrasParsed);
    // NEVER LoadExternalBuffers/LoadExternalImages -- the IO-source
    // abstraction invariant [byte_source.h]. Every sources::URI this
    // parse produces is resolved by THIS file, through `source`, never
    // by fastgltf's own std::filesystem-backed loadFileFromUri.
    constexpr fastgltf::Options options = fastgltf::Options::None;
    auto assetExpected = parser.loadGltf(dataBuffer.get(), std::filesystem::path{}, options, fastgltf::Category::All);
    if (assetExpected.error() != fastgltf::Error::None) {
        result.error = mapFastgltfError(assetExpected.error());
        RX_LOG_ERROR("rx_asset: importGltf: parse failed: {} ({})", importErrorName(result.error),
                     fastgltf::getErrorMessage(assetExpected.error()));
        setStage(stageOut, ImportStage::Failed);
        return result;
    }
    fastgltf::Asset asset = std::move(assetExpected.get());

    // ---- Generic extensionsUsed/Required surfacing [log-don't-drop mechanism] ----
    if (!asset.extensionsUsed.empty()) {
        std::string summary;
        for (const auto& name : asset.extensionsUsed) {
            if (!summary.empty()) {
                summary += ", ";
            }
            summary += std::string(name) + "=" + std::string(extensionDisposition(name));
        }
        RX_LOG_INFO("rx_asset: importGltf: extensionsUsed [{}]", summary);
    }
    // extensionsRequired containing anything fastgltf could not parse at
    // all already failed above (Error::UnknownRequiredExtension); every
    // entry that reaches this point was satisfiable.

    // ---- `extras` presence surfacing [log-don't-drop] ----
    if (!extrasCounts.empty()) {
        std::string summary;
        for (const auto& [category, count] : extrasCounts) {
            if (!summary.empty()) {
                summary += ", ";
            }
            summary += std::string(categoryName(category)) + "=" + std::to_string(count);
        }
        RX_LOG_INFO("rx_asset: importGltf: extras present [{}] (detected, not yet preserved for host use -- N3)",
                     summary);
    }

    if (isCancelled(cancelled)) {
        result.cancelled = true;
        return result;
    }

    ResolvedBuffers buffers(asset, source);
    // Sequential pre-pass [D5/matrix-issue02 row 15]: every buffer is
    // resolved once, HERE, on whichever thread called this function --
    // see ResolvedBuffers::bytes()'s own comment for why this is required
    // before the parallelFor fan-out below touches the same cache from
    // multiple worker threads. [Task 15 scope note -- see this file's own
    // anonymous-namespace comment above ResolvedBuffers] this is real
    // ByteSource I/O, run on whichever thread computeGltfImport() itself
    // runs on (a background worker for the async path) -- NOT the
    // dedicated IO thread, a documented scope simplification.
    buffers.resolveAllBuffersSequentially();
    DecodedMeshoptViews meshoptViews(asset, buffers);
    ImportBufferDataAdapter adapter{asset, buffers, meshoptViews};

    setStage(stageOut, ImportStage::Decoding);

    // ================= Materials =================
    result.materials.resize(asset.materials.size());
    // grain=1: material count is typically small (dozens, not thousands)
    // and each unit of work (a handful of texture transcodes) is
    // meaningfully heavier than autoGrainSize()'s own floor would assume
    // -- explicit grain=1 fans every material out to its own chunk,
    // matching this project's own established "measurement affordance"
    // carve-out (scheduler.h) for a workload shape autoGrainSize() was
    // not designed around.
    std::mutex materialLogMutex;
    RX_PLOT("async import: materials", static_cast<int64_t>(asset.materials.size()));
    scheduler.parallelFor(static_cast<uint32_t>(asset.materials.size()), 1,
                           [&](uint32_t begin, uint32_t end, uint32_t) {
        RX_ZONE_NAMED("material texture decode chunk");
        for (uint32_t mi = begin; mi < end; ++mi) {
            const fastgltf::Material& src = asset.materials[mi];
            PendingMaterialCompute& pendingMat = result.materials[mi];
            MaterialAsset& dst = pendingMat.asset;
            dst.name = std::string(src.name);
            dst.disposition = src.unlit ? MaterialDisposition::Unlit : MaterialDisposition::StandardPBR;  // [gate ruling C5]

            dst.baseColorFactor = glm::vec4(src.pbrData.baseColorFactor[0], src.pbrData.baseColorFactor[1],
                                             src.pbrData.baseColorFactor[2], src.pbrData.baseColorFactor[3]);
            dst.metallicFactor = src.pbrData.metallicFactor;
            dst.roughnessFactor = src.pbrData.roughnessFactor;
            dst.emissiveFactor = glm::vec3(src.emissiveFactor[0], src.emissiveFactor[1], src.emissiveFactor[2]);
            dst.emissiveStrength = src.emissiveStrength;
            dst.alphaCutoff = src.alphaCutoff;
            dst.doubleSided = src.doubleSided;
            switch (src.alphaMode) {
                case fastgltf::AlphaMode::Opaque:
                    dst.alphaMode = AlphaMode::Opaque;
                    break;
                case fastgltf::AlphaMode::Mask:
                    dst.alphaMode = AlphaMode::Mask;
                    break;
                case fastgltf::AlphaMode::Blend:
                    dst.alphaMode = AlphaMode::Blend;
                    break;
            }
            if (src.normalTexture) {
                dst.normalScale = src.normalTexture->scale;
            }
            if (src.occlusionTexture) {
                dst.occlusionStrength = src.occlusionTexture->strength;
            }

            // [D24/D11] Slot-driven texture resolution -- WORKER-SAFE
            // decode only (TextureCache::decodeForUpload()); GPU
            // registration is deferred to marshal time (main thread).
            const auto fillRef = [&](TextureRef& ref, PendingTextureLoad& pending, const fastgltf::TextureInfo* info,
                                      TextureRole role) {
                if (info == nullptr) {
                    // [D11] UNBOUND slot -- final value already, no
                    // texture-cache round-trip needed at all. Reads the
                    // main-thread-snapshotted value (textureFallbacks),
                    // NEVER TextureCache::fallbackHandle() directly --
                    // see TextureFallbackHandles' own comment
                    // (import_pipeline.h) for why: that accessor is
                    // RX_ASSERT_MAIN_THREAD-guarded and this lambda runs
                    // on a worker thread for the async path.
                    if (textures != nullptr) {
                        ref.handle = textureFallbacks.byRole[static_cast<size_t>(role)];
                    }
                    return;
                }
                ref.present = true;
                ref.handle = fallbackTextureHandleForUncached;  // [D11] Task-13 baseline -- the textures==nullptr contract, unchanged
                if (textures != nullptr) {
                    ref.handle = textureFallbacks.checkerboard;  // see the comment just above
                }
                if (info->textureIndex < asset.textures.size()) {
                    const fastgltf::Texture& tex = asset.textures[info->textureIndex];
                    std::optional<size_t> effectiveImageIndex = tex.basisuImageIndex ? tex.basisuImageIndex : tex.imageIndex;
                    if (effectiveImageIndex) {
                        ref.imageIndex = static_cast<uint32_t>(*effectiveImageIndex);
                    }
                    if (tex.samplerIndex && *tex.samplerIndex < asset.samplers.size()) {
                        const fastgltf::Sampler& s = asset.samplers[*tex.samplerIndex];
                        ref.sampler.wrapS = static_cast<uint32_t>(s.wrapS);
                        ref.sampler.wrapT = static_cast<uint32_t>(s.wrapT);
                        ref.sampler.magFilter = s.magFilter ? static_cast<uint32_t>(*s.magFilter) : 0U;
                        ref.sampler.minFilter = s.minFilter ? static_cast<uint32_t>(*s.minFilter) : 0U;
                    }

                    if (textures != nullptr && effectiveImageIndex) {
                        auto bytes = resolveImageBytes(asset, *effectiveImageIndex, buffers, source);
                        if (bytes.has_value()) {
                            std::string debugName = dst.name.empty()
                                                         ? ("material#" + std::to_string(mi) + " " + textureRoleName(role))
                                                         : (dst.name + " " + textureRoleName(role));
                            // WORKER-SAFE decode ONLY -- see
                            // TextureCache::decodeForUpload()'s own
                            // comment. Registration happens at marshal
                            // time, main thread.
                            pending.attempted = true;
                            pending.role = role;
                            pending.debugName = std::move(debugName);
                            pending.decoded = textures->decodeForUpload(std::span<const std::byte>(*bytes), role);
                            if (itemsCompleted != nullptr) {
                                itemsCompleted->fetch_add(1, std::memory_order_relaxed);
                            }
                        } else {
                            std::lock_guard<std::mutex> lock(materialLogMutex);
                            RX_LOG_WARN(
                                "rx_asset: material '{}': {} texture (image #{}) bytes could not be resolved -- D11 fallback",
                                dst.name, textureRoleName(role), *effectiveImageIndex);
                        }
                    }
                }
                ref.texCoordIndex = static_cast<uint32_t>(info->texCoordIndex);
                if (info->transform) {
                    ref.uvOffset = glm::vec2(info->transform->uvOffset.x(), info->transform->uvOffset.y());
                    ref.uvScale = glm::vec2(info->transform->uvScale.x(), info->transform->uvScale.y());
                    if (info->transform->rotation != 0.0F) {
                        std::lock_guard<std::mutex> lock(materialLogMutex);
                        RX_LOG_WARN(
                            "rx_asset: material '{}': KHR_texture_transform rotation={} radians is not applied "
                            "(log-don't-drop; offset/scale are consumed)",
                            dst.name, info->transform->rotation);
                    }
                }
            };
            fillRef(dst.baseColorTexture, pendingMat.textureSlots[static_cast<size_t>(MaterialTextureSlot::BaseColor)],
                    src.pbrData.baseColorTexture ? &*src.pbrData.baseColorTexture : nullptr, TextureRole::BaseColor);
            fillRef(dst.metallicRoughnessTexture,
                    pendingMat.textureSlots[static_cast<size_t>(MaterialTextureSlot::MetallicRoughness)],
                    src.pbrData.metallicRoughnessTexture ? &*src.pbrData.metallicRoughnessTexture : nullptr,
                    TextureRole::MetallicRoughness);
            fillRef(dst.normalTexture, pendingMat.textureSlots[static_cast<size_t>(MaterialTextureSlot::Normal)],
                    src.normalTexture ? static_cast<const fastgltf::TextureInfo*>(&*src.normalTexture) : nullptr,
                    TextureRole::Normal);
            fillRef(dst.occlusionTexture, pendingMat.textureSlots[static_cast<size_t>(MaterialTextureSlot::Occlusion)],
                    src.occlusionTexture ? static_cast<const fastgltf::TextureInfo*>(&*src.occlusionTexture) : nullptr,
                    TextureRole::Occlusion);
            fillRef(dst.emissiveTexture, pendingMat.textureSlots[static_cast<size_t>(MaterialTextureSlot::Emissive)],
                    src.emissiveTexture ? &*src.emissiveTexture : nullptr, TextureRole::Emissive);

            const auto warnExt = [&](bool present, std::string_view extName, std::string_view extra = {}) {
                if (!present) {
                    return;
                }
                std::lock_guard<std::mutex> lock(materialLogMutex);
                if (extra.empty()) {
                    RX_LOG_WARN("rx_asset: material '{}': {} is parsed but not consumed (log-don't-drop)", dst.name, extName);
                } else {
                    RX_LOG_WARN("rx_asset: material '{}': {} is parsed but not consumed ({})", dst.name, extName, extra);
                }
            };
            if (src.emissiveStrength != 1.0F) {
                std::lock_guard<std::mutex> lock(materialLogMutex);
                RX_LOG_WARN("rx_asset: material '{}': KHR_materials_emissive_strength is parsed but not consumed (value={})",
                            dst.name, src.emissiveStrength);
            }
            warnExt(src.specular != nullptr, "KHR_materials_specular");
            warnExt(src.ior != 1.5F, "KHR_materials_ior");
            warnExt(src.iridescence != nullptr, "KHR_materials_iridescence");
            warnExt(src.volume != nullptr, "KHR_materials_volume");
            warnExt(src.transmission != nullptr, "KHR_materials_transmission", "material will render OPAQUE (transmission ignored)");
            warnExt(src.sheen != nullptr, "KHR_materials_sheen");
            warnExt(src.clearcoat != nullptr, "KHR_materials_clearcoat");
            warnExt(src.anisotropy != nullptr, "KHR_materials_anisotropy");
            warnExt(src.dispersion != 0.0F, "KHR_materials_dispersion");
            warnExt(src.diffuseTransmission != nullptr, "KHR_materials_diffuse_transmission");
        }
    });

    // ================= Meshes / primitives =================
    struct WorkItem {
        size_t meshIndex;
        size_t primitiveIndex;
    };
    std::vector<WorkItem> workItems;
    for (size_t mi = 0; mi < asset.meshes.size(); ++mi) {
        const fastgltf::Mesh& mesh = asset.meshes[mi];
        for (size_t pi = 0; pi < mesh.primitives.size(); ++pi) {
            const fastgltf::Primitive& prim = mesh.primitives[pi];
            if (prim.type != fastgltf::PrimitiveType::Triangles) {
                static const std::unordered_map<fastgltf::PrimitiveType, std::string_view> kModeNames = {
                    {fastgltf::PrimitiveType::Points, "POINTS"},
                    {fastgltf::PrimitiveType::Lines, "LINES"},
                    {fastgltf::PrimitiveType::LineLoop, "LINE_LOOP"},
                    {fastgltf::PrimitiveType::LineStrip, "LINE_STRIP"},
                    {fastgltf::PrimitiveType::TriangleStrip, "TRIANGLE_STRIP"},
                    {fastgltf::PrimitiveType::TriangleFan, "TRIANGLE_FAN"},
                };
                RX_LOG_WARN("rx_asset: mesh {} primitive {}: non-TRIANGLES mode ({}) is skipped (convert offline if needed)", mi, pi,
                            kModeNames.count(prim.type) ? kModeNames.at(prim.type) : "?");
                continue;
            }
            if (prim.findAttribute("POSITION") == prim.attributes.end()) {
                RX_LOG_WARN("rx_asset: mesh {} primitive {}: no POSITION attribute -- skipped", mi, pi);
                continue;
            }
            workItems.push_back({mi, pi});

            for (auto* extra : {"COLOR_0"}) {
                if (prim.findAttribute(extra) != prim.attributes.end()) {
                    RX_LOG_WARN("rx_asset: mesh {} primitive {}: attribute {} is present but not consumed", mi, pi, extra);
                }
            }
            for (uint32_t set = 1; set <= 4; ++set) {
                std::string name = "TEXCOORD_" + std::to_string(set);
                if (prim.findAttribute(name) != prim.attributes.end()) {
                    RX_LOG_WARN("rx_asset: mesh {} primitive {}: attribute {} is present but not consumed", mi, pi, name);
                }
                for (std::string prefix : {std::string("JOINTS_"), std::string("WEIGHTS_")}) {
                    std::string jn = prefix + std::to_string(set);
                    if (prim.findAttribute(jn) != prim.attributes.end()) {
                        RX_LOG_WARN("rx_asset: mesh {} primitive {}: attribute {} (set >= 1) is present but not consumed", mi, pi, jn);
                    }
                }
            }

            if (!prim.mappings.empty()) {
                RX_LOG_WARN(
                    "rx_asset: mesh {} primitive {}: KHR_materials_variants present ({} variant mapping(s)) -- not "
                    "consumed, the primitive's own default material is used for every variant",
                    mi, pi, prim.mappings.size());
            }
        }
    }

    result.itemsTotal = static_cast<uint32_t>(workItems.size());
    for (const auto& mat : result.materials) {
        for (const auto& slot : mat.textureSlots) {
            if (slot.attempted) {
                ++result.itemsTotal;
            }
        }
    }
    if (itemsTotalOut != nullptr) {
        itemsTotalOut->store(result.itemsTotal, std::memory_order_release);
    }

    setStage(stageOut, ImportStage::Optimizing);

    if (isCancelled(cancelled)) {
        result.cancelled = true;
        return result;
    }

    // pending[w] mirrors today's per-primitive processing exactly --
    // written by INDEX (never appended), so file order survives
    // regardless of worker completion order (the ordering-rule
    // determinism criterion).
    struct PendingPrimitive {
        size_t meshIndex = 0;
        PrimitiveOutput output;
        std::optional<size_t> materialSourceIndex;
        SkinVertexData skinVertices;
        std::vector<MorphTarget> morphTargets;
    };
    std::vector<PendingPrimitive> pending(workItems.size());
    std::mutex logMutex;
    RX_PLOT("async import: primitives", static_cast<int64_t>(workItems.size()));
    scheduler.parallelFor(static_cast<uint32_t>(workItems.size()), [&](uint32_t begin, uint32_t end, uint32_t) {
        RX_ZONE_NAMED("primitive decode/optimize chunk");
        for (uint32_t w = begin; w < end; ++w) {
            const size_t mi = workItems[w].meshIndex;
            const size_t pi = workItems[w].primitiveIndex;
            const fastgltf::Primitive& prim = asset.meshes[mi].primitives[pi];

            std::vector<glm::vec3> positions;
            std::vector<glm::vec3> normals;
            std::vector<glm::vec2> uvs;
            std::vector<glm::vec4> tangents;
            std::vector<uint32_t> indices;
            SkinVertexData skinVertices;
            std::vector<MorphTarget> morphTargets;

            if (prim.dracoCompression) {
                DracoDecodeResult draco = decodeDracoPrimitive(asset, prim, buffers);
                if (!draco.ok) {
                    continue;  // logged inside decodeDracoPrimitive(); pending[w].output.ok stays false
                }
                positions = std::move(draco.positions);
                normals = std::move(draco.normals);
                uvs = std::move(draco.uvs);
                tangents = std::move(draco.tangents);
                indices = std::move(draco.indices);
            } else {
                auto posAttr = prim.findAttribute("POSITION");
                const fastgltf::Accessor& posAccessor = asset.accessors[posAttr->accessorIndex];
                positions = readAccessor<glm::vec3>(asset, posAccessor, adapter);

                if (auto n = prim.findAttribute("NORMAL"); n != prim.attributes.end()) {
                    normals = readAccessor<glm::vec3>(asset, asset.accessors[n->accessorIndex], adapter);
                }
                if (auto uv = prim.findAttribute("TEXCOORD_0"); uv != prim.attributes.end()) {
                    uvs = readAccessor<glm::vec2>(asset, asset.accessors[uv->accessorIndex], adapter);
                }
                if (auto t = prim.findAttribute("TANGENT"); t != prim.attributes.end()) {
                    tangents = readAccessor<glm::vec4>(asset, asset.accessors[t->accessorIndex], adapter);
                }
                if (prim.indicesAccessor) {
                    indices = readAccessor<uint32_t>(asset, asset.accessors[*prim.indicesAccessor], adapter);
                }
                if (auto j = prim.findAttribute("JOINTS_0"); j != prim.attributes.end()) {
                    skinVertices.joints = readAccessor<glm::u16vec4>(asset, asset.accessors[j->accessorIndex], adapter);
                }
                if (auto wgt = prim.findAttribute("WEIGHTS_0"); wgt != prim.attributes.end()) {
                    skinVertices.weights = readAccessor<glm::vec4>(asset, asset.accessors[wgt->accessorIndex], adapter);
                }
                for (const auto& targetAttrs : prim.targets) {
                    MorphTarget target;
                    for (const auto& attr : targetAttrs) {
                        if (attr.name == "POSITION") {
                            target.positionDeltas = readAccessor<glm::vec3>(asset, asset.accessors[attr.accessorIndex], adapter);
                        } else if (attr.name == "NORMAL") {
                            target.normalDeltas = readAccessor<glm::vec3>(asset, asset.accessors[attr.accessorIndex], adapter);
                        } else if (attr.name == "TANGENT") {
                            target.tangentDeltas = readAccessor<glm::vec3>(asset, asset.accessors[attr.accessorIndex], adapter);
                        }
                    }
                    morphTargets.push_back(std::move(target));
                }
            }

            PrimitiveInput input;
            input.positions = positions;
            input.normals = normals;
            input.uvs = uvs;
            input.tangents = tangents;
            input.indices = indices;

            PendingPrimitive& out = pending[w];
            out.meshIndex = mi;
            out.output = processPrimitive(input);
            out.skinVertices = std::move(skinVertices);
            out.morphTargets = std::move(morphTargets);
            if (prim.materialIndex) {
                out.materialSourceIndex = *prim.materialIndex;
            }

            if (!out.output.ok) {
                std::lock_guard<std::mutex> lock(logMutex);
                if (out.output.rejectedNonFinite) {
                    RX_LOG_WARN("rx_asset: mesh {} primitive {}: non-finite position data -- primitive rejected", mi, pi);
                } else if (positions.empty()) {
                    RX_LOG_WARN("rx_asset: mesh {} primitive {}: zero vertices -- primitive skipped", mi, pi);
                }
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(logMutex);
                if (out.output.generatedNormals) {
                    RX_LOG_WARN("rx_asset: mesh {} primitive {}: no NORMAL attribute -- flat normals generated", mi, pi);
                }
                if (out.output.zeroFilledUVs) {
                    RX_LOG_WARN("rx_asset: mesh {} primitive {}: no TEXCOORD_0 attribute -- UVs zero-filled, tangent forced +X/w=1",
                                mi, pi);
                }
            }
            if (itemsCompleted != nullptr) {
                itemsCompleted->fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    if (isCancelled(cancelled)) {
        result.cancelled = true;
        return result;
    }

    // ================= ONE combined vertex/index array for the whole
    // file [matrix row 12: sync import waits ONCE, never per-primitive;
    // the async path uploads ONCE too -- same combined-buffer shape] ====
    size_t totalVertices = 0, totalIndices = 0;
    for (const auto& p : pending) {
        if (p.output.ok) {
            totalVertices += p.output.vertices.size();
            totalIndices += p.output.indices.size();
        }
    }
    result.combinedVertices.reserve(totalVertices);
    result.combinedIndices.reserve(totalIndices);

    result.submeshes.resize(pending.size());
    for (size_t i = 0; i < pending.size(); ++i) {
        PendingSubmeshCompute& sub = result.submeshes[i];
        sub.meshIndex = pending[i].meshIndex;
        sub.ok = pending[i].output.ok;
        if (!sub.ok) {
            continue;
        }
        sub.vertexStart = result.combinedVertices.size();
        sub.vertexCount = pending[i].output.vertices.size();
        sub.indexStart = result.combinedIndices.size();
        sub.indexCount = pending[i].output.indices.size();
        sub.bounds = pending[i].output.bounds;
        sub.materialIndex = pending[i].materialSourceIndex.value_or(SIZE_MAX);
        sub.skinVertices = std::move(pending[i].skinVertices);
        sub.morphTargets = std::move(pending[i].morphTargets);
        result.combinedVertices.insert(result.combinedVertices.end(), pending[i].output.vertices.begin(),
                                        pending[i].output.vertices.end());
        result.combinedIndices.insert(result.combinedIndices.end(), pending[i].output.indices.begin(),
                                       pending[i].output.indices.end());
    }

    // ================= Per-mesh LOCAL bounds (needed by node-flatten
    // below, computed WITHOUT any registry access -- unlike the old
    // single-pipeline code, which read this back from an already-
    // registered MeshAsset) + morph weights =================
    result.meshCount = asset.meshes.size();
    result.meshMorphWeights.resize(result.meshCount);
    result.meshSkins.resize(result.meshCount);
    std::vector<AABB> meshBoundsLocal(result.meshCount);
    for (size_t mi = 0; mi < asset.meshes.size(); ++mi) {
        for (float w : asset.meshes[mi].weights) {
            result.meshMorphWeights[mi].push_back(w);
        }
    }
    for (const PendingSubmeshCompute& sub : result.submeshes) {
        if (!sub.ok) {
            continue;
        }
        meshBoundsLocal[sub.meshIndex] = AABB::unionOf(meshBoundsLocal[sub.meshIndex], sub.bounds);
    }

    // ================= D12: node-graph flattening + preserved cameras/lights =================
    std::vector<glm::mat4> localTransforms(asset.nodes.size());
    for (size_t i = 0; i < asset.nodes.size(); ++i) {
        localTransforms[i] = nodeLocalTransform(asset.nodes[i]);
    }

    const std::function<void(size_t, const glm::mat4&)> visit = [&](size_t nodeIndex, const glm::mat4& parentWorld) {
        const fastgltf::Node& node = asset.nodes[nodeIndex];
        const glm::mat4 world = parentWorld * localTransforms[nodeIndex];

        if (node.meshIndex) {
            const size_t mi = *node.meshIndex;
            const AABB& localBounds = mi < meshBoundsLocal.size() ? meshBoundsLocal[mi] : AABB{};
            if (!node.instancingAttributes.empty()) {
                // [N2 ADOPTED] EXT_mesh_gpu_instancing: expand into one
                // InstanceRecord per instance.
                std::vector<glm::vec3> translations, scales;
                std::vector<glm::quat> rotations;
                size_t instanceCount = 0;
                if (auto it = node.findInstancingAttribute("TRANSLATION"); it != node.instancingAttributes.end()) {
                    translations = readAccessor<glm::vec3>(asset, asset.accessors[it->accessorIndex], adapter);
                    instanceCount = std::max(instanceCount, translations.size());
                }
                if (auto it = node.findInstancingAttribute("SCALE"); it != node.instancingAttributes.end()) {
                    scales = readAccessor<glm::vec3>(asset, asset.accessors[it->accessorIndex], adapter);
                    instanceCount = std::max(instanceCount, scales.size());
                }
                if (auto it = node.findInstancingAttribute("ROTATION"); it != node.instancingAttributes.end()) {
                    auto raw = readAccessor<glm::vec4>(asset, asset.accessors[it->accessorIndex], adapter);
                    rotations.reserve(raw.size());
                    for (auto& q : raw) {
                        rotations.emplace_back(q.w, q.x, q.y, q.z);
                    }
                    instanceCount = std::max(instanceCount, rotations.size());
                }
                for (size_t inst = 0; inst < instanceCount; ++inst) {
                    const glm::vec3 t = inst < translations.size() ? translations[inst] : glm::vec3(0.0F);
                    const glm::quat r = inst < rotations.size() ? rotations[inst] : glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
                    const glm::vec3 s = inst < scales.size() ? scales[inst] : glm::vec3(1.0F);
                    const glm::mat4 instanceLocal =
                        glm::translate(glm::mat4(1.0F), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0F), s);
                    const glm::mat4 instanceWorld = world * instanceLocal;

                    PendingInstance rec;
                    rec.meshIndex = mi;
                    rec.worldTransform = instanceWorld;
                    rec.negativeDeterminant = hasNegativeDeterminant(instanceWorld);
                    rec.sourceNodeIndex = static_cast<int32_t>(nodeIndex);
                    rec.worldBounds = localBounds.transformed(instanceWorld);
                    if (rec.negativeDeterminant) {
                        RX_LOG_WARN("rx_asset: node {} instance {}: negative-determinant transform (winding flip)", nodeIndex, inst);
                    }
                    result.instances.push_back(rec);
                }
            } else {
                PendingInstance rec;
                rec.meshIndex = mi;
                rec.worldTransform = world;
                rec.negativeDeterminant = hasNegativeDeterminant(world);
                rec.sourceNodeIndex = static_cast<int32_t>(nodeIndex);
                rec.worldBounds = localBounds.transformed(world);
                if (rec.negativeDeterminant) {
                    RX_LOG_WARN("rx_asset: node {} ('{}'): negative-determinant transform (winding flip)", nodeIndex,
                                std::string(node.name));
                }
                result.instances.push_back(rec);
            }
        }

        if (node.cameraIndex) {
            const fastgltf::Camera& cam = asset.cameras[*node.cameraIndex];
            CameraData cd;
            cd.name = std::string(cam.name);
            cd.worldTransform = world;
            cd.sourceNodeIndex = static_cast<int32_t>(nodeIndex);
            if (const auto* persp = std::get_if<fastgltf::Camera::Perspective>(&cam.camera)) {
                cd.type = CameraData::Type::Perspective;
                cd.yfov = persp->yfov;
                cd.znear = persp->znear;
                if (persp->aspectRatio) {
                    cd.aspectRatio = *persp->aspectRatio;
                }
                if (persp->zfar) {
                    cd.zfar = *persp->zfar;
                }
            } else {
                const auto& ortho = std::get<fastgltf::Camera::Orthographic>(cam.camera);
                cd.type = CameraData::Type::Orthographic;
                cd.xmag = ortho.xmag;
                cd.ymag = ortho.ymag;
                cd.znear = ortho.znear;
                cd.zfar = ortho.zfar;
            }
            result.cameras.push_back(cd);
        }

        if (node.lightIndex) {
            const fastgltf::Light& light = asset.lights[*node.lightIndex];
            LightData ld;
            ld.name = std::string(light.name);
            ld.worldTransform = world;
            ld.sourceNodeIndex = static_cast<int32_t>(nodeIndex);
            ld.color = glm::vec3(light.color[0], light.color[1], light.color[2]);
            ld.intensity = light.intensity;
            if (light.range) {
                ld.range = *light.range;
            }
            if (light.innerConeAngle) {
                ld.innerConeAngle = *light.innerConeAngle;
            }
            if (light.outerConeAngle) {
                ld.outerConeAngle = *light.outerConeAngle;
            }
            switch (light.type) {
                case fastgltf::LightType::Directional:
                    ld.type = LightData::Type::Directional;
                    break;
                case fastgltf::LightType::Spot:
                    ld.type = LightData::Type::Spot;
                    break;
                case fastgltf::LightType::Point:
                    ld.type = LightData::Type::Point;
                    break;
            }
            result.lights.push_back(ld);
        }

        for (size_t child : node.children) {
            visit(child, world);
        }
    };

    if (asset.scenes.empty()) {
        RX_LOG_WARN("rx_asset: importGltf: document has zero scenes -- empty ImportedScene (no instances)");
    } else {
        size_t sceneIndex = 0;
        if (asset.defaultScene) {
            sceneIndex = *asset.defaultScene;
        } else {
            RX_LOG_INFO("rx_asset: importGltf: no default scene specified -- using scene 0");
        }
        if (sceneIndex < asset.scenes.size()) {
            for (size_t nodeIdx : asset.scenes[sceneIndex].nodeIndices) {
                visit(nodeIdx, glm::mat4(1.0F));
            }
        }
    }

    // ================= Preserve-later: animations, skins =================
    for (const fastgltf::Animation& anim : asset.animations) {
        AnimationClipData clip;
        clip.name = std::string(anim.name);
        for (const fastgltf::AnimationSampler& samp : anim.samplers) {
            AnimationSamplerData sd;
            switch (samp.interpolation) {
                case fastgltf::AnimationInterpolation::Linear:
                    sd.interpolation = AnimationInterpolation::Linear;
                    break;
                case fastgltf::AnimationInterpolation::Step:
                    sd.interpolation = AnimationInterpolation::Step;
                    break;
                case fastgltf::AnimationInterpolation::CubicSpline:
                    sd.interpolation = AnimationInterpolation::CubicSpline;
                    break;
            }
            sd.inputTimes = readAccessor<float>(asset, asset.accessors[samp.inputAccessor], adapter);
            const fastgltf::Accessor& outAcc = asset.accessors[samp.outputAccessor];

            switch (outAcc.type) {
                case fastgltf::AccessorType::Scalar: {
                    sd.elementStride = 1;
                    sd.outputFlat = readAccessor<float>(asset, outAcc, adapter);
                    break;
                }
                case fastgltf::AccessorType::Vec3: {
                    sd.elementStride = 3;
                    auto vals = readAccessor<glm::vec3>(asset, outAcc, adapter);
                    sd.outputFlat.reserve(vals.size() * 3);
                    for (const auto& v : vals) {
                        sd.outputFlat.push_back(v.x);
                        sd.outputFlat.push_back(v.y);
                        sd.outputFlat.push_back(v.z);
                    }
                    break;
                }
                case fastgltf::AccessorType::Vec4: {
                    sd.elementStride = 4;
                    auto vals = readAccessor<glm::vec4>(asset, outAcc, adapter);
                    sd.outputFlat.reserve(vals.size() * 4);
                    for (const auto& v : vals) {
                        sd.outputFlat.push_back(v.x);
                        sd.outputFlat.push_back(v.y);
                        sd.outputFlat.push_back(v.z);
                        sd.outputFlat.push_back(v.w);
                    }
                    break;
                }
                default:
                    RX_LOG_WARN("rx_asset: animation '{}': unexpected sampler output accessor type -- output left empty",
                                clip.name);
                    break;
            }
            clip.samplers.push_back(std::move(sd));
        }
        for (const fastgltf::AnimationChannel& ch : anim.channels) {
            AnimationChannelData cd;
            if (ch.nodeIndex) {
                cd.targetNodeIndex = static_cast<int32_t>(*ch.nodeIndex);
            }
            cd.samplerIndex = static_cast<uint32_t>(ch.samplerIndex);
            switch (ch.path) {
                case fastgltf::AnimationPath::Translation:
                    cd.path = AnimationPath::Translation;
                    break;
                case fastgltf::AnimationPath::Rotation:
                    cd.path = AnimationPath::Rotation;
                    break;
                case fastgltf::AnimationPath::Scale:
                    cd.path = AnimationPath::Scale;
                    break;
                case fastgltf::AnimationPath::Weights:
                    cd.path = AnimationPath::Weights;
                    break;
            }
            clip.channels.push_back(cd);
        }
        result.animations.push_back(std::move(clip));
    }

    // Attach skin data [seed 14] to each mesh's LOCAL compute-side record
    // (no registry access -- unlike the old single-pipeline code, which
    // mutated an already-registered MeshAsset* in place).
    for (size_t nodeIdx = 0; nodeIdx < asset.nodes.size(); ++nodeIdx) {
        const fastgltf::Node& node = asset.nodes[nodeIdx];
        if (!node.meshIndex || !node.skinIndex) {
            continue;
        }
        const size_t mi = *node.meshIndex;
        if (mi >= result.meshSkins.size() || result.meshSkins[mi].present) {
            continue;
        }
        const fastgltf::Skin& skin = asset.skins[*node.skinIndex];
        SkinData sd;
        sd.present = true;
        sd.name = std::string(skin.name);
        for (size_t j : skin.joints) {
            sd.jointNodeIndices.push_back(static_cast<int32_t>(j));
        }
        if (skin.skeleton) {
            sd.skeletonRootNodeIndex = static_cast<int32_t>(*skin.skeleton);
        }
        if (skin.inverseBindMatrices) {
            sd.inverseBindMatrices = readAccessor<glm::mat4>(asset, asset.accessors[*skin.inverseBindMatrices], adapter);
        }
        result.meshSkins[mi] = std::move(sd);
    }

    if (isCancelled(cancelled)) {
        result.cancelled = true;
        return result;
    }

    result.error = ImportError::None;
    return result;
}

// ---------------------------------------------------------------------
// The marshal-phase entry points [Task 15] -- MarshalPendingImport itself
// is fully defined in import_pipeline.h (not here): it is held as a
// std::unique_ptr<MarshalPendingImport> MEMBER of registry.cpp's own
// AsyncImportJob, which needs the complete type to generate its own
// destructor in THAT translation unit -- the classic reason a pImpl-style
// forward declaration alone does not suffice once a unique_ptr to it
// becomes a data member elsewhere.
// ---------------------------------------------------------------------

namespace {

// Shared by marshalGltfImportSync() and marshalGltfImportPrepareDeferred()
// -- registers every decoded texture (main-thread GPU registration) and
// every material, then patches submesh ranges against `combinedRange` and
// assembles the final per-mesh MeshAsset array. Does NOT touch Registry at
// all (mesh/material REGISTRATION is the caller's own job, since the sync
// and deferred paths differ in exactly when that is safe to do -- sync:
// immediately; deferred: only after upload tickets complete).
void registerDecodedTexturesAndPatchMaterials(TextureCache* textures, ImportComputeResult& compute) {
    if (textures == nullptr) {
        return;
    }
    for (PendingMaterialCompute& mat : compute.materials) {
        for (size_t slotIdx = 0; slotIdx < mat.textureSlots.size(); ++slotIdx) {
            PendingTextureLoad& slot = mat.textureSlots[slotIdx];
            if (!slot.attempted) {
                continue;
            }
            TextureHandle handle = textures->registerDecoded(slot.decoded, slot.debugName);
            textureRefForSlot(mat.asset, static_cast<MaterialTextureSlot>(slotIdx)).handle = handle;
        }
    }
}

std::vector<MeshAsset> assembleMeshAssets(ImportComputeResult& compute, const MeshRange& combinedRange,
                                           const std::vector<MaterialHandle>& materialHandles) {
    std::vector<MeshAsset> meshAssets(compute.meshCount);
    for (size_t mi = 0; mi < compute.meshCount; ++mi) {
        meshAssets[mi].morphWeights = compute.meshMorphWeights[mi];
        meshAssets[mi].skin = compute.meshSkins[mi];
    }
    for (PendingSubmeshCompute& sub : compute.submeshes) {
        if (!sub.ok) {
            continue;
        }
        Submesh out;
        out.range.blockId = combinedRange.blockId;
        out.range.vertexOffset = combinedRange.vertexOffset + static_cast<int32_t>(sub.vertexStart);
        out.range.firstIndex = combinedRange.firstIndex + static_cast<uint32_t>(sub.indexStart);
        out.range.indexCount = static_cast<uint32_t>(sub.indexCount);
        out.bounds = sub.bounds;
        out.material = (sub.materialIndex != SIZE_MAX && sub.materialIndex < materialHandles.size())
                           ? materialHandles[sub.materialIndex]
                           : MaterialHandle{};
        out.skinVertices = std::move(sub.skinVertices);
        out.morphTargets = std::move(sub.morphTargets);
        MeshAsset& meshAsset = meshAssets[sub.meshIndex];
        meshAsset.bounds = AABB::unionOf(meshAsset.bounds, out.bounds);
        meshAsset.submeshes.push_back(std::move(out));
    }
    return meshAssets;
}

}  // namespace

ImportResult marshalGltfImportSync(Registry& registry, GeometryPool& pool, TextureCache* textures,
                                    ImportComputeResult&& computeIn) {
    RX_ZONE_NAMED("marshalGltfImportSync (upload-marshal + registry-commit)");
    ImportComputeResult compute = std::move(computeIn);
    ImportResult result;
    if (compute.error != ImportError::None || compute.cancelled) {
        result.error = compute.error != ImportError::None ? compute.error : ImportError::None;
        return result;
    }

    // Textures + materials.
    registerDecodedTexturesAndPatchMaterials(textures, compute);
    result.materials.reserve(compute.materials.size());
    for (PendingMaterialCompute& mat : compute.materials) {
        result.materials.push_back(registry.registerMaterial(std::move(mat.asset)));
    }

    // Geometry: ONE blocking upload, matching the pre-Task-15 contract
    // exactly (GeometryPool::upload() flushes+waits internally).
    MeshRange combinedRange;
    if (!compute.combinedVertices.empty()) {
        combinedRange = pool.upload(compute.combinedVertices, compute.combinedIndices);
        result.poolUploadCallCountForTesting += 1;
    }

    std::vector<MeshAsset> meshAssets = assembleMeshAssets(compute, combinedRange, result.materials);
    result.meshes.reserve(meshAssets.size());
    for (MeshAsset& meshAsset : meshAssets) {
        result.meshes.push_back(registry.registerMesh(std::move(meshAsset)));
    }

    result.scene.instances.reserve(compute.instances.size());
    for (const PendingInstance& pi : compute.instances) {
        InstanceRecord rec;
        rec.mesh = (pi.meshIndex != SIZE_MAX && pi.meshIndex < result.meshes.size()) ? result.meshes[pi.meshIndex]
                                                                                       : MeshHandle{};
        rec.worldTransform = pi.worldTransform;
        rec.worldBounds = pi.worldBounds;
        rec.negativeDeterminant = pi.negativeDeterminant;
        rec.sourceNodeIndex = pi.sourceNodeIndex;
        result.scene.instances.push_back(rec);
    }
    result.scene.cameras = std::move(compute.cameras);
    result.scene.lights = std::move(compute.lights);
    result.scene.animations = std::move(compute.animations);

    result.error = ImportError::None;
    return result;
}

std::unique_ptr<MarshalPendingImport> marshalGltfImportBeginDeferred(ImportComputeResult&& computeIn) {
    auto pending = std::make_unique<MarshalPendingImport>();
    pending->compute = std::move(computeIn);
    if (pending->compute.error != ImportError::None || pending->compute.cancelled) {
        pending->texturesPrepared = true;
        pending->geometryPrepared = true;
    }
    return pending;
}

bool marshalGltfImportPrepareStep(GeometryPool& pool, TextureCache* textures, MarshalPendingImport& pending) {
    RX_ZONE_NAMED("marshalGltfImportPrepareStep (upload-marshal)");
    ImportComputeResult& compute = pending.compute;

    // Texture slots, ONE at a time, in (material, slot) file order --
    // registered NOW (main thread), but neither flushed nor waited on --
    // see TextureCache::registerDecoded()'s own comment. Nothing outside
    // this MarshalPendingImport can reach the freshly-registered handles
    // yet (Registry mutation -- the only thing that WOULD expose them --
    // happens strictly later, in marshalGltfImportFinalize(), only once
    // every ticket below is confirmed complete), so registering slightly
    // ahead of GPU completion is safe: no renderer can sample an
    // in-flight transfer's destination through a handle nothing has
    // published yet.
    if (!pending.texturesPrepared) {
        if (textures == nullptr) {
            pending.texturesPrepared = true;
        } else {
            while (pending.nextMaterialIndex < compute.materials.size()) {
                PendingMaterialCompute& mat = compute.materials[pending.nextMaterialIndex];
                if (pending.nextSlotIndex >= mat.textureSlots.size()) {
                    pending.nextSlotIndex = 0;
                    ++pending.nextMaterialIndex;
                    continue;
                }
                const size_t slotIdx = pending.nextSlotIndex++;
                PendingTextureLoad& slot = mat.textureSlots[slotIdx];
                if (!slot.attempted) {
                    continue;  // free -- no GPU work, keep draining this same step
                }
                TextureHandle handle = textures->registerDecoded(slot.decoded, slot.debugName);
                textureRefForSlot(mat.asset, static_cast<MaterialTextureSlot>(slotIdx)).handle = handle;
                // [Fix round 2, CRITICAL] Marks this slot as having
                // actually reached marshal -- see PendingTextureLoad's
                // own comment (import_pipeline.h) for why
                // marshalGltfImportRollback() needs this, distinct from
                // `attempted`, to avoid releasing the shared checkerboard
                // fallback for a slot cancellation caught before its own
                // marshal turn arrived.
                slot.registered = true;
                pending.hasTextureUploads = true;
                return true;  // ONE real texture registered this call -- yield to the caller's own pump loop
            }
            pending.texturesPrepared = true;
            if (pending.hasTextureUploads) {
                pending.textureTicket = textures->flushPendingUploads();
                pending.textureTicketIssued = true;
            }
        }
        return true;  // still have the geometry step left, even if no texture work remained
    }

    // Geometry: ONE combined upload for the whole file (matching the
    // sync path's own "upload once" contract) -- cheap enough relative to
    // the texture case above (this task's own wall-clock-gate testing
    // found the texture batch, not geometry, was what blew the budget)
    // that it does not need its own further slicing.
    if (!pending.geometryPrepared) {
        if (!compute.combinedVertices.empty()) {
            RX_PLOT("async import: bytes uploaded (geometry)",
                    static_cast<int64_t>(compute.combinedVertices.size() * sizeof(PoolVertex) +
                                          compute.combinedIndices.size() * sizeof(uint32_t)));
            pending.combinedRange = pool.uploadDeferred(compute.combinedVertices, compute.combinedIndices);
            pending.hasGeometry = pending.combinedRange.indexCount > 0;
        }
        if (pending.hasGeometry) {
            pending.poolTicket = pool.flushPendingUploads();
            pending.poolTicketIssued = true;
        }
        pending.geometryPrepared = true;
        return true;
    }

    return false;  // fully prepared -- caller now polls ticket completion
}

bool marshalGltfImportUploadsComplete(const GeometryPool& pool, const TextureCache* textures,
                                       const MarshalPendingImport& pending) {
    if (pending.poolTicketIssued && !pool.isUploadComplete(pending.poolTicket)) {
        return false;
    }
    if (pending.textureTicketIssued && textures != nullptr && !textures->isUploadComplete(pending.textureTicket)) {
        return false;
    }
    return true;
}

ImportResult marshalGltfImportFinalize(Registry& registry, std::unique_ptr<MarshalPendingImport> pendingPtr) {
    RX_ZONE_NAMED("marshalGltfImportFinalize (registry-commit)");
    MarshalPendingImport pending = std::move(*pendingPtr);
    ImportComputeResult& compute = pending.compute;
    ImportResult result;
    if (compute.error != ImportError::None || compute.cancelled) {
        result.error = compute.error;
        return result;
    }

    result.materials.reserve(compute.materials.size());
    for (PendingMaterialCompute& mat : compute.materials) {
        result.materials.push_back(registry.registerMaterial(std::move(mat.asset)));
    }

    std::vector<MeshAsset> meshAssets = assembleMeshAssets(compute, pending.combinedRange, result.materials);
    result.meshes.reserve(meshAssets.size());
    for (MeshAsset& meshAsset : meshAssets) {
        result.meshes.push_back(registry.registerMesh(std::move(meshAsset)));
    }
    result.poolUploadCallCountForTesting = pending.hasGeometry ? 1 : 0;

    result.scene.instances.reserve(compute.instances.size());
    for (const PendingInstance& pi : compute.instances) {
        InstanceRecord rec;
        rec.mesh = (pi.meshIndex != SIZE_MAX && pi.meshIndex < result.meshes.size()) ? result.meshes[pi.meshIndex]
                                                                                       : MeshHandle{};
        rec.worldTransform = pi.worldTransform;
        rec.worldBounds = pi.worldBounds;
        rec.negativeDeterminant = pi.negativeDeterminant;
        rec.sourceNodeIndex = pi.sourceNodeIndex;
        result.scene.instances.push_back(rec);
    }
    result.scene.cameras = std::move(compute.cameras);
    result.scene.lights = std::move(compute.lights);
    result.scene.animations = std::move(compute.animations);

    result.error = ImportError::None;
    return result;
}

void marshalGltfImportEnsureRollbackTicketed(TextureCache* textures, MarshalPendingImport& pending) {
    if (textures != nullptr && pending.hasTextureUploads && !pending.textureTicketIssued) {
        pending.textureTicket = textures->flushPendingUploads();
        pending.textureTicketIssued = true;
    }
}

// [Fix round 2, ITEM 3] Loud-failure backstop for the "no registered
// upload survives into marshalGltfImportRollback() without an issued
// ticket" invariant this file's own comments (marshalGltfImportRollback's
// call sites) document. Deliberately NOT a bare assert() -- this
// project's presets compile with `-DNDEBUG` in every configuration
// (including CI), which would silently compile a bare assert() away
// entirely (the same reasoning rx_core/debug_checks.h's own top comment
// documents at length for RX_ASSERT_MAIN_THREAD); RX_DEBUG_CHECKS is
// this engine's own always-available switch instead (ON by default in
// both dev presets), independent of NDEBUG.
void checkRollbackTicketInvariant(bool ok, const char* what) {
#ifdef RX_DEBUG_CHECKS
    if (!ok) {
        RX_LOG_ERROR("rx_asset: marshalGltfImportRollback: invariant violated -- {}", what);
        std::abort();
    }
#else
    (void)ok;
    (void)what;
#endif
}

void marshalGltfImportRollback(GeometryPool& pool, TextureCache* textures, std::unique_ptr<MarshalPendingImport> pendingPtr) {
    if (!pendingPtr) {
        return;
    }
    MarshalPendingImport pending = std::move(*pendingPtr);
    // [Fix round 2, ITEM 3] Geometry-side half of the same invariant class
    // marshalGltfImportEnsureRollbackTicketed()'s own comment documents
    // for textures -- provably unreachable given marshalGltfImportPrepareStep()'s
    // own geometry branch (uploadDeferred()+flushPendingUploads() issued
    // together, no yield between them), enforced here rather than merely
    // asserted in a comment so a future change that breaks that atomicity
    // fails loudly instead of silently reintroducing this bug class.
    checkRollbackTicketInvariant(!pending.hasGeometry || pending.poolTicketIssued,
                                  "GeometryPool upload registered without an issued ticket -- rollback would free "
                                  "an in-flight range");
    if (pending.hasGeometry) {
        pool.free(pending.combinedRange);
    }
    // [Fix round 2, ITEM 3] The texture-side half of the SAME invariant --
    // by this point, EVERY caller must already have run
    // marshalGltfImportEnsureRollbackTicketed() (registry.cpp's own
    // rollbackAsyncImportWhenSafe() always does), which forces a real
    // ticket into existence for any registered-but-unflushed texture
    // batch. Unlike the geometry case above, this one is NOT provably
    // unreachable by construction (registerDecoded() genuinely can yield
    // mid-batch, unticketed) -- it is made unreachable by the caller
    // contract instead, so this check is the loud-failure backstop for
    // that contract specifically, not a restatement of an already-provable
    // invariant.
    checkRollbackTicketInvariant(!pending.hasTextureUploads || pending.textureTicketIssued,
                                  "texture upload(s) registered without an issued ticket -- call "
                                  "marshalGltfImportEnsureRollbackTicketed() before marshalGltfImportRollback()");
    if (textures != nullptr) {
        for (PendingMaterialCompute& mat : pending.compute.materials) {
            for (size_t slotIdx = 0; slotIdx < mat.textureSlots.size(); ++slotIdx) {
                PendingTextureLoad& slot = mat.textureSlots[slotIdx];
                // [Fix round 2, CRITICAL] `registered` is the primary
                // gate, NOT `attempted` -- an attempted-but-not-yet-
                // registered slot's own TextureRef::handle still holds
                // the COMPUTE-time placeholder (the shared checkerboard
                // fallback, import_gltf.cpp's own fillRef() lambda),
                // never a handle this rollback owns. `decoded.outcome ==
                // Ready` still matters SEPARATELY even once registered:
                // a registered-but-Failed/Checkerboard-outcome slot's
                // handle is applyDecodeResult()'s own shared
                // failure/checkerboard fallback (texture_cache.cpp), not
                // a real per-import allocation either -- both gates are
                // required, neither alone is sufficient (see
                // PendingTextureLoad's own comment, import_pipeline.h,
                // and task-15-report.md's round-2 section for the
                // empirical reproduction this fix closes).
                if (!slot.registered || slot.decoded.outcome != TextureDecodeResult::Outcome::Ready) {
                    continue;
                }
                TextureHandle handle = textureRefForSlot(mat.asset, static_cast<MaterialTextureSlot>(slotIdx)).handle;
                textures->releaseUnpublished(handle);
            }
        }
    }
}

// ---------------------------------------------------------------------
// The pre-Task-15 sync orchestration entry point -- now a thin wrapper.
// ---------------------------------------------------------------------

ImportResult importGltfPipeline(Registry& registry, std::span<const std::byte> documentBytes, ByteSource& source, GeometryPool& pool,
                                 rx::task::Scheduler& scheduler, TextureCache* textures) {
    ImportComputeResult compute =
        computeGltfImport(documentBytes, source, scheduler, textures, registry.fallbackTextureHandle(),
                           snapshotTextureFallbackHandles(textures), /*cancelled=*/nullptr, /*itemsCompleted=*/nullptr,
                           /*itemsTotalOut=*/nullptr, /*stageOut=*/nullptr);
    if (compute.error != ImportError::None) {
        ImportResult result;
        result.error = compute.error;
        return result;
    }
    return marshalGltfImportSync(registry, pool, textures, std::move(compute));
}

}  // namespace rx::asset
