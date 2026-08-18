#pragma once

// rx_asset/mesh_asset.h -- Thread-affinity: none (plain data; every type
// in this header is a value type with no synchronization of its own).
// Public, engine-owned data types produced by the glTF importer [Phase 4
// Stage 1 Task 13, spec D6/D7/D12; FG2 (lights/cameras parse-and-
// preserve); seed 14 (skinning preserved)].
//
// Deliberately independent of fastgltf's own struct shapes -- no
// fastgltf type appears anywhere in this header -- so consuming this API
// never requires fastgltf's own include directory, matching this
// codebase's established header-hygiene convention (rx_graph/
// render_graph.h's "no volk in public headers" precedent, rx_graph/
// executor.h's identical rx_rhi_vk boundary). import_gltf.cpp is the
// only translation unit that ever names a fastgltf type; it converts
// everything into these types before Registry::importGltf() returns.
//
// PRESERVE-LATER DATA'S INDEX SPACE [deliberate simplification, see the
// task report]: per-vertex skin (JOINTS_0/WEIGHTS_0) and morph-target
// deltas below are stored in the SOURCE glTF accessor's own vertex
// order, NOT the post-meshoptimizer pool-vertex order the D8 render
// format (submesh geometry) ends up in. Nothing in Phase 4 consumes
// either -- there is no skinning or morphing renderer yet -- so there is
// no "correct" target order to remap them into today; whatever consumer
// lands in the animation phase will define its own vertex layout at that
// time regardless (very likely NOT the D8 render-optimized order, since
// skinning/morphing typically wants a compute-friendly SoA layout of its
// own). Keeping preserve-later data in source order keeps its own
// round-trip tests direct (compare against the file's own accessor
// values with no derived permutation to reason about) and avoids
// inventing a mapping nothing yet reads.

#include <rx_asset/geometry_pool.h>
#include <rx_core/handle.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace rx::asset {

using MeshHandle = rx::core::Handle<struct MeshTag>;
using TextureHandle = rx::core::Handle<struct TextureTag>;
using MaterialHandle = rx::core::Handle<struct MatTag>;

// Axis-aligned bounding box [G11]. Always computed from FINAL,
// post-meshoptimizer vertex positions -- NEVER trusted from a glTF
// accessor's own declared min/max (files lie; nothing revalidates them
// at load time, and the glTF spec does not require a loader to).
struct AABB {
    glm::vec3 min{std::numeric_limits<float>::infinity()};
    glm::vec3 max{-std::numeric_limits<float>::infinity()};

    // True for any AABB that has seen at least one expand() call,
    // including a single-point box (min == max, zero volume but valid);
    // false only for a default-constructed, never-expanded AABB.
    [[nodiscard]] bool isValid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }

    void expand(const glm::vec3& p);

    // Union of two AABBs. An invalid operand contributes nothing (the
    // other operand's box passes through unchanged); the union of two
    // invalid boxes is invalid.
    [[nodiscard]] static AABB unionOf(const AABB& a, const AABB& b);

    // Transforms all 8 corners by `m` and returns the AABB of the
    // resulting point cloud -- the only correct approach under rotation
    // and negative scale (transforming only min/max is wrong the moment
    // `m` rotates). Returns an invalid AABB unchanged (no corners to
    // transform).
    [[nodiscard]] AABB transformed(const glm::mat4& m) const;
};

// ---------------------------------------------------------------------
// Skinning [seed 14, preserve-later] -- entirely inert in Phase 4:
// nothing renders skinned geometry yet. Round-trip storage only.
// ---------------------------------------------------------------------

struct SkinData {
    bool present = false;
    std::string name;
    std::vector<int32_t> jointNodeIndices;       // glTF NODE indices (not rx handles -- node identity is not retained past flattening otherwise)
    std::vector<glm::mat4> inverseBindMatrices;   // parallel to jointNodeIndices; empty if the source skin omitted them (spec: identity implied)
    std::optional<int32_t> skeletonRootNodeIndex;
};

// Per-vertex JOINTS_0/WEIGHTS_0 for one primitive, in SOURCE accessor
// order (see this header's own top comment).
struct SkinVertexData {
    std::vector<glm::u16vec4> joints;   // JOINTS_0, widened to u16 (glTF permits u8 or u16 storage)
    std::vector<glm::vec4> weights;      // WEIGHTS_0
};

// ---------------------------------------------------------------------
// Morph targets [preserve-later] -- in SOURCE accessor order (see this
// header's own top comment).
// ---------------------------------------------------------------------

struct MorphTarget {
    std::vector<glm::vec3> positionDeltas;  // empty if this target carried no POSITION
    std::vector<glm::vec3> normalDeltas;    // empty if this target carried no NORMAL
    // Per the glTF spec, morph target TANGENT deltas are VEC3 (xyz
    // only -- handedness does not vary across a morph target, unlike
    // the base TANGENT attribute's own VEC4).
    std::vector<glm::vec3> tangentDeltas;   // empty if this target carried no TANGENT
};

// ---------------------------------------------------------------------
// Materials [D22-adjacent parameter set -- textures resolve in Task 14;
// D11 fallback handles carried until then].
// ---------------------------------------------------------------------

enum class MaterialDisposition : uint8_t {
    StandardPBR,
    // [gate ruling C5] KHR_materials_unlit maps here at import.
    Unlit,
};

enum class AlphaMode : uint8_t { Opaque, Mask, Blend };

struct SamplerDesc {
    // Raw glTF GL wrap/filter enum values (Wrap::Repeat=10497 etc.) --
    // carried verbatim; Task 14's sampler cache maps these to VkSampler
    // state. 0 means "unspecified" for the two filters (spec default:
    // implementation-chosen/auto filtering).
    uint32_t wrapS = 10497;
    uint32_t wrapT = 10497;
    uint32_t magFilter = 0;
    uint32_t minFilter = 0;
};

// One material's reference to one texture slot. `handle` is always the
// registry's D11 fallback in Task 13 (no TextureCache exists yet); the
// raw `imageIndex`/sampler/UV fields are carried so Task 14 can resolve
// the real texture without re-parsing the source glTF.
struct TextureRef {
    bool present = false;
    TextureHandle handle;
    uint32_t imageIndex = 0;
    uint32_t texCoordIndex = 0;
    SamplerDesc sampler;

    // KHR_texture_transform [consume-now per gate ruling C4]: offset/
    // scale applied here; rotation stays log-don't-drop (always 0 in
    // this field -- see the importer's own WARN when the source
    // document's rotation != 0).
    glm::vec2 uvOffset{0.0F, 0.0F};
    glm::vec2 uvScale{1.0F, 1.0F};
};

struct MaterialAsset {
    MaterialDisposition disposition = MaterialDisposition::StandardPBR;

    glm::vec4 baseColorFactor{1.0F, 1.0F, 1.0F, 1.0F};
    float metallicFactor = 1.0F;
    float roughnessFactor = 1.0F;
    glm::vec3 emissiveFactor{0.0F, 0.0F, 0.0F};
    float emissiveStrength = 1.0F;  // KHR_materials_emissive_strength value carried when != 1 (log-don't-drop; free from the parsed field)
    float normalScale = 1.0F;
    float occlusionStrength = 1.0F;

    TextureRef baseColorTexture;
    TextureRef metallicRoughnessTexture;
    TextureRef normalTexture;
    TextureRef occlusionTexture;
    TextureRef emissiveTexture;

    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5F;
    bool doubleSided = false;

    std::string name;
};

// ---------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------

struct Submesh {
    MeshRange range;
    AABB bounds;
    MaterialHandle material;

    // [Preserve-later] populated only when the source primitive carried
    // the corresponding data; empty otherwise. See this header's own top
    // comment for the source-order rationale.
    SkinVertexData skinVertices;
    std::vector<MorphTarget> morphTargets;
};

struct MeshAsset {
    std::vector<Submesh> submeshes;
    AABB bounds;                     // union of every submesh's bounds
    SkinData skin;                    // [seed 14] preserved, unused
    std::vector<float> morphWeights;  // Mesh::weights (default weights), preserved
};

// ---------------------------------------------------------------------
// Node graph -- flattened at import [D12]
// ---------------------------------------------------------------------

struct InstanceRecord {
    MeshHandle mesh;
    glm::mat4 worldTransform{1.0F};

    // [D12 amendment] true iff worldTransform's upper-left 3x3 has a
    // negative determinant (a winding flip -- e.g. one negative scale
    // axis). Flagged and WARNed at import (matrix-issue02 row "Negative /
    // non-uniform scale"); Phase 4 does not itself correct winding, so a
    // future consumer (draw list / pipeline selection) can react to this
    // flag.
    bool negativeDeterminant = false;

    int32_t sourceNodeIndex = -1;  // diagnostic only, not a stable identity across reimports
};

// [FG2, preserve-later] Parsed and stored; unconsumed until the
// techniques phase.
struct CameraData {
    enum class Type : uint8_t { Perspective, Orthographic };
    Type type = Type::Perspective;

    // Perspective
    std::optional<float> aspectRatio;
    float yfov = 0.0F;
    std::optional<float> zfar;  // absent = infinite-perspective projection
    float znear = 0.0F;

    // Orthographic
    float xmag = 0.0F;
    float ymag = 0.0F;
    // (orthographic zfar/znear reuse the fields above -- the glTF spec's
    // own orthographic camera also has zfar/znear, just not zfar-optional)

    std::string name;
    glm::mat4 worldTransform{1.0F};
    int32_t sourceNodeIndex = -1;
};

// [FG2, preserve-later] KHR_lights_punctual.
struct LightData {
    enum class Type : uint8_t { Directional, Spot, Point };
    Type type = Type::Directional;

    glm::vec3 color{1.0F, 1.0F, 1.0F};
    float intensity = 1.0F;
    std::optional<float> range;
    std::optional<float> innerConeAngle;  // spot only
    std::optional<float> outerConeAngle;  // spot only

    std::string name;
    glm::mat4 worldTransform{1.0F};
    int32_t sourceNodeIndex = -1;
};

// ---------------------------------------------------------------------
// Animations [preserve-later]
// ---------------------------------------------------------------------

enum class AnimationInterpolation : uint8_t { Linear, Step, CubicSpline };
enum class AnimationPath : uint8_t { Translation, Rotation, Scale, Weights };

// One glTF animation.sampler. `outputFlat` is the RAW output accessor
// data, flattened, in source order -- length =
// keyframeCount * elementStride * (interpolation == CubicSpline ? 3 : 1).
// `elementStride` is 3 for Translation/Scale, 4 for Rotation (quaternion
// x,y,z,w), and the morph-target count for Weights. This is the
// glTF-native on-disk layout verbatim (including CUBICSPLINE's
// in-tangent/value/out-tangent triplets) -- the simplest representation
// that is losslessly, directly round-trippable without inventing a fixed
// element width that would truncate a >4-morph-target Weights sampler.
struct AnimationSamplerData {
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
    std::vector<float> inputTimes;
    std::vector<float> outputFlat;
    uint32_t elementStride = 3;
};

struct AnimationChannelData {
    std::optional<int32_t> targetNodeIndex;
    AnimationPath path = AnimationPath::Translation;
    uint32_t samplerIndex = 0;
};

struct AnimationClipData {
    std::string name;
    std::vector<AnimationChannelData> channels;
    std::vector<AnimationSamplerData> samplers;
};

struct ImportedScene {
    std::vector<InstanceRecord> instances;
    std::vector<CameraData> cameras;
    std::vector<LightData> lights;
    std::vector<AnimationClipData> animations;
};

// ---------------------------------------------------------------------
// Textures [Task 14 owns real content; this is the seed-14-style
// placeholder the Registry's fallback texture handle resolves to until
// then -- see registry.h's own comment].
// ---------------------------------------------------------------------

struct TextureAsset {
    bool isFallback = true;
};

}  // namespace rx::asset
