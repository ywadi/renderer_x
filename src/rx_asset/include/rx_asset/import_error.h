#pragma once

// rx_asset/import_error.h -- Thread-affinity: none (plain enum).
//
// Named error taxonomy for Registry::importGltf() [matrix-issue02 row 4].
// Mirrors fastgltf::Error's 15 members 1:1 (see gltf_error.h/.cpp, the
// ONLY translation unit that names fastgltf::Error, for the exhaustive
// compiler-checked mapping) plus a small number of renderer-side failure
// modes fastgltf has no equivalent for. Deliberately does not include
// <fastgltf/core.hpp> -- this header has no fastgltf dependency, keeping
// the public rx_asset API fastgltf-free (see mesh_asset.h's own header-
// hygiene comment).

#include <string_view>

namespace rx::asset {

enum class ImportError : uint8_t {
    None = 0,
    InvalidPath,
    MissingExtensions,
    UnknownRequiredExtension,
    InvalidJson,
    InvalidGltf,
    InvalidOrMissingAssetField,
    InvalidGLB,
    MissingField,
    MissingExternalBuffer,
    UnsupportedVersion,
    InvalidURI,
    InvalidFileData,
    FailedWritingFiles,
    FileBufferAllocationFailed,

    // Renderer-side (no fastgltf::Error equivalent):
    ByteSourceUnavailable,  // the main document's bytes could not be read (path overload: file open failed)
};

[[nodiscard]] std::string_view importErrorName(ImportError error);

}  // namespace rx::asset
