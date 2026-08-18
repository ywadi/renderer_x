#include <rx_asset/byte_source.h>
#include <rx_core/log.h>
#include <fstream>

namespace rx::asset {

FilesystemByteSource::FilesystemByteSource(std::filesystem::path baseDir) : baseDir_(std::move(baseDir)) {}

std::optional<std::vector<std::byte>> FilesystemByteSource::read(const std::string& uri) {
    // Defensive net only -- import_gltf.cpp's own absolute/network-URI
    // detection (matrix-issue02 row "Absolute / http(s) URIs") is what
    // decides policy and logs the WARN; this class never gets called for
    // those in normal operation, but never silently reads an unintended
    // absolute path off this process's root filesystem either if it
    // somehow is.
    std::filesystem::path uriPath(uri);
    if (uriPath.is_absolute()) {
        return std::nullopt;
    }

    std::filesystem::path resolved = baseDir_ / uriPath;
    std::ifstream file(resolved, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }

    const auto size = file.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    file.seekg(0);

    std::vector<std::byte> bytes(static_cast<size_t>(size));
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return std::nullopt;
    }
    return bytes;
}

}  // namespace rx::asset
