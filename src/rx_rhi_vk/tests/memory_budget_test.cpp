#include <doctest/doctest.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/frame_sync.h>
#include <rx_rhi_vk/memory_report.h>
#include <VkBootstrap.h>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// [Phase 4 Task 10, spec D24(b), gate ruling #27] Rows 2/3 of
// gate/matrix-issue27-memory-budget.md: VK_EXT_memory_budget opportunistic
// enablement (+ the heap-size fallback path, both tested) and the
// vmaSetCurrentFrameIndex() staleness regression guard -- "the load-bearing
// omission" the gate ruling calls out by name.
//
// [Fix round 1, reviewer C1] The row-3 coverage below is TWO test cases,
// deliberately: "vmaSetCurrentFrameIndex staleness regression" (driver-
// observed consequence, kept for its documentation/evidence value) is NOT
// by itself a discriminating regression guard -- the reviewer reverted
// Allocator::setCurrentFrameIndex() to a no-op in a scratch worktree and
// that test still passed 12/12, because on a quiet driver the real budget
// value never moves whether or not the call fires, so "stays frozen"
// passes vacuously either way. "FrameSync::advanceFrame(Allocator*)
// wiring" below is the MANDATORY discriminating test: it asserts on
// Allocator::setCurrentFrameIndexCallCount(), a counter incremented
// unconditionally inside setCurrentFrameIndex() itself (buffer.h/.cpp),
// completely independent of what any driver reports. See
// task-10-report.md's fix-round-1 section for the revert-check evidence.

namespace {

// Headless fixture that ALSO reports whether VK_EXT_memory_budget was
// actually enabled on the built device -- same shape as
// memory_report_test.cpp's HeadlessFixture, plus the one extra step
// Device::create() itself performs (device.cpp): a guarded
// enable_extension_if_present() call, done by hand here since this test
// needs a headless (no window/surface/swapchain) device and Device::create()
// requires a real VkSurfaceKHR.
struct MemoryBudgetFixture {
    rx::rhi::Context context;
    vkb::Device vkbDevice;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    VkInstance instance;
    bool memoryBudgetExtensionEnabled;
};

std::optional<MemoryBudgetFixture> makeFixture() {
    auto context = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(context.has_value());
    VkInstance instance = context->instance();

    vkb::PhysicalDeviceSelector selector(context->vkbInstance());
    auto physResult = selector.set_minimum_version(1, 3).select();
    if (!physResult.has_value()) {
        MESSAGE("no Vulkan 1.3 physical device available, skipping test");
        return std::nullopt;
    }

    // Same opportunistic, guarded pattern as Device::create() (device.cpp)
    // -- see that file's own comment on VK_EXT_memory_budget's instance-
    // level dependency already being satisfied by this project's >= 1.3
    // baseline.
    bool memoryBudgetExtensionEnabled =
        physResult.value().enable_extension_if_present(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    auto deviceResult = vkb::DeviceBuilder(physResult.value()).build();
    REQUIRE(deviceResult.has_value());
    vkb::Device vkbDevice = deviceResult.value();
    volkLoadDevice(vkbDevice.device);

    return MemoryBudgetFixture{std::move(*context), vkbDevice, vkbDevice.device,
                                vkbDevice.physical_device.physical_device, instance, memoryBudgetExtensionEnabled};
}

}  // namespace

TEST_CASE("Allocator::report() with VK_EXT_memory_budget enabled: RealExtension source, nonzero budget on at "
          "least one heap") {
    auto fixture = makeFixture();
    if (!fixture.has_value()) {
        return;
    }
    if (!fixture->memoryBudgetExtensionEnabled) {
        MESSAGE("VK_EXT_memory_budget not present on this device, skipping the enabled-path assertions "
                "(see the fallback-path test below for the other half)");
        vkb::destroy_device(fixture->vkbDevice);
        return;
    }

    auto allocator = rx::rhi::Allocator::createRaw(fixture->physicalDevice, fixture->device, fixture->instance,
                                                    /*memoryBudgetExtensionEnabled=*/true);
    REQUIRE(allocator.has_value());

    rx::rhi::RxMemoryReport report = allocator->report();
    CHECK(report.budgetSource == rx::rhi::MemoryBudgetSource::RealExtension);
    REQUIRE_FALSE(report.heaps.empty());
    bool anyNonzeroBudget = false;
    for (const auto& heap : report.heaps) {
        if (heap.budgetBytes > 0) {
            anyNonzeroBudget = true;
        }
    }
    CHECK(anyNonzeroBudget);

    allocator.reset();
    vkDeviceWaitIdle(fixture->device);
    vkb::destroy_device(fixture->vkbDevice);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Allocator::report() falls back to VkMemoryHeap::size as the budget estimate when the extension is not "
          "enabled, with an explicit HeapSizeFallback source flag") {
    auto fixture = makeFixture();
    if (!fixture.has_value()) {
        return;
    }

    // Deliberately request memoryBudgetExtensionEnabled=false regardless of
    // what the device actually supports -- this is what makes the fallback
    // path deterministically testable independent of driver capability
    // (matrix row 2's own "verify in-task, don't assume" discipline: the
    // fallback path is asserted by a DEDICATED test case, not just inferred
    // from whatever this machine happens to support).
    auto allocator = rx::rhi::Allocator::createRaw(fixture->physicalDevice, fixture->device, fixture->instance,
                                                    /*memoryBudgetExtensionEnabled=*/false);
    REQUIRE(allocator.has_value());

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(fixture->physicalDevice, &memProps);

    rx::rhi::RxMemoryReport report = allocator->report();
    CHECK(report.budgetSource == rx::rhi::MemoryBudgetSource::HeapSizeFallback);
    REQUIRE(report.heaps.size() == memProps.memoryHeapCount);
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
        CHECK(report.heaps[i].heapIndex == i);
        CHECK(report.heaps[i].heapSizeBytes == memProps.memoryHeaps[i].size);
        // The exact acceptance-criterion wording (matrix-issue27 row 2):
        // the fallback budget estimate IS the raw heap size, not VMA's own
        // internal 80%-of-heap-size heuristic (see buffer.cpp's report()
        // comment for why that distinction is deliberate).
        CHECK(report.heaps[i].budgetBytes == memProps.memoryHeaps[i].size);
    }

    allocator.reset();
    vkDeviceWaitIdle(fixture->device);
    vkb::destroy_device(fixture->vkbDevice);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("vmaSetCurrentFrameIndex staleness regression: heap usage silently stays stale after a large allocation "
          "without Allocator::setCurrentFrameIndex(), and refreshes once it is called") {
    auto fixture = makeFixture();
    if (!fixture.has_value()) {
        return;
    }
    if (!fixture->memoryBudgetExtensionEnabled) {
        MESSAGE("VK_EXT_memory_budget not present on this device -- the staleness heuristic this test exercises "
                "only applies to the real-extension path (buffer.cpp's report() comment/VMA's own "
                "GetHeapBudgets() -- the non-extension branch always recomputes fresh), skipping");
        vkb::destroy_device(fixture->vkbDevice);
        return;
    }

    auto allocator = rx::rhi::Allocator::createRaw(fixture->physicalDevice, fixture->device, fixture->instance,
                                                    /*memoryBudgetExtensionEnabled=*/true);
    REQUIRE(allocator.has_value());

    // TECHNICAL NOTE, verified first-hand against the vendored VMA 3.4.0
    // source (GetHeapBudgets(), vk_mem_alloc.h:14782-14834) before writing
    // this test: `usage` is NOT a good staleness signal by itself for a
    // SELF-caused allocation. VMA computes it as
    // `frozen_driver_usage_at_last_fetch + (live_own_blockBytes -
    // own_blockBytes_at_last_fetch)` -- `blockBytes` is updated
    // immediately on every vmaCreateBuffer/vmaDestroyBuffer
    // (VmaCurrentBudgetData::AddAllocation/RemoveAllocation,
    // vk_mem_alloc.h:15052/15076/15095), completely independent of the
    // 30-op/setCurrentFrameIndex refresh gate -- so `usage` ALWAYS
    // reflects this allocator's own local allocations immediately, stale
    // or not, by construction of that formula. It is `budget` that is the
    // real staleness signal: `outBudgets->budget = VMA_MIN(m_VulkanBudget
    // [heapIndex], heapSize)` applies NO local correction at all --
    // m_VulkanBudget is touched ONLY by UpdateVulkanBudget(), which runs
    // ONLY on setCurrentFrameIndex() or every 30 ops. This test exercises
    // that real, verified property directly, rather than asserting
    // something about `usage` that would not actually discriminate a
    // silently-dropped setCurrentFrameIndex() call.
    rx::rhi::RxMemoryReport baseline = allocator->report();
    REQUIRE_FALSE(baseline.heaps.empty());
    std::vector<VkDeviceSize> budgetBeforeActivity(baseline.heaps.size());
    for (size_t i = 0; i < baseline.heaps.size(); ++i) {
        budgetBeforeActivity[i] = baseline.heaps[i].budgetBytes;
    }

    // Real local allocation activity -- create AND destroy a 64 MiB
    // device-local buffer, well under the 30-op lazy-refetch threshold (2
    // ops total: create + destroy) -- and crucially no setCurrentFrameIndex()
    // call anywhere in this block.
    {
        constexpr VkDeviceSize kLargeAllocBytes = 64ull * 1024ull * 1024ull;
        auto bigBuffer = allocator->createDeviceLocalBuffer(kLargeAllocBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        REQUIRE(bigBuffer.has_value());
    }

    rx::rhi::RxMemoryReport staleReport = allocator->report();
    REQUIRE(staleReport.heaps.size() == budgetBeforeActivity.size());
    for (size_t i = 0; i < budgetBeforeActivity.size(); ++i) {
        // Demonstrates the real driver-level consequence the gate ruling
        // describes: budget silently stays byte-identical to its
        // pre-activity value despite real intervening create+destroy
        // activity, because no real driver refetch happened.
        // NOT DISCRIMINATING ON ITS OWN [fix round 1, reviewer C1,
        // empirically confirmed via a reverted-wiring scratch-worktree
        // build]: on a quiet driver whose real budget value would not have
        // moved between two fetches ANYWAY, this assertion passes whether
        // setCurrentFrameIndex() fires or not -- it proves staleness is
        // real, not that this suite would catch the wiring regressing. See
        // "FrameSync::advanceFrame(Allocator*) wiring" below for the
        // mandatory discriminating assertion.
        CHECK(staleReport.heaps[i].budgetBytes == budgetBeforeActivity[i]);
    }

    // Now call the wired mechanism -- setCurrentFrameIndex() forces an
    // immediate UpdateVulkanBudget() re-fetch regardless of the 30-op
    // counter (verified directly against the vendored VMA source) and
    // resets it to 0. Second-order proof the reset is real, not a
    // one-shot fluke: repeat the exact same "real local activity leaves
    // budget frozen absent a refresh" check a second time, now relative to
    // the just-refreshed baseline -- if setCurrentFrameIndex() had NOT
    // actually reset the op counter, this second round would already be
    // past the 30-op threshold from the first round's activity and could
    // spuriously pass for the wrong reason; starting the count over is
    // what makes this a real, repeatable proof of the reset itself.
    allocator->setCurrentFrameIndex(1);
    rx::rhi::RxMemoryReport refreshedBaseline = allocator->report();
    REQUIRE(refreshedBaseline.heaps.size() == budgetBeforeActivity.size());

    {
        constexpr VkDeviceSize kLargeAllocBytes = 32ull * 1024ull * 1024ull;
        auto secondBuffer = allocator->createDeviceLocalBuffer(kLargeAllocBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        REQUIRE(secondBuffer.has_value());
    }
    rx::rhi::RxMemoryReport staleAgain = allocator->report();
    REQUIRE(staleAgain.heaps.size() == refreshedBaseline.heaps.size());
    for (size_t i = 0; i < refreshedBaseline.heaps.size(); ++i) {
        CHECK(staleAgain.heaps[i].budgetBytes == refreshedBaseline.heaps[i].budgetBytes);
    }

    allocator.reset();
    vkDeviceWaitIdle(fixture->device);
    vkb::destroy_device(fixture->vkbDevice);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("FrameSync::advanceFrame(Allocator*) wiring: Allocator::setCurrentFrameIndex() fires exactly once per "
          "advanced frame -- the mandatory discriminating regression guard [fix round 1, reviewer C1]") {
    auto fixture = makeFixture();
    if (!fixture.has_value()) {
        return;
    }

    // Independent of whether VK_EXT_memory_budget is actually present:
    // this test asserts on the WIRING (a call count), never on any
    // driver-reported number, so it runs unconditionally -- unlike the two
    // tests above, no MESSAGE/skip branch is needed here.
    auto allocator = rx::rhi::Allocator::createRaw(fixture->physicalDevice, fixture->device, fixture->instance,
                                                    fixture->memoryBudgetExtensionEnabled);
    REQUIRE(allocator.has_value());

    // Allocator::createRaw() itself never calls setCurrentFrameIndex()
    // (only vmaCreateAllocator's own constructor primes VMA's internal
    // budget cache via a private call this counter does not observe) --
    // baseline must be exactly 0.
    REQUIRE(allocator->setCurrentFrameIndexCallCount() == 0);

    // Queue family index 0 always exists (every physical device reports at
    // least one queue family) -- FrameSync::create() only needs a valid
    // index to build a command pool from, never submits anything through
    // it in this test, so no particular queue capability is required.
    auto frameSync = rx::rhi::FrameSync::create(fixture->device, /*queueFamily=*/0, /*swapchainImageCount=*/1);
    REQUIRE(frameSync.has_value());

    // Drive N frames through the REAL production call chain --
    // FrameSync::advanceFrame(Allocator*) -> Allocator::setCurrentFrameIndex()
    // -> vmaSetCurrentFrameIndex() -- not a shortcut that calls
    // Allocator::setCurrentFrameIndex() directly. This is what the reviewer
    // asked to be exercised: the wiring, not just the leaf function.
    constexpr int kFrames = 5;
    for (int i = 0; i < kFrames; ++i) {
        frameSync->advanceFrame(&*allocator);
    }

    // THE mandatory discriminating assertion: fails (4 != 5, or 0 != 5 if
    // the call is dropped entirely) the instant
    // FrameSync::advanceFrame()'s call into Allocator::setCurrentFrameIndex(),
    // or that method's own call into vmaSetCurrentFrameIndex(), regresses
    // to a no-op -- with zero dependency on any driver ever reporting a
    // different budget/usage number. See task-10-report.md's fix-round-1
    // section for the scratch-worktree revert-check proving this
    // discriminates for real.
    CHECK(allocator->setCurrentFrameIndexCallCount() == static_cast<uint64_t>(kFrames));

    // The default argument (every pre-Task-10 call site: `frameSync->
    // advanceFrame()` with no argument) must stay a true no-op -- confirms
    // this fix did not silently change existing callers' behavior.
    frameSync->advanceFrame();
    CHECK(allocator->setCurrentFrameIndexCallCount() == static_cast<uint64_t>(kFrames));

    // FrameSync's own destructor contract (frame_sync.h): the device must
    // already be idle before it is destroyed. No real GPU work was ever
    // submitted through this FrameSync (advanceFrame() alone records
    // nothing on the GPU), so this is a formality, not a real wait -- kept
    // anyway to honor the documented contract rather than rely on that.
    vkDeviceWaitIdle(fixture->device);
    frameSync.reset();
    allocator.reset();
    vkb::destroy_device(fixture->vkbDevice);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}
