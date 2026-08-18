#include "gltf_error.h"

namespace rx::asset {

// Exhaustive, compiler-checked mapping [matrix-issue02 row 4]: no
// `default:` case, deliberately -- this project's presets do not build
// with -Werror (verified: neither preset's CMake passes -Wall/-Werror),
// so this cannot be made a hard build failure, but omitting `default:`
// still gets a `-Wswitch` diagnostic the moment fastgltf ever adds a
// 16th Error member and this switch is not updated to match, which is
// the intent of "exhaustive compiler-enforced" here -- a silent fallback
// would defeat that entirely.
ImportError mapFastgltfError(fastgltf::Error error) {
    switch (error) {
        case fastgltf::Error::None:
            return ImportError::None;
        case fastgltf::Error::InvalidPath:
            return ImportError::InvalidPath;
        case fastgltf::Error::MissingExtensions:
            return ImportError::MissingExtensions;
        case fastgltf::Error::UnknownRequiredExtension:
            return ImportError::UnknownRequiredExtension;
        case fastgltf::Error::InvalidJson:
            return ImportError::InvalidJson;
        case fastgltf::Error::InvalidGltf:
            return ImportError::InvalidGltf;
        case fastgltf::Error::InvalidOrMissingAssetField:
            return ImportError::InvalidOrMissingAssetField;
        case fastgltf::Error::InvalidGLB:
            return ImportError::InvalidGLB;
        case fastgltf::Error::MissingField:
            return ImportError::MissingField;
        case fastgltf::Error::MissingExternalBuffer:
            return ImportError::MissingExternalBuffer;
        case fastgltf::Error::UnsupportedVersion:
            return ImportError::UnsupportedVersion;
        case fastgltf::Error::InvalidURI:
            return ImportError::InvalidURI;
        case fastgltf::Error::InvalidFileData:
            return ImportError::InvalidFileData;
        case fastgltf::Error::FailedWritingFiles:
            return ImportError::FailedWritingFiles;
        case fastgltf::Error::FileBufferAllocationFailed:
            return ImportError::FileBufferAllocationFailed;
    }
    // Unreachable for any of the 15 known members (see above); a future
    // 16th member falls through to here at runtime until this switch is
    // updated, so fail loudly rather than silently misreport it as None.
    return ImportError::InvalidFileData;
}

}  // namespace rx::asset
