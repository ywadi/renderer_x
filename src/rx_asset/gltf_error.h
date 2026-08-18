#pragma once

// gltf_error.h -- PRIVATE (not installed). The one translation unit that
// maps fastgltf::Error -> rx::asset::ImportError [matrix-issue02 row 4:
// "exhaustive switch over all 15 fastgltf::Error members"]. Deliberately
// isolated in its own tiny .cpp so the exhaustive switch (no `default:`
// case, on purpose -- see gltf_error.cpp) stays a single, easy-to-audit
// unit even though fastgltf/core.hpp's own Error enum is otherwise
// invisible to the rest of rx_asset's public API.
#include <rx_asset/import_error.h>
#include <fastgltf/core.hpp>

namespace rx::asset {

[[nodiscard]] ImportError mapFastgltfError(fastgltf::Error error);

}  // namespace rx::asset
