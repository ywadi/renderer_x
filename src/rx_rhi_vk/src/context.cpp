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
