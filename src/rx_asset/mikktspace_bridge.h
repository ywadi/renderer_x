#pragma once

// mikktspace_bridge.h -- PRIVATE (not installed). Thread-affinity: NONE
// -- MikkTSpace's own header documents both entry points as thread-safe
// ("these are both thread safe!", mikktspace.h), so this is safe to call
// from a parallelFor() worker chunk [D5/matrix-issue02 row 15, row 5's
// "MikkTSpace's documented thread-safety permits per-primitive parallel
// generation"].
#include <glm/glm.hpp>
#include <span>
#include <vector>

namespace rx::asset {

// Generates per-corner (DEINDEXED) tangents via MikkTSpace [G2, matrix
// row 5]. `positions`/`normals`/`uvs` must all be the same length, a
// multiple of 3 (one entry per triangle corner) -- MikkTSpace's own
// interface is strictly per-face-corner, and its own doc comment is
// explicit that averaging results back through an existing index list
// "WILL produce INCORRECT results. DO NOT!" -- the caller (gltf_pipeline.cpp)
// deindexes before calling this and re-welds afterward via
// meshopt_generateVertexRemap over the FULL 48-byte vertex (tangent
// included), never by hand.
//
// Returns one glm::vec4 per corner (xyz = unit tangent, w = handedness
// sign, exactly ±1), same length/order as the inputs. Degenerate
// triangles (zero-area position or UV) inherit a neighboring corner's
// tangent space per MikkTSpace's own MARK_DEGENERATE handling -- this
// function additionally clamps any NaN/Inf MikkTSpace could still
// theoretically emit (e.g. an isolated fully-degenerate primitive with
// no non-degenerate neighbor to inherit from) to a safe +X/w=1 default,
// so a degenerate-UV fixture can never produce a NaN tangent regardless
// of MikkTSpace's own internal fallback coverage [matrix row 5's
// "degenerate-UV fixture imports without NaN tangents"].
[[nodiscard]] std::vector<glm::vec4> generateTangentsDeindexed(std::span<const glm::vec3> positions,
                                                                 std::span<const glm::vec3> normals,
                                                                 std::span<const glm::vec2> uvs);

}  // namespace rx::asset
