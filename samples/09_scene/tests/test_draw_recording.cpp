// samples/09_scene/tests/test_draw_recording.cpp -- proves
// splitByBlockAndGroup() (draw_recording.h) correctly subdivides a
// resolved pipeline group that spans two GeometryPool blocks -- the exact
// gap resolveDrawGroups()'s own materialIndex-adjacency scan leaves open
// (draw_recording.h's own top comment has the full derivation): that
// function groups purely by adjacent DrawPayload::materialIndex, with no
// blockId awareness at all, so two renderables in DIFFERENT blocks that
// happen to share one materialIndex collapse into ONE ResolvedDrawGroup a
// naive recorder would record as if it were entirely inside a single
// GeometryPool::bind() call.
#include "../draw_recording.h"

#include <doctest/doctest.h>

using rx::samples9::RecordSpan;
using rx::samples9::splitByBlockAndGroup;
using rx::scene::BlockRange;
using rx::scene::ResolvedDrawGroup;

TEST_CASE("splitByBlockAndGroup: a single resolved group spanning two blocks splits into two spans, one per "
          "block, both carrying the group's own pipeline token [discriminates the resolveDrawGroups() "
          "blockId-blind-spot this header exists to close]") {
    // Two blocks, two commands each: block 0 = commands [0,2), block 1 =
    // commands [2,4).
    const std::vector<BlockRange> blocks{
        BlockRange{/*blockId=*/0, /*firstCommand=*/0, /*commandCount=*/2},
        BlockRange{/*blockId=*/1, /*firstCommand=*/2, /*commandCount=*/2},
    };
    // ONE resolved group spanning ALL FOUR commands (the adversarial case:
    // the same materialIndex happened to repeat across the block
    // boundary, so resolveDrawGroups() produced a single run).
    const std::vector<ResolvedDrawGroup> groups{
        ResolvedDrawGroup{/*firstCommand=*/0, /*commandCount=*/4, /*pipelineToken=*/100},
    };

    const auto spans = splitByBlockAndGroup(groups, blocks, /*rangeStart=*/0, /*rangeCount=*/4);

    REQUIRE(spans.size() == 2);
    CHECK(spans[0] == RecordSpan{/*blockId=*/0, /*pipelineToken=*/100, /*commandOffset=*/0, /*commandCount=*/2});
    CHECK(spans[1] == RecordSpan{/*blockId=*/1, /*pipelineToken=*/100, /*commandOffset=*/2, /*commandCount=*/2});
}

TEST_CASE("splitByBlockAndGroup: a group boundary WITHOUT a block change still splits (block-uniform, "
          "pipeline-varying case, the ordinary/common path)") {
    const std::vector<BlockRange> blocks{
        BlockRange{/*blockId=*/7, /*firstCommand=*/0, /*commandCount=*/4},
    };
    const std::vector<ResolvedDrawGroup> groups{
        ResolvedDrawGroup{/*firstCommand=*/0, /*commandCount=*/3, /*pipelineToken=*/10},
        ResolvedDrawGroup{/*firstCommand=*/3, /*commandCount=*/1, /*pipelineToken=*/20},
    };

    const auto spans = splitByBlockAndGroup(groups, blocks, 0, 4);

    REQUIRE(spans.size() == 2);
    CHECK(spans[0] == RecordSpan{7, 10, 0, 3});
    CHECK(spans[1] == RecordSpan{7, 20, 3, 1});
}

TEST_CASE("splitByBlockAndGroup: a chunk's own partial [rangeStart, rangeStart+rangeCount) slice -- neither "
          "starting nor ending on a group/block boundary -- still produces correctly-clamped spans (the real "
          "chunked-recording call shape: a chunk owns an arbitrary sub-range of the FULL command list, not "
          "necessarily aligned to any group/block boundary)") {
    const std::vector<BlockRange> blocks{
        BlockRange{0, 0, 3},
        BlockRange{1, 3, 3},
    };
    const std::vector<ResolvedDrawGroup> groups{
        ResolvedDrawGroup{0, 2, 100},
        ResolvedDrawGroup{2, 4, 200},
    };
    // This chunk owns commands [1, 5) -- starts mid-group-0/mid-block-0,
    // ends mid-group-1/mid-block-1.
    const auto spans = splitByBlockAndGroup(groups, blocks, /*rangeStart=*/1, /*rangeCount=*/4);

    REQUIRE(spans.size() == 3);
    CHECK(spans[0] == RecordSpan{0, 100, 1, 1});  // rest of group 0, still block 0 -- [1,2)
    CHECK(spans[1] == RecordSpan{0, 200, 2, 1});  // group 1 begins, still block 0 -- [2,3)
    CHECK(spans[2] == RecordSpan{1, 200, 3, 2});  // group 1 continues, now block 1 -- [3,5)
}

TEST_CASE("splitByBlockAndGroup: a single command (one block, one group) is one span") {
    const std::vector<BlockRange> blocks{BlockRange{3, 0, 1}};
    const std::vector<ResolvedDrawGroup> groups{ResolvedDrawGroup{0, 1, 999}};

    const auto spans = splitByBlockAndGroup(groups, blocks, 0, 1);

    REQUIRE(spans.size() == 1);
    CHECK(spans[0] == RecordSpan{3, 999, 0, 1});
}

TEST_CASE("splitByBlockAndGroup: rangeCount == 0 returns an empty span list") {
    const std::vector<BlockRange> blocks{BlockRange{0, 0, 1}};
    const std::vector<ResolvedDrawGroup> groups{ResolvedDrawGroup{0, 1, 1}};
    CHECK(splitByBlockAndGroup(groups, blocks, 0, 0).empty());
}
