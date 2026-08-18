#include <rx_asset/registry.h>
#include "fallbacks.h"
#include "import_gltf.h"
#include <rx_core/log.h>
#include <fstream>

namespace rx::asset {

namespace {

uint64_t packHandle(uint32_t index, uint32_t generation) { return (static_cast<uint64_t>(index) << 32) | generation; }

template <typename Tag>
uint64_t packHandle(rx::core::Handle<Tag> handle) {
    return packHandle(handle.index(), handle.generation());
}

}  // namespace

Registry::Registry() {
    fallbackMesh_ = meshes_.acquire(makeFallbackMeshAsset());
    fallbackMaterial_ = materials_.acquire(makeFallbackMaterialAsset());
    fallbackTexture_ = textures_.acquire(makeFallbackTextureAsset());
}

Registry::~Registry() = default;

ImportResult Registry::importGltf(std::span<const std::byte> documentBytes, ByteSource& source, GeometryPool& pool,
                                   rx::task::Scheduler& scheduler, TextureCache* textures) {
    return importGltfPipeline(*this, documentBytes, source, pool, scheduler, textures);
}

ImportResult Registry::importGltf(const std::filesystem::path& path, GeometryPool& pool, rx::task::Scheduler& scheduler,
                                   TextureCache* textures) {
    // The one sanctioned direct-filesystem read in this whole library
    // [byte_source.h's own top comment]: reading the MAIN document
    // itself for this convenience overload. Every OTHER byte (every
    // external buffer/image URI the document goes on to reference)
    // still resolves exclusively through the FilesystemByteSource this
    // wraps below.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        RX_LOG_ERROR("rx_asset: Registry::importGltf: failed to open '{}'", path.string());
        ImportResult result;
        result.error = ImportError::ByteSourceUnavailable;
        return result;
    }

    const auto size = file.tellg();
    if (size < 0) {
        RX_LOG_ERROR("rx_asset: Registry::importGltf: failed to determine size of '{}'", path.string());
        ImportResult result;
        result.error = ImportError::ByteSourceUnavailable;
        return result;
    }
    file.seekg(0);

    std::vector<std::byte> bytes(static_cast<size_t>(size));
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        RX_LOG_ERROR("rx_asset: Registry::importGltf: failed to read '{}'", path.string());
        ImportResult result;
        result.error = ImportError::ByteSourceUnavailable;
        return result;
    }

    FilesystemByteSource fsSource(path.has_parent_path() ? path.parent_path() : std::filesystem::path("."));
    return importGltf(bytes, fsSource, pool, scheduler, textures);
}

const MeshAsset& Registry::mesh(MeshHandle handle) const {
    if (nonresidentMesh_.find(packHandle(handle)) == nonresidentMesh_.end()) {
        if (const MeshAsset* asset = meshes_.get(handle)) {
            return *asset;
        }
    }
    const MeshAsset* fallback = meshes_.get(fallbackMesh_);
    return *fallback;  // always resident -- created in the constructor, never evicted/released
}

const MaterialAsset& Registry::material(MaterialHandle handle) const {
    if (nonresidentMaterial_.find(packHandle(handle)) == nonresidentMaterial_.end()) {
        if (const MaterialAsset* asset = materials_.get(handle)) {
            return *asset;
        }
    }
    const MaterialAsset* fallback = materials_.get(fallbackMaterial_);
    return *fallback;
}

const TextureAsset& Registry::texture(TextureHandle handle) const {
    if (const TextureAsset* asset = textures_.get(handle)) {
        return *asset;
    }
    const TextureAsset* fallback = textures_.get(fallbackTexture_);
    return *fallback;
}

void Registry::evictForTesting(MeshHandle handle) { nonresidentMesh_.insert(packHandle(handle)); }

void Registry::evictForTesting(MaterialHandle handle) { nonresidentMaterial_.insert(packHandle(handle)); }

}  // namespace rx::asset
