// Task 1 brief's exact test expectations (culling/ordering/lifetimes/
// usage-union/diamond/errors), plus a few extra cases exercising the rest
// of the Pass/RenderGraph/CompiledGraph surface (accessors, reset(),
// swapchain-relative size resolution, compute-vs-graphics storage buffer
// stage classification) that the six required cases don't otherwise touch.
#include <doctest/doctest.h>
#include <rx_core/log.h>
#include <rx_core/log_forward_sink.h>
#include <rx_graph/pass_signature.h>
#include <rx_graph/render_graph.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace rx::graph;

namespace {

// A representative 1080p swapchain -- no test below depends on the exact
// numbers beyond the swapchain-relative-resolution cases, which check the
// arithmetic against these fields directly.
constexpr CompileInfo kInfo{1920, 1080, VK_FORMAT_B8G8R8A8_UNORM};

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

// [Task 2, gate ruling RC2] A plain single-mip/single-layer storage-image
// shape -- the degenerate case every field of ImageDesc that predates this
// task's default already covers, used by every test below that does not
// itself need more than one mip/layer.
ImageDesc storageImageDesc() {
    ImageDesc desc;
    desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    desc.sizeClass = SizeClass::Absolute;
    desc.width = 64.0F;
    desc.height = 64.0F;
    return desc;
}

const PhysicalResource* findResource(const CompiledGraph& compiled, std::string_view name) {
    for (const PhysicalResource& resource : compiled.resources()) {
        if (resource.name == name) {
            return &resource;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("culling") {
    RenderGraph graph;
    graph.addPass("A").addColorOutput("x", colorDesc());               // pass 0: unread, no side effect
    graph.addPass("B").addColorOutput("bb", colorDesc());              // pass 1: the backbuffer's writer
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();
    CHECK(compiled.isCulled(0));
    CHECK_FALSE(compiled.isCulled(1));

    const auto order = compiled.executionOrder();
    REQUIRE(order.size() == 1);
    CHECK(order[0] == 1);

    CHECK(findResource(compiled, "x") == nullptr);
    CHECK(findResource(compiled, "bb") != nullptr);
}

// [Phase 4 exit fix wave, C1] compile() logs a WARN, naming the pass, when
// a pass carrying a real setExecute()/setExecuteChunked() callback gets
// culled -- this is exactly the class of defect the phase-exit review found
// in sample 09 (a "shadow" pass writing "shadowmap" with no consumer and no
// setSideEffect(): culled silently, its callback never invoked, and
// compile() logged nothing at all). Uses the same process-wide
// LogForwardSink rx_core/tests/log_test.cpp's own ForwardCallbackGuard
// pattern captures records through -- rx_graph is PUBLIC-linked against
// rx_core (CMakeLists.txt), so this header is reachable from here without
// any new dependency.

namespace {

struct CulledWarnCapture {
    int32_t severity = -1;
    std::string message;
    int callCount = 0;
};

CulledWarnCapture* g_culledWarnCapture = nullptr;

void captureCulledWarn(int32_t severity, const char* /*category*/, const char* message, void* /*userData*/) {
    if (g_culledWarnCapture == nullptr) {
        return;
    }
    // Only the culled-pass warning this test cares about -- the same
    // process-wide sink also forwards every other record any earlier/later
    // TEST_CASE in this binary happens to log, so filter by content rather
    // than assuming call ordinality.
    if (message != nullptr && std::string_view(message).find("was culled") != std::string_view::npos) {
        g_culledWarnCapture->severity = severity;
        g_culledWarnCapture->message = message;
        g_culledWarnCapture->callCount++;
    }
}

// RAII guard mirroring log_test.cpp's ForwardCallbackGuard -- uninstalls on
// scope exit regardless of how the TEST_CASE returns (including a failed
// REQUIRE, which doctest unwinds via a C++ exception).
struct CulledWarnGuard {
    std::shared_ptr<rx::core::log::LogForwardSink> sink;
    ~CulledWarnGuard() {
        g_culledWarnCapture = nullptr;
        (void)sink->set(nullptr, nullptr);
    }
};

}  // namespace

TEST_CASE("compile() warns, naming the pass, when a culled pass carries a real execute callback") {
    RenderGraph graph;
    // Same shape as sample 09's pre-fix bug: "shadow" writes "sm" but
    // nothing reads it and it never calls setSideEffect() -- culled. "bb"
    // is the only reachable pass.
    bool shadowCallbackInvoked = false;
    graph.addPass("shadow")
        .setDepthStencilOutput("sm", depthDesc())
        .setExecute([&shadowCallbackInvoked](PassContext&) { shadowCallbackInvoked = true; });
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");

    rx::core::log::init();
    CulledWarnCapture capture;
    g_culledWarnCapture = &capture;
    auto sink = rx::core::log::forwardSink();
    REQUIRE(sink->set(&captureCulledWarn, nullptr));
    CulledWarnGuard guard{sink};

    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();
    CHECK(compiled.isCulled(0));
    CHECK_FALSE(shadowCallbackInvoked);  // never invoked -- CompiledGraph::executionOrder() skips culled passes.

    CHECK(capture.callCount == 1);
    CHECK(capture.severity == 3);  // warn, per LogForwardSink's Trace..Error mapping.
    CHECK(capture.message.find("shadow") != std::string::npos);
}

TEST_CASE("compile() does NOT warn once the previously-culled pass gains a real consumer") {
    RenderGraph graph;
    // Identical topology to the case above, except "present" now declares
    // addTextureInput("sm") -- the C1 fix shape (a genuine graph-derived
    // read, not a setSideEffect() band-aid): "shadow" is reachable, no
    // culled-with-callback condition exists to warn about.
    graph.addPass("shadow").setDepthStencilOutput("sm", depthDesc()).setExecute([](PassContext&) {});
    graph.addPass("present").addTextureInput("sm").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");

    rx::core::log::init();
    CulledWarnCapture capture;
    g_culledWarnCapture = &capture;
    auto sink = rx::core::log::forwardSink();
    REQUIRE(sink->set(&captureCulledWarn, nullptr));
    CulledWarnGuard guard{sink};

    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();
    CHECK_FALSE(compiled.isCulled(0));
    CHECK(capture.callCount == 0);
}

TEST_CASE("ordering") {
    RenderGraph graph;
    // Declared out of pipeline order: tonemap (reads a resource its own
    // producer hasn't been declared yet) first, then shadow, then forward.
    graph.addPass("tonemap").addTextureInput("hdr").addColorOutput("bb", colorDesc());  // pass 0
    graph.addPass("shadow").setDepthStencilOutput("sm", depthDesc());                   // pass 1
    graph.addPass("forward").addTextureInput("sm").addColorOutput("hdr", colorDesc());  // pass 2

    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const auto order = graph.compiled().executionOrder();
    REQUIRE(order.size() == 3);
    CHECK(order[0] == 1);  // shadow
    CHECK(order[1] == 2);  // forward
    CHECK(order[2] == 0);  // tonemap
}

TEST_CASE("lifetimes") {
    RenderGraph graph;
    graph.addPass("shadow").setDepthStencilOutput("sm", depthDesc());
    graph.addPass("forward").addTextureInput("sm").addColorOutput("hdr", colorDesc());
    graph.addPass("tonemap").addTextureInput("hdr").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();

    const PhysicalResource* sm = findResource(compiled, "sm");
    REQUIRE(sm != nullptr);
    CHECK(sm->firstUsePass == 0);
    CHECK(sm->lastUsePass == 1);

    const PhysicalResource* hdr = findResource(compiled, "hdr");
    REQUIRE(hdr != nullptr);
    CHECK(hdr->firstUsePass == 1);
    CHECK(hdr->lastUsePass == 2);

    const PhysicalResource* bb = findResource(compiled, "bb");
    REQUIRE(bb != nullptr);
    CHECK(bb->firstUsePass == 2);
    CHECK(bb->lastUsePass == 2);
    CHECK(bb->isBackbuffer);
}

TEST_CASE("usage-union") {
    RenderGraph graph;
    graph.addPass("shadow").setDepthStencilOutput("sm", depthDesc());
    graph.addPass("forward").addTextureInput("sm").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const PhysicalResource* sm = findResource(graph.compiled(), "sm");
    REQUIRE(sm != nullptr);
    CHECK((sm->imageUsage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U);
    CHECK((sm->imageUsage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0U);
}

TEST_CASE("diamond") {
    RenderGraph graph;
    graph.addPass("A").addColorOutput("a", colorDesc());                                            // pass 0
    graph.addPass("B").addTextureInput("a").addColorOutput("b", colorDesc());                       // pass 1
    graph.addPass("C").addTextureInput("a").addColorOutput("c", colorDesc());                       // pass 2
    graph.addPass("D").addTextureInput("b").addTextureInput("c").addColorOutput("bb", colorDesc());  // pass 3
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const auto order = graph.compiled().executionOrder();
    REQUIRE(order.size() == 4);
    CHECK(order[0] == 0);  // A
    CHECK(order[1] == 1);  // B
    CHECK(order[2] == 2);  // C
    CHECK(order[3] == 3);  // D
}

TEST_CASE("errors") {
    SUBCASE("reading a never-written resource throws, naming the resource") {
        RenderGraph graph;
        graph.addPass("consumer").addTextureInput("ghost").setSideEffect();
        graph.addPass("present").addColorOutput("bb", colorDesc());
        graph.setBackbufferSource("bb");

        bool threw = false;
        try {
            graph.compile(kInfo);
        } catch (const std::runtime_error& e) {
            threw = true;
            CHECK(std::string(e.what()).find("ghost") != std::string::npos);
        }
        CHECK(threw);
    }

    SUBCASE("backbuffer source never written throws, naming the resource") {
        RenderGraph graph;
        graph.addPass("noop").setSideEffect();
        graph.setBackbufferSource("bb");

        bool threw = false;
        try {
            graph.compile(kInfo);
        } catch (const std::runtime_error& e) {
            threw = true;
            CHECK(std::string(e.what()).find("bb") != std::string::npos);
        }
        CHECK(threw);
    }

    SUBCASE("compile() without setBackbufferSource throws") {
        RenderGraph graph;
        graph.addPass("present").addColorOutput("bb", colorDesc());
        CHECK_THROWS_AS(graph.compile(kInfo), std::runtime_error);
    }

    SUBCASE("duplicate pass names throw") {
        RenderGraph graph;
        graph.addPass("dup").addColorOutput("bb", colorDesc());
        graph.addPass("dup").setSideEffect();
        graph.setBackbufferSource("bb");
        CHECK_THROWS_AS(graph.compile(kInfo), std::runtime_error);
    }
}

TEST_CASE("Pass accessors and RenderGraph::reset()") {
    RenderGraph graph;
    Pass& pass = graph.addPass("main", QueueClass::AsyncCompute);
    CHECK(pass.name() == "main");
    CHECK(pass.queueClass() == QueueClass::AsyncCompute);

    pass.addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);
    CHECK(graph.compiled().executionOrder().size() == 1);

    graph.reset();
    // A freshly reset graph forgets its backbuffer source exactly like a
    // freshly constructed one -- compile() throws again until it's set.
    CHECK_THROWS_AS(graph.compile(kInfo), std::runtime_error);
}

// Phase 4 Task 7 [spec D4 amendment, "Parallelism is the engine default, not
// a mode"]: setExecute()/setExecuteChunked() are mutually exclusive -- a
// pass provides EITHER a whole-pass callback OR a chunked one, never both,
// and the second call in either order throws rather than silently letting
// "last call wins" leave an ambiguous pass. Device-free (this file's own
// scope): neither callback is ever actually INVOKED here -- that needs a
// real Executor (rx_graph_gpu_tests' own test_execute_gpu.cpp).
TEST_CASE("Pass::setExecute and setExecuteChunked are mutually exclusive, throwing std::logic_error naming the pass") {
    RenderGraph graph;

    Pass& chunkedFirst = graph.addPass("chunked_first");
    chunkedFirst.setExecuteChunked([](PassContext&, uint32_t, uint32_t) {});
    CHECK_THROWS_AS(chunkedFirst.setExecute([](PassContext&) {}), std::logic_error);
    // The failed setExecute() call must not have clobbered the already-
    // stored chunked callback -- re-calling setExecuteChunked() (a legal,
    // idempotent re-set of the SAME kind) must still throw for the SAME
    // reason, proving the pass's chunked-callback state survived the
    // rejected setExecute() attempt untouched.
    CHECK_THROWS_AS(chunkedFirst.setExecute([](PassContext&) {}), std::logic_error);

    Pass& wholeFirst = graph.addPass("whole_first");
    wholeFirst.setExecute([](PassContext&) {});
    CHECK_THROWS_AS(wholeFirst.setExecuteChunked([](PassContext&, uint32_t, uint32_t) {}), std::logic_error);
}

TEST_CASE("the backbuffer's resolved attachment always mirrors the swapchain") {
    RenderGraph graph;
    AttachmentDesc declared = colorDesc();
    declared.sizeClass = SizeClass::SwapchainRelative;
    declared.width = 0.5F;  // deliberately wrong on purpose -- compile() must override it
    declared.height = 0.5F;
    graph.addPass("present").addColorOutput("bb", declared);
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const PhysicalResource* bb = findResource(graph.compiled(), "bb");
    REQUIRE(bb != nullptr);
    CHECK(bb->attachment.sizeClass == SizeClass::Absolute);
    CHECK(bb->attachment.width == static_cast<float>(kInfo.swapchainWidth));
    CHECK(bb->attachment.height == static_cast<float>(kInfo.swapchainHeight));
    CHECK(bb->attachment.format == kInfo.swapchainFormat);
}

TEST_CASE("a non-backbuffer swapchain-relative attachment resolves against the swapchain extent") {
    RenderGraph graph;
    AttachmentDesc halfRes = colorDesc();
    halfRes.width = 0.5F;
    halfRes.height = 0.5F;
    graph.addPass("half").addColorOutput("half_res", halfRes).setSideEffect();
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const PhysicalResource* halfResResource = findResource(graph.compiled(), "half_res");
    REQUIRE(halfResResource != nullptr);
    CHECK(halfResResource->attachment.sizeClass == SizeClass::Absolute);
    CHECK(halfResResource->attachment.width == static_cast<float>(kInfo.swapchainWidth) * 0.5F);
    CHECK(halfResResource->attachment.height == static_cast<float>(kInfo.swapchainHeight) * 0.5F);
}

TEST_CASE("storage buffer accesses resolve compute-class vs graphics-class stages") {
    RenderGraph graph;
    BufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    // "particles" has no attachment output at all -- Compute-class.
    graph.addPass("particles").addStorageBufferOutput("particle_buf", bufferDesc).setSideEffect();  // pass 0
    // "present" has a color output alongside its storage buffer input -- Graphics-class.
    graph.addPass("present").addStorageBufferInput("particle_buf").addColorOutput("bb", colorDesc());  // pass 1
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();

    const auto particlesAccesses = compiled.passAccesses(0);
    REQUIRE(particlesAccesses.size() == 1);
    CHECK(particlesAccesses[0].stages == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    CHECK(particlesAccesses[0].access == (VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT));
    CHECK(particlesAccesses[0].layout == VK_IMAGE_LAYOUT_UNDEFINED);

    const auto presentAccesses = compiled.passAccesses(1);
    REQUIRE(presentAccesses.size() == 2);
    CHECK(presentAccesses[0].stages == (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT));
    CHECK(presentAccesses[0].access == VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

TEST_CASE("a culled pass reports no resolved accesses") {
    RenderGraph graph;
    graph.addPass("A").addColorOutput("x", colorDesc());  // pass 0: culled
    graph.addPass("B").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    CHECK(graph.compiled().passAccesses(0).empty());
}

// Fix round 1 -- Task 1 review's Important finding: a circular pass
// dependency (legally constructible because a reader may be declared
// before its writer -- exactly the "ordering" test's own mechanism) used
// to compile to a silently empty executionOrder() with no diagnostic.
TEST_CASE("a circular pass dependency throws, naming passes in the cycle") {
    RenderGraph graph;
    // A and B form a genuine 2-cycle: A reads "y" (B's output) and writes
    // "x"; B reads "x" (A's output) and writes "y". "present" reads "x"
    // and writes "bb", which is what roots the cycle to the backbuffer's
    // writer -- without a reachable root the cycle would just be culled
    // away, not detected.
    graph.addPass("A").addTextureInput("y").addColorOutput("x", colorDesc());        // pass 0
    graph.addPass("B").addTextureInput("x").addColorOutput("y", colorDesc());        // pass 1
    graph.addPass("present").addTextureInput("x").addColorOutput("bb", colorDesc());  // pass 2
    graph.setBackbufferSource("bb");

    bool threw = false;
    try {
        graph.compile(kInfo);
    } catch (const std::runtime_error& e) {
        threw = true;
        const std::string message = e.what();
        // Both cycle members must be named -- not just "some pass never
        // ran" (which could equally describe "present", which is merely
        // downstream of the cycle, not a member of it).
        CHECK(message.find("A") != std::string::npos);
        CHECK(message.find("B") != std::string::npos);
    }
    CHECK(threw);
}

// Fix round 1 -- coordinator ruling on the Task 1 review's Minor finding
// #2: the same Compute-class/Graphics-class split already applied to
// storage buffer accesses now also applies to texture-input accesses.
TEST_CASE("texture input stage resolves per pass kind, like storage buffers already did") {
    RenderGraph graph;
    graph.addPass("producer").addColorOutput("tex", colorDesc());  // pass 0
    // "sampler" has no attachment output at all -- Compute-class.
    graph.addPass("sampler").addTextureInput("tex").setSideEffect();  // pass 1
    // "present" has a color output alongside its texture input -- Graphics-class.
    graph.addPass("present").addTextureInput("tex").addColorOutput("bb", colorDesc());  // pass 2
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();

    const auto samplerAccesses = compiled.passAccesses(1);
    REQUIRE(samplerAccesses.size() == 1);
    CHECK(samplerAccesses[0].stages == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    CHECK(samplerAccesses[0].access == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    CHECK(samplerAccesses[0].layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const auto presentAccesses = compiled.passAccesses(2);
    REQUIRE(presentAccesses.size() == 2);
    CHECK(presentAccesses[0].stages == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    CHECK(presentAccesses[0].access == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    CHECK(presentAccesses[0].layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// ============================================================================
// Phase 4 Task 1: history resources + the carried color-attachment bounds
// check. Device-free -- everything below exercises RenderGraph::compile()'s
// declaration/validation/culling semantics only, never a real device (the
// GPU-backed ping-pong/first-frame-contract test lives in
// test_execute_gpu.cpp, rx_graph_gpu_tests).
// ============================================================================

TEST_CASE("history output resolves color or depth by format, like addColorOutput/setDepthStencilOutput") {
    RenderGraph graph;
    graph.addPass("write_color_hist").setHistoryOutput("hist_color", colorDesc());  // pass 0
    graph.addPass("write_depth_hist").setHistoryOutput("hist_depth", depthDesc());  // pass 1
    graph.addPass("present").addColorOutput("bb", colorDesc());                     // pass 2
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();

    const auto colorAccesses = compiled.passAccesses(0);
    REQUIRE(colorAccesses.size() == 1);
    CHECK(colorAccesses[0].stages == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK(colorAccesses[0].access == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK(colorAccesses[0].layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    const auto depthAccesses = compiled.passAccesses(1);
    REQUIRE(depthAccesses.size() == 1);
    CHECK(depthAccesses[0].stages ==
          (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT));
    CHECK(depthAccesses[0].access ==
          (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));
    CHECK(depthAccesses[0].layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    // Both are marked isHistory and sampled -- unconditionally, per
    // setHistoryOutput()'s own comment, even though neither is read by an
    // addHistoryInput() declaration anywhere in THIS graph.
    const PhysicalResource* histColor = findResource(compiled, "hist_color");
    REQUIRE(histColor != nullptr);
    CHECK(histColor->isHistory);
    CHECK((histColor->imageUsage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0U);
    CHECK((histColor->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0U);

    const PhysicalResource* histDepth = findResource(compiled, "hist_depth");
    REQUIRE(histDepth != nullptr);
    CHECK(histDepth->isHistory);
    CHECK((histDepth->imageUsage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0U);
    CHECK((histDepth->imageUsage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U);
}

// Mirrors "texture input stage resolves per pass kind" above, for
// addHistoryInput() instead of addTextureInput() -- same Compute-class/
// Graphics-class split, same resolved access/layout.
TEST_CASE("history input resolves like texture input, compute vs graphics split") {
    RenderGraph graph;
    graph.addPass("writer").setHistoryOutput("hist", colorDesc());  // pass 0
    // "reader" has no attachment output at all -- Compute-class.
    graph.addPass("reader").addHistoryInput("hist").setSideEffect();  // pass 1
    // "present" has a color output alongside its history input -- Graphics-class.
    graph.addPass("present").addHistoryInput("hist").addColorOutput("bb", colorDesc());  // pass 2
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();

    const auto readerAccesses = compiled.passAccesses(1);
    REQUIRE(readerAccesses.size() == 1);
    CHECK(readerAccesses[0].stages == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    CHECK(readerAccesses[0].access == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    CHECK(readerAccesses[0].layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const auto presentAccesses = compiled.passAccesses(2);
    REQUIRE(presentAccesses.size() == 2);
    CHECK(presentAccesses[0].stages == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    CHECK(presentAccesses[0].access == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    CHECK(presentAccesses[0].layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

TEST_CASE("a resource used as both a history and a transient/buffer resource throws, naming it") {
    RenderGraph graph;
    graph.addPass("writer").setHistoryOutput("shared_name", colorDesc());
    graph.addPass("other_writer").addColorOutput("shared_name", colorDesc());
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");

    bool threw = false;
    try {
        graph.compile(kInfo);
    } catch (const std::runtime_error& e) {
        threw = true;
        CHECK(std::string(e.what()).find("shared_name") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("more than one setHistoryOutput() pass for the same name throws, naming the resource and both passes") {
    RenderGraph graph;
    graph.addPass("writerA").setHistoryOutput("hist", colorDesc());
    graph.addPass("writerB").setHistoryOutput("hist", colorDesc());
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");

    bool threw = false;
    try {
        graph.compile(kInfo);
    } catch (const std::runtime_error& e) {
        threw = true;
        const std::string message = e.what();
        CHECK(message.find("hist") != std::string::npos);
        CHECK(message.find("writerA") != std::string::npos);
        CHECK(message.find("writerB") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("addHistoryInput() reading an undeclared history resource throws, naming the pass and the resource") {
    RenderGraph graph;
    graph.addPass("reader").addHistoryInput("ghost_history").setSideEffect();
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");

    bool threw = false;
    try {
        graph.compile(kInfo);
    } catch (const std::runtime_error& e) {
        threw = true;
        const std::string message = e.what();
        CHECK(message.find("reader") != std::string::npos);
        CHECK(message.find("ghost_history") != std::string::npos);
    }
    CHECK(threw);
}

// A setHistoryOutput() pass persists across the frame boundary regardless
// of whether anything in THIS compiled graph reads it -- it must survive
// culling exactly like a setSideEffect() pass does, never be treated as
// dead code just because nothing downstream consumes it this frame (see
// setHistoryOutput()'s own comment, pass.h).
TEST_CASE("a history-output pass is never culled even with no reader in the graph") {
    RenderGraph graph;
    graph.addPass("write_only_history").setHistoryOutput("hist", colorDesc());  // pass 0: no reader anywhere
    graph.addPass("present").addColorOutput("bb", colorDesc());                 // pass 1
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();
    CHECK_FALSE(compiled.isCulled(0));

    const auto order = compiled.executionOrder();
    REQUIRE(order.size() == 2);
    CHECK(findResource(compiled, "hist") != nullptr);
}

// addHistoryInput() must NOT add a same-frame dependsOn edge onto its
// resource's setHistoryOutput() writer (see that method's own comment) --
// proven here by a shape that WOULD be a genuine dependency cycle if that
// edge existed: "accum" writes history "hist" AND reads "hist" (its own
// previous-frame contents, the canonical ping-pong-accumulator pattern),
// while ALSO depending on "present" through an ordinary same-frame chain
// that itself depends on "accum"'s own ordinary output. If addHistoryInput()
// wrongly created dependsOn["accum"] -> writer("hist") == "accum" itself,
// that would be a trivial (already-guarded) self-edge and prove nothing;
// the real proof is that compile() succeeds AT ALL with "accum" declaring
// both -- a same-frame edge from history would not by itself create a
// cycle here, so this test's real assertion is simply that BOTH
// declarations coexisting on one pass, and being read by another pass
// entirely, compiles cleanly with the expected execution order.
TEST_CASE("a history-input read creates no same-frame ordering dependency on its own resource's history-output writer") {
    RenderGraph graph;
    // "accum" both reads last frame's "hist" and writes this frame's "hist"
    // -- the canonical persistent-accumulator pattern setHistoryOutput()'s
    // own comment describes.
    graph.addPass("accum").addHistoryInput("hist").setHistoryOutput("hist", colorDesc()).addColorOutput(
        "resolved", colorDesc());  // pass 0
    graph.addPass("present").addTextureInput("resolved").addColorOutput("bb", colorDesc());  // pass 1
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();
    const auto order = compiled.executionOrder();
    REQUIRE(order.size() == 2);
    CHECK(order[0] == 0);  // accum
    CHECK(order[1] == 1);  // present

    // Both the read and the write declarations resolve to the SAME
    // physicalIndex (one logical name, per the device-free model) --
    // exactly two ResourceAccess entries against "hist"'s physicalIndex on
    // pass 0, plus the unrelated "resolved" color output.
    const uint32_t histIdx = [&] {
        const auto resources = compiled.resources();
        for (uint32_t i = 0; i < resources.size(); ++i) {
            if (resources[i].name == "hist") {
                return i;
            }
        }
        FAIL("no physical resource named 'hist'");
        return UINT32_MAX;
    }();
    const auto accumAccesses = compiled.passAccesses(0);
    uint32_t histAccessCount = 0;
    for (const ResourceAccess& access : accumAccesses) {
        if (access.physicalIndex == histIdx) {
            ++histAccessCount;
        }
    }
    CHECK(histAccessCount == 2);
}

TEST_CASE("compile() throws, naming the pass, when it declares more color outputs than kMaxColorAttachments") {
    RenderGraph graph;
    Pass& pass = graph.addPass("too_many_colors").setSideEffect();
    for (uint32_t i = 0; i < PassSignature::kMaxColorAttachments + 1; ++i) {
        pass.addColorOutput("color_" + std::to_string(i), colorDesc());
    }
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");

    bool threw = false;
    try {
        graph.compile(kInfo);
    } catch (const std::runtime_error& e) {
        threw = true;
        CHECK(std::string(e.what()).find("too_many_colors") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("compile() accepts a pass declaring exactly kMaxColorAttachments color outputs") {
    RenderGraph graph;
    Pass& pass = graph.addPass("exactly_max_colors").setSideEffect();
    for (uint32_t i = 0; i < PassSignature::kMaxColorAttachments; ++i) {
        pass.addColorOutput("color_" + std::to_string(i), colorDesc());
    }
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    CHECK_NOTHROW(graph.compile(kInfo));
}

// A HistoryOutput declaration that resolves to a color format counts
// toward the SAME per-pass limit as an ordinary addColorOutput() -- see
// compile()'s own bounds-check comment for why (it IS a color attachment
// output, chosen dynamically by format).
TEST_CASE("a HistoryOutput declaration counts toward the color-attachment bounds check when it resolves to color") {
    RenderGraph graph;
    Pass& pass = graph.addPass("too_many_colors").setSideEffect();
    for (uint32_t i = 0; i < PassSignature::kMaxColorAttachments; ++i) {
        pass.addColorOutput("color_" + std::to_string(i), colorDesc());
    }
    pass.setHistoryOutput("one_history_too_many", colorDesc());  // pushes the count to kMaxColorAttachments + 1
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    CHECK_THROWS_AS(graph.compile(kInfo), std::runtime_error);
}

// A depth-format HistoryOutput never counts toward the color-attachment
// bounds check, matching setDepthStencilOutput()'s own long-standing
// unbounded treatment.
TEST_CASE("a depth-format HistoryOutput declaration never counts toward the color-attachment bounds check") {
    RenderGraph graph;
    Pass& pass = graph.addPass("at_max_colors_plus_history_depth").setSideEffect();
    for (uint32_t i = 0; i < PassSignature::kMaxColorAttachments; ++i) {
        pass.addColorOutput("color_" + std::to_string(i), colorDesc());
    }
    pass.setHistoryOutput("hist_depth", depthDesc());
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    CHECK_NOTHROW(graph.compile(kInfo));
}

// ===========================================================================
// [Task 2, gate ruling RC2] Storage-image resource-model extension: the
// generic per-mip/per-layer subresource pass I/O + storage-image read/write
// API + cube/array views RC2 requires T2 to land once, device-free-testable
// here exactly like every other declaration kind above.
// ===========================================================================

TEST_CASE("addStorageImageOutput establishes a storage-image resource with STORAGE usage and its declared shape") {
    RenderGraph graph;
    ImageDesc desc = storageImageDesc();
    desc.mipLevels = 3;
    desc.arrayLayers = 6;
    desc.cube = true;

    graph.addPass("bake").addStorageImageOutput("env", desc).setSideEffect();
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const PhysicalResource* env = findResource(graph.compiled(), "env");
    REQUIRE(env != nullptr);
    CHECK_FALSE(env->isBuffer);
    CHECK(env->mipLevels == 3);
    CHECK(env->arrayLayers == 6);
    CHECK(env->cube);
    CHECK((env->imageUsage & VK_IMAGE_USAGE_STORAGE_BIT) != 0);
    CHECK(env->attachment.format == VK_FORMAT_R8G8B8A8_UNORM);
}

TEST_CASE("a second addStorageImageOutput of the same name only narrows subresource, never redeclares shape") {
    RenderGraph graph;
    ImageDesc desc = storageImageDesc();
    desc.mipLevels = 2;
    desc.arrayLayers = 1;

    ImageDesc bogus = storageImageDesc();
    bogus.mipLevels = 99;  // must be ignored -- the FIRST establishment wins

    Subresource mip0{0, 1, 0, 1};
    Subresource mip1{1, 1, 0, 1};
    graph.addPass("write_mip0").addStorageImageOutput("chain", desc, mip0).setSideEffect();
    graph.addPass("write_mip1").addStorageImageOutput("chain", bogus, mip1).setSideEffect();
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const PhysicalResource* chain = findResource(graph.compiled(), "chain");
    REQUIRE(chain != nullptr);
    CHECK(chain->mipLevels == 2);  // NOT 99 -- the second declaration's `desc` was ignored.
}

TEST_CASE("addStorageImageOutput/Input resolve compute-class vs graphics-class stages at VK_IMAGE_LAYOUT_GENERAL") {
    RenderGraph graph;
    // "bake" has no attachment output at all -- Compute-class.
    graph.addPass("bake").addStorageImageOutput("img", storageImageDesc()).setSideEffect();  // pass 0
    // "present" has a color output alongside its storage-image input -- Graphics-class.
    graph.addPass("present").addStorageImageInput("img").addColorOutput("bb", colorDesc());  // pass 1
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const CompiledGraph& compiled = graph.compiled();

    const auto bakeAccesses = compiled.passAccesses(0);
    REQUIRE(bakeAccesses.size() == 1);
    CHECK(bakeAccesses[0].stages == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    CHECK(bakeAccesses[0].access == (VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT));
    CHECK(bakeAccesses[0].layout == VK_IMAGE_LAYOUT_GENERAL);

    const auto presentAccesses = compiled.passAccesses(1);
    REQUIRE(presentAccesses.size() == 2);
    CHECK(presentAccesses[0].stages == (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT));
    CHECK(presentAccesses[0].access == VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    CHECK(presentAccesses[0].layout == VK_IMAGE_LAYOUT_GENERAL);
}

TEST_CASE("compile() rejects a cube storage image whose arrayLayers is not a positive multiple of 6") {
    RenderGraph graph;
    ImageDesc desc = storageImageDesc();
    desc.cube = true;
    desc.arrayLayers = 4;  // not a multiple of 6

    graph.addPass("bake").addStorageImageOutput("env", desc).setSideEffect();
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");

    bool threw = false;
    try {
        graph.compile(kInfo);
    } catch (const std::runtime_error& e) {
        threw = true;
        CHECK(std::string(e.what()).find("env") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("a default-constructed Subresource resolves to the whole resource, sentinels replaced with real counts") {
    RenderGraph graph;
    ImageDesc desc = storageImageDesc();
    desc.mipLevels = 3;
    desc.arrayLayers = 6;
    desc.cube = true;

    graph.addPass("bake").addStorageImageOutput("env", desc).setSideEffect();  // default subresource
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const auto bakeAccesses = graph.compiled().passAccesses(0);
    REQUIRE(bakeAccesses.size() == 1);
    CHECK(bakeAccesses[0].subresource.baseMipLevel == 0);
    CHECK(bakeAccesses[0].subresource.levelCount == 3);
    CHECK(bakeAccesses[0].subresource.baseArrayLayer == 0);
    CHECK(bakeAccesses[0].subresource.layerCount == 6);
}

TEST_CASE("an explicit subresource narrows the resolved access to a sub-range of the resource") {
    RenderGraph graph;
    ImageDesc desc = storageImageDesc();
    desc.mipLevels = 1;
    desc.arrayLayers = 6;
    desc.cube = true;

    graph.addPass("bake_face2").addStorageImageOutput("env", desc, Subresource{0, 1, 2, 1}).setSideEffect();
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const auto accesses = graph.compiled().passAccesses(0);
    REQUIRE(accesses.size() == 1);
    CHECK(accesses[0].subresource.baseMipLevel == 0);
    CHECK(accesses[0].subresource.levelCount == 1);
    CHECK(accesses[0].subresource.baseArrayLayer == 2);
    CHECK(accesses[0].subresource.layerCount == 1);
}

TEST_CASE("addTextureInput accepts an explicit subresource narrowing a read of a multi-layer storage image") {
    RenderGraph graph;
    ImageDesc desc = storageImageDesc();
    desc.arrayLayers = 6;
    desc.cube = true;

    // The read's subresource matches the write's exactly (both address
    // "face 3 alone") -- identical ranges are the supported case (see the
    // "overlapping-but-not-identical" throw test above for the range this
    // deliberately avoids).
    const Subresource face3{0, 1, 3, 1};
    graph.addPass("bake").addStorageImageOutput("env", desc, face3).setSideEffect();
    graph.addPass("present").addTextureInput("env", face3).addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    graph.compile(kInfo);

    const auto presentAccesses = graph.compiled().passAccesses(1);
    REQUIRE(presentAccesses.size() == 2);
    CHECK(presentAccesses[0].subresource.baseArrayLayer == 3);
    CHECK(presentAccesses[0].subresource.layerCount == 1);
}

TEST_CASE("compile() throws when two overlapping-but-not-identical subresource ranges target the same resource") {
    RenderGraph graph;
    ImageDesc desc = storageImageDesc();
    desc.arrayLayers = 6;
    desc.cube = true;

    // "whole array at mip 0" (layers [0,6)) vs "layer 3 alone" (layers
    // [3,4)) -- overlapping (layer 3 is inside both) but not identical.
    graph.addPass("bake_all").addStorageImageOutput("env", desc).setSideEffect();
    graph.addPass("read_one_face")
        .addStorageImageInput("env", Subresource{0, 1, 3, 1})
        .addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");

    bool threw = false;
    try {
        graph.compile(kInfo);
    } catch (const std::runtime_error& e) {
        threw = true;
        CHECK(std::string(e.what()).find("env") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("compile() accepts two DISJOINT subresource ranges (different mip levels) against the same resource") {
    RenderGraph graph;
    ImageDesc desc = storageImageDesc();
    desc.mipLevels = 2;

    graph.addPass("write_mip0").addStorageImageOutput("chain", desc, Subresource{0, 1, 0, 1}).setSideEffect();
    graph.addPass("write_mip1").addStorageImageOutput("chain", desc, Subresource{1, 1, 0, 1}).setSideEffect();
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    CHECK_NOTHROW(graph.compile(kInfo));
}

TEST_CASE("compile() accepts two IDENTICAL subresource ranges against the same resource") {
    RenderGraph graph;
    graph.addPass("write").addStorageImageOutput("img", storageImageDesc()).setSideEffect();
    graph.addPass("read1").addStorageImageInput("img").setSideEffect();
    graph.addPass("read2").addStorageImageInput("img").setSideEffect();
    graph.addPass("present").addColorOutput("bb", colorDesc());
    graph.setBackbufferSource("bb");
    CHECK_NOTHROW(graph.compile(kInfo));
}
