#include "import_gltf.h"
#include "gltf_error.h"
#include "gltf_pipeline.h"
#include <rx_asset/geometry_pool.h>
#include <rx_asset/texture_cache.h>
#include <rx_core/log.h>
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

}  // namespace

// ---------------------------------------------------------------------
// The orchestration entry point
// ---------------------------------------------------------------------

ImportResult importGltfPipeline(Registry& registry, std::span<const std::byte> documentBytes, ByteSource& source, GeometryPool& pool,
                                 rx::task::Scheduler& scheduler, TextureCache* textures) {
    ImportResult result;

    auto dataBuffer = fastgltf::GltfDataBuffer::FromBytes(reinterpret_cast<const std::byte*>(documentBytes.data()),
                                                            documentBytes.size());
    if (dataBuffer.error() != fastgltf::Error::None) {
        result.error = mapFastgltfError(dataBuffer.error());
        RX_LOG_ERROR("rx_asset: importGltf: failed to wrap document bytes: {}", importErrorName(result.error));
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

    ResolvedBuffers buffers(asset, source);
    // Sequential pre-pass [D5/matrix-issue02 row 15]: every buffer is
    // resolved once, HERE, on the calling (main) thread -- see
    // ResolvedBuffers::bytes()'s own comment for why this is required
    // before the parallelFor fan-out below touches the same cache from
    // multiple worker threads.
    buffers.resolveAllBuffersSequentially();
    DecodedMeshoptViews meshoptViews(asset, buffers);
    ImportBufferDataAdapter adapter{asset, buffers, meshoptViews};

    // ================= Materials =================
    std::vector<MaterialAsset> materials(asset.materials.size());
    for (size_t mi = 0; mi < asset.materials.size(); ++mi) {
        const fastgltf::Material& src = asset.materials[mi];
        MaterialAsset& dst = materials[mi];
        dst.name = std::string(src.name);
        dst.disposition = src.unlit ? MaterialDisposition::Unlit : MaterialDisposition::StandardPBR;  // [gate ruling C5]

        dst.baseColorFactor = glm::vec4(src.pbrData.baseColorFactor[0], src.pbrData.baseColorFactor[1], src.pbrData.baseColorFactor[2],
                                         src.pbrData.baseColorFactor[3]);
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

        // Takes a raw (non-owning) pointer rather than the Optional<T>
        // wrapper directly: fastgltf::TextureInfo carries a
        // std::unique_ptr<TextureTransform> member (move-only), so it
        // cannot be copied into a by-value/by-reference-to-Optional
        // parameter of a DIFFERENT Optional specialization -- callers
        // for NormalTextureInfo/OcclusionTextureInfo (both PUBLICLY
        // DERIVE from TextureInfo) pass `&*opt` upcast to the base
        // pointer instead of slicing a copy.
        // [Task 14, matrix-issue03 "KHR_texture_basisu wiring from
        // import" row] `role` is the MATERIAL SLOT this call fills --
        // slot-driven, never filename-driven (the misleading-filename
        // test this row calls for constructs a fixture whose file name
        // says one thing and whose material slot says another, and
        // asserts the SLOT wins). `textures` is nullptr-tolerant (Task
        // 13's own existing contract, unchanged): when absent, every ref
        // still resolves to registry.fallbackTextureHandle(), exactly as
        // before this task.
        const auto fillRef = [&](TextureRef& ref, const fastgltf::TextureInfo* info, TextureRole role) {
            if (info == nullptr) {
                // [D11] UNBOUND slot -- the source material carries no
                // texture reference for this role AT ALL (`ref.present`
                // stays false, unchanged from its default -- a consumer
                // is expected to check that first). Still points
                // `ref.handle` at the role-appropriate UTILITY texture
                // (never the checkerboard -- matrix's own "a magenta
                // normal map would shade garbage" reasoning) rather than
                // leaving it at an invalid default: a shader that always
                // samples every slot (D26.1's per-draw-addressing,
                // branchless pattern -- multiply the sampled value by the
                // slot's own factor, texture*factor==factor when the
                // texture is neutral) needs a resolvable handle here
                // regardless of whether every future consumer remembers
                // to special-case `!present` itself. No-op when no
                // TextureCache was supplied (nothing to resolve against).
                if (textures != nullptr) {
                    ref.handle = textures->fallbackHandle(role);
                }
                return;
            }
            ref.present = true;
            ref.handle = registry.fallbackTextureHandle();  // [D11] Task-13 baseline -- the textures==nullptr contract, unchanged
            // [Fix round 1, reviewer IMPORTANT-1] The instant a TextureCache
            // exists, EVERY failure path below (out-of-range textureIndex,
            // no resolvable image index, resolveImageBytes() failure --
            // malformed bufferView, an absolute/network URI never fetched,
            // a missing file) must leave `ref.handle` pointing into THAT
            // TextureCache's OWN handle space, never
            // registry.fallbackTextureHandle() -- a handle from Registry's
            // completely separate `textures_` HandlePool. The two were
            // never meant to be interchangeable: they only ever compared
            // equal because BOTH classes' constructors unconditionally
            // acquire their own single fallback entry first, into an
            // otherwise-untouched pool (Registry's `textures_` never grows
            // past that one acquire anywhere in this codebase), so both
            // land on Handle(index=0, generation=1) in every reachable
            // test today -- a structural coincidence of this task's
            // current code, not a real cross-pool identity, and the first
            // future change to either build order (Registry acquiring a
            // second texture, or TextureCache::create() building its
            // fallbacks in a different sequence) would silently start
            // resolving to the wrong pool's entry. Set unconditionally
            // here (before any of the resolution attempts below, all of
            // which overwrite it again on success) so every failure path
            // is correct by construction rather than by accident.
            if (textures != nullptr) {
                ref.handle = textures->checkerboardHandle();
            }
            if (info->textureIndex < asset.textures.size()) {
                const fastgltf::Texture& tex = asset.textures[info->textureIndex];
                // KHR_texture_basisu [D10]: a texture's basisuImageIndex,
                // when present, names the KTX2 image this texture ACTUALLY
                // uses -- takes priority over the core imageIndex per the
                // extension's own documented fallback order (glTF spec:
                // "clients that support this extension SHOULD use... the
                // image referenced by this extension" -- imageIndex is the
                // non-KTX2 fallback for viewers without the extension).
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
                        ref.handle = textures->loadFromBytes(std::span<const std::byte>(*bytes), role, debugName);
                    } else {
                        RX_LOG_WARN("rx_asset: material '{}': {} texture (image #{}) bytes could not be resolved -- D11 fallback",
                                    dst.name, textureRoleName(role), *effectiveImageIndex);
                    }
                }
            }
            ref.texCoordIndex = static_cast<uint32_t>(info->texCoordIndex);
            if (info->transform) {
                // [gate ruling C4] offset/scale consume-now; rotation
                // stays log-don't-drop.
                ref.uvOffset = glm::vec2(info->transform->uvOffset.x(), info->transform->uvOffset.y());
                ref.uvScale = glm::vec2(info->transform->uvScale.x(), info->transform->uvScale.y());
                if (info->transform->rotation != 0.0F) {
                    RX_LOG_WARN(
                        "rx_asset: material '{}': KHR_texture_transform rotation={} radians is not applied (log-don't-drop; "
                        "offset/scale are consumed)",
                        dst.name, info->transform->rotation);
                }
            }
        };
        fillRef(dst.baseColorTexture, src.pbrData.baseColorTexture ? &*src.pbrData.baseColorTexture : nullptr,
                TextureRole::BaseColor);
        fillRef(dst.metallicRoughnessTexture,
                src.pbrData.metallicRoughnessTexture ? &*src.pbrData.metallicRoughnessTexture : nullptr,
                TextureRole::MetallicRoughness);
        fillRef(dst.normalTexture, src.normalTexture ? static_cast<const fastgltf::TextureInfo*>(&*src.normalTexture) : nullptr,
                TextureRole::Normal);
        fillRef(dst.occlusionTexture,
                src.occlusionTexture ? static_cast<const fastgltf::TextureInfo*>(&*src.occlusionTexture) : nullptr,
                TextureRole::Occlusion);
        fillRef(dst.emissiveTexture, src.emissiveTexture ? &*src.emissiveTexture : nullptr, TextureRole::Emissive);

        // Every unimplemented KHR_materials_* [log-don't-drop, one WARN
        // per material+extension -- matrix-issue02 §2B shared criterion].
        const auto warnExt = [&](bool present, std::string_view extName, std::string_view extra = {}) {
            if (!present) {
                return;
            }
            if (extra.empty()) {
                RX_LOG_WARN("rx_asset: material '{}': {} is parsed but not consumed (log-don't-drop)", dst.name, extName);
            } else {
                RX_LOG_WARN("rx_asset: material '{}': {} is parsed but not consumed ({})", dst.name, extName, extra);
            }
        };
        // [fix round 1] KHR_materials_emissive_strength: the value IS
        // carried for free (dst.emissiveStrength above, MaterialAsset's
        // own field) -- but per the shared KHR_materials_* log-don't-drop
        // criterion, a non-default value must ALSO produce a WARN naming
        // the material and the value (this project's own row text: "the
        // value is already in the parsed material if the techniques
        // phase wants it preserved-in-parameter-set at zero cost" --
        // "preserved at zero cost" is not the same claim as "logged";
        // this call is what the log half actually requires, and was
        // previously missing entirely despite the field being carried).
        if (src.emissiveStrength != 1.0F) {
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

    // ================= Meshes / primitives =================
    struct PendingSubmesh {
        size_t meshIndex = 0;
        PrimitiveOutput output;
        std::optional<size_t> materialSourceIndex;
        MaterialHandle material;
        SkinVertexData skinVertices;
        std::vector<MorphTarget> morphTargets;
    };

    // One flattened (meshIndex, primitiveIndex) work-list entry per
    // TRIANGLES primitive with a POSITION attribute -- everything else
    // is filtered out (WARNed) before the parallel fan-out so worker
    // chunks never need to reason about skip conditions.
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

            // [log-don't-drop] unconsumed attribute sets.
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

            // [KHR_materials_variants, log-don't-drop, matrix-issue02 §2B
            // row -- fix round 1] `Primitive::mappings` is PRIMITIVE-level
            // (a per-primitive variant->material override list), not
            // material-level -- it cannot live in the material warnExt
            // loop above (which only ever sees one Material at a time,
            // with no notion of which primitive/variant selected it).
            // The primitive's own `materialIndex` (already the base/
            // default material per the extension's own spec text) is
            // used unconditionally; only the WARN is new here.
            if (!prim.mappings.empty()) {
                RX_LOG_WARN(
                    "rx_asset: mesh {} primitive {}: KHR_materials_variants present ({} variant mapping(s)) -- not "
                    "consumed, the primitive's own default material is used for every variant",
                    mi, pi, prim.mappings.size());
            }
        }
    }

    std::vector<PendingSubmesh> pending(workItems.size());
    std::mutex logMutex;
    scheduler.parallelFor(static_cast<uint32_t>(workItems.size()), [&](uint32_t begin, uint32_t end, uint32_t) {
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
                // [seed 14, preserve-later] JOINTS_0/WEIGHTS_0, in SOURCE order.
                if (auto j = prim.findAttribute("JOINTS_0"); j != prim.attributes.end()) {
                    skinVertices.joints = readAccessor<glm::u16vec4>(asset, asset.accessors[j->accessorIndex], adapter);
                }
                if (auto wgt = prim.findAttribute("WEIGHTS_0"); wgt != prim.attributes.end()) {
                    skinVertices.weights = readAccessor<glm::vec4>(asset, asset.accessors[wgt->accessorIndex], adapter);
                }
                // [preserve-later] Morph targets, in SOURCE order (see
                // mesh_asset.h's own comment). One MorphTarget per
                // primitive.targets[] entry; each target's own
                // POSITION/NORMAL/TANGENT sub-attribute is independently
                // optional per the glTF spec.
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

            PendingSubmesh& out = pending[w];
            out.meshIndex = mi;
            out.output = processPrimitive(input);
            out.skinVertices = std::move(skinVertices);
            out.morphTargets = std::move(morphTargets);
            if (prim.materialIndex) {
                // Resolved to a real MaterialHandle in the sequential
                // pass just after this parallelFor() call, once
                // materials are registered (main-thread-only, D5).
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
        }
    });

    // ================= Register materials (must happen before mesh
    // registration resolves the stashed handles above) =================
    for (auto& mat : materials) {
        result.materials.push_back(registry.registerMaterial(mat));
    }
    // Resolve the stashed source material indices from the parallel
    // section above into real MaterialHandles now that materials are
    // registered.
    for (auto& p : pending) {
        p.material = (p.materialSourceIndex && *p.materialSourceIndex < result.materials.size())
                         ? result.materials[*p.materialSourceIndex]
                         : registry.fallbackMaterialHandle();
    }

    // ================= ONE combined GeometryPool upload for the whole
    // file [matrix row 12: sync import waits ONCE, never per-primitive] =================
    size_t totalVertices = 0, totalIndices = 0;
    for (const auto& p : pending) {
        if (p.output.ok) {
            totalVertices += p.output.vertices.size();
            totalIndices += p.output.indices.size();
        }
    }

    std::vector<PoolVertex> combinedVertices;
    std::vector<uint32_t> combinedIndices;
    combinedVertices.reserve(totalVertices);
    combinedIndices.reserve(totalIndices);

    struct SubmeshOffset {
        size_t vertexStart, vertexCount, indexStart, indexCount;
    };
    std::vector<SubmeshOffset> offsets(pending.size());
    for (size_t i = 0; i < pending.size(); ++i) {
        if (!pending[i].output.ok) {
            continue;
        }
        offsets[i].vertexStart = combinedVertices.size();
        offsets[i].vertexCount = pending[i].output.vertices.size();
        offsets[i].indexStart = combinedIndices.size();
        offsets[i].indexCount = pending[i].output.indices.size();
        combinedVertices.insert(combinedVertices.end(), pending[i].output.vertices.begin(), pending[i].output.vertices.end());
        combinedIndices.insert(combinedIndices.end(), pending[i].output.indices.begin(), pending[i].output.indices.end());
    }

    MeshRange combinedRange;
    if (!combinedVertices.empty()) {
        combinedRange = pool.upload(combinedVertices, combinedIndices);
        result.poolUploadCallCountForTesting += 1;
    }

    // ================= Assemble MeshAsset per source mesh =================
    result.meshes.resize(asset.meshes.size());
    for (size_t mi = 0; mi < asset.meshes.size(); ++mi) {
        MeshAsset meshAsset;
        for (float w : asset.meshes[mi].weights) {
            meshAsset.morphWeights.push_back(w);
        }
        for (size_t i = 0; i < pending.size(); ++i) {
            if (pending[i].meshIndex != mi || !pending[i].output.ok) {
                continue;
            }
            Submesh sub;
            sub.range.blockId = combinedRange.blockId;
            sub.range.vertexOffset = combinedRange.vertexOffset + static_cast<int32_t>(offsets[i].vertexStart);
            sub.range.firstIndex = combinedRange.firstIndex + static_cast<uint32_t>(offsets[i].indexStart);
            sub.range.indexCount = static_cast<uint32_t>(offsets[i].indexCount);
            sub.bounds = pending[i].output.bounds;
            sub.material = pending[i].material;
            sub.skinVertices = std::move(pending[i].skinVertices);
            sub.morphTargets = std::move(pending[i].morphTargets);
            meshAsset.bounds = AABB::unionOf(meshAsset.bounds, sub.bounds);
            meshAsset.submeshes.push_back(std::move(sub));
        }
        result.meshes[mi] = registry.registerMesh(std::move(meshAsset));
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

                    InstanceRecord rec;
                    rec.mesh = result.meshes[mi];
                    rec.worldTransform = instanceWorld;
                    rec.negativeDeterminant = hasNegativeDeterminant(instanceWorld);
                    rec.sourceNodeIndex = static_cast<int32_t>(nodeIndex);
                    if (const MeshAsset* meshAsset = registry.meshes_.get(rec.mesh)) {
                        rec.worldBounds = meshAsset->bounds.transformed(instanceWorld);
                    }
                    if (rec.negativeDeterminant) {
                        RX_LOG_WARN("rx_asset: node {} instance {}: negative-determinant transform (winding flip)", nodeIndex, inst);
                    }
                    result.scene.instances.push_back(rec);
                }
            } else {
                InstanceRecord rec;
                rec.mesh = result.meshes[mi];
                rec.worldTransform = world;
                rec.negativeDeterminant = hasNegativeDeterminant(world);
                rec.sourceNodeIndex = static_cast<int32_t>(nodeIndex);
                if (const MeshAsset* meshAsset = registry.meshes_.get(rec.mesh)) {
                    rec.worldBounds = meshAsset->bounds.transformed(world);
                }
                if (rec.negativeDeterminant) {
                    RX_LOG_WARN("rx_asset: node {} ('{}'): negative-determinant transform (winding flip)", nodeIndex,
                                std::string(node.name));
                }
                result.scene.instances.push_back(rec);
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
            result.scene.cameras.push_back(cd);
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
            result.scene.lights.push_back(ld);
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

            // Animation sampler outputs are SCALAR (Weights path -- a
            // flat "morphTargetCount per keyframe" list, per the glTF
            // spec's own animation.sampler schema), VEC3 (Translation/
            // Scale), or VEC4 (Rotation quaternions). Dispatch on the
            // accessor's OWN declared type and flatten via the normal
            // accessor tools -- never a raw byte poke -- matching
            // AnimationSamplerData::outputFlat's documented "glTF-native
            // on-disk layout verbatim" contract (mesh_asset.h).
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
        result.scene.animations.push_back(std::move(clip));
    }

    // Attach skin data [seed 14] to each mesh from whichever node
    // referenced (mesh, skin) together.
    for (size_t nodeIdx = 0; nodeIdx < asset.nodes.size(); ++nodeIdx) {
        const fastgltf::Node& node = asset.nodes[nodeIdx];
        if (!node.meshIndex || !node.skinIndex) {
            continue;
        }
        const fastgltf::Skin& skin = asset.skins[*node.skinIndex];
        MeshAsset* meshAsset = registry.meshes_.get(result.meshes[*node.meshIndex]);
        if (meshAsset == nullptr || meshAsset->skin.present) {
            continue;
        }
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
        meshAsset->skin = std::move(sd);
    }

    result.error = ImportError::None;
    return result;
}

}  // namespace rx::asset
