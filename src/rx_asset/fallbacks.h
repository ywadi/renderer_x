#pragma once

// fallbacks.h -- PRIVATE (not installed). D11 registry-owned default
// assets: "a bad asset must be visible and named, never a crash and
// never silently absent." Pure factory functions (no Registry access
// needed) so registry.cpp's constructor can insert their results
// directly into its own HandlePools.
#include <rx_asset/mesh_asset.h>

namespace rx::asset {

// Empty mesh (zero submeshes, invalid/zero bounds) -- resolved by
// mesh() for a dead or nonresident MeshHandle [D24]. There is no D11
// text specifically about a fallback MESH (D11 talks about textures/
// materials only), but D24's residency-tolerant-resolve invariant
// applies uniformly across every Registry-owned asset kind, and
// mesh()'s return type is `const MeshAsset&` -- something concrete has
// to back that reference. An empty submesh list draws nothing, which is
// the correct "visible and named" behavior for a mesh specifically (an
// invisible placeholder, not a crash) -- see the task report.
[[nodiscard]] MeshAsset makeFallbackMeshAsset();

// D11's "error material (unlit magenta)".
[[nodiscard]] MaterialAsset makeFallbackMaterialAsset();

// D11's texture-role fallbacks are role-typed (checkerboard vs flat-
// normal vs neutral-MR) and belong to Task 14's TextureCache, which does
// not exist yet -- see mesh_asset.h's own comment on TextureAsset. This
// is the single placeholder Task 13's Registry needs so MaterialAsset's
// TextureRef::handle always resolves to SOMETHING.
[[nodiscard]] TextureAsset makeFallbackTextureAsset();

}  // namespace rx::asset
