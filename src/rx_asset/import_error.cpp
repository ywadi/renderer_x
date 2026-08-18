#include <rx_asset/import_error.h>

namespace rx::asset {

std::string_view importErrorName(ImportError error) {
    switch (error) {
        case ImportError::None:
            return "None";
        case ImportError::InvalidPath:
            return "InvalidPath";
        case ImportError::MissingExtensions:
            return "MissingExtensions";
        case ImportError::UnknownRequiredExtension:
            return "UnknownRequiredExtension";
        case ImportError::InvalidJson:
            return "InvalidJson";
        case ImportError::InvalidGltf:
            return "InvalidGltf";
        case ImportError::InvalidOrMissingAssetField:
            return "InvalidOrMissingAssetField";
        case ImportError::InvalidGLB:
            return "InvalidGLB";
        case ImportError::MissingField:
            return "MissingField";
        case ImportError::MissingExternalBuffer:
            return "MissingExternalBuffer";
        case ImportError::UnsupportedVersion:
            return "UnsupportedVersion";
        case ImportError::InvalidURI:
            return "InvalidURI";
        case ImportError::InvalidFileData:
            return "InvalidFileData";
        case ImportError::FailedWritingFiles:
            return "FailedWritingFiles";
        case ImportError::FileBufferAllocationFailed:
            return "FileBufferAllocationFailed";
        case ImportError::ByteSourceUnavailable:
            return "ByteSourceUnavailable";
    }
    return "Unknown";
}

}  // namespace rx::asset
