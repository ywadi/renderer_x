#include <doctest/doctest.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/memory_report.h>
#include <rx_rhi_vk/texture.h>
#include <VkBootstrap.h>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

// [Phase 4 Task 10, spec D24(a)/(c)] Rows 1/4/10/16 of
// gate/matrix-issue27-memory-budget.md: category-attributed accounting
// balance, the RxMemoryReport POD shape, the "single choke point ->
// correct attribution" composition contract, and the teardown leak/balance
// check. Rows 2/3 (budget extension + staleness) live in
// memory_budget_test.cpp; rows 5-9 (OOM handling) live in
// oom_handling_test.cpp; rows 13-15 (eviction contract) live in
// eviction_contract_test.cpp -- deliberately split, matching the matrix's
// own row groupings, rather than one giant file.

TEST_CASE("MemoryAccounting balances bytes/counts across interleaved record/release across categories") {
    rx::rhi::MemoryAccounting ledger;
    CHECK_FALSE(ledger.hasOutstanding());

    ledger.record(rx::rhi::MemoryCategory::GeometryPool, 100);
    ledger.record(rx::rhi::MemoryCategory::Texture, 200);
    ledger.record(rx::rhi::MemoryCategory::GeometryPool, 50);
    CHECK(ledger.snapshot(rx::rhi::MemoryCategory::GeometryPool).bytes == 150);
    CHECK(ledger.snapshot(rx::rhi::MemoryCategory::GeometryPool).count == 2);
    CHECK(ledger.snapshot(rx::rhi::MemoryCategory::Texture).bytes == 200);
    CHECK(ledger.snapshot(rx::rhi::MemoryCategory::Texture).count == 1);
    CHECK(ledger.hasOutstanding());

    // Interleaved release -- not just monotonic growth (matrix row 1's own
    // wording): destroy the FIRST GeometryPool allocation while the second
    // is still live.
    ledger.release(rx::rhi::MemoryCategory::GeometryPool, 100);
    CHECK(ledger.snapshot(rx::rhi::MemoryCategory::GeometryPool).bytes == 50);
    CHECK(ledger.snapshot(rx::rhi::MemoryCategory::GeometryPool).count == 1);

    ledger.record(rx::rhi::MemoryCategory::Staging, 999);
    ledger.release(rx::rhi::MemoryCategory::Texture, 200);
    CHECK(ledger.snapshot(rx::rhi::MemoryCategory::Texture).bytes == 0);
    CHECK(ledger.snapshot(rx::rhi::MemoryCategory::Texture).count == 0);

    ledger.release(rx::rhi::MemoryCategory::GeometryPool, 50);
    ledger.release(rx::rhi::MemoryCategory::Staging, 999);
    CHECK(ledger.snapshot(rx::rhi::MemoryCategory::GeometryPool).bytes == 0);
    CHECK(ledger.snapshot(rx::rhi::MemoryCategory::GeometryPool).count == 0);
    CHECK(ledger.snapshot(rx::rhi::MemoryCategory::Staging).bytes == 0);
    CHECK(ledger.snapshot(rx::rhi::MemoryCategory::Staging).count == 0);
    CHECK_FALSE(ledger.hasOutstanding());
}

TEST_CASE("RxMemoryReport: every field (category ledger + per-heap + budget-source) is independently exercised, "
          "device-free") {
    rx::rhi::MemoryAccounting ledger;
    ledger.record(rx::rhi::MemoryCategory::GeometryPool, 1000);
    ledger.record(rx::rhi::MemoryCategory::GeometryPool, 500);
    ledger.record(rx::rhi::MemoryCategory::Texture, 2048);

    rx::rhi::RxMemoryReport report;
    for (size_t i = 0; i < rx::rhi::kMemoryCategoryCount; ++i) {
        report.categories[i] = ledger.snapshot(static_cast<rx::rhi::MemoryCategory>(i));
    }
    CHECK(report.category(rx::rhi::MemoryCategory::GeometryPool).bytes == 1500);
    CHECK(report.category(rx::rhi::MemoryCategory::GeometryPool).count == 2);
    CHECK(report.category(rx::rhi::MemoryCategory::Texture).bytes == 2048);
    CHECK(report.category(rx::rhi::MemoryCategory::Texture).count == 1);
    CHECK(report.category(rx::rhi::MemoryCategory::Transient).bytes == 0);
    CHECK(report.category(rx::rhi::MemoryCategory::Staging).count == 0);
    CHECK(report.category(rx::rhi::MemoryCategory::Internal).count == 0);

    // Per-heap/budget-source fields: hand-built, independent of any live
    // device -- this is the struct-shape half of row 4's "every field"
    // requirement (a real per-heap query needs a live device -- see
    // memory_budget_test.cpp).
    report.budgetSource = rx::rhi::MemoryBudgetSource::HeapSizeFallback;
    report.heaps.push_back(rx::rhi::MemoryHeapReport{/*heapIndex=*/0, /*heapSizeBytes=*/1'000'000,
                                                       /*usageBytes=*/400'000, /*budgetBytes=*/800'000});
    report.heaps.push_back(rx::rhi::MemoryHeapReport{/*heapIndex=*/1, /*heapSizeBytes=*/2'000'000,
                                                       /*usageBytes=*/0, /*budgetBytes=*/2'000'000});
    REQUIRE(report.heaps.size() == 2);
    CHECK(report.heaps[0].heapIndex == 0);
    CHECK(report.heaps[0].heapSizeBytes == 1'000'000);
    CHECK(report.heaps[0].usageBytes == 400'000);
    CHECK(report.heaps[0].budgetBytes == 800'000);
    CHECK(report.heaps[1].heapIndex == 1);

    CHECK(std::string_view(rx::rhi::memoryBudgetSourceName(report.budgetSource)) == "HeapSizeFallback");
    CHECK(std::string_view(rx::rhi::memoryBudgetSourceName(rx::rhi::MemoryBudgetSource::RealExtension)) ==
          "RealExtension");

    // summarizeMemoryReport() must not crash/throw on an arbitrary,
    // hand-built report and must mention every category name -- the "loud,
    // named failure with the report attached" log line (buffer.cpp's
    // noteAllocationFailure()) depends on this never being a no-op.
    std::string summary = rx::rhi::summarizeMemoryReport(report);
    CHECK(summary.find("GeometryPool") != std::string::npos);
    CHECK(summary.find("Texture") != std::string::npos);
    CHECK(summary.find("Transient") != std::string::npos);
    CHECK(summary.find("Staging") != std::string::npos);
    CHECK(summary.find("Internal") != std::string::npos);
    CHECK(summary.find("HeapSizeFallback") != std::string::npos);
}

TEST_CASE("memoryCategoryName covers every MemoryCategory value distinctly") {
    CHECK(std::string_view(rx::rhi::memoryCategoryName(rx::rhi::MemoryCategory::GeometryPool)) == "GeometryPool");
    CHECK(std::string_view(rx::rhi::memoryCategoryName(rx::rhi::MemoryCategory::Texture)) == "Texture");
    CHECK(std::string_view(rx::rhi::memoryCategoryName(rx::rhi::MemoryCategory::Transient)) == "Transient");
    CHECK(std::string_view(rx::rhi::memoryCategoryName(rx::rhi::MemoryCategory::Staging)) == "Staging");
    CHECK(std::string_view(rx::rhi::memoryCategoryName(rx::rhi::MemoryCategory::Internal)) == "Internal");
}

namespace {

// Pure headless fixture (no window/surface/swapchain) -- same shape as
// deletion_queue_test.cpp's own HeadlessFixture, protected by
// doctest_main.cpp's process-wide vk-bootstrap warm-up exactly like that
// file's headless tests.
struct HeadlessFixture {
    rx::rhi::Context context;
    vkb::Device vkbDevice;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    VkInstance instance;
};

std::optional<HeadlessFixture> makeHeadlessFixture() {
    auto context = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(context.has_value());
    VkInstance instance = context->instance();

    vkb::PhysicalDeviceSelector selector(context->vkbInstance());
    auto physResult = selector.set_minimum_version(1, 3).select();
    if (!physResult.has_value()) {
        MESSAGE("no Vulkan 1.3 physical device available, skipping test");
        return std::nullopt;
    }

    auto deviceResult = vkb::DeviceBuilder(physResult.value()).build();
    REQUIRE(deviceResult.has_value());
    vkb::Device vkbDevice = deviceResult.value();
    volkLoadDevice(vkbDevice.device);

    return HeadlessFixture{std::move(*context), vkbDevice, vkbDevice.device, vkbDevice.physical_device.physical_device,
                            instance};
}

}  // namespace

TEST_CASE("Allocator::report() composes real category-tagged Buffer/Texture2D allocations from a single choke "
          "point, and returns to zero once they're destroyed (rows 1/4/10/16 integration)") {
    auto fixture = makeHeadlessFixture();
    if (!fixture.has_value()) {
        return;
    }

    auto allocator = rx::rhi::Allocator::createRaw(fixture->physicalDevice, fixture->device, fixture->instance);
    REQUIRE(allocator.has_value());

    {
        auto buf = allocator->createDeviceLocalBuffer(65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                        rx::rhi::MemoryCategory::GeometryPool);
        REQUIRE(buf.has_value());
        CHECK(buf->category() == rx::rhi::MemoryCategory::GeometryPool);
        CHECK(buf->allocatedBytes() >= 65536);

        auto tex = rx::rhi::Texture2D::create(fixture->physicalDevice, fixture->device, *allocator, VkExtent2D{64, 64},
                                               VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT,
                                               /*requestedMipLevels=*/1, rx::rhi::MemoryCategory::Texture);
        REQUIRE(tex.has_value());
        CHECK(tex->category() == rx::rhi::MemoryCategory::Texture);
        CHECK(tex->allocatedBytes() > 0);

        // A synthetic Transient-tagged buffer stands in for a
        // TransientPool-shaped caller (matrix row 10's own "mock
        // TextureCache-shaped caller" allowance -- TransientPool/
        // GeometryPool/TextureCache real call sites are other tasks' scope).
        auto transientBuf = allocator->createHostVisibleBuffer(4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                                  rx::rhi::MemoryCategory::Transient);
        REQUIRE(transientBuf.has_value());

        rx::rhi::RxMemoryReport report = allocator->report();
        CHECK(report.category(rx::rhi::MemoryCategory::GeometryPool).count == 1);
        CHECK(report.category(rx::rhi::MemoryCategory::GeometryPool).bytes == buf->allocatedBytes());
        CHECK(report.category(rx::rhi::MemoryCategory::Texture).count == 1);
        CHECK(report.category(rx::rhi::MemoryCategory::Texture).bytes == tex->allocatedBytes());
        CHECK(report.category(rx::rhi::MemoryCategory::Transient).count == 1);
        CHECK_FALSE(report.heaps.empty());
    }

    // Every tagged resource above just went out of scope (destroyed) --
    // the ledger must read exactly zero again (row 16's own "asserts the
    // report reads all-zero at the end").
    rx::rhi::RxMemoryReport afterReport = allocator->report();
    for (size_t i = 0; i < rx::rhi::kMemoryCategoryCount; ++i) {
        CHECK(afterReport.categories[i].bytes == 0);
        CHECK(afterReport.categories[i].count == 0);
    }

    vkDeviceWaitIdle(fixture->device);
    allocator.reset();
    vkb::destroy_device(fixture->vkbDevice);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}
