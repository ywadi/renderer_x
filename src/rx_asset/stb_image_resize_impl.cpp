// The sole STB_IMAGE_RESIZE_IMPLEMENTATION translation unit for this whole
// program -- mirrors rx_rhi_vk/src/stb_impl.cpp's own single-TU-
// implementation discipline (stb_image.h's own real implementation TU),
// applied to stb_image_resize2.h: same vendored `stb` GitHub repo,
// third_party/CMakeLists.txt's own single stb FetchContent_Populate (both
// headers live in the same checkout, `stb_SOURCE_DIR`) -- so no new
// third_party/CMakeLists.txt wiring was needed, just this new TU added to
// rx_asset's own add_library() sources (rx_asset already inherits
// `stb_SOURCE_DIR` on its include path transitively, via its PUBLIC link
// against rx_rhi_vk, which already exposes it PUBLIC).
//
// [texture-path round, D10 Option A -- runtime mip-chain generation for
// the stb PNG/JPG decode path, .superpowers/sdd/2026-08-11-phase4-scene-
// assets/sponza-visual-investigation.md §2.7] This project's own "prefer
// ready-made libraries" rule (CLAUDE.md): stb_image_resize2 already
// expresses exactly the two box-average kernels rx_asset/texture_decode.cpp's
// generateStbMipChain() needs (sRGB-aware STBIR_TYPE_UINT8_SRGB, and plain
// linear STBIR_4CHANNEL with no alpha weighting) -- only the normal-map
// renormalization step on top (something no general-purpose image resizer
// has any reason to know about) is hand-rolled, in texture_decode.cpp
// itself, never here.
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
