// Task 1 brief's exact test expectations (culling/ordering/lifetimes/
// usage-union/diamond/errors), plus a few extra cases exercising the rest
// of the Pass/RenderGraph/CompiledGraph surface (accessors, reset(),
// swapchain-relative size resolution, compute-vs-graphics storage buffer
// stage classification) that the six required cases don't otherwise touch.
#include <doctest/doctest.h>
#include <rx_graph/render_graph.h>

#include <stdexcept>
#include <string>

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
