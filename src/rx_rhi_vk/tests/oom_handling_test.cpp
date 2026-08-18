#include <doctest/doctest.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/memory_report.h>
#include <rx_rhi_vk/texture.h>
#include <VkBootstrap.h>
#include <optional>
#include <string_view>
#include <utility>

// [Phase 4 Task 10, spec D24(d)] VK_ERROR_OUT_OF_DEVICE_MEMORY handling at
// the 5 enumerated allocation choke points (gate/matrix-issue27-memory-
// budget.md rows 5-9 / the plan Task 10's own audit checklist):
//   1. vmaCreateAllocator            (Allocator::createRaw)
//   2. Allocator::createHostVisibleBuffer
//   3. Allocator::createDeviceLocalBuffer
//   4. Allocator::createUploadRingBuffer
//   5. Texture2D::create (vmaCreateImage vs vkCreateImageView, two
//      DISTINCT failure classes)
//
// Rows 6-9 are forced via VmaAllocatorCreateInfo::pHeapSizeLimit
// (Allocator::createRaw()'s own `forcedHeapSizeLimitBytes` parameter) --
// VMA's own documented, host-side "test how your program behaves with
// limited memory" mechanism (vk_mem_alloc.h's `heap_memory_limit` docs):
// deterministically returns VK_ERROR_OUT_OF_DEVICE_MEMORY on the next
// allocation that would exceed the forced ceiling, with NO dependency on
// actually exhausting real GPU memory or on any driver's overallocation
// behavior -- exactly the "tiny mock/--budget-override allocator ceiling"
// the plan's own Task 10 steps call for.
//
// Row 5 (vmaCreateAllocator itself) is DELIBERATELY not exercised via a
// real forced failure: verified directly against the vendored VMA 3.4.0
// source that vmaCreateAllocator's constructor does
// `VMA_ASSERT(pCreateInfo->physicalDevice && pCreateInfo->device &&
// pCreateInfo->instance)` (vk_mem_alloc.h:13343) -- passing null/invalid
// handles to force a failure would abort() the WHOLE test process (VMA's
// default VMA_ASSERT is a real assert(), not a soft check), not fail this
// one test case gracefully. The matrix itself marks this row "Lower
// priority than rows 6-9". Instead, classifyAllocationFailure() -- the
// single source of truth Allocator::createRaw()'s own vmaCreateAllocator
// failure branch (buffer.cpp) uses for its log message -- is exercised
// directly, device-free.

TEST_CASE("classifyAllocationFailure names OutOfDeviceMemory/OutOfHostMemory distinctly from a generic failure "
          "(row 5's classification logic, device-free)") {
    CHECK(rx::rhi::classifyAllocationFailure(VK_SUCCESS) == rx::rhi::AllocationFailureKind::None);
    CHECK(rx::rhi::classifyAllocationFailure(VK_ERROR_OUT_OF_DEVICE_MEMORY) ==
          rx::rhi::AllocationFailureKind::OutOfDeviceMemory);
    CHECK(rx::rhi::classifyAllocationFailure(VK_ERROR_OUT_OF_HOST_MEMORY) ==
          rx::rhi::AllocationFailureKind::OutOfHostMemory);
    CHECK(rx::rhi::classifyAllocationFailure(VK_ERROR_INITIALIZATION_FAILED) == rx::rhi::AllocationFailureKind::Other);
    CHECK(rx::rhi::classifyAllocationFailure(VK_ERROR_DEVICE_LOST) == rx::rhi::AllocationFailureKind::Other);
}

TEST_CASE("allocationFailureKindName covers every AllocationFailureKind value distinctly") {
    using rx::rhi::AllocationFailureKind;
    CHECK(std::string_view(rx::rhi::allocationFailureKindName(AllocationFailureKind::None)) == "None");
    CHECK(std::string_view(rx::rhi::allocationFailureKindName(AllocationFailureKind::OutOfDeviceMemory)) ==
          "OutOfDeviceMemory");
    CHECK(std::string_view(rx::rhi::allocationFailureKindName(AllocationFailureKind::OutOfHostMemory)) ==
          "OutOfHostMemory");
    CHECK(std::string_view(rx::rhi::allocationFailureKindName(AllocationFailureKind::Other)) == "Other");
}

namespace {

// Same headless fixture shape as memory_report_test.cpp/memory_budget_test.cpp.
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

// A tiny-but-nonzero forced heap ceiling: every memory heap on the device
// is capped to this many bytes at allocator-creation time
// (VmaAllocatorCreateInfo::pHeapSizeLimit), so ANY allocation attempt
// larger than this deterministically fails with
// VK_ERROR_OUT_OF_DEVICE_MEMORY -- see this file's own top comment.
constexpr VkDeviceSize kForcedHeapCeilingBytes = 4096;
constexpr VkDeviceSize kOversizedRequestBytes = 16ull * 1024ull * 1024ull;  // 16 MiB, far over the ceiling

}  // namespace

TEST_CASE("Allocator::createHostVisibleBuffer surfaces a named OutOfDeviceMemory failure, report-attached, under a "
          "forced heap ceiling (row 6)") {
    auto fixture = makeHeadlessFixture();
    if (!fixture.has_value()) {
        return;
    }
    auto allocator =
        rx::rhi::Allocator::createRaw(fixture->physicalDevice, fixture->device, fixture->instance,
                                       /*memoryBudgetExtensionEnabled=*/false, kForcedHeapCeilingBytes);
    REQUIRE(allocator.has_value());

    auto buffer = allocator->createHostVisibleBuffer(kOversizedRequestBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                       rx::rhi::MemoryCategory::Internal);
    CHECK_FALSE(buffer.has_value());
    CHECK(allocator->lastAllocationFailureKind() == rx::rhi::AllocationFailureKind::OutOfDeviceMemory);
    CHECK_FALSE(allocator->lastAllocationFailureReport().heaps.empty());

    allocator.reset();
    vkb::destroy_device(fixture->vkbDevice);
}

TEST_CASE("Allocator::createDeviceLocalBuffer surfaces a named OutOfDeviceMemory failure, report-attached, under a "
          "forced heap ceiling (row 7 -- the highest-traffic site)") {
    auto fixture = makeHeadlessFixture();
    if (!fixture.has_value()) {
        return;
    }
    auto allocator =
        rx::rhi::Allocator::createRaw(fixture->physicalDevice, fixture->device, fixture->instance,
                                       /*memoryBudgetExtensionEnabled=*/false, kForcedHeapCeilingBytes);
    REQUIRE(allocator.has_value());

    auto buffer = allocator->createDeviceLocalBuffer(kOversizedRequestBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                       rx::rhi::MemoryCategory::GeometryPool);
    CHECK_FALSE(buffer.has_value());
    CHECK(allocator->lastAllocationFailureKind() == rx::rhi::AllocationFailureKind::OutOfDeviceMemory);
    CHECK_FALSE(allocator->lastAllocationFailureReport().heaps.empty());

    // The failed allocation must not have been recorded into the ledger --
    // a failed create() has nothing to release() later, and the ledger
    // must stay exactly zero.
    CHECK(allocator->report().category(rx::rhi::MemoryCategory::GeometryPool).count == 0);

    allocator.reset();
    vkb::destroy_device(fixture->vkbDevice);
}

TEST_CASE("Allocator::createUploadRingBuffer surfaces a named OutOfDeviceMemory failure, report-attached, under a "
          "forced heap ceiling (row 8)") {
    auto fixture = makeHeadlessFixture();
    if (!fixture.has_value()) {
        return;
    }
    auto allocator =
        rx::rhi::Allocator::createRaw(fixture->physicalDevice, fixture->device, fixture->instance,
                                       /*memoryBudgetExtensionEnabled=*/false, kForcedHeapCeilingBytes);
    REQUIRE(allocator.has_value());

    auto buffer = allocator->createUploadRingBuffer(kOversizedRequestBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    CHECK_FALSE(buffer.has_value());
    CHECK(allocator->lastAllocationFailureKind() == rx::rhi::AllocationFailureKind::OutOfDeviceMemory);
    CHECK_FALSE(allocator->lastAllocationFailureReport().heaps.empty());

    allocator.reset();
    vkb::destroy_device(fixture->vkbDevice);
}

TEST_CASE("Texture2D::create surfaces a named OutOfDeviceMemory failure, report-attached, under a forced heap "
          "ceiling on its vmaCreateImage step (row 9)") {
    auto fixture = makeHeadlessFixture();
    if (!fixture.has_value()) {
        return;
    }
    auto allocator =
        rx::rhi::Allocator::createRaw(fixture->physicalDevice, fixture->device, fixture->instance,
                                       /*memoryBudgetExtensionEnabled=*/false, kForcedHeapCeilingBytes);
    REQUIRE(allocator.has_value());

    // A 1024x1024 RGBA8 image (4 MiB of pixel data alone) comfortably
    // exceeds the 4 KiB forced ceiling regardless of alignment/tiling
    // padding.
    auto texture = rx::rhi::Texture2D::create(fixture->physicalDevice, fixture->device, *allocator,
                                               VkExtent2D{1024, 1024}, VK_FORMAT_R8G8B8A8_UNORM,
                                               VK_IMAGE_USAGE_SAMPLED_BIT, /*requestedMipLevels=*/1,
                                               rx::rhi::MemoryCategory::Texture);
    CHECK_FALSE(texture.has_value());
    CHECK(allocator->lastAllocationFailureKind() == rx::rhi::AllocationFailureKind::OutOfDeviceMemory);
    CHECK_FALSE(allocator->lastAllocationFailureReport().heaps.empty());
    CHECK(allocator->report().category(rx::rhi::MemoryCategory::Texture).count == 0);

    allocator.reset();
    vkb::destroy_device(fixture->vkbDevice);
}

TEST_CASE("Texture2D::create's vmaCreateImage-failure and vkCreateImageView-failure classes are distinguishable: "
          "the non-memory path never touches Allocator's memory-failure state (row 9)") {
    auto fixture = makeHeadlessFixture();
    if (!fixture.has_value()) {
        return;
    }
    auto allocator = rx::rhi::Allocator::createRaw(fixture->physicalDevice, fixture->device, fixture->instance);
    REQUIRE(allocator.has_value());

    // Seed a KNOWN memory-failure state via the real OOM path first (a
    // small, otherwise-successful allocator; force it by hand via
    // noteAllocationFailure() directly -- this test's own point is
    // Allocator-state isolation, not re-proving row 7's forcing mechanism
    // again).
    allocator->noteAllocationFailure("test-seed", rx::rhi::MemoryCategory::Texture, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    REQUIRE(allocator->lastAllocationFailureKind() == rx::rhi::AllocationFailureKind::OutOfDeviceMemory);
    rx::rhi::RxMemoryReport seededReport = allocator->lastAllocationFailureReport();

    // A vkCreateImageView-class failure (device/API-misuse, not a memory
    // event) must NOT overwrite that seeded state -- exactly what keeps
    // the two failure classes distinguishable to a caller inspecting
    // Allocator state afterward.
    allocator->noteNonMemoryFailure("Texture2D::create(vkCreateImageView)", VK_ERROR_FORMAT_NOT_SUPPORTED);

    CHECK(allocator->lastAllocationFailureKind() == rx::rhi::AllocationFailureKind::OutOfDeviceMemory);
    CHECK(allocator->lastAllocationFailureReport().budgetSource == seededReport.budgetSource);
    for (size_t i = 0; i < rx::rhi::kMemoryCategoryCount; ++i) {
        CHECK(allocator->lastAllocationFailureReport().categories[i].bytes == seededReport.categories[i].bytes);
        CHECK(allocator->lastAllocationFailureReport().categories[i].count == seededReport.categories[i].count);
    }

    allocator.reset();
    vkb::destroy_device(fixture->vkbDevice);
}
