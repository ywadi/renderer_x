#include <rx_rhi_vk/context.h>
#include <rx_core/log.h>
#include <VkBootstrap.h>
#include <memory>
#include <string_view>

namespace rx::rhi {

namespace {

// Known false positive, narrowly scoped: vk-bootstrap's InstanceBuilder
// unconditionally enables VK_KHR_portability_enumeration (the extension and
// the corresponding VkInstanceCreateInfo::flags bit) whenever the Vulkan
// *loader* reports it available, with no InstanceBuilder API to opt out
// (checked directly against vk-bootstrap's header: only
// PhysicalDeviceSelector::disable_portability_subset() exists, a different,
// device-level knob -- nothing at the instance level). On a host whose
// installed VK_LAYER_KHRONOS_validation predates that extension (verified:
// this machine's apt-packaged layer is 1.3.204, with no newer apt candidate
// available), the layer doesn't recognize it and reports two messages for
// this one root cause:
//   1. a WARNING that the extension "is not supported by this layer"
//   2. an ERROR misreporting VkInstanceCreateInfo::flags as invalid
//      (VUID-VkInstanceCreateInfo-flags-zerobitmask), because the layer
//      doesn't know the extension defines a legal nonzero flag bit
// Both are matched on their distinctive, specific text/VUID -- not a broad
// "ignore anything portability-related" -- so a genuine, different
// validation failure is never silently swallowed. (VUID_Undefined, the
// warning's own "VUID", is a generic Khronos placeholder used by many
// unrelated informational messages, so it is deliberately NOT used as a
// match key on its own; the extension name plus its exact phrase is used
// instead.)
bool isKnownPortabilityEnumerationLayerBug(const char* message) {
    if (message == nullptr) {
        return false;
    }
    const std::string_view msg(message);
    if (msg.find("VUID-VkInstanceCreateInfo-flags-zerobitmask") != std::string_view::npos) {
        return true;
    }
    return msg.find("VK_KHR_portability_enumeration") != std::string_view::npos &&
           msg.find("is not supported by this layer") != std::string_view::npos;
}

// Second known false positive, same root cause (this machine's apt-packaged
// validation layer, 1.3.204.1, bundles a SPIRV-Tools build older than a
// legitimate upstream addition it doesn't recognize) but a different SPIR-V
// module check: SPIR-V's SourceLanguage enum gained `Slang = 11` when Slang
// became an officially registered Khronos SPIR-V source language -- verified
// directly against a current SPIRV-Headers spirv/unified1/spirv.h, which
// defines `SpvSourceLanguageSlang = 11`. slangc (the pinned 2026.14.1
// prebuilt consumed by shaders/CMakeLists.txt) always emits `OpSource Slang
// 1` into every module it produces, unconditionally: verified by recompiling
// samples/01_triangle's triangle.vert.slang with `-g0` (strip debug info)
// and disassembling the result via spirv-dis -- the OpSource line is
// unchanged, confirming this is base module-provenance metadata, not
// optional embedded source text `-g`/`-debug-info-include-source` could
// suppress. It has zero effect on module semantics or execution; the
// installed layer's OpSource operand-range check simply doesn't know enum
// value 11 yet and rejects any module carrying it. Matched narrowly on the
// check's own distinctive message text plus its "UNASSIGNED" category
// (SPIRV-Tools' own internal check, not a formal Vulkan spec VUID) so a
// genuinely different "module not valid" failure is never silently
// swallowed.
bool isKnownUnrecognizedSlangSourceLanguageBug(const char* message) {
    if (message == nullptr) {
        return false;
    }
    const std::string_view msg(message);
    return msg.find("UNASSIGNED-CoreValidation-Shader-InconsistentSpirv") != std::string_view::npos &&
           msg.find("Invalid source language operand: 11") != std::string_view::npos;
}

// Third known false positive, this repo's own coordinator addition
// (enabling VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT)
// surfaced it: this machine's apt-packaged validation layer (1.3.204.1,
// same layer the two guards above already document as stale) misclassifies
// a `Texture2D`+`SamplerState` (SEPARATE image/sampler descriptors, never
// VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER -- exactly the bindless
// convention every sample in this repo uses, see rx_rhi_vk/bindless.h)
// `.Sample()` read as SYNC_*_SHADER_STORAGE_READ internally, then hazards
// that misclassified read against whatever the tracked `write_barriers`
// value is for the layout-transition barrier that already made the data
// visible -- two variants observed empirically, differing only in that
// trailing field depending on what the resource's last real write was:
//   - `write_barriers: SYNC_FRAGMENT_SHADER_SHADER_SAMPLED_READ` (the
//     layer's OWN correct derivation for the transition barrier itself,
//     disagreeing with its own STORAGE_READ misclassification of the
//     read) -- src/rx_graph/tests/test_execute_gpu.cpp's "invert" case,
//     and samples/05_multipass's shadow-map read.
//   - `write_barriers: SYNC_COLOR_ATTACHMENT_OUTPUT_COLOR_ATTACHMENT_WRITE`
//     (the resource's last color-attachment write) --
//     samples/05_multipass's HDR-texture read (written by the forward
//     pass, sampled by the tonemap pass).
// Both share the same root cause and the same four other stable fields
// matched below; only `write_barriers`' value is deliberately NOT part of
// the match (it varies with what the resource's last write happened to
// be, not with whether this is the same underlying bug). Reproduces
// reliably for a texture written earlier in the SAME command buffer (a
// layout-transition barrier immediately followed by a bindless-indexed
// sampled read of that same image, with no intervening submission/fence-
// wait in between) -- exactly the shape rx_graph's Executor produces for a
// graph pass that reads another pass's output within one execute() call.
// Never reproduces for a texture whose write and read are separated by a
// real submission boundary (e.g. every existing sample's Uploader-
// populated bindless texture, read only in a later frame) -- consistent
// with an intra-command-buffer barrier-tracking bug specific to this
// access shape, not a blanket "bindless reads are unchecked" gap.
//
// VERIFIED, not assumed, for BOTH variants: each reproduces under the
// apt-packaged 1.3.204.1 layer and reports ZERO hazards, byte-for-byte the
// same shader/graph/barriers, against a substantially newer
// VK_LAYER_KHRONOS_validation build (api_version 1.4.357) run via
// VK_LAYER_PATH pointed at that build instead -- i.e. an actively-
// maintained implementation of this same validation feature agrees both
// accesses are correctly synchronized. Matched on the full combination of
// this message's OTHER stable substrings (the hazard ID, the descriptor
// type, the misclassified usage, and the barrier command that (correctly)
// already covered it) -- deliberately NOT on "SYNC-HAZARD-READ_AFTER_WRITE"
// alone, which is the generic ID any unrelated real read-after-write
// hazard would also carry.
bool isKnownSyncValidationSeparateSamplerMisclassification(const char* message) {
    if (message == nullptr) {
        return false;
    }
    const std::string_view msg(message);
    return msg.find("SYNC-HAZARD-READ_AFTER_WRITE") != std::string_view::npos &&
           msg.find("type: VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE") != std::string_view::npos &&
           msg.find("usage: SYNC_FRAGMENT_SHADER_SHADER_STORAGE_READ") != std::string_view::npos &&
           msg.find("prior_usage: SYNC_IMAGE_LAYOUT_TRANSITION") != std::string_view::npos &&
           msg.find("command: vkCmdPipelineBarrier2") != std::string_view::npos;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* userData) {
    auto* errorCount = static_cast<int*>(userData);
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (isKnownPortabilityEnumerationLayerBug(data->pMessage)) {
            RX_LOG_WARN("[vulkan validation] (known false positive: validation layer predates VK_KHR_portability_enumeration) {}", data->pMessage);
        } else if (isKnownUnrecognizedSlangSourceLanguageBug(data->pMessage)) {
            RX_LOG_WARN("[vulkan validation] (known false positive: validation layer predates SPIR-V SourceLanguage=Slang) {}", data->pMessage);
        } else if (isKnownSyncValidationSeparateSamplerMisclassification(data->pMessage)) {
            RX_LOG_WARN("[vulkan validation] (known false positive: validation layer's sync validation misclassifies a separate-sampler Texture2D.Sample() read; verified clean against a newer layer build) {}", data->pMessage);
        } else {
            RX_LOG_ERROR("[vulkan validation] {}", data->pMessage);
            (*errorCount)++;
        }
    } else {
        RX_LOG_INFO("[vulkan validation] {}", data->pMessage);
    }
    return VK_FALSE;
}

}  // namespace

std::optional<Context> Context::create(std::vector<const char*> requiredExtensions, bool enableValidation) {
    rx::core::log::init();

    if (volkInitialize() != VK_SUCCESS) {
        RX_LOG_ERROR("volkInitialize failed");
        return std::nullopt;
    }

    auto errorCount = std::make_shared<int>(0);

    vkb::InstanceBuilder builder;
    builder.set_app_name("renderer_x")
        .require_api_version(1, 3, 0)
        .set_headless(requiredExtensions.empty());

    for (const char* ext : requiredExtensions) {
        builder.enable_extension(ext);
    }

    if (enableValidation) {
        builder.request_validation_layers()
            .set_debug_callback(debugCallback)
            .set_debug_callback_user_data_pointer(errorCount.get());

        // Vulkan SYNCHRONIZATION validation is a distinct opt-in feature of
        // VK_LAYER_KHRONOS_validation (missing/incorrect barriers, hazards
        // sync2 alone cannot statically prove) layered on top of the core
        // validation enabled just above -- request_validation_layers() does
        // not turn it on by itself. VK_EXT_validation_features is what lets
        // a VkValidationFeaturesEXT struct request it at all; per this
        // repo's "don't reinvent the wheel" policy, that struct/pNext chain
        // is built by vk-bootstrap's own InstanceBuilder::
        // add_validation_feature_enable() (verified directly against the
        // pinned vk-bootstrap source: it chains a real
        // VkValidationFeaturesEXT, VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        // into VkInstanceCreateInfo::pNext) rather than this file hand-
        // rolling that same struct itself.
        //
        // VK_EXT_validation_features is a LAYER-provided instance extension
        // (implemented by VK_LAYER_KHRONOS_validation, not the loader), so
        // it only ever appears in vkb::SystemInfo's available-extensions
        // list when that same layer is actually installed -- exactly the
        // condition request_validation_layers() above already degrades
        // gracefully on when missing. Unlike that soft request, though,
        // InstanceBuilder::enable_extension() is unconditional: asking for
        // an extension that turns out unavailable fails build() outright.
        // This probe keeps a machine with no validation layer installed
        // (enableValidation=true requested anyway) failing exactly the way
        // it already did before this change -- via request_validation_
        // layers()'s own soft degrade -- rather than a new, harder failure
        // mode introduced by this addition.
        auto systemInfo = vkb::SystemInfo::get_system_info();
        if (systemInfo && systemInfo->is_extension_available(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
            builder.enable_extension(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)
                .add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
        } else {
            RX_LOG_WARN(
                "VK_EXT_validation_features unavailable (validation layer not installed?); Vulkan synchronization "
                "validation will not be active for this run");
        }
    }

    auto result = builder.build();
    if (!result) {
        RX_LOG_ERROR("vkb::InstanceBuilder::build failed: {}", result.error().message());
        return std::nullopt;
    }

    vkb::Instance vkbInstance = result.value();
    volkLoadInstance(vkbInstance.instance);

    return Context(std::move(vkbInstance), errorCount);
}

Context::Context(Context&& other) noexcept
    : vkbInstance_(std::move(other.vkbInstance_)), errorCount_(std::move(other.errorCount_)) {
    other.vkbInstance_.instance = VK_NULL_HANDLE;
}

Context& Context::operator=(Context&& other) noexcept {
    if (this != &other) {
        if (vkbInstance_.instance != VK_NULL_HANDLE) {
            // destroy_instance() tears down both the debug messenger (if
            // any) and the instance itself, in that order.
            vkb::destroy_instance(vkbInstance_);
        }
        vkbInstance_ = std::move(other.vkbInstance_);
        errorCount_ = std::move(other.errorCount_);
        other.vkbInstance_.instance = VK_NULL_HANDLE;
    }
    return *this;
}

Context::~Context() {
    if (vkbInstance_.instance != VK_NULL_HANDLE) {
        vkb::destroy_instance(vkbInstance_);
    }
}

}  // namespace rx::rhi
