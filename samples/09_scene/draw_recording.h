#pragma once
// samples/09_scene/draw_recording.h -- the block/pipeline-group command
// split a chunked forward-pass recorder needs that
// `rx::scene::resolveDrawGroups()` does not itself provide [Phase 4 Stage
// 2 Task 24].
//
// WHY THIS EXISTS: `resolveDrawGroups()` (rx_scene/draw_list.h) groups
// `ViewLists::commands` into contiguous runs by DrawPayload::materialIndex
// ADJACENCY ONLY -- it has no notion of `blockId` at all (that field lives
// on `ViewLists::blocks`, a SEPARATE list `resolveDrawGroups()` never
// reads; see draw_list.h's own `ResolvedDrawGroup` comment: "the ONLY
// thing a RecordChunkFn chunk callback ever sees"). Since `blockId` is the
// SORT key's outermost tier (draw_list.h's own "SORT KEY" section) but is
// NOT itself part of the 64-bit encoded key `resolveDrawGroups()`'s
// materialIndex-adjacency scan walks, a resolved `ResolvedDrawGroup` CAN
// legitimately span two different blocks whenever the exact same
// `materialIndex` value happens to repeat immediately across a block
// boundary (two renderables, different GeometryPool blocks, coincidentally
// sharing one material). `GeometryPool::bind()`'s own contract requires
// every draw between two `bind()` calls to share one block -- a recorder
// that trusts `ResolvedDrawGroup` alone for its `vkCmdBindPipeline`/bind-
// block loop can silently issue draws against the WRONG bound block for
// part of a group. This header closes that gap on the CALLER side (no
// change to rx_scene's own delivered seam needed): `splitByBlockAndGroup()`
// further subdivides a `[rangeStart, rangeStart+rangeCount)` slice of
// `ViewLists::commands` (a chunk's own assigned command range) at EVERY
// group boundary AND every block boundary, whichever comes first, so each
// emitted `RecordSpan` is guaranteed to lie within exactly one resolved
// pipeline group AND exactly one GeometryPool block.
//
// Device-free (plain data in, plain data out) -- unit-tested directly in
// samples/09_scene/tests/test_draw_recording.cpp with a synthetic
// group-spans-two-blocks fixture (the discriminating case this header's
// own top comment names), matching this project's own "prove the failure
// mode, not just the happy path" convention.
#include <rx_scene/draw_list.h>

#include <cstdint>
#include <span>
#include <vector>

namespace rx::samples9 {

// One contiguous sub-range of `ViewLists::commands` that is safe to record
// as a single "bind block (if changed) -> bind pipeline (if changed) ->
// issue every command in [commandOffset, commandOffset+commandCount)"
// unit -- see this header's own top comment for why both boundaries are
// necessary.
struct RecordSpan {
    uint32_t blockId = 0;
    uint64_t pipelineToken = 0;
    uint32_t commandOffset = 0;
    uint32_t commandCount = 0;

    bool operator==(const RecordSpan&) const = default;
};

// `groups`/`blocks` are the FULL, pass-wide lists (`resolveDrawGroups()`'s
// own return value and `ViewLists::blocks` respectively) -- both are
// sorted and contiguous, together covering `[0, totalCommandCount)`
// exactly once each (draw_list.h's own documented invariant). `rangeStart`/
// `rangeCount` is the caller's own chunk-local slice of that same command
// index space (e.g. a chunked forward pass's own ceil-division partition,
// samples/07_stress/main.cpp's own `recordForwardChunked()` convention,
// applied here to command indices instead of a flat instance array).
// Returns an empty vector for `rangeCount == 0`. `groups`/`blocks` must
// each be non-empty and must together cover the full `[rangeStart,
// rangeStart+rangeCount)` range (true by construction whenever `groups`
// came from `resolveDrawGroups(lists, ...)` and `blocks` is that SAME
// `lists.blocks` -- this function does not itself validate that
// precondition, matching this project's own "device-free helper trusts
// its caller's own already-established invariant" convention for a
// tightly-scoped internal seam like this one).
[[nodiscard]] std::vector<RecordSpan> splitByBlockAndGroup(std::span<const rx::scene::ResolvedDrawGroup> groups,
                                                             std::span<const rx::scene::BlockRange> blocks,
                                                             uint32_t rangeStart, uint32_t rangeCount);

// The REAL `DrawPayload::materialIndex` a `RecordSpan` was built from --
// recovers the identity `ResolvedDrawGroup`/`RecordSpan` themselves discard
// (both types carry only a `pipelineToken`, deliberately: `resolveDrawGroups()`
// groups by materialIndex-ADJACENCY, but its returned token is a real
// `VkPipeline`, and DISTINCT materialIndex values legitimately collapse onto
// the SAME pipeline whenever they share identical fixed-function state
// [D28] -- e.g. Sponza's own glTF: 22 of its 25 materials are all
// (StandardPBR, Opaque, single-sided), so they all resolve to ONE shared
// VkPipeline. A caller that keys its MATERIAL PARAMS descriptor-set choice
// off `pipelineToken` alone [the historical bug this function exists to let
// a caller avoid] binds whichever ONE of those 22 materials happened to be
// registered last for every one of the other 21's geometry -- reproducible
// on any driver, not lighting- or platform-dependent. Every command within
// one `RecordSpan` shares one materialIndex by construction (the SAME
// invariant `splitByBlockAndGroup()`'s own header comment documents for
// block identity: subdividing a group only ever shrinks a run, never
// merges two different materialIndex runs), so reading it off the span's
// OWN first command is exact, not a heuristic -- `DrawCommand::firstInstance`
// addresses `payloads[]` directly [D26.1], and `payloads[i].materialIndex`
// is the ground truth every `DrawDataGpu::materialIndex` row is itself
// populated from (main.cpp's own `updateSceneFrame()`).
[[nodiscard]] uint32_t materialIndexForSpan(std::span<const rx::scene::DrawCommand> commands,
                                             std::span<const rx::scene::DrawPayload> payloads, const RecordSpan& span);

}  // namespace rx::samples9
