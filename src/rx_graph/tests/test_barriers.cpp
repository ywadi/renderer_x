// Task 2 brief's exact required doctest cases, asserting every one of the
// six mask/layout fields on the barrier structs it names, plus one direct
// unit test of applyAccess()'s WAR (write-after-read) rule in isolation.
//
// "war-execution-only" cannot be built by declaring passes through
// RenderGraph's public API: Task 1's compile() binds every reader of a
// declared resource name to that name's *final* declared writer (see
// render_graph.cpp's step 2 comment), so a read of "T" can never itself be
// ordered ahead of a later write of "T" -- any attempt to declare a second
// writer of the same name forces every reader to depend on it instead,
// which reorders the read to *after* both writes (write, write, read),
// never write, read, write. Confirmed empirically (see this task's
// report) before writing this file, rather than assumed. applyAccess()
// (barriers.h) is exposed at namespace scope precisely so the WAR
// accounting rule itself -- a real, necessary rule for a production
// barrier deriver even though today's RenderGraph pass topology can't
// exercise it end to end -- can still be verified directly, against a
// synthetic access sequence.
#include <doctest/doctest.h>
#include <rx_graph/render_graph.h>

using namespace rx::graph;

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
// asserting the list's total size.
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
    const ImageBarrier* hdrBarrier = findImageBarrier(barriers[1].imageBarriers, hdr);
    REQUIRE(hdrBarrier != nullptr);
    checkImageBarrier(*hdrBarrier, hdr, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
    RenderGraph graph;
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();
    const uint32_t bb = physicalIndexOf(compiled, "bb");

    const PassBarriers& final_ = compiled.finalBarriers();
    REQUIRE(final_.imageBarriers.size() == 1);
    CHECK(final_.bufferBarriers.empty());
    checkImageBarrier(final_.imageBarriers[0], bb, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
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
    const ImageBarrier* sharedBarrier = findImageBarrier(barriers[1].imageBarriers, shared);
    REQUIRE(sharedBarrier != nullptr);
    checkImageBarrier(*sharedBarrier, shared, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
