#include <rx_debug_ui/overlay.h>

#include <rx_core/debug_checks.h>
#include <rx_core/log.h>
#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/frame_sync.h>

#include <volk.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <array>
#include <cstring>

namespace rx::debug_ui {

namespace {

// ---- vkQueueWaitIdle test-interception state [gate ruling #16 row 5,
// overlay.h's own detail::setQueueWaitIdleHookForTests comment] -----------
detail::QueueWaitIdleFn g_queueWaitIdleHookForTests = nullptr;

VkResult interceptedQueueWaitIdle(VkQueue queue) { return g_queueWaitIdleHookForTests(queue); }

// Loader function handed to ImGui_ImplVulkan_LoadFunctions() -- see
// third_party/CMakeLists.txt's own comment on the vendored `imgui` target's
// VK_NO_PROTOTYPES choice for why this project uses this path (rather than
// IMGUI_IMPL_VULKAN_USE_VOLK) at all. `userData` is the VkInstance passed
// at the call site below (create()). Every symbol resolves through the
// real `vkGetInstanceProcAddr` (itself a volk-loaded global -- this
// project's own loader strategy, rx_rhi_vk's PUBLIC VK_NO_PROTOTYPES
// define) -- Vulkan's own spec permits resolving device-level commands
// through the instance-level query too (a slightly less direct dispatch,
// irrelevant here: every symbol this backend loads is resolved exactly
// once, at init, never on a hot path), and this backend's own
// LoadDynamicRenderingFunctions() (imgui_impl_vulkan.cpp) already tries
// the CORE (non-KHR) "vkCmdBeginRendering"/"vkCmdEndRendering" names FIRST
// -- both real, core-promoted Vulkan 1.3 entry points on this project's
// own Vulkan 1.3 baseline, resolvable this way with no extension
// enablement needed at all -- exactly matching how rx_graph/executor.cpp's
// own vkCmdBeginRendering() calls already work.
//
// EXCEPT "vkQueueWaitIdle" when a test hook is installed
// (rx::debug_ui::detail::setQueueWaitIdleHookForTests) -- see overlay.h's
// own comment on why this loader function is the natural interception
// seam for the "at-most-once vkQueueWaitIdle" GPU test assertion [gate
// ruling #16 row 5].
PFN_vkVoidFunction imguiVulkanFunctionLoader(const char* functionName, void* userData) {
    if (g_queueWaitIdleHookForTests != nullptr && std::strcmp(functionName, "vkQueueWaitIdle") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&interceptedQueueWaitIdle);
    }
    VkInstance instance = static_cast<VkInstance>(userData);
    return vkGetInstanceProcAddr(instance, functionName);
}

// Wired into ImGui_ImplVulkan_InitInfo::CheckVkResultFn -- left null by
// default (a silent no-op, per imgui_impl_vulkan.cpp's own check_vk_result()),
// which would swallow a real Vulkan failure inside the vendored backend
// entirely. Logged, not fatal: matches this project's own "log, don't
// crash the caller" convention for a best-effort/degrade-gracefully path
// elsewhere in this codebase (e.g. rx::platform::Window's own SDL-call
// failure handling) -- a genuinely fatal Vulkan error surfaces anyway,
// downstream, as a real validation error or a subsequent call's own
// failure, which this project's GPU tests already gate on.
void checkVkResult(VkResult err) {
    if (err != VK_SUCCESS) {
        RX_LOG_ERROR("rx_debug_ui: a Vulkan call inside the vendored ImGui Vulkan backend failed: VkResult={}",
                     static_cast<int>(err));
    }
}

// See overlay.h's own comment on `colorFormat_`: ImGui_ImplVulkan_InitInfo's
// nested PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats
// is a POINTER the backend copies BY VALUE into its own persistent
// bd->VulkanInitInfo (imgui_impl_vulkan.cpp: `bd->VulkanInitInfo = *info;`,
// verified directly against the pinned tag) -- the POINTEE must therefore
// outlive the ImGui_ImplVulkan_Init() call itself, not just its duration.
// A function-local (stack) variable inside create() would dangle the
// moment create() returns; process-lifetime static storage sidesteps this
// entirely, consistent with this class's own documented "at most one live
// Overlay per process" contract (a second Overlay's create() call simply
// overwrites this same storage with ITS OWN colorFormat -- never a
// problem, since only one Overlay may be alive at a time regardless).
VkFormat g_pipelineColorFormatStorage = VK_FORMAT_UNDEFINED;

}  // namespace

namespace detail {
void setQueueWaitIdleHookForTests(QueueWaitIdleFn hook) { g_queueWaitIdleHookForTests = hook; }
}  // namespace detail

Overlay::Overlay(Overlay&& other) noexcept
    : device_(other.device_), descriptorPool_(other.descriptorPool_), colorFormat_(other.colorFormat_) {
    other.device_ = VK_NULL_HANDLE;
    other.descriptorPool_ = VK_NULL_HANDLE;
    other.colorFormat_ = VK_FORMAT_UNDEFINED;
}

Overlay& Overlay::operator=(Overlay&& other) noexcept {
    if (this != &other) {
        destroyAll();
        device_ = other.device_;
        descriptorPool_ = other.descriptorPool_;
        colorFormat_ = other.colorFormat_;
        other.device_ = VK_NULL_HANDLE;
        other.descriptorPool_ = VK_NULL_HANDLE;
        other.colorFormat_ = VK_FORMAT_UNDEFINED;
    }
    return *this;
}

Overlay::~Overlay() { destroyAll(); }

void Overlay::destroyAll() {
    if (device_ == VK_NULL_HANDLE) {
        return;  // default-constructed-by-move-from -- nothing owned, nothing to tear down.
    }
    // Order matches every upstream ImGui example: renderer backend, then
    // platform backend, then the context both were built against, then
    // this class's own resources the backends never took ownership of
    // (the descriptor pool -- see create()'s own comment: DescriptorPool
    // was supplied by us, not DescriptorPoolSize, so
    // ImGui_ImplVulkan_Shutdown() never destroys it itself).
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    colorFormat_ = VK_FORMAT_UNDEFINED;
}

std::optional<Overlay> Overlay::create(rx::rhi::Device& device, rx::platform::Window& window, VkFormat colorFormat) {
    RX_ASSERT_MAIN_THREAD("Overlay::create");

    // ---- own precondition checks, BEFORE calling into the vendored
    // backend [gate matrix row 2] -- see this class's own header comment:
    // the vendored backend's IM_ASSERT preconditions (e.g.
    // `IM_ASSERT(info->MinImageCount >= 2)`) compile to nothing in this
    // project's actual NDEBUG-defined RelWithDebInfo builds (both
    // presets), so relying on them alone would silently let a bad
    // configuration proceed instead of failing loudly. `colorFormat` is
    // the one caller-tunable input this class's own minimal
    // `create(Device&, Window&, format)` surface exposes at all (every
    // other ImGui_ImplVulkan_InitInfo field below is derived internally,
    // never caller-supplied) -- validating it is what makes "fail loudly,
    // not silently proceed" a real, testable property of THIS class's own
    // actual public surface, not merely something the matrix's cited
    // library-level example (an out-of-range MinImageCount) illustrates.
    if (colorFormat == VK_FORMAT_UNDEFINED) {
        RX_LOG_ERROR("rx_debug_ui: Overlay::create() called with VK_FORMAT_UNDEFINED colorFormat");
        return std::nullopt;
    }
    if (device.instance() == VK_NULL_HANDLE || device.physicalDevice() == VK_NULL_HANDLE ||
        device.device() == VK_NULL_HANDLE || device.graphicsQueue() == VK_NULL_HANDLE) {
        RX_LOG_ERROR("rx_debug_ui: Overlay::create() called with an incompletely-initialized Device");
        return std::nullopt;
    }

    // ---- own descriptor pool [gate ruling #16, BINDING: two typed pool-
    // size entries, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT] ----
    // SAMPLED_IMAGE sized to double the vendored backend's own stated
    // per-atlas minimum (IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE
    // == 8, imgui_impl_vulkan.h) -- modest headroom for HUD textures a
    // future sample registers beyond the font atlas alone (e.g. icon
    // atlases), not the bare minimum. SAMPLER stays at the exact minimum
    // (IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE == 2, "linear +
    // nearest") -- the backend itself creates exactly these two samplers,
    // never more, regardless of how many textures get registered.
    constexpr uint32_t kSampledImagePoolSize = 16;
    constexpr uint32_t kSamplerPoolSize = 2;
    std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSampledImagePoolSize},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kSamplerPoolSize},
    }};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = kSampledImagePoolSize + kSamplerPoolSize;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkResult poolResult = vkCreateDescriptorPool(device.device(), &poolInfo, nullptr, &descriptorPool);
    if (poolResult != VK_SUCCESS) {
        RX_LOG_ERROR("rx_debug_ui: vkCreateDescriptorPool failed: VkResult={}", static_cast<int>(poolResult));
        return std::nullopt;
    }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Middleware posture: never write an imgui.ini/imgui_log.txt file next
    // to whatever a host process's current working directory happens to
    // be -- HUD-layout persistence/logging is a host concern (a future
    // SDK surface could opt back in explicitly), not this module's own
    // default behavior.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    if (!ImGui_ImplSDL3_InitForVulkan(window.sdlWindow())) {
        RX_LOG_ERROR("rx_debug_ui: ImGui_ImplSDL3_InitForVulkan failed");
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(device.device(), descriptorPool, nullptr);
        return std::nullopt;
    }

    // [gate ruling #16 row 7, BINDING] Immediately after the SDL3 backend's
    // own init -- kills the backend's default AutoFirst gamepad-open race
    // against rx_platform::Window's own gamepad lifecycle (Task 20): zero
    // manually-provided gamepads, Manual mode, so this backend NEVER
    // itself calls SDL_OpenGamepad/SDL_CloseGamepad. Gamepad HUD
    // navigation (ImGuiConfigFlags_NavEnableGamepad) is deliberately NOT
    // enabled this phase as a direct consequence.
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);

    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, &imguiVulkanFunctionLoader,
                                         reinterpret_cast<void*>(device.instance()))) {
        RX_LOG_ERROR("rx_debug_ui: ImGui_ImplVulkan_LoadFunctions failed");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(device.device(), descriptorPool, nullptr);
        return std::nullopt;
    }

    g_pipelineColorFormatStorage = colorFormat;

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = device.instance();
    initInfo.PhysicalDevice = device.physicalDevice();
    initInfo.Device = device.device();
    initInfo.QueueFamily = device.graphicsQueueFamily();
    initInfo.Queue = device.graphicsQueue();
    initInfo.DescriptorPool = descriptorPool;  // own pool, NOT the DescriptorPoolSize convenience path -- see class comment.
    initInfo.DescriptorPoolSize = 0;
    initInfo.MinImageCount = rx::rhi::FrameSync::kFramesInFlight;
    initInfo.ImageCount = rx::rhi::FrameSync::kFramesInFlight;
    initInfo.PipelineCache = VK_NULL_HANDLE;
    initInfo.MinAllocationSize = 1024 * 1024;  // the header's own documented "satisfy zealous best-practices validation" default.
    initInfo.CheckVkResultFn = &checkVkResult;
    initInfo.UseDynamicRendering = true;
    // [gate ruling #16, BINDING: 2025-09-26 breaking change] the NESTED
    // PipelineInfoMain.PipelineRenderingCreateInfo -- the flat top-level
    // field the plan's pseudocode/an earlier research doc assumed no
    // longer exists at this pinned tag (imgui_impl_vulkan.h:120-123 keeps
    // it only as a commented-out migration breadcrumb, verified directly).
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_pipelineColorFormatStorage;

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        // Unreachable in practice at this pinned tag (ImGui_ImplVulkan_Init()
        // always returns true barring an IM_ASSERT trip, which compiles to
        // nothing in this project's builds -- see class comment) -- handled
        // anyway per "both return bool and must be checked" [gate matrix
        // row 2].
        RX_LOG_ERROR("rx_debug_ui: ImGui_ImplVulkan_Init failed");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(device.device(), descriptorPool, nullptr);
        return std::nullopt;
    }

    // ---- force the font atlas's one-time GPU upload [gate ruling #16,
    // FONT-UPLOAD RULING, BINDING; see overlay.h's own FONT/TEXTURE UPLOAD
    // section for the full rationale] -----------------------------------
    // The vendored backend uploads textures LAZILY, inside
    // ImGui_ImplVulkan_RenderDrawData() itself (via ImGui_ImplVulkan_
    // UpdateTexture(), iterating ImGui::GetPlatformIO().Textures[]) -- so
    // the only way to force it to happen HERE, at construction, rather
    // than on whatever frame the caller's own first ImGui::Render() call
    // happens to land, is to run one full, real NewFrame -> build a UI ->
    // Render -> RenderDrawData cycle ourselves, right now, synchronously.
    // A single glyph-referencing draw command (a plain text label, in an
    // otherwise invisible/no-input/no-background window) guarantees the
    // font atlas is unambiguously part of THIS frame's draw data. Uses a
    // throwaway, real one-shot command buffer (rx::rhi::CommandContext::
    // runOnce(), the SAME synchronous setup/test-utility this codebase
    // already uses everywhere a one-off submit-and-wait is needed) --
    // whatever draw commands ImGui_ImplVulkan_RenderDrawData() records
    // into it are harmless (this priming frame is never presented; its
    // whole purpose is triggering the upload path, not producing visible
    // output), but a REAL, validly-begun command buffer is still required
    // (an unbegun/null one would fail validation the moment
    // ImGui_ImplVulkan_RenderDrawData() records a vkCmdBindPipeline into
    // it, if any glyph is actually drawn -- which this priming frame
    // deliberately ensures).
    auto cmdCtx =
        rx::rhi::CommandContext::create(device.device(), device.graphicsQueue(), device.graphicsQueueFamily());
    if (!cmdCtx.has_value()) {
        RX_LOG_ERROR("rx_debug_ui: CommandContext::create failed while forcing the font atlas upload");
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(device.device(), descriptorPool, nullptr);
        return std::nullopt;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("##rx_debug_ui_font_prime", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                      ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("rx_debug_ui");
    ImGui::End();
    ImGui::Render();
    cmdCtx->runOnce([](VkCommandBuffer cmd) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); });

    return Overlay(device.device(), descriptorPool, colorFormat);
}

void Overlay::processEvent(const SDL_Event& event) {
    RX_ASSERT_MAIN_THREAD("Overlay::processEvent");
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void Overlay::beginFrame() {
    RX_ASSERT_MAIN_THREAD("Overlay::beginFrame");
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void Overlay::addPass(rx::graph::RenderGraph& graph, std::string_view targetName) {
    RX_ASSERT_MAIN_THREAD("Overlay::addPass");

    rx::graph::AttachmentDesc desc;
    desc.format = colorFormat_;
    // sizeClass/width/height/samples left at AttachmentDesc's own defaults
    // (SwapchainRelative, 1.0x1.0, 1 sample) -- see colorFormat_'s own
    // header comment for why this is the correct, and only supported,
    // choice for this class.

    // LOAD-not-CLEAR [gate ruling #16, named criterion]: this is NOT a
    // parameter here -- rx_graph::Executor derives the real
    // VkAttachmentLoadOp automatically from whether `targetName` was
    // already written by an earlier pass THIS frame (executor.cpp: `info.
    // loadOp = attachmentEverWritten[physIdx] ? VK_ATTACHMENT_LOAD_OP_LOAD
    // : VK_ATTACHMENT_LOAD_OP_CLEAR`). Declaring THIS pass (below) after
    // the caller's own scene pass(es) against the SAME `targetName` is
    // what makes that resolve to LOAD -- see this method's own header
    // comment (overlay.h) for the full contract.
    graph.addPass("imgui_overlay")
        .addColorOutput(targetName, desc)
        .setSideEffect()
        .setExecute([](rx::graph::PassContext& ctx) {
            ImGui::Render();
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), ctx.cmd);
        });
}

}  // namespace rx::debug_ui
