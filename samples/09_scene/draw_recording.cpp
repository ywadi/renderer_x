#include "draw_recording.h"

#include <algorithm>

namespace rx::samples9 {

std::vector<RecordSpan> splitByBlockAndGroup(std::span<const rx::scene::ResolvedDrawGroup> groups,
                                              std::span<const rx::scene::BlockRange> blocks, uint32_t rangeStart,
                                              uint32_t rangeCount) {
    std::vector<RecordSpan> out;
    if (rangeCount == 0 || groups.empty() || blocks.empty()) {
        return out;
    }
    const uint32_t rangeEnd = rangeStart + rangeCount;

    // Locate the first group/block whose own [firstCommand, firstCommand+
    // commandCount) interval contains `rangeStart` -- a linear scan is
    // correct and cheap at this scale (both lists are, in practice, tiny
    // relative to `commands` itself: one entry per distinct materialIndex
    // run / per distinct-block run, never per command).
    size_t groupIdx = 0;
    while (groupIdx + 1 < groups.size() && groups[groupIdx].firstCommand + groups[groupIdx].commandCount <= rangeStart) {
        ++groupIdx;
    }
    size_t blockIdx = 0;
    while (blockIdx + 1 < blocks.size() && blocks[blockIdx].firstCommand + blocks[blockIdx].commandCount <= rangeStart) {
        ++blockIdx;
    }

    uint32_t cursor = rangeStart;
    while (cursor < rangeEnd) {
        const rx::scene::ResolvedDrawGroup& g = groups[groupIdx];
        const rx::scene::BlockRange& b = blocks[blockIdx];
        const uint32_t groupEnd = g.firstCommand + g.commandCount;
        const uint32_t blockEnd = b.firstCommand + b.commandCount;
        const uint32_t spanEnd = std::min({groupEnd, blockEnd, rangeEnd});

        out.push_back(RecordSpan{b.blockId, g.pipelineToken, cursor, spanEnd - cursor});

        cursor = spanEnd;
        if (cursor >= groupEnd && groupIdx + 1 < groups.size()) {
            ++groupIdx;
        }
        if (cursor >= blockEnd && blockIdx + 1 < blocks.size()) {
            ++blockIdx;
        }
    }
    return out;
}

uint32_t materialIndexForSpan(std::span<const rx::scene::DrawCommand> commands,
                               std::span<const rx::scene::DrawPayload> payloads, const RecordSpan& span) {
    // Both branches below are defensive only -- see this function's own
    // header comment for why `span.commandCount > 0` and
    // `firstInstance < payloads.size()` hold by construction for every
    // `RecordSpan` this file's own `splitByBlockAndGroup()` produces from a
    // real `ViewLists`. Falling back to materialIndex 0 rather than
    // indexing out of bounds keeps this a safe, if degraded, no-op instead
    // of UB if a future caller ever violates that contract.
    if (span.commandCount == 0 || span.commandOffset >= commands.size()) {
        return 0;
    }
    const rx::scene::DrawCommand& firstCommand = commands[span.commandOffset];
    if (firstCommand.firstInstance >= payloads.size()) {
        return 0;
    }
    return payloads[firstCommand.firstInstance].materialIndex;
}

}  // namespace rx::samples9
