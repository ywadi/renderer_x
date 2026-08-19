#include <rx_scene/draw_list.h>

#include <rx_asset/registry.h>
#include <rx_core/debug_checks.h>
#include <rx_core/profile.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace rx::scene {

// ---------------------------------------------------------------------
// Injected resolution seams -- production adapters.
// ---------------------------------------------------------------------

MeshSubmeshesFn meshSubmeshesFromRegistry(const asset::Registry& registry) {
    // See draw_list.h's own comment: forwards verbatim to
    // Registry::mesh(handle).submeshes, already D24 residency-tolerant (a
    // dead/evicted handle resolves to the D11 fallback mesh's EMPTY
    // submesh list -- rx_asset/fallbacks.cpp's makeFallbackMeshAsset() --
    // so a renderable referencing an evicted mesh contributes zero draw
    // records here, never a crash).
    return [&registry](asset::MeshHandle mesh) -> std::span<const asset::Submesh> { return registry.mesh(mesh).submeshes; };
}

MaterialResolveFn materialResolveFromRegistry(const asset::Registry& registry) {
    return [&registry](asset::MaterialHandle handle) -> ResolvedMaterial {
        const asset::MaterialAsset& mat = registry.material(handle);  // D24 residency-tolerant
        return ResolvedMaterial{handle.index(), mat.alphaMode, mat.doubleSided, mat.disposition};
    };
}

// ---------------------------------------------------------------------
// Sort key.
// ---------------------------------------------------------------------

namespace sortkey {

uint32_t depthBucketBits(float ndcDepth, uint32_t bucketBits) {
    const float clamped = std::clamp(ndcDepth, 0.0F, 1.0F);
    // Reinterpreting a non-negative float's raw bit pattern as a uint32_t
    // preserves ordering (IEEE-754's own monotonicity property for
    // non-negative values) -- no multiplication/rescale, per the gate
    // ruling's own "never a linear rescale" requirement.
    const uint32_t bits = std::bit_cast<uint32_t>(clamped);
    if (bucketBits >= 32) {
        return bits;
    }
    return bits >> (32 - bucketBits);
}

uint16_t pipelineKeyOf(asset::MaterialDisposition disposition, asset::AlphaMode alphaMode, bool doubleSided) {
    uint16_t key = 0;
    key = static_cast<uint16_t>(key | ((static_cast<uint16_t>(disposition) & 0x1U) << 3));
    key = static_cast<uint16_t>(key | ((static_cast<uint16_t>(alphaMode) & 0x3U) << 1));
    key = static_cast<uint16_t>(key | (doubleSided ? 1U : 0U));
    return key;
}

uint16_t meshKeyOf(uint32_t firstIndex, int32_t vertexOffset) {
    // Plain multiplicative hash (Knuth's constants) -- only used to spread
    // distinct (firstIndex,vertexOffset) pairs across the sort key's mesh
    // tier for ORDERING purposes; actual instancing-collapse identity is
    // re-verified against the real fields directly (draw_list.cpp's
    // sameDrawIdentity()), so a hash collision here can only ever cost a
    // missed collapse opportunity or a slightly coarser sort grouping,
    // never a correctness bug.
    uint32_t h = firstIndex * 2654435761U;
    h ^= static_cast<uint32_t>(vertexOffset) * 2246822519U;
    h ^= h >> 15;
    return static_cast<uint16_t>(h & 0xFFFFU);
}

namespace {
uint64_t field(uint64_t value, uint32_t bits, uint32_t shift) {
    const uint64_t mask = (bits >= 64) ? ~uint64_t{0} : ((uint64_t{1} << bits) - 1);
    return (value & mask) << shift;
}
uint64_t extract(uint64_t key, uint32_t bits, uint32_t shift) {
    const uint64_t mask = (bits >= 64) ? ~uint64_t{0} : ((uint64_t{1} << bits) - 1);
    return (key >> shift) & mask;
}
}  // namespace

uint64_t encodeOpaque(uint8_t priority, uint16_t pipelineKey, uint16_t materialKey, float ndcDepth, uint32_t tieBreak) {
    const uint32_t rawDepth = depthBucketBits(ndcDepth, kOpaqueDepthBits);
    const uint32_t depthMask = static_cast<uint32_t>((uint64_t{1} << kOpaqueDepthBits) - 1);
    // Bitwise-complement within the field width: ascending key order becomes
    // DEcreasing depth (nearest/highest-reversed-Z first) -- Filament's own
    // `~distanceBits` trick, applied to the OPAQUE partition instead (see
    // this header's own direction-test comment).
    const uint32_t invertedDepth = (~rawDepth) & depthMask;

    uint64_t key = 0;
    key |= field(priority, kOpaquePriorityBits, kOpaquePriorityShift);
    key |= field(pipelineKey, kOpaquePipelineKeyBits, kOpaquePipelineKeyShift);
    key |= field(materialKey, kOpaqueMaterialKeyBits, kOpaqueMaterialKeyShift);
    key |= field(invertedDepth, kOpaqueDepthBits, kOpaqueDepthShift);
    key |= field(tieBreak, kOpaqueTieBreakBits, kOpaqueTieBreakShift);
    return key;
}

uint64_t encodeBlend(uint8_t priority, float ndcDepth, uint32_t tieBreak) {
    // NOT inverted: ascending key order is INcreasing depth
    // (farthest/lowest-reversed-Z first) -- strict back-to-front, D14.
    const uint32_t depth = depthBucketBits(ndcDepth, kBlendDepthBits);

    uint64_t key = 0;
    key |= field(priority, kBlendPriorityBits, kBlendPriorityShift);
    // reservedBlendOrder [RC5/#5]: always 0 in Phase 4 -- no write (bits
    // already zero-initialized); a future per-primitive override lands
    // here with no ABI break.
    key |= field(depth, kBlendDepthBits, kBlendDepthShift);
    key |= field(tieBreak, kBlendTieBreakBits, kBlendTieBreakShift);
    return key;
}

uint64_t encodeShadow(uint16_t pipelineKey, uint16_t meshKey, uint32_t tieBreak) {
    uint64_t key = 0;
    key |= field(pipelineKey, kShadowPipelineKeyBits, kShadowPipelineKeyShift);
    key |= field(meshKey, kShadowMeshKeyBits, kShadowMeshKeyShift);
    key |= field(tieBreak, kShadowTieBreakBits, kShadowTieBreakShift);
    return key;
}

DecodedOpaque decodeOpaque(uint64_t key) {
    DecodedOpaque d;
    d.tieBreak = static_cast<uint32_t>(extract(key, kOpaqueTieBreakBits, kOpaqueTieBreakShift));
    d.invertedDepthBucket = static_cast<uint32_t>(extract(key, kOpaqueDepthBits, kOpaqueDepthShift));
    d.materialKey = static_cast<uint16_t>(extract(key, kOpaqueMaterialKeyBits, kOpaqueMaterialKeyShift));
    d.pipelineKey = static_cast<uint16_t>(extract(key, kOpaquePipelineKeyBits, kOpaquePipelineKeyShift));
    d.priority = static_cast<uint8_t>(extract(key, kOpaquePriorityBits, kOpaquePriorityShift));
    return d;
}

DecodedBlend decodeBlend(uint64_t key) {
    DecodedBlend d;
    d.tieBreak = static_cast<uint32_t>(extract(key, kBlendTieBreakBits, kBlendTieBreakShift));
    d.depthBucket = static_cast<uint32_t>(extract(key, kBlendDepthBits, kBlendDepthShift));
    d.reservedBlendOrder = static_cast<uint32_t>(extract(key, kBlendReservedBlendOrderBits, kBlendReservedBlendOrderShift));
    d.priority = static_cast<uint8_t>(extract(key, kBlendPriorityBits, kBlendPriorityShift));
    return d;
}

DecodedShadow decodeShadow(uint64_t key) {
    DecodedShadow d;
    d.tieBreak = static_cast<uint32_t>(extract(key, kShadowTieBreakBits, kShadowTieBreakShift));
    d.meshKey = static_cast<uint16_t>(extract(key, kShadowMeshKeyBits, kShadowMeshKeyShift));
    d.pipelineKey = static_cast<uint16_t>(extract(key, kShadowPipelineKeyBits, kShadowPipelineKeyShift));
    return d;
}

}  // namespace sortkey

namespace {

// ---------------------------------------------------------------------
// Small, free helpers -- pure functions, no DrawListBuilder state.
// ---------------------------------------------------------------------

// p-vertex (positive-vertex) AABB-vs-frustum test [Real-Time Rendering,
// 4th ed.]: for each plane, the AABB corner most aligned with the plane's
// own normal sign per axis is the corner LEAST likely to be rejected; if
// even THAT corner is outside, the whole box is outside. One dot product
// per plane instead of testing all 8 corners. `box.isValid() == false`
// (a never-expanded AABB, e.g. a renderable whose mesh resolved to the D24
// fallback's empty submesh list) is treated as never-visible -- nothing to
// draw regardless, and there is no sensible corner to test.
bool aabbOutsidePlane(const asset::AABB& box, const glm::vec4& plane) {
    const glm::vec3 p(plane.x >= 0.0F ? box.max.x : box.min.x, plane.y >= 0.0F ? box.max.y : box.min.y,
                       plane.z >= 0.0F ? box.max.z : box.min.z);
    return (plane.x * p.x + plane.y * p.y + plane.z * p.z + plane.w) < 0.0F;
}

bool aabbVisible(const asset::AABB& box, const FrustumPlanes& planes) {
    if (!box.isValid()) {
        return false;
    }
    for (const glm::vec4& plane : planes) {
        if (aabbOutsidePlane(box, plane)) {
            return false;
        }
    }
    return true;
}

// Standard AABB-vs-AABB overlap test. `orthoBox.max.z == +infinity`
// (buildShadow()'s own conservative extrusion -- see that method's
// comment for the full worked-example derivation) makes the
// Z-upper-bound half of the test trivially true, so a caster arbitrarily
// far upstream of the light's own travel direction is never excluded on
// that account -- without any special-casing here.
bool aabbOverlaps(const asset::AABB& a, const asset::AABB& b) {
    if (!a.isValid() || !b.isValid()) {
        return false;
    }
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y && a.min.z <= b.max.z &&
           a.max.z >= b.min.z;
}

float computeNdcDepth(const glm::mat4& viewProj, const glm::vec3& worldPos) {
    const glm::vec4 clip = viewProj * glm::vec4(worldPos, 1.0F);
    if (clip.w <= 1e-6F) {
        // Defensive only: nothing that survived frustum culling should
        // reach here with a non-positive clip.w. Treat as "nearest"
        // (depth 1.0 under reversed-Z) rather than dividing by a
        // near-zero/negative denominator.
        return 1.0F;
    }
    return std::clamp(clip.z / clip.w, 0.0F, 1.0F);
}

// An arbitrary, consistent orthonormal basis with `forward` as its Z axis
// -- used only to fit/extrude the shadow ortho box (buildShadow()), never
// to bind a real GPU shadow-pass projection (Task 22's own scope). `eye`
// is irrelevant for a DIRECTIONAL light (only orientation matters), so the
// origin is used; the up-hint falls back to +Z when `forward` is
// near-parallel to the default +Y up axis, avoiding glm::lookAt()'s own
// degenerate (cross-product-near-zero) case.
glm::mat4 lightSpaceView(const glm::vec3& lightDir) {
    const glm::vec3 forward = glm::normalize(lightDir);
    glm::vec3 upHint(0.0F, 1.0F, 0.0F);
    if (std::abs(glm::dot(forward, upHint)) > 0.99F) {
        upHint = glm::vec3(0.0F, 0.0F, 1.0F);
    }
    return glm::lookAt(glm::vec3(0.0F), forward, upHint);
}

asset::AABB unionOfBounds(std::span<const asset::AABB> allBounds, std::span<const uint32_t> indices) {
    asset::AABB result;
    for (uint32_t idx : indices) {
        result = asset::AABB::unionOf(result, allBounds[idx]);
    }
    return result;
}

// Tracks contiguous [firstCommand, firstCommand+commandCount) runs of a
// single blockId as commands are appended in final sorted order, emitting
// one `BlockRange` each time the block changes (or emit()/emitShadow()
// explicitly close the run at partition end) -- see BlockRange's own
// header comment for why this invariant matters (GeometryPool::bind()'s
// one-block-per-MDI-submission contract).
class BlockRunTracker {
public:
    explicit BlockRunTracker(std::vector<BlockRange>& blocks) : blocks_(blocks) {}

    void note(uint32_t blockId, uint32_t commandIndex) {
        if (have_ && blockId == currentBlockId_) {
            return;
        }
        close(commandIndex);
        currentBlockId_ = blockId;
        currentFirst_ = commandIndex;
        have_ = true;
    }

    void close(uint32_t endExclusiveCommandIndex) {
        if (have_) {
            blocks_.push_back(BlockRange{currentBlockId_, currentFirst_, endExclusiveCommandIndex - currentFirst_});
        }
        have_ = false;
    }

private:
    std::vector<BlockRange>& blocks_;
    uint32_t currentBlockId_ = 0;
    uint32_t currentFirst_ = 0;
    bool have_ = false;
};

}  // namespace

// ---------------------------------------------------------------------
// DrawListBuilder::Impl -- reused private scratch storage [zero-alloc].
// ---------------------------------------------------------------------

struct DrawListBuilder::Impl {
    MeshSubmeshesFn meshSubmeshes;
    MaterialResolveFn materialResolve;

    enum class CullReason : uint8_t { Dead, Layer, Frustum, Visible };

    // One row per SCENE SLOT (dead slots included -- see this class's own
    // aliveSpan()/generationsSpan() comment on scene.h) -- reused, resized
    // (never reassigned) every call; only grows on a new peak slot count.
    std::vector<CullReason> cullReasons;
    std::vector<uint32_t> visibleIndices;        // build()'s visible set, or buildShadow()'s caster set
    std::vector<uint32_t> cameraVisibleIndices;  // buildShadow()'s own camera-visible-bounds sub-step

    // Pre-sort per-submesh draw record, expanded from `visibleIndices` --
    // one entry per (visible renderable, submesh) pair.
    struct PendingRecord {
        uint32_t indexCount = 0;
        uint32_t firstIndex = 0;
        int32_t vertexOffset = 0;
        uint32_t blockId = 0;
        uint32_t materialIndex = 0;
        uint32_t instanceDataIndex = 0;
        uint16_t pipelineKey = 0;
        uint16_t meshKey = 0;
        uint8_t priority = 0;
        bool isBlend = false;
        float ndcDepth = 0.0F;
        uint32_t tieBreak = 0;
        uint64_t sortKey = 0;
    };
    std::vector<PendingRecord> pending;
    std::vector<PendingRecord> opaqueRecords;
    std::vector<PendingRecord> blendRecords;

    // One instancing-collapsed [D26.3] opaque draw, formed from a
    // contiguous identity-run of `opaqueRecords` (see collapseAndSortOpaque()
    // below) -- `[firstMember, firstMember+memberCount)` indexes directly
    // into `opaqueRecords`, whose relative order within that range is FIXED
    // once the identity sort runs and never touched again (only the GROUPS
    // themselves are re-sorted afterward, by depth/pipeline/material).
    struct CollapsedGroup {
        uint32_t indexCount = 0;
        uint32_t firstIndex = 0;
        int32_t vertexOffset = 0;
        uint32_t blockId = 0;
        uint8_t priority = 0;
        uint16_t pipelineKey = 0;
        uint32_t materialIndex = 0;
        float representativeDepth = 0.0F;  // the NEAREST member's NDC depth -- this run's own best early-Z position
        uint32_t tieBreak = 0;             // the smallest member tieBreak -- deterministic regardless of std::sort internals
        uint32_t firstMember = 0;
        uint32_t memberCount = 0;
        uint64_t sortKey = 0;
    };
    std::vector<CollapsedGroup> opaqueGroups;

    static bool sameDrawIdentity(const PendingRecord& a, const PendingRecord& b) {
        // The REAL (pipeline, material, mesh range, block) identity check
        // D26.3 requires for collapse -- deliberately independent of the
        // sort key's own (hashed/truncated) pipelineKey/materialKey fields,
        // which exist only to influence the FINAL depth-ordering, never to
        // decide collapse eligibility (a hash collision there can only ever
        // cost a slightly coarser sort grouping, never a correctness bug).
        // `priority` is checked separately by collapseAndSortOpaque()'s own
        // run-detection loop, not folded in here, so this function stays a
        // pure statement of "these two draws are GPU-identical."
        return a.blockId == b.blockId && a.indexCount == b.indexCount && a.firstIndex == b.firstIndex &&
               a.vertexOffset == b.vertexOffset && a.materialIndex == b.materialIndex;
    }

    // Frustum + camera cullMask culling [D15] -- the parallel AABB-batch
    // pass (gate ruling: fixed index-range chunks; the DETERMINISTIC
    // mechanism here is simpler than "concatenate chunk outputs in
    // chunk-index order" while giving the identical guarantee -- see this
    // method's own comment below).
    void cullView(const Scene& scene, const Camera& camera, task::Scheduler& scheduler,
                  std::vector<uint32_t>& outVisibleIndices, CullCounters& counters) {
        RX_ZONE_NAMED("DrawListBuilder::cullView");
        const std::span<const uint8_t> alive = scene.aliveSpan();
        const std::span<const asset::AABB> bounds = scene.worldBoundsSpan();
        const std::span<const uint32_t> layers = scene.layersSpan();
        const uint32_t n = static_cast<uint32_t>(alive.size());

        // Reused (never reassigned) -- resize() only allocates when `n`
        // exceeds this vector's current capacity (a new peak), matching the
        // zero-alloc-in-steady-state policy every reused buffer in this
        // class follows.
        cullReasons.assign(n, CullReason::Dead);

        const FrustumPlanes planes = camera.cullingFrustumPlanes();
        const uint32_t cullMask = camera.cullMask;

        if (n > 0) {
            // DETERMINISM [gate ruling #6]: the ruling's own named
            // mechanism is "fixed index-range chunks, cull into per-chunk
            // buffers, concatenate in chunk-index order." This pass
            // achieves the SAME guarantee (byte-identical output regardless
            // of --threads) via an even simpler construction: each index's
            // written CullReason depends ONLY on that index's own data
            // (embarrassingly parallel, no shared mutable ordering state
            // written here), and the SERIAL compaction pass below always
            // scans slot-index order 0..n-1 -- a fixed, thread-count-
            // independent order by construction, with no chunk-boundary
            // bookkeeping to get wrong.
            scheduler.parallelFor(n, /*grainSize=*/512, [&](uint32_t begin, uint32_t end, uint32_t) {
                for (uint32_t i = begin; i < end; ++i) {
                    if (alive[i] == 0) {
                        cullReasons[i] = CullReason::Dead;
                        continue;
                    }
                    if ((layers[i] & cullMask) == 0) {
                        cullReasons[i] = CullReason::Layer;
                        continue;
                    }
                    cullReasons[i] = aabbVisible(bounds[i], planes) ? CullReason::Visible : CullReason::Frustum;
                }
            });
        }

        outVisibleIndices.clear();
        counters = CullCounters{};
        for (uint32_t i = 0; i < n; ++i) {
            switch (cullReasons[i]) {
                case CullReason::Dead:
                    break;
                case CullReason::Layer:
                    ++counters.totalCandidates;
                    ++counters.culledByLayerMask;
                    break;
                case CullReason::Frustum:
                    ++counters.totalCandidates;
                    ++counters.culledByFrustum;
                    break;
                case CullReason::Visible:
                    ++counters.totalCandidates;
                    ++counters.visible;
                    outVisibleIndices.push_back(i);
                    break;
            }
        }
    }

    // Shadow-caster eligibility + extruded-ortho-box culling [D15/RC5].
    // `orthoBoxExtruded` is already light-space with its near (Z-minus)
    // side unbounded -- see buildShadow()'s own comment for the derivation.
    void cullShadowCasters(const Scene& scene, const asset::AABB& orthoBoxExtruded, uint8_t lightChannels,
                            const glm::mat4& lightView, task::Scheduler& scheduler, std::vector<uint32_t>& outVisibleIndices,
                            CullCounters& counters) {
        RX_ZONE_NAMED("DrawListBuilder::cullShadowCasters");
        const std::span<const uint8_t> alive = scene.aliveSpan();
        const std::span<const asset::AABB> bounds = scene.worldBoundsSpan();
        const std::span<const uint8_t> channels = scene.channelsSpan();
        const std::span<const uint8_t> castsShadows = scene.castsShadowsSpan();
        const uint32_t n = static_cast<uint32_t>(alive.size());

        cullReasons.assign(n, CullReason::Dead);

        if (n > 0) {
            scheduler.parallelFor(n, /*grainSize=*/512, [&](uint32_t begin, uint32_t end, uint32_t) {
                for (uint32_t i = begin; i < end; ++i) {
                    if (alive[i] == 0) {
                        cullReasons[i] = CullReason::Dead;
                        continue;
                    }
                    // "Layer" reused as "caster-eligibility failed" [RC5
                    // coupled model: castsShadows opt-out (RC5's NEW field)
                    // OR a channel mismatch excludes a renderable from THIS
                    // light's caster list entirely -- same AND-test shape
                    // as camera cullMask, just against light channels].
                    if (castsShadows[i] == 0 || (channels[i] & lightChannels) == 0) {
                        cullReasons[i] = CullReason::Layer;
                        continue;
                    }
                    const asset::AABB lightSpaceBox = bounds[i].transformed(lightView);
                    cullReasons[i] = aabbOverlaps(lightSpaceBox, orthoBoxExtruded) ? CullReason::Visible : CullReason::Frustum;
                }
            });
        }

        outVisibleIndices.clear();
        counters = CullCounters{};
        for (uint32_t i = 0; i < n; ++i) {
            switch (cullReasons[i]) {
                case CullReason::Dead:
                    break;
                case CullReason::Layer:
                    ++counters.totalCandidates;
                    ++counters.culledByLayerMask;
                    break;
                case CullReason::Frustum:
                    ++counters.totalCandidates;
                    ++counters.culledByFrustum;
                    break;
                case CullReason::Visible:
                    ++counters.totalCandidates;
                    ++counters.visible;
                    outVisibleIndices.push_back(i);
                    break;
            }
        }
    }

    // Serial (main-thread) expansion of `indices` into per-submesh draw
    // records -- see draw_list.h's own class comment for why this stage is
    // not itself parallelized (variable-length-per-object work, and it
    // touches `scene`/the injected resolvers, which are not asserted safe
    // for concurrent worker access the way the immutable snapshot spans
    // cullView()/cullShadowCasters() use are).
    void generateRecords(const Scene& scene, const Camera& camera, std::span<const uint32_t> indices,
                          std::vector<PendingRecord>& outPending) {
        RX_ZONE_NAMED("DrawListBuilder::generateRecords");
        outPending.clear();
        const std::span<const asset::AABB> bounds = scene.worldBoundsSpan();
        const std::span<const asset::MeshHandle> meshes = scene.meshSpan();
        const std::span<const uint8_t> priorities = scene.prioritySpan();
        const std::span<const uint32_t> generations = scene.generationsSpan();
        const glm::mat4 viewProj = camera.viewProj();

        for (uint32_t idx : indices) {
            const RenderableHandle handle(idx, generations[idx]);
            const std::span<const asset::Submesh> submeshes = meshSubmeshes(meshes[idx]);
            if (submeshes.empty()) {
                continue;  // D24: evicted/empty mesh contributes zero records, never a crash.
            }
            const std::span<const std::optional<asset::MaterialHandle>> overrides = scene.submeshMaterialOverrides(handle);
            const asset::AABB& box = bounds[idx];
            const glm::vec3 center = box.isValid() ? (box.min + box.max) * 0.5F : glm::vec3(0.0F);
            const float ndcDepth = computeNdcDepth(viewProj, center);
            const uint8_t priority = priorities[idx];

            for (size_t s = 0; s < submeshes.size(); ++s) {
                const asset::Submesh& sub = submeshes[s];
                asset::MaterialHandle matHandle = sub.material;
                if (s < overrides.size() && overrides[s].has_value()) {
                    matHandle = *overrides[s];
                }
                const ResolvedMaterial resolved = materialResolve(matHandle);

                PendingRecord rec;
                rec.indexCount = sub.range.indexCount;
                rec.firstIndex = sub.range.firstIndex;
                rec.vertexOffset = sub.range.vertexOffset;
                rec.blockId = sub.range.blockId;
                rec.materialIndex = resolved.materialIndex;
                rec.instanceDataIndex = idx;
                rec.priority = priority;
                rec.isBlend = (resolved.alphaMode == asset::AlphaMode::Blend);
                rec.pipelineKey = sortkey::pipelineKeyOf(resolved.disposition, resolved.alphaMode, resolved.doubleSided);
                rec.meshKey = sortkey::meshKeyOf(rec.firstIndex, rec.vertexOffset);
                rec.ndcDepth = ndcDepth;
                rec.tieBreak = idx;
                outPending.push_back(rec);
            }
        }
    }

    // Splits `pending` into opaque(+MASK)/blend, then applies each
    // partition's own discipline: opaque COLLAPSES first (D26.3), then
    // sorts the resulting groups by the perceptual D14 key; blend is never
    // collapsed and sorts directly by (blockId, encodeBlend(...)) -- see
    // draw_list.h's own "COLLAPSE BEFORE SORT" comment for why these are
    // NOT the same two-step shape.
    void partitionAndSort() {
        RX_ZONE_NAMED("DrawListBuilder::partitionAndSort");
        opaqueRecords.clear();
        blendRecords.clear();
        for (const PendingRecord& rec : pending) {
            (rec.isBlend ? blendRecords : opaqueRecords).push_back(rec);
        }

        collapseAndSortOpaque();

        for (PendingRecord& rec : blendRecords) {
            rec.sortKey = sortkey::encodeBlend(rec.priority, rec.ndcDepth, rec.tieBreak);
        }
        std::sort(blendRecords.begin(), blendRecords.end(), [](const PendingRecord& a, const PendingRecord& b) {
            if (a.blockId != b.blockId) {
                return a.blockId < b.blockId;
            }
            return a.sortKey < b.sortKey;
        });
    }

    // [D26.3] Phase 1: sort `opaqueRecords` by exact draw IDENTITY (blockId,
    // indexCount, firstIndex, vertexOffset, materialIndex, priority -- NEVER
    // by depth) so every collapse-eligible run becomes contiguous
    // regardless of the instances' individual depths (a scattered
    // "identical-mesh forest" scene collapses into ONE instanced draw even
    // though its members are at wildly different depths) -- the final
    // `tieBreak` tier makes the WITHIN-group member order deterministic
    // too, regardless of std::sort's own internals. Phase 2: fold each
    // identity-run into one CollapsedGroup (representative depth = the
    // NEAREST member), then sort the resulting, far-fewer, groups by
    // (blockId, encodeOpaque(priority, pipeline, material, depth,
    // tieBreak)) -- D14's own front-to-back contract, now applied to
    // whole draws instead of individual records.
    //
    // CROSS-GROUP ORDERING [review round 1, finding 2]: per-INSTANCE
    // front-to-back order is NOT preserved across a group boundary --
    // group A's own far member can still be submitted before group B's
    // near member if group A's NEAREST member out-ranks group B's nearest
    // member (representative-depth comparison is group-vs-group, not a
    // flattened per-instance merge). This is intentional, not a
    // correctness gap: D14's front-to-back ordering exists as an early-Z
    // REJECTION-RATE optimization (drawing nearer geometry first lets the
    // depth test reject more hidden fragments in later draws) -- opaque
    // rendering correctness itself is guaranteed by the per-fragment depth
    // TEST, which is fully submission-order-independent by construction
    // (Vulkan's depth test does not care what order draws were recorded
    // in). Sacrificing global per-instance ordering for group-level
    // ordering is the ONLY way to keep D26.3's collapse guarantee
    // unconditional (see the interleaved-depths cross-group test in
    // draw_list_test.cpp, which pins down and asserts exactly this
    // tradeoff rather than leaving it implicit).
    void collapseAndSortOpaque() {
        std::sort(opaqueRecords.begin(), opaqueRecords.end(), [](const PendingRecord& a, const PendingRecord& b) {
            if (a.blockId != b.blockId) {
                return a.blockId < b.blockId;
            }
            if (a.indexCount != b.indexCount) {
                return a.indexCount < b.indexCount;
            }
            if (a.firstIndex != b.firstIndex) {
                return a.firstIndex < b.firstIndex;
            }
            if (a.vertexOffset != b.vertexOffset) {
                return a.vertexOffset < b.vertexOffset;
            }
            if (a.materialIndex != b.materialIndex) {
                return a.materialIndex < b.materialIndex;
            }
            if (a.priority != b.priority) {
                return a.priority < b.priority;
            }
            return a.tieBreak < b.tieBreak;
        });

        opaqueGroups.clear();
        size_t i = 0;
        while (i < opaqueRecords.size()) {
            size_t j = i + 1;
            while (j < opaqueRecords.size() && sameDrawIdentity(opaqueRecords[i], opaqueRecords[j]) &&
                   opaqueRecords[i].priority == opaqueRecords[j].priority) {
                ++j;
            }

            CollapsedGroup group;
            group.indexCount = opaqueRecords[i].indexCount;
            group.firstIndex = opaqueRecords[i].firstIndex;
            group.vertexOffset = opaqueRecords[i].vertexOffset;
            group.blockId = opaqueRecords[i].blockId;
            group.priority = opaqueRecords[i].priority;
            group.pipelineKey = opaqueRecords[i].pipelineKey;
            group.materialIndex = opaqueRecords[i].materialIndex;
            group.firstMember = static_cast<uint32_t>(i);
            group.memberCount = static_cast<uint32_t>(j - i);
            float nearest = opaqueRecords[i].ndcDepth;
            uint32_t minTieBreak = opaqueRecords[i].tieBreak;
            for (size_t k = i; k < j; ++k) {
                nearest = std::max(nearest, opaqueRecords[k].ndcDepth);
                minTieBreak = std::min(minTieBreak, opaqueRecords[k].tieBreak);
            }
            group.representativeDepth = nearest;
            group.tieBreak = minTieBreak;
            opaqueGroups.push_back(group);
            i = j;
        }

        const uint32_t materialKeyMask = static_cast<uint32_t>((uint64_t{1} << sortkey::kOpaqueMaterialKeyBits) - 1);
        for (CollapsedGroup& g : opaqueGroups) {
            const uint16_t materialKey = static_cast<uint16_t>(g.materialIndex & materialKeyMask);
            g.sortKey = sortkey::encodeOpaque(g.priority, g.pipelineKey, materialKey, g.representativeDepth, g.tieBreak);
        }
        std::sort(opaqueGroups.begin(), opaqueGroups.end(), [](const CollapsedGroup& a, const CollapsedGroup& b) {
            if (a.blockId != b.blockId) {
                return a.blockId < b.blockId;
            }
            return a.sortKey < b.sortKey;
        });
    }

    // Emits `out.commands`/`out.payloads`/`out.blocks` from the already
    // sorted opaque groups + blend records: opaque groups already carry
    // their collapsed instanceCount ([D26.3]; one payload entry per member,
    // in the member order collapseAndSortOpaque()'s identity sort fixed --
    // keeping commands/payloads in lockstep by construction, since
    // `firstInstance` is recorded as the payload array's size() at the
    // exact moment each group's own members are about to be appended).
    // Blend is never collapsed [D14].
    void emit(ViewLists& out) {
        BlockRunTracker tracker(out.blocks);

        for (const CollapsedGroup& g : opaqueGroups) {
            DrawCommand cmd;
            cmd.indexCount = g.indexCount;
            cmd.firstIndex = g.firstIndex;
            cmd.vertexOffset = g.vertexOffset;
            cmd.instanceCount = g.memberCount;
            cmd.firstInstance = static_cast<uint32_t>(out.payloads.size());
            out.commands.push_back(cmd);
            tracker.note(g.blockId, static_cast<uint32_t>(out.commands.size() - 1));
            for (uint32_t k = g.firstMember; k < g.firstMember + g.memberCount; ++k) {
                out.payloads.push_back(DrawPayload{opaqueRecords[k].materialIndex, opaqueRecords[k].instanceDataIndex});
            }
        }
        tracker.close(static_cast<uint32_t>(out.commands.size()));
        out.opaqueCommandCount = static_cast<uint32_t>(out.commands.size());

        for (const PendingRecord& rec : blendRecords) {
            DrawCommand cmd;
            cmd.indexCount = rec.indexCount;
            cmd.firstIndex = rec.firstIndex;
            cmd.vertexOffset = rec.vertexOffset;
            cmd.instanceCount = 1;
            cmd.firstInstance = static_cast<uint32_t>(out.payloads.size());
            out.commands.push_back(cmd);
            tracker.note(rec.blockId, static_cast<uint32_t>(out.commands.size() - 1));
            out.payloads.push_back(DrawPayload{rec.materialIndex, rec.instanceDataIndex});
        }
        tracker.close(static_cast<uint32_t>(out.commands.size()));

        out.counters.drawsSubmitted = static_cast<uint32_t>(out.commands.size());
    }

    // Shadow variant: single partition, no collapse [Phase 4 scope -- see
    // draw_list.h's own ShadowLists comment], sorted by (blockId,
    // pipeline|meshRange).
    void emitShadow(ShadowLists& out) {
        BlockRunTracker tracker(out.blocks);
        for (const PendingRecord& rec : pending) {
            DrawCommand cmd;
            cmd.indexCount = rec.indexCount;
            cmd.firstIndex = rec.firstIndex;
            cmd.vertexOffset = rec.vertexOffset;
            cmd.instanceCount = 1;
            cmd.firstInstance = static_cast<uint32_t>(out.payloads.size());
            out.commands.push_back(cmd);
            tracker.note(rec.blockId, static_cast<uint32_t>(out.commands.size() - 1));
            out.payloads.push_back(DrawPayload{rec.materialIndex, rec.instanceDataIndex});
        }
        tracker.close(static_cast<uint32_t>(out.commands.size()));
        out.opaqueCommandCount = static_cast<uint32_t>(out.commands.size());
        out.counters.drawsSubmitted = static_cast<uint32_t>(out.commands.size());
    }
};

DrawListBuilder::DrawListBuilder(MeshSubmeshesFn meshSubmeshes, MaterialResolveFn materialResolve)
    : impl_(std::make_unique<Impl>()) {
    impl_->meshSubmeshes = std::move(meshSubmeshes);
    impl_->materialResolve = std::move(materialResolve);
}

DrawListBuilder::~DrawListBuilder() = default;

void DrawListBuilder::build(const Scene& scene, const Camera& camera, task::Scheduler& scheduler, ViewLists& out) {
    RX_ZONE;
    // [Zero-alloc, caller-owned storage -- D26] .clear() retains capacity;
    // never reassigned/shrink_to_fit'd.
    out.commands.clear();
    out.payloads.clear();
    out.blocks.clear();
    out.opaqueCommandCount = 0;
    out.counters = CullCounters{};

    impl_->cullView(scene, camera, scheduler, impl_->visibleIndices, out.counters);
    impl_->generateRecords(scene, camera, impl_->visibleIndices, impl_->pending);
    out.counters.recordsIn = static_cast<uint32_t>(impl_->pending.size());

    impl_->partitionAndSort();
    impl_->emit(out);
}

void DrawListBuilder::buildShadow(const Scene& scene, LightHandle light, const Camera& camera, task::Scheduler& scheduler,
                                   ShadowLists& out) {
    RX_ZONE;
    out.commands.clear();
    out.payloads.clear();
    out.blocks.clear();
    out.opaqueCommandCount = 0;
    out.counters = CullCounters{};

    // A dead handle or a light with shadows disabled produces an empty,
    // well-formed ShadowLists -- a per-frame QUERY, not a mutator, so this
    // is documented as a defined no-op rather than a throw (draw_list.h's
    // own buildShadow() comment).
    if (!scene.isLightAlive(light) || !scene.lightCastsShadows(light)) {
        return;
    }

    const glm::vec3 lightDir = scene.lightDirection(light);
    const uint8_t lightChannels = scene.lightChannels(light);
    const glm::mat4 lightView = lightSpaceView(lightDir);

    // Step 1: camera-visible bounds -- reuses the exact same cull pass
    // build() uses (camera cullMask + camera frustum only); its own
    // per-object COUNTERS are discarded here (buildShadow() reports its own
    // shadow-shaped counters below, not the camera's).
    CullCounters cameraCounters;
    impl_->cullView(scene, camera, scheduler, impl_->cameraVisibleIndices, cameraCounters);
    const asset::AABB visibleBoundsWorld = unionOfBounds(scene.worldBoundsSpan(), impl_->cameraVisibleIndices);
    if (!visibleBoundsWorld.isValid()) {
        return;  // nothing visible from the camera -> nothing to shadow.
    }

    // Step 2: fit + conservatively extrude the ortho box [D15]. The
    // cross-section (X/Y in light space) is exactly the camera-visible
    // bounds' own light-space footprint. For the extrusion axis: in
    // `lightView` (an eye-at-origin glm::lookAt() basis with `forward =
    // lightDir`), a world point X's view-space Z is `dot(X, -lightDir)`
    // (glm::lookAt's own zaxis = -forward) -- i.e. view-Z DEcreases the
    // further X sits DOWNSTREAM along lightDir (deeper into where the light
    // is travelling TOWARD). A caster C can shadow a point P (P = C +
    // t*lightDir, t>=0) iff C sits upstream of SOME point in the visible
    // box, i.e. iff `C.viewZ >= visBoxLS.min.z` (the box's own most-
    // downstream extent; NO upper bound -- a caster can be arbitrarily far
    // upstream/"behind" the light's own path and still cast into the box).
    // So the extrusion widens `max.z` to +infinity and leaves `min.z` at
    // the visible bounds' own tight value -- verified against a concrete
    // overhead-sun worked example (light dir (0,-1,0): a caster ABOVE the
    // ground, viewZ = world Y directly under that basis, correctly passes
    // `caster.viewZ >= groundMinViewZ`; a caster BELOW the ground correctly
    // fails it) before writing this, not asserted from intuition alone.
    const asset::AABB visBoxLS = visibleBoundsWorld.transformed(lightView);
    asset::AABB orthoBoxExtruded = visBoxLS;
    orthoBoxExtruded.max.z = std::numeric_limits<float>::infinity();

    // Step 3: caster candidates.
    impl_->cullShadowCasters(scene, orthoBoxExtruded, lightChannels, lightView, scheduler, impl_->visibleIndices, out.counters);
    out.counters.shadowCastersConsidered = out.counters.visible + out.counters.culledByFrustum;
    out.counters.shadowCastersVisible = out.counters.visible;

    // Step 4: record generation, BLEND excluded [RC3].
    impl_->generateRecords(scene, camera, impl_->visibleIndices, impl_->pending);
    size_t writeIdx = 0;
    for (size_t r = 0; r < impl_->pending.size(); ++r) {
        if (!impl_->pending[r].isBlend) {
            impl_->pending[writeIdx++] = impl_->pending[r];
        }
    }
    impl_->pending.resize(writeIdx);
    out.counters.recordsIn = static_cast<uint32_t>(impl_->pending.size());

    // Step 5: sort -- single partition, (blockId, pipeline|meshRange).
    {
        RX_ZONE_NAMED("DrawListBuilder::buildShadow sort");
        for (Impl::PendingRecord& rec : impl_->pending) {
            rec.sortKey = sortkey::encodeShadow(rec.pipelineKey, rec.meshKey, rec.tieBreak);
        }
        std::sort(impl_->pending.begin(), impl_->pending.end(),
                  [](const Impl::PendingRecord& a, const Impl::PendingRecord& b) {
                      if (a.blockId != b.blockId) {
                          return a.blockId < b.blockId;
                      }
                      return a.sortKey < b.sortKey;
                  });
    }

    // Step 6: emit, no collapse [Phase 4 scope].
    impl_->emitShadow(out);
}

// ---------------------------------------------------------------------
// recordDrawList [D27].
// ---------------------------------------------------------------------

std::vector<ResolvedDrawGroup> resolveDrawGroups(const ViewLists& lists, uint64_t passSignatureHash,
                                                    uint32_t specializationBits, const PipelineResolveFn& resolvePipeline) {
    RX_ASSERT_MAIN_THREAD("rx::scene::resolveDrawGroups");
    RX_ZONE;
    std::vector<ResolvedDrawGroup> groups;
    const size_t n = lists.commands.size();
    if (n == 0) {
        return groups;
    }

    auto materialOf = [&](size_t commandIndex) -> uint32_t {
        return lists.payloads[lists.commands[commandIndex].firstInstance].materialIndex;
    };
    // `passSignatureHash`/`specializationBits` are constant for this whole
    // call (the caller's own pass, not per-group) -- folded into every key
    // alongside that group's own materialIndex, mirroring
    // rx::material::PipelineRequest's three fields exactly (see this
    // header's own top comment).
    auto keyFor = [&](uint32_t materialIndex) {
        return PipelineRequestKey{materialIndex, passSignatureHash, specializationBits};
    };

    uint32_t groupStart = 0;
    uint32_t currentMaterial = materialOf(0);
    uint64_t currentToken = resolvePipeline(keyFor(currentMaterial));
    for (size_t i = 1; i <= n; ++i) {
        const bool atEnd = (i == n);
        const uint32_t mat = atEnd ? 0 : materialOf(i);
        if (atEnd || mat != currentMaterial) {
            groups.push_back(ResolvedDrawGroup{groupStart, static_cast<uint32_t>(i) - groupStart, currentToken});
            if (!atEnd) {
                groupStart = static_cast<uint32_t>(i);
                currentMaterial = mat;
                currentToken = resolvePipeline(keyFor(currentMaterial));
            }
        }
    }
    return groups;
}

void recordDrawList(const ViewLists& lists, task::Scheduler& scheduler, uint32_t chunkCount, uint64_t passSignatureHash,
                     uint32_t specializationBits, const PipelineResolveFn& resolvePipeline,
                     const RecordChunkFn& recordChunk) {
    RX_ZONE;
    const std::vector<ResolvedDrawGroup> groups = resolveDrawGroups(lists, passSignatureHash, specializationBits, resolvePipeline);
    const uint32_t effectiveChunkCount = (chunkCount == 0) ? 1 : chunkCount;
    scheduler.parallelFor(effectiveChunkCount, /*grainSize=*/1, [&](uint32_t begin, uint32_t end, uint32_t) {
        for (uint32_t c = begin; c < end; ++c) {
            recordChunk(c, effectiveChunkCount, groups);
        }
    });
}

namespace detail {

DrawListBuilderCapacitiesForTesting capacitiesForTesting(const DrawListBuilder& builder) {
    DrawListBuilderCapacitiesForTesting result;
    result.cullReasons = builder.impl_->cullReasons.capacity();
    result.visibleIndices = builder.impl_->visibleIndices.capacity();
    result.cameraVisibleIndices = builder.impl_->cameraVisibleIndices.capacity();
    result.pending = builder.impl_->pending.capacity();
    result.opaqueRecords = builder.impl_->opaqueRecords.capacity();
    result.blendRecords = builder.impl_->blendRecords.capacity();
    result.opaqueGroups = builder.impl_->opaqueGroups.capacity();
    return result;
}

}  // namespace detail

}  // namespace rx::scene
