// Task 2 brief's exact required doctest cases, asserting every one of the
// six mask/layout fields on the barrier structs it names; plus, from fix
// round 1 (Critical finding: the per-resource state machine under-
// synchronized a second reader in an uncovered pipeline stage, and that
// compounded into an under-synchronized WAR write), two more cases
// targeting exactly that pattern -- one at the RenderGraph API level
// ("multi-stage-read-gets-own-barrier") and one unit-level, direct
// reproduction of the reviewer's own compounding scenario
// ("war-unions-all-reader-stages").
//
// "war-execution-only" and "war-unions-all-reader-stages" cannot be built
// by declaring passes through RenderGraph's public API: Task 1's compile()
// binds every reader of a declared resource name to that name's *final*
// declared writer (see render_graph.cpp's step 2 comment), so a read of
// "T" can never itself be ordered ahead of a later write of "T" -- any
// attempt to declare a second writer of the same name forces every reader
// to depend on it instead, which reorders the read to *after* both writes
// (write, write, read), never write, read, write. Confirmed empirically
// (see this task's report) before writing this file, rather than assumed.
// detail::applyAccess() (barriers.h) is exposed specifically so rules like
// WAR/WAW -- real, necessary rules for a production barrier deriver even
// though today's RenderGraph pass topology can't exercise every one of
// them end to end -- can still be verified directly, against a synthetic
// access sequence. It lives in rx::graph::detail (not the top-level
// rx::graph API the brief's ImageBarrier/BufferBarrier/PassBarriers/
// buildBarriers() interface specifies) precisely to mark it as that kind
// of seam, not a second, equally-stable public entry point [Task 2 fix
// round 1, Important finding].
#include <doctest/doctest.h>
#include <rx_graph/render_graph.h>

#include <algorithm>

using namespace rx::graph;
using rx::graph::detail::applyAccess;
using rx::graph::detail::ResourceBarrierState;

namespace {

constexpr CompileInfo kInfo{1920, 1080, VK_FORMAT_B8G8R8A8_UNORM};

constexpr VkPipelineStageFlags2 kDepthTestStages =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
constexpr VkAccessFlags2 kDepthReadWrite =
    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

AttachmentDesc colorDesc() {
    AttachmentDesc desc;
    desc.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    return desc;
}

AttachmentDesc depthDesc() {
    AttachmentDesc desc;
    desc.format = VK_FORMAT_D32_SFLOAT;
    return desc;
}

// [Task 2, gate ruling RC2] A plain storage-image shape -- see
// test_compile.cpp's identically-named/-shaped helper.
ImageDesc storageImageDesc() {
    ImageDesc desc;
    desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    desc.sizeClass = SizeClass::Absolute;
    desc.width = 64.0F;
    desc.height = 64.0F;
    return desc;
}

uint32_t physicalIndexOf(const CompiledGraph& compiled, std::string_view name) {
    const auto resources = compiled.resources();
    for (uint32_t i = 0; i < resources.size(); ++i) {
        if (resources[i].name == name) {
            return i;
        }
    }
    FAIL("no physical resource named '" << name << "'");
    return UINT32_MAX;
}

void checkImageBarrier(const ImageBarrier& b, uint32_t physicalIndex, VkPipelineStageFlags2 srcStage,
                        VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                        VkImageLayout oldLayout, VkImageLayout newLayout) {
    CHECK(b.physicalIndex == physicalIndex);
    CHECK(b.srcStage == srcStage);
    CHECK(b.srcAccess == srcAccess);
    CHECK(b.dstStage == dstStage);
    CHECK(b.dstAccess == dstAccess);
    CHECK(b.oldLayout == oldLayout);
    CHECK(b.newLayout == newLayout);
}

void checkBufferBarrier(const BufferBarrier& b, uint32_t physicalIndex, VkPipelineStageFlags2 srcStage,
                         VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    CHECK(b.physicalIndex == physicalIndex);
    CHECK(b.srcStage == srcStage);
    CHECK(b.srcAccess == srcAccess);
    CHECK(b.dstStage == dstStage);
    CHECK(b.dstAccess == dstAccess);
}

// Several cases below need the reading pass to itself be Graphics-class
// (Task 1's hasAttachmentOutput() classification, keyed on that *same*
// pass's own declared outputs -- see pass.h) so the read resolves to
// FRAGMENT_SHADER rather than COMPUTE_SHADER; the simplest way to do that
// without a throwaway attachment is to have the reading pass double as the
// present pass (write "bb" directly). That pass's own first-use write to
// "bb" then legitimately owns a second image barrier in the same list, so
// these tests locate the barrier under test by physicalIndex instead of
// asserting the list's total size -- but assert there is EXACTLY ONE match
// (via countMatchingImageBarriers(), not just "at least one" via
// findImageBarrier() alone) before inspecting its fields [Task 2 fix round
// 1, Minor finding: a regression that both duplicated the barrier under
// test AND dropped the pass's own unrelated one would previously still
// have passed a bare `size == 2` + first-match check].
size_t countMatchingImageBarriers(const std::vector<ImageBarrier>& barriers, uint32_t physicalIndex) {
    return static_cast<size_t>(
        std::count_if(barriers.begin(), barriers.end(), [&](const ImageBarrier& b) { return b.physicalIndex == physicalIndex; }));
}

const ImageBarrier* findImageBarrier(const std::vector<ImageBarrier>& barriers, uint32_t physicalIndex) {
    for (const ImageBarrier& b : barriers) {
        if (b.physicalIndex == physicalIndex) {
            return &b;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("shadow-then-sample") {
    RenderGraph graph;
    graph.addPass("shadow").setDepthStencilOutput("sm", depthDesc());  // pass 0
    // "forward" doubles as the present pass so it has an attachment output
    // of its own -- Graphics-class, so its "sm" read resolves to
    // FRAGMENT_SHADER (see the findImageBarrier() comment above).
    graph.addPass("forward").addTextureInput("sm").addColorOutput("bb", colorDesc());  // pass 1
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();
    const auto order = compiled.executionOrder();
    REQUIRE(order.size() == 2);
    CHECK(order[0] == 0);
    CHECK(order[1] == 1);

    const uint32_t sm = physicalIndexOf(compiled, "sm");
    const auto barriers = compiled.passBarriers();
    REQUIRE(barriers.size() == 2);

    // Before "shadow" itself: UNDEFINED -> DEPTH_ATTACHMENT_OPTIMAL, first
    // use, so src is NONE/0.
    REQUIRE(barriers[0].imageBarriers.size() == 1);
    checkImageBarrier(barriers[0].imageBarriers[0], sm, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, kDepthTestStages,
                       kDepthReadWrite, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    CHECK(barriers[0].bufferBarriers.empty());

    // Before "forward": depth write -> fragment sample. "forward" also
    // owns a second, unrelated first-use barrier for its own "bb" output.
    REQUIRE(barriers[1].imageBarriers.size() == 2);
    REQUIRE(countMatchingImageBarriers(barriers[1].imageBarriers, sm) == 1);
    const ImageBarrier* smBarrier = findImageBarrier(barriers[1].imageBarriers, sm);
    REQUIRE(smBarrier != nullptr);
    checkImageBarrier(*smBarrier, sm, kDepthTestStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(barriers[1].bufferBarriers.empty());
}

TEST_CASE("no-redundant-read") {
    RenderGraph graph;
    graph.addPass("producer").addColorOutput("tex", colorDesc());   // pass 0
    graph.addPass("readerA").addTextureInput("tex").setSideEffect();  // pass 1
    graph.addPass("readerB").addTextureInput("tex").setSideEffect();  // pass 2
    graph.addPass("present").addColorOutput("bb", colorDesc());     // pass 3
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();
    const auto order = compiled.executionOrder();
    REQUIRE(order.size() == 4);
    CHECK(order[0] == 0);
    CHECK(order[1] == 1);
    CHECK(order[2] == 2);  // readerB immediately follows readerA

    const auto barriers = compiled.passBarriers();
    REQUIRE(barriers.size() == 4);

    // readerA (position 1) needs a barrier -- write(producer) -> read.
    CHECK(barriers[1].imageBarriers.size() == 1);

    // readerB (position 2) reads the same texture again, same stage/access
    // as readerA -- zero barriers for it.
    CHECK(barriers[2].imageBarriers.empty());
    CHECK(barriers[2].bufferBarriers.empty());
}

TEST_CASE("hdr-tonemap") {
    RenderGraph graph;
    graph.addPass("hdr_pass").addColorOutput("hdr", colorDesc());  // pass 0
    // "tonemap" doubles as the present pass, same reasoning as
    // "shadow-then-sample" above.
    graph.addPass("tonemap").addTextureInput("hdr").addColorOutput("bb", colorDesc());  // pass 1
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();
    const uint32_t hdr = physicalIndexOf(compiled, "hdr");
    const auto barriers = compiled.passBarriers();
    REQUIRE(barriers.size() == 2);

    REQUIRE(barriers[1].imageBarriers.size() == 2);
    REQUIRE(countMatchingImageBarriers(barriers[1].imageBarriers, hdr) == 1);
    const ImageBarrier* hdrBarrier = findImageBarrier(barriers[1].imageBarriers, hdr);
    REQUIRE(hdrBarrier != nullptr);
    checkImageBarrier(*hdrBarrier, hdr, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Task 2 fix round 1, Critical finding's required test (a): a resource
// read by two passes in two *different* pipeline stages, with no
// intervening write. The first port's bug fully cleared its aggregate
// invalidated-visibility state the moment the first reader's barrier
// resolved the pending flush, so the second reader (a different,
// never-covered stage) got zero barriers -- a real GPU synchronization
// hazard, not a missed optimization. The fix tracks per-stage-bit
// visibility (detail::ResourceBarrierState::invalidatedInStage), so B's
// own stage being uncovered is detected correctly and B still chains a
// correct barrier off the persisted last-write source.
TEST_CASE("multi-stage-read-gets-own-barrier") {
    RenderGraph graph;
    graph.addPass("W").addColorOutput("T", colorDesc());                              // pass 0
    // "A" doubles as the present pass (Graphics-class -> FRAGMENT_SHADER read).
    graph.addPass("A").addTextureInput("T").addColorOutput("bb", colorDesc());          // pass 1
    // "B" has no attachment output at all -> Compute-class -> COMPUTE_SHADER read,
    // a stage A's own barrier never touches.
    graph.addPass("B").addTextureInput("T").setSideEffect();                            // pass 2
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();
    const auto order = compiled.executionOrder();
    REQUIRE(order.size() == 3);
    CHECK(order[0] == 0);
    CHECK(order[1] == 1);
    CHECK(order[2] == 2);

    const uint32_t t = physicalIndexOf(compiled, "T");
    const auto barriers = compiled.passBarriers();
    REQUIRE(barriers.size() == 3);

    // Before "A": W's write -> A's fragment-shader sample (the flush barrier).
    REQUIRE(barriers[1].imageBarriers.size() == 2);  // T's flush barrier + bb's first-use one
    REQUIRE(countMatchingImageBarriers(barriers[1].imageBarriers, t) == 1);
    const ImageBarrier* aBarrier = findImageBarrier(barriers[1].imageBarriers, t);
    REQUIRE(aBarrier != nullptr);
    checkImageBarrier(*aBarrier, t, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Before "B": its own invalidate barrier -- no layout change (A already
    // transitioned "T" to SHADER_READ_ONLY_OPTIMAL), srcAccess=0 (already
    // made available to A), but a real barrier all the same, chained off
    // the same persisted write source A's barrier used, so COMPUTE_SHADER
    // actually gets a visibility guarantee instead of none.
    REQUIRE(barriers[2].imageBarriers.size() == 1);
    checkImageBarrier(barriers[2].imageBarriers[0], t, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Task 2 fix round 1, Critical finding's required test (b): the
// reviewer's own compounding scenario, reproduced directly against
// detail::applyAccess() -- write, read(S1), read(S2, different stage),
// then a write (WAR). The WAR write's srcStage must be the union of every
// reader's stage since the last write (S1 | S2), not just the most recent
// reader, or the write could start before an in-flight access in the
// *other* stage has finished.
TEST_CASE("war-unions-all-reader-stages") {
    ResourceBarrierState state;

    auto write1 = applyAccess(state, /*isBuffer=*/false, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    REQUIRE(write1.has_value());
    CHECK(write1->srcStage == VK_PIPELINE_STAGE_2_NONE);
    CHECK(write1->srcAccess == VK_ACCESS_2_NONE);
    CHECK(write1->dstStage == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK(write1->dstAccess == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK(write1->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK(write1->newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    auto read1 = applyAccess(state, false, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    REQUIRE(read1.has_value());
    CHECK(read1->srcStage == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK(read1->srcAccess == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK(read1->dstStage == VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
    CHECK(read1->dstAccess == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    CHECK(read1->oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(read1->newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // read2, a different stage than read1, with no intervening write --
    // this used to be nullopt (the Critical bug); now it must still get a
    // barrier, chained off the same persisted write source as read1's.
    auto read2 = applyAccess(state, false, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    REQUIRE(read2.has_value());
    CHECK(read2->srcStage == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK(read2->srcAccess == VK_ACCESS_2_NONE);  // already made available by read1's barrier
    CHECK(read2->dstStage == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    CHECK(read2->dstAccess == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    CHECK(read2->oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(read2->newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);  // no layout change

    // The WAR write: srcStage must be VERTEX_SHADER | COMPUTE_SHADER (the
    // union of both readers), not just one of them and not the original
    // write's stage.
    auto warWrite = applyAccess(state, false, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    REQUIRE(warWrite.has_value());
    CHECK(warWrite->srcStage == (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT));
    CHECK(warWrite->srcAccess == VK_ACCESS_2_NONE);
    CHECK(warWrite->dstStage == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK(warWrite->dstAccess == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK(warWrite->oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(warWrite->newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

TEST_CASE("war-execution-only") {
    // Direct unit test of applyAccess()'s WAR rule -- see this file's own
    // header comment for why this cannot be built through RenderGraph's
    // public API.
    ResourceBarrierState state;

    // The resource's original write (some earlier pass establishing it),
    // then a read (sampled) -- fast-forwarding to "just read, nothing left
    // to flush" without needing a whole pass graph.
    applyAccess(state, /*isBuffer=*/false, kDepthTestStages, kDepthReadWrite, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    auto readTransition = applyAccess(state, /*isBuffer=*/false, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    REQUIRE(readTransition.has_value());  // write -> read barrier, not under test here

    // Now a later pass depth-writes the same resource: WAR.
    auto writeTransition =
        applyAccess(state, /*isBuffer=*/false, kDepthTestStages, kDepthReadWrite, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    REQUIRE(writeTransition.has_value());
    CHECK(writeTransition->srcStage == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    CHECK(writeTransition->srcAccess == VK_ACCESS_2_NONE);  // execution-only -- nothing to flush from a read
    CHECK(writeTransition->dstStage == kDepthTestStages);
    CHECK(writeTransition->dstAccess == kDepthReadWrite);
    CHECK(writeTransition->oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(writeTransition->newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
}

TEST_CASE("compute-to-draw-buffer") {
    RenderGraph graph;
    BufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    graph.addPass("compute").addStorageBufferOutput("particles", bufferDesc).setSideEffect();  // pass 0
    graph.addPass("draw").addStorageBufferInput("particles").addColorOutput("bb", colorDesc());  // pass 1
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();
    const auto order = compiled.executionOrder();
    REQUIRE(order.size() == 2);
    CHECK(order[0] == 0);
    CHECK(order[1] == 1);

    const uint32_t particles = physicalIndexOf(compiled, "particles");
    const auto barriers = compiled.passBarriers();
    REQUIRE(barriers.size() == 2);

    // compute's first write to a fresh buffer needs no barrier at all.
    CHECK(barriers[0].bufferBarriers.empty());

    REQUIRE(barriers[1].bufferBarriers.size() == 1);
    checkBufferBarrier(barriers[1].bufferBarriers[0], particles, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

TEST_CASE("present-final") {
    // Task 3 ambiguity resolution #1: CompileInfo::backbufferFinalLayout
    // defaults to VK_IMAGE_LAYOUT_PRESENT_SRC_KHR (Task 2's original,
    // unchanged behavior) but is now caller-selectable -- an offscreen
    // "backbuffer" (no presentation engine involved at all, e.g. Task 3's
    // GPU test) needs some other final layout instead. Both subcases
    // share everything except that one CompileInfo field and its expected
    // effect on finalBarriers().
    SUBCASE("default backbufferFinalLayout is PRESENT_SRC_KHR (Task 2 behavior, unchanged)") {
        RenderGraph graph;
        graph.addPass("present").addColorOutput("bb", colorDesc());
        graph.setBackbufferSource("bb");
        graph.compile(kInfo);

        const CompiledGraph& compiled = graph.compiled();
        const uint32_t bb = physicalIndexOf(compiled, "bb");

        const PassBarriers& final_ = compiled.finalBarriers();
        REQUIRE(final_.imageBarriers.size() == 1);
        CHECK(final_.bufferBarriers.empty());
        // [Phase 5 Task 5 review round, CI runs 32463376885/32466037296]
        // dstStage was VK_PIPELINE_STAGE_2_NONE until this round --
        // barriers.cpp's own finalBarriers()-building comment has the full
        // account (SYNC-HAZARD-PRESENT-AFTER-WRITE: a NONE dst gives
        // PresentLoop's signal semaphore no real stage to chain from).
        checkImageBarrier(final_.imageBarriers[0], bb, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }

    SUBCASE("explicit backbufferFinalLayout overrides PRESENT_SRC_KHR (Task 3: offscreen backbuffer)") {
        RenderGraph graph;
        graph.addPass("present").addColorOutput("bb", colorDesc());
        graph.setBackbufferSource("bb");

        CompileInfo info = kInfo;
        info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        graph.compile(info);

        const CompiledGraph& compiled = graph.compiled();
        const uint32_t bb = physicalIndexOf(compiled, "bb");

        const PassBarriers& final_ = compiled.finalBarriers();
        REQUIRE(final_.imageBarriers.size() == 1);
        CHECK(final_.bufferBarriers.empty());
        // Every field but newLayout is unaffected by this override -- same
        // srcStage/srcAccess/oldLayout as the default subcase above
        // (including the same VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT dstStage
        // fix -- see that subcase's own comment).
        checkImageBarrier(final_.imageBarriers[0], bb, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
}

TEST_CASE("culled-contributes-nothing") {
    RenderGraph graph;
    graph.addPass("producer").addColorOutput("shared", colorDesc());  // pass 0
    // Reads "shared" but produces nothing anything else depends on, and
    // isn't a side effect -- culled. Its texture-input access has no
    // attachment output alongside it, so (if it ran) it would resolve to
    // COMPUTE_SHADER/SHADER_SAMPLED_READ, a *different* stage than
    // "consumer" below. If buildBarriers() ever walked culled passes too,
    // this pass's read would wrongly resolve "shared"'s pending flush
    // before "consumer" sees it, leaving "consumer" with zero barriers
    // instead of the write->read one it actually needs.
    graph.addPass("deadend").addTextureInput("shared");                    // pass 1: culled
    graph.addPass("consumer").addTextureInput("shared").addColorOutput("bb", colorDesc());  // pass 2
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();
    CHECK(compiled.isCulled(1));

    const auto order = compiled.executionOrder();
    REQUIRE(order.size() == 2);
    CHECK(order[0] == 0);
    CHECK(order[1] == 2);

    const uint32_t shared = physicalIndexOf(compiled, "shared");
    const auto barriers = compiled.passBarriers();
    REQUIRE(barriers.size() == 2);

    // "consumer" also owns a second, unrelated first-use barrier for its
    // own "bb" output.
    REQUIRE(barriers[1].imageBarriers.size() == 2);
    REQUIRE(countMatchingImageBarriers(barriers[1].imageBarriers, shared) == 1);
    const ImageBarrier* sharedBarrier = findImageBarrier(barriers[1].imageBarriers, shared);
    REQUIRE(sharedBarrier != nullptr);
    checkImageBarrier(*sharedBarrier, shared, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// ===========================================================================
// [Task 2, gate ruling RC2] Per-(resource, subresource) barrier-state
// independence -- the load-bearing correctness property the storage-image
// resource-model extension's "per-mip/per-layer subresource pass I/O"
// requires of buildBarriers() itself (as opposed to test_compile.cpp's own
// coverage of the DECLARATION-side plumbing that produces the resolved
// Subresource each ResourceAccess/ImageBarrier now carries).
// ===========================================================================

TEST_CASE("two disjoint mip-level subresources of the same storage image get independently tracked barrier state") {
    RenderGraph graph;
    ImageDesc desc = storageImageDesc();
    desc.mipLevels = 2;

    graph.addPass("write_mip0").addStorageImageOutput("chain", desc, Subresource{0, 1, 0, 1}).setSideEffect();  // 0
    graph.addPass("write_mip1").addStorageImageOutput("chain", desc, Subresource{1, 1, 0, 1}).setSideEffect();  // 1
    graph.addPass("present").addColorOutput("bb", colorDesc());                                                  // 2
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const auto barriers = graph.compiled().passBarriers();
    REQUIRE(barriers.size() == 3);
    const uint32_t chain = physicalIndexOf(graph.compiled(), "chain");

    // Each write is its OWN subresource's first-ever access in this
    // compile walk -- an IMAGE always needs a real UNDEFINED->GENERAL
    // layout-transition barrier on first use (unlike a buffer, which has
    // no layout at all to transition -- see barriers.cpp's own
    // applyAccess() comment on that asymmetry), srcStage NONE since
    // nothing precedes either write. Under the PRE-Task-2
    // whole-resource-only state machine, "write_mip1" would incorrectly
    // inherit "write_mip0"'s already-GENERAL currentLayout and
    // everAccessed=true, producing a WAW-shaped barrier (srcStage
    // COMPUTE_SHADER, chained off the OTHER mip's write) instead of its
    // own independent fresh first-use one.
    REQUIRE(barriers[0].imageBarriers.size() == 1);
    REQUIRE(barriers[1].imageBarriers.size() == 1);
    checkImageBarrier(barriers[0].imageBarriers[0], chain, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    checkImageBarrier(barriers[1].imageBarriers[0], chain, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    CHECK(barriers[0].imageBarriers[0].subresource.baseMipLevel == 0);
    CHECK(barriers[1].imageBarriers[0].subresource.baseMipLevel == 1);
}

TEST_CASE("a write-after-write on one subresource does not disturb an unrelated subresource's independent state") {
    RenderGraph graph;
    ImageDesc desc = storageImageDesc();
    desc.mipLevels = 2;

    const Subresource mip0{0, 1, 0, 1};
    const Subresource mip1{1, 1, 0, 1};

    graph.addPass("write_mip0_a").addStorageImageOutput("chain", desc, mip0).setSideEffect();  // 0
    graph.addPass("write_mip0_b").addStorageImageOutput("chain", desc, mip0).setSideEffect();  // 1: WAW on mip0
    graph.addPass("write_mip1").addStorageImageOutput("chain", desc, mip1).setSideEffect();     // 2: mip1's first use
    graph.addPass("present").addColorOutput("bb", colorDesc());                                 // 3
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const auto order = graph.compiled().executionOrder();
    REQUIRE(order.size() == 4);
    // write_mip0_a/write_mip0_b share a name -> a real write-after-write
    // dependsOn edge orders them relative to each other; write_mip1 has no
    // edge to either (a disjoint subresource of the same name still only
    // ever chains a dependsOn edge by NAME, per step 1's writersByName
    // construction -- see render_graph.cpp -- so all three of these
    // passes' declaration-order positions [0, 1, 2] are still exactly
    // Kahn's ascending-index tie-break result among what remain,
    // structurally, three independent roots).
    CHECK(order[0] == 0);
    CHECK(order[1] == 1);
    CHECK(order[2] == 2);

    const auto barriers = graph.compiled().passBarriers();
    const uint32_t chain = physicalIndexOf(graph.compiled(), "chain");

    // pass 0 (write_mip0_a): first-ever access to (chain, mip0) -- a real
    // UNDEFINED->GENERAL layout-transition barrier (every image's first use
    // needs one -- srcStage NONE, nothing precedes it).
    REQUIRE(barriers[0].imageBarriers.size() == 1);
    checkImageBarrier(barriers[0].imageBarriers[0], chain, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    // pass 1 (write_mip0_b): WAW on the SAME subresource -- chains off
    // pass 0's own write (srcStage COMPUTE_SHADER, not NONE); both layouts
    // GENERAL already (no transition needed, only an execution+
    // availability dependency).
    REQUIRE(barriers[1].imageBarriers.size() == 1);
    checkImageBarrier(barriers[1].imageBarriers[0], chain, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    CHECK(barriers[1].imageBarriers[0].subresource.baseMipLevel == 0);

    // pass 2 (write_mip1): a COMPLETELY DISJOINT subresource from mip0's
    // own WAW activity just above -- its OWN fresh UNDEFINED->GENERAL,
    // srcStage NONE first-use barrier, proving mip0's state (already
    // GENERAL, already written twice) never leaked into mip1's.
    REQUIRE(barriers[2].imageBarriers.size() == 1);
    checkImageBarrier(barriers[2].imageBarriers[0], chain, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    CHECK(barriers[2].imageBarriers[0].subresource.baseMipLevel == 1);
}
