#pragma once

// rx_asset/byte_source.h -- Thread-affinity: none intrinsically (a
// ByteSource implementation may be called from a worker thread, since
// the importer's own per-primitive CPU work runs in parallel [D5,
// matrix-issue02 row 15]; FilesystemByteSource below is safe for
// concurrent read() calls -- each call opens its own std::ifstream).
//
// THE IO-SOURCE ABSTRACTION INVARIANT [2026-08-12 amendment; gate
// matrix-issue02 row 2, Conflict C2]: asset packaging/mounting (loose
// files, a PAK archive, PhysFS, memory, network) is HOST-ENGINE policy,
// never something this renderer hardcodes. fastgltf CAN fully abstract
// the main DOCUMENT's own bytes (GltfDataGetter/GltfDataBuffer::FromBytes,
// used directly by import_gltf.cpp -- no custom GltfDataGetter subclass
// is needed for that half), but it has NO callback for resolving a
// glTF-referenced external URI (buffer.uri / image.uri) at all --
// verified directly against fastgltf v0.9.0: `Parser::loadFileFromUri`
// (src/io.cpp) hardcodes std::filesystem/std::ifstream with no override
// hook, reachable only when `Options::LoadExternalBuffers`/
// `LoadExternalImages` is set. This importer NEVER sets either option
// (import_gltf.cpp) and instead resolves every `fastgltf::sources::URI`
// itself, through exactly this interface, after parsing -- see
// import_gltf.cpp's `resolveExternalSources()`.
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rx::asset {

// Host-injectable byte reader. The importer depends on this ABSTRACT
// interface only -- FilesystemByteSource below is merely the convenience
// default the path-taking `Registry::importGltf()` overload wraps; a
// host with a different asset-mounting policy supplies its own
// implementation instead and nothing in the importer changes.
class ByteSource {
public:
    virtual ~ByteSource() = default;

    // Returns the full byte contents addressed by `uri` -- exactly the
    // (already percent-decoded, per fastgltf's own URI handling) string
    // fastgltf parsed from the glTF document's `buffer.uri`/`image.uri`
    // field, verbatim, including any `../` or absolute/network form
    // (the HOST decides policy for those; this importer only logs the
    // request at debug level and, for absolute/network URIs, never
    // calls read() at all -- see import_gltf.cpp's own handling and
    // matrix-issue02 row "Absolute / http(s) URIs"). Returns
    // std::nullopt (never throws) if the URI cannot be resolved; the
    // CALLER is responsible for logging the failure with import-specific
    // context (which primitive/image needed it), not this interface.
    virtual std::optional<std::vector<std::byte>> read(const std::string& uri) = 0;
};

// The thin filesystem-backed default [matrix row 2: "the path-taking
// convenience overload wraps this"]. Resolves `uri` relative to a fixed
// base directory fixed at construction. This class is POLICY, not the
// invariant above -- it exists purely so `Registry::importGltf(path, ...)`
// has something to hand a caller who does not want to implement
// ByteSource themselves for the common "just read from disk" case.
class FilesystemByteSource final : public ByteSource {
public:
    explicit FilesystemByteSource(std::filesystem::path baseDir);

    std::optional<std::vector<std::byte>> read(const std::string& uri) override;

private:
    std::filesystem::path baseDir_;
};

}  // namespace rx::asset
