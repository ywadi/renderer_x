// GPU smoke test for rx::debug_ui::Overlay [Phase 4 Stage 2 Task 21, spec
// D20, gate matrix-issue16 as amended by gate/rulings-2026-08-18.md #16].
// Same windowed-but-headless fixture pattern as rx_graph/tests/
// test_execute_gpu.cpp/rx_rhi_vk's own GPU tests -- a real, validated
// rx::rhi::Device via rx::platform::Window (needs a real VkSurfaceKHR even
// though nothing here ever presents to it).
//
// Coverage, mapped to the gate matrix's own rows:
//   - row 2:  Overlay::create() fails loudly (returns std::nullopt, zero
//             validation errors) on a deliberately-invalid colorFormat.
//   - row 4:  the descriptor pool is exercised with the font atlas PLUS a
//             second, explicitly-registered texture (ImGui_ImplVulkan_
//             AddTexture) -- pool creation with the two-typed-entry shape
//             succeeds and produces zero validation errors under that
//             load.
//   - row 5/FONT-UPLOAD RULING: `Overlay::create()`'s forced init-time
//     upload trips the vendored backend's internal vkQueueWaitIdle exactly
//     once; a fixed N-frame steady-state run (no further texture churn)
//     adds zero more.
//   - row 6: a synthetic mouse click over a known HUD widget position sets
//     `ImGui::GetIO().WantCaptureMouse` (the device-free HALF of that row's
//     criterion this GPU test CAN exercise -- the "camera stops moving"
//     half is explicitly routed to #15/sample 09 as MANUAL_VERIFICATION,
//     per the matrix's own text).
//   - row 9/11: the overlay pass renders as a real declared rx_graph pass
//     (side-effect, addColorOutput, setExecute) against a known non-black
//     pattern already written by an earlier pass THIS frame -- readback
//     proves LOAD (not CLEAR): the pattern survives OUTSIDE the HUD's own
//     drawn pixels, and the HUD's own content is visible INSIDE them.
//   - zero unfiltered Vulkan validation errors, every case.
#include <doctest/doctest.h>
#include <rx_debug_ui/overlay.h>

#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_task/scheduler.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

using namespace rx::graph;

namespace {

constexpr uint32_t kExtentPx = 128;
constexpr VkExtent2D kExtent{kExtentPx, kExtentPx};
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

// Same skip-guarded windowed-device fixture pattern as rx_graph/tests/
// test_execute_gpu.cpp's own GpuFixture.
struct GpuFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    std::unique_ptr<rx::task::Scheduler> scheduler;
    std::unique_ptr<Executor> executor;
};

std::optional<GpuFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping rx_debug_ui GPU test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping rx_debug_ui GPU "
                 "test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    auto allocator = rx::rhi::Allocator::create(*context, *device);
    REQUIRE(allocator.has_value());

    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);

    auto executor = Executor::create(*device, *scheduler);
    REQUIRE(executor != nullptr);

    return GpuFixture{std::move(*window),    std::move(*context),  std::move(*device),
                       std::move(*allocator), std::move(scheduler), std::move(executor)};
}

// --- Offscreen "backbuffer" -- same non-RAII, manual-teardown pattern as
// rx_graph/tests/test_execute_gpu.cpp's own OffscreenImage. -----------------
struct OffscreenImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

std::optional<OffscreenImage> createImage(VkDevice device, VkPhysicalDevice physicalDevice, VkFormat format,
                                           VkExtent2D extent, VkImageUsageFlags usage) {
    OffscreenImage result;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &result.image) != VK_SUCCESS) {
        return std::nullopt;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, result.image, &memReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1U << i)) != 0U &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            memoryTypeIndex = i;
            break;
        }
    }
    if (memoryTypeIndex == UINT32_MAX) {
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &result.memory) != VK_SUCCESS) {
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }
    if (vkBindImageMemory(device, result.image, result.memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, result.memory, nullptr);
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = result.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &result.view) != VK_SUCCESS) {
        vkFreeMemory(device, result.memory, nullptr);
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }

    return result;
}

void destroyImage(VkDevice device, OffscreenImage& img) {
    if (img.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, img.view, nullptr);
    }
    if (img.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, img.image, nullptr);
    }
    if (img.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, img.memory, nullptr);
    }
    img = OffscreenImage{};
}

// A known, non-black, non-white solid pattern -- the "scene" content the
// HUD must be drawn LOAD-not-CLEAR on top of [gate matrix row 9]. Uses
// vkCmdClearAttachments (legal INSIDE an active dynamic-rendering scope,
// unlike vkCmdClearColorImage) rather than a real pipeline -- this pass's
// entire job is producing a known, verifiable color, not exercising a
// draw call.
constexpr VkClearColorValue kPatternColor{{0.2F, 0.6F, 0.9F, 1.0F}};

void addPatternPass(RenderGraph& graph, std::string_view targetName) {
    AttachmentDesc desc;
    desc.format = kFormat;
    graph.addPass("pattern").addColorOutput(targetName, desc).setExecute([](PassContext& ctx) {
        VkClearAttachment clearAttachment{};
        clearAttachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clearAttachment.colorAttachment = 0;
        clearAttachment.clearValue.color = kPatternColor;
        VkClearRect clearRect{};
        clearRect.rect = VkRect2D{{0, 0}, ctx.renderArea};
        clearRect.layerCount = 1;
        vkCmdClearAttachments(ctx.cmd, 1, &clearAttachment, 1, &clearRect);
    });
}

std::array<uint8_t, 4> pixelAt(const uint8_t* pixels, uint32_t width, uint32_t x, uint32_t y) {
    std::array<uint8_t, 4> px{};
    std::memcpy(px.data(), pixels + (static_cast<size_t>(y) * width + x) * 4, px.size());
    return px;
}

bool approxEqual(const std::array<uint8_t, 4>& a, const std::array<uint8_t, 4>& b, int tolerance) {
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])) > tolerance) {
            return false;
        }
    }
    return true;
}

// Renders `graph` (already compiled + realized against `executor`) into
// `target` and reads the whole thing back to a host-visible buffer --
// same readback machinery as rx_graph/tests/test_execute_gpu.cpp:754-768.
std::vector<uint8_t> renderAndReadBack(rx::rhi::Allocator& allocator, Executor& executor, RenderGraph& graph,
                                        rx::rhi::CommandContext& cmdCtx, const OffscreenImage& target) {
    cmdCtx.runOnce(
        [&](VkCommandBuffer cmd) { executor.execute(graph, cmd, target.image, target.view, kExtent); });

    const VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(kExtent.width) * kExtent.height * 4;
    auto readback = allocator.createHostVisibleBuffer(pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());

    cmdCtx.runOnce([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {kExtent.width, kExtent.height, 1};
        vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1,
                                &region);
    });
    readback->invalidate();

    std::vector<uint8_t> pixels(static_cast<size_t>(pixelBytes));
    std::memcpy(pixels.data(), readback->mappedData(), pixels.size());
    return pixels;
}

CompileInfo makeCompileInfo() {
    CompileInfo info;
    info.swapchainWidth = kExtent.width;
    info.swapchainHeight = kExtent.height;
    info.swapchainFormat = kFormat;
    // Offscreen, never-presented target -- same Task 3 ambiguity
    // resolution #1 reasoning as test_execute_gpu.cpp: this test's own
    // readback needs TRANSFER_SRC_OPTIMAL, not PRESENT_SRC_KHR.
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return info;
}

// A 1x1 SAMPLED texture, already in SHADER_READ_ONLY_OPTIMAL -- content is
// irrelevant (never sampled by anything this test draws); its only job is
// giving ImGui_ImplVulkan_AddTexture() a real VkImageView to register,
// exercising the descriptor pool with a texture beyond the font atlas
// [gate matrix row 4].
std::optional<OffscreenImage> createDummySampledTexture(rx::rhi::Device& device, rx::rhi::CommandContext& cmdCtx) {
    auto tex = createImage(device.device(), device.physicalDevice(), kFormat, VkExtent2D{1, 1},
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!tex.has_value()) {
        return std::nullopt;
    }
    cmdCtx.runOnce([&](VkCommandBuffer cmd) {
        rx::rhi::transitionImage(cmd, tex->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    return tex;
}

}  // namespace

TEST_CASE("Overlay::create() fails loudly (returns nullopt, zero validation errors) on a deliberately-invalid "
          "colorFormat [gate matrix row 2]") {
    auto fixture = makeFixture("rx_debug_ui_invalid_format");
    if (!fixture.has_value()) {
        return;
    }

    auto overlay = rx::debug_ui::Overlay::create(fixture->device, fixture->window, VK_FORMAT_UNDEFINED);
    CHECK_FALSE(overlay.has_value());

    vkDeviceWaitIdle(fixture->device.device());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Overlay renders as a declared rx_graph pass: LOAD-not-CLEAR preserves an earlier pass's pattern, the "
          "HUD's own content is visible, the descriptor pool serves the font atlas plus a second registered "
          "texture, and zero validation errors [gate matrix rows 4, 9, 11]") {
    auto fixture = makeFixture("rx_debug_ui_render_smoke");
    if (!fixture.has_value()) {
        return;
    }

    auto offscreen = createImage(fixture->device.device(), fixture->device.physicalDevice(), kFormat, kExtent,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(offscreen.has_value());

    auto cmdCtx = rx::rhi::CommandContext::create(fixture->device.device(), fixture->device.graphicsQueue(),
                                                    fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    auto overlay = rx::debug_ui::Overlay::create(fixture->device, fixture->window, kFormat);
    REQUIRE(overlay.has_value());

    auto dummyTexture = createDummySampledTexture(fixture->device, *cmdCtx);
    REQUIRE(dummyTexture.has_value());
    VkDescriptorSet dummyTextureId = ImGui_ImplVulkan_AddTexture(dummyTexture->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    REQUIRE(dummyTextureId != VK_NULL_HANDLE);

    // Deterministic layout: no window rounding/AA fringe near the sampled
    // probe pixels below.
    ImGui::GetStyle().WindowRounding = 0.0F;
    ImGui::GetStyle().WindowBorderSize = 0.0F;

    overlay->beginFrame();
    ImGui::SetNextWindowPos(ImVec2(10.0F, 10.0F));
    ImGui::SetNextWindowSize(ImVec2(60.0F, 40.0F));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.0F, 1.0F, 1.0F, 1.0F));
    ImGui::Begin("hud", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoResize);
    ImGui::Button("X", ImVec2(30.0F, 20.0F));
    ImGui::End();
    ImGui::PopStyleColor();

    RenderGraph graph;
    addPatternPass(graph, "bb");
    overlay->addPass(graph, "bb");
    graph.setBackbufferSource("bb");
    CompileInfo info = makeCompileInfo();
    graph.compile(info);
    fixture->executor->realize(graph);

    std::vector<uint8_t> pixels =
        renderAndReadBack(fixture->allocator, *fixture->executor, graph, *cmdCtx, *offscreen);

    // OUTSIDE the (10,10)-(70,50) HUD window -- LOAD, not CLEAR, must have
    // preserved the pattern pass's own solid color.
    const std::array<uint8_t, 4> outside = pixelAt(pixels.data(), kExtent.width, 100, 100);
    const std::array<uint8_t, 4> expectedPattern{
        static_cast<uint8_t>(std::lround(kPatternColor.float32[0] * 255.0)),
        static_cast<uint8_t>(std::lround(kPatternColor.float32[1] * 255.0)),
        static_cast<uint8_t>(std::lround(kPatternColor.float32[2] * 255.0)),
        static_cast<uint8_t>(std::lround(kPatternColor.float32[3] * 255.0))};
    CHECK(approxEqual(outside, expectedPattern, 8));

    // INSIDE the HUD window's own body -- the overlay pass genuinely
    // rendered its white background over the pattern here.
    const std::array<uint8_t, 4> inside = pixelAt(pixels.data(), kExtent.width, 30, 25);
    const std::array<uint8_t, 4> expectedWhite{255, 255, 255, 255};
    CHECK(approxEqual(inside, expectedWhite, 8));
    // ... and the same pixel must NOT be the pattern color -- the two
    // regions are genuinely distinct, not a coincidental readback bug.
    CHECK_FALSE(approxEqual(inside, expectedPattern, 8));

    ImGui_ImplVulkan_RemoveTexture(dummyTextureId);

    vkDeviceWaitIdle(fixture->device.device());
    overlay.reset();
    destroyImage(fixture->device.device(), *dummyTexture);
    destroyImage(fixture->device.device(), *offscreen);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("A synthetic mouse click over a known HUD widget position sets ImGui::GetIO().WantCaptureMouse [gate "
          "matrix row 6, the device-free-exercisable half]") {
    auto fixture = makeFixture("rx_debug_ui_want_capture_mouse");
    if (!fixture.has_value()) {
        return;
    }

    auto offscreen = createImage(fixture->device.device(), fixture->device.physicalDevice(), kFormat, kExtent,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(offscreen.has_value());

    auto cmdCtx = rx::rhi::CommandContext::create(fixture->device.device(), fixture->device.graphicsQueue(),
                                                    fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    auto overlay = rx::debug_ui::Overlay::create(fixture->device, fixture->window, kFormat);
    REQUIRE(overlay.has_value());

    ImGui::GetStyle().WindowRounding = 0.0F;

    // Frame 1: establish a real window's layout at a KNOWN position/size
    // and fully render it -- ImGui's own hover/capture test
    // (UpdateHoveredWindowAndCaptureFlags, run at the START of the NEXT
    // NewFrame()) compares the mouse position against the PREVIOUS
    // frame's committed window layout, per ImGui's own documented,
    // well-established one-frame-lagged design -- not this frame's.
    overlay->beginFrame();
    ImGui::SetNextWindowPos(ImVec2(10.0F, 10.0F));
    ImGui::SetNextWindowSize(ImVec2(60.0F, 40.0F));
    ImGui::Begin("hud", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoResize);
    ImGui::Button("X", ImVec2(30.0F, 20.0F));
    ImGui::End();

    RenderGraph graph1;
    addPatternPass(graph1, "bb");
    overlay->addPass(graph1, "bb");
    graph1.setBackbufferSource("bb");
    CompileInfo info = makeCompileInfo();
    graph1.compile(info);
    fixture->executor->realize(graph1);
    cmdCtx->runOnce(
        [&](VkCommandBuffer cmd) { fixture->executor->execute(graph1, cmd, offscreen->image, offscreen->view, kExtent); });

    // Baseline: before any click has been processed, the mouse has never
    // moved into the window -- WantCaptureMouse must be false.
    CHECK_FALSE(ImGui::GetIO().WantCaptureMouse);

    // A synthetic left-button-down at (20, 20) -- comfortably inside the
    // (10,10)-(70,50) window frame 1 just committed -- fed through
    // Overlay::processEvent() exactly like Window::pumpEvents()'s
    // preDispatch seam would (gate ruling #16).
    const SDL_WindowID windowId = SDL_GetWindowID(fixture->window.sdlWindow());
    SDL_Event motionEvent{};
    motionEvent.motion.type = SDL_EVENT_MOUSE_MOTION;
    motionEvent.motion.windowID = windowId;
    motionEvent.motion.x = 20.0F;
    motionEvent.motion.y = 20.0F;
    overlay->processEvent(motionEvent);

    SDL_Event clickEvent{};
    clickEvent.button.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    clickEvent.button.windowID = windowId;
    clickEvent.button.button = SDL_BUTTON_LEFT;
    clickEvent.button.down = true;
    clickEvent.button.x = 20.0F;
    clickEvent.button.y = 20.0F;
    overlay->processEvent(clickEvent);

    // Frame 2: NewFrame() recomputes hover/capture against frame 1's
    // window layout + the just-processed mouse position.
    overlay->beginFrame();
    CHECK(ImGui::GetIO().WantCaptureMouse);

    // Close frame 2 out cleanly (Begin the same window again, matching
    // ImGui's own "call Begin/End every frame a window should stay alive"
    // contract) rather than leaving a NewFrame() dangling with no Render().
    ImGui::SetNextWindowPos(ImVec2(10.0F, 10.0F));
    ImGui::SetNextWindowSize(ImVec2(60.0F, 40.0F));
    ImGui::Begin("hud", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoResize);
    ImGui::Button("X", ImVec2(30.0F, 20.0F));
    ImGui::End();

    RenderGraph graph2;
    addPatternPass(graph2, "bb");
    overlay->addPass(graph2, "bb");
    graph2.setBackbufferSource("bb");
    graph2.compile(info);
    fixture->executor->realize(graph2);
    cmdCtx->runOnce(
        [&](VkCommandBuffer cmd) { fixture->executor->execute(graph2, cmd, offscreen->image, offscreen->view, kExtent); });

    vkDeviceWaitIdle(fixture->device.device());
    overlay.reset();
    destroyImage(fixture->device.device(), *offscreen);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

namespace {
// [gate ruling #16 row 5, FONT-UPLOAD RULING] Test-only interception hook
// -- see rx_debug_ui/overlay.h's own detail::setQueueWaitIdleHookForTests
// comment for the full contract. A plain, captureless free function
// (required: the hook type is a bare function pointer, not std::function)
// forwarding to the REAL global `vkQueueWaitIdle` (this project's own
// volk-loaded symbol, already visible in this TU via rx_rhi_vk/command.h's
// own <volk.h> include) after recording the call -- never skips the real
// wait (skipping it would leave the font atlas upload genuinely
// incomplete, corrupting later reads of it, not merely a test shortcut).
std::atomic<uint32_t>* g_queueWaitIdleCounterForTest = nullptr;

VkResult countingQueueWaitIdleHook(VkQueue queue) {
    if (g_queueWaitIdleCounterForTest != nullptr) {
        g_queueWaitIdleCounterForTest->fetch_add(1, std::memory_order_relaxed);
    }
    return vkQueueWaitIdle(queue);
}

// RAII guard restoring the production default (nullptr -- real,
// unintercepted vkQueueWaitIdle resolution) on scope exit regardless of
// how the TEST_CASE exits -- same convention as rx_material/tests/
// test_material_system.cpp's own BindInstanceGuardHookScope.
struct QueueWaitIdleHookGuard {
    ~QueueWaitIdleHookGuard() {
        rx::debug_ui::detail::setQueueWaitIdleHookForTests(nullptr);
        g_queueWaitIdleCounterForTest = nullptr;
    }
};
}  // namespace

TEST_CASE("Overlay's forced init-time font upload trips the vendored backend's internal vkQueueWaitIdle exactly "
          "once, and zero additional times across a fixed N-frame steady-state run with no further texture churn "
          "[gate ruling #16, FONT-UPLOAD RULING -- guards the D25/D24 documented exception's bound]") {
    auto fixture = makeFixture("rx_debug_ui_queue_wait_idle_bound");
    if (!fixture.has_value()) {
        return;
    }

    auto offscreen = createImage(fixture->device.device(), fixture->device.physicalDevice(), kFormat, kExtent,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(offscreen.has_value());

    auto cmdCtx = rx::rhi::CommandContext::create(fixture->device.device(), fixture->device.graphicsQueue(),
                                                    fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    std::atomic<uint32_t> waitIdleCount{0};
    g_queueWaitIdleCounterForTest = &waitIdleCount;
    // Installed BEFORE create() so create()'s own forced-upload call is
    // counted -- the guard is declared BEFORE `overlay` below so it is
    // destroyed AFTER `overlay` (reverse declaration order), keeping the
    // hook installed through Overlay's own teardown too.
    rx::debug_ui::detail::setQueueWaitIdleHookForTests(&countingQueueWaitIdleHook);
    QueueWaitIdleHookGuard hookGuard;

    auto overlay = rx::debug_ui::Overlay::create(fixture->device, fixture->window, kFormat);
    REQUIRE(overlay.has_value());
    CHECK(waitIdleCount.load(std::memory_order_relaxed) == 1);

    // DELIBERATELY IDENTICAL content on every iteration, reusing ONLY
    // glyphs create()'s own priming frame already packed -- see the two
    // findings below, both reproduced directly this task and both real,
    // correct-per-upstream behavior (not bugs in Overlay): ImGui v1.92.x's
    // dynamic font atlas (ImGuiBackendFlags_RendererHasTextures) packs
    // GLYPHS lazily, on first use, not the whole font upfront, and marks
    // the atlas ImTextureStatus_WantUpdates -- re-triggering
    // UpdateTexture()'s own vkQueueWaitIdle -- the MOMENT any new
    // character is drawn for the first time, wherever that happens:
    //   1. A first attempt used "frame %d" (a new digit -- '1'/'2'/'3'/'4'
    //      -- first appearing on a DIFFERENT steady-state iteration each
    //      time) -- observed 6 total waitIdle calls, one per newly-seen
    //      digit, not just the one from create().
    //   2. A second attempt fixed the text but left the window's title
    //      bar enabled (no ImGuiWindowFlags_NoDecoration) -- "steady" is
    //      NOT a substring of the priming frame's own "rx_debug_ui" text,
    //      so its title bar's OWN glyphs ('s','t','a' -- not otherwise
    //      used) were new on the FIRST steady-state frame only --
    //      observed exactly 2 total (1 + 1), stable afterward, never 3+:
    //      this by itself already proved the acceptance criterion's real
    //      concern (no ONGOING per-frame churn) but was strictly more
    //      than the "== 1" this test asserts, for a reason unrelated to
    //      Overlay itself.
    // NoDecoration below (hiding the title bar entirely) plus reusing the
    // exact "rx_debug_ui" text is what makes every steady-state frame
    // touch ZERO characters the priming frame did not already pack.
    constexpr int kSteadyStateFrames = 5;
    for (int frame = 0; frame < kSteadyStateFrames; ++frame) {
        overlay->beginFrame();
        ImGui::Begin("##steady", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                          ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("rx_debug_ui");
        ImGui::End();
        (void)frame;

        RenderGraph graph;
        addPatternPass(graph, "bb");
        overlay->addPass(graph, "bb");
        graph.setBackbufferSource("bb");
        CompileInfo info = makeCompileInfo();
        graph.compile(info);
        fixture->executor->realize(graph);
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, kExtent);
        });
    }

    // The bound this whole test exists to guard: steady-state frames with
    // no NEW texture registered/updated must add ZERO further
    // vkQueueWaitIdle calls -- still exactly the one from create()'s own
    // forced upload.
    CHECK(waitIdleCount.load(std::memory_order_relaxed) == 1);

    vkDeviceWaitIdle(fixture->device.device());
    overlay.reset();
    destroyImage(fixture->device.device(), *offscreen);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}
