#include "fallbacks.h"

namespace rx::asset {

MeshAsset makeFallbackMeshAsset() {
    MeshAsset asset;
    // Deliberately empty submeshes + invalid bounds -- see fallbacks.h.
    return asset;
}

MaterialAsset makeFallbackMaterialAsset() {
    MaterialAsset material;
    material.disposition = MaterialDisposition::Unlit;
    material.baseColorFactor = glm::vec4(1.0F, 0.0F, 1.0F, 1.0F);  // magenta [D11]
    material.name = "rx::asset fallback (error) material";
    return material;
}

TextureAsset makeFallbackTextureAsset() {
    TextureAsset texture;
    texture.isFallback = true;
    return texture;
}

}  // namespace rx::asset
