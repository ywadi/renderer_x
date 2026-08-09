// DOCTEST_CONFIG_IMPLEMENT (not _WITH_MAIN): main() below needs to run code
// before any TEST_CASE executes, so it constructs and drives the
// doctest::Context itself instead of using doctest's generated main().
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <rx_rhi_vk/context.h>
#include <rx_platform/window.h>

// This is where every future rx_rhi_vk test file's code joins this binary,
// so this is where a real vk-bootstrap hazard gets a permanent, structural
// fix rather than a per-test-file workaround: see the WARNING on
// rx::rhi::Context::create() (rx_rhi_vk/context.h) for the full defect.
// Short version -- the pinned vk-bootstrap commit caches instance-level
// Vulkan function pointers process-wide from whichever vkb::Instance is
// built *first*, forever, with no reset. If any test in this binary ever
// builds a Context with narrower capabilities (fewer extensions -- e.g.
// headless -- or enableValidation=false) before another test builds one
// with broader capabilities (e.g. windowed and/or validated, needed by
// Device::create() and by context_test.cpp respectively), every later,
// broader call inherits null function pointers cached from the narrower
// instance: this has been observed as both a segfault (null
// vkGetPhysicalDeviceSurfaceSupportKHR) and a clean `Context::create`
// failure (null vkCreateDebugUtilsMessengerEXT -- see the warm-up function
// below for that second one specifically).
//
// An earlier version of this fix pinned doctest's test *order* instead
// (--order-by=suite, sorting windowed Device tests before the headless
// Context test). That was fragile: doctest's suite sort falls back to
// filename order on ties, so any future test file that is alphabetically
// earlier than device_test.cpp and happens to build a narrower Context
// would silently reintroduce this exact crash. Ordering test cases can
// never fully close this hole -- only pre-warming the cache can, since it
// makes the hazard's precondition ("the first vkb::Instance in the process
// is narrower than a later one needs") impossible to hit in the first
// place, regardless of what any test file does or what order it runs in.
//
// So: before running any TEST_CASE, build (and immediately destroy) one
// throwaway Context using the *broadest* capabilities this binary will ever
// need -- a real window's requiredVulkanInstanceExtensions() when a display
// is available, plus enableValidation=true, so the process-wide cache gets
// warmed with real, valid function pointers for both up front. When no
// display is available (headless CI), fall back to a headless (but still
// validated) warm-up: the windowed Device tests skip themselves on such
// machines anyway (see device_test.cpp's own skip guard), so a
// narrower-on-extensions warm-up is harmless there -- there is no broader
// instance any test in this binary will actually try to build.
namespace {

void warmUpVkBootstrapInstanceFunctionCache() {
    std::vector<const char*> extensions;
    auto window = rx::platform::Window::create("rx_rhi_vk_tests_warmup", 64, 64, /*visible=*/false);
    if (window.has_value()) {
        extensions = window->requiredVulkanInstanceExtensions();
    }
    // enableValidation=true here is not incidental: the same process-wide
    // vk-bootstrap cache also covers fp_vkCreateDebugUtilsMessengerEXT (and
    // its destroy counterpart), which is only non-null when the instance
    // that first resolves it actually requested validation layers / a debug
    // callback. An earlier version of this warm-up used
    // enableValidation=false and broke every *validated* Context built
    // afterward in this same process (vkb::InstanceBuilder::build failing
    // with "failed_create_debug_messenger") -- the exact same class of bug
    // as the surface-function one above, just on a different extension.
    // "Broadest instance this binary needs" has to mean broadest along
    // every axis vk-bootstrap caches per-process, not just surface support.
    //
    // Best-effort: if this fails outright (no Vulkan driver at all), every
    // real test below will fail too and report that clearly on its own --
    // nothing to specially handle here.
    rx::rhi::Context::create(extensions, /*enableValidation=*/true);
}

}  // namespace

int main(int argc, char** argv) {
    warmUpVkBootstrapInstanceFunctionCache();

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    int res = context.run();
    if (context.shouldExit()) {
        return res;
    }
    return res;
}
