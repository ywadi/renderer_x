#include <doctest/doctest.h>
#include <rx_asset/geometry_pool.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/upload.h>
#include <rx_platform/window.h>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// accounting_test.cpp -- the #27 cross-ticket integration acceptance
// criterion [Phase 4 Stage 1 Task 12, matrix-issue21 row "Pool memory
// attributed in #27's accounting categories"]: "after GeometryPool
// allocates a chunk, the #27 accounting report's 'geometry pool' category
// byte count increases by exactly the chunk's allocated size." Proves
// GeometryPool threads MemoryCategory::GeometryPool through the SAME
// Allocator::createDeviceLocalBuffer() choke point every other
// device-local buffer in this engine uses (rather than a private,
// invisible ledger) -- see geometry_pool.cpp's addBlock().

namespace {

struct AccountingFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    rx::rhi::Uploader uploader;
};

std::optional<AccountingFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping GeometryPool accounting test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions, skipping GeometryPool accounting test");
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

    auto uploader = rx::rhi::Uploader::create(*allocator, *device);
    REQUIRE(uploader.has_value());

    return AccountingFixture{std::move(*window), std::move(*context), std::move(*device), std::move(*allocator),
                              std::move(*uploader)};
}

}  // namespace

TEST_CASE("GeometryPool chunk allocation grows the #27 accounting report's geometry-pool category by exactly the "
          "chunk's real allocated size") {
    auto fixture = makeFixture("rx_asset_gp_accounting");
    if (!fixture.has_value()) {
        return;
    }

    const rx::rhi::RxMemoryReport before = fixture->allocator.report();
    const rx::rhi::MemoryCategoryStats beforeStats = before.category(rx::rhi::MemoryCategory::GeometryPool);

    // Chunk sizes chosen as EXACT multiples of PoolVertex's/uint32_t's
    // element stride (48/4 bytes) -- GeometryPool's own addBlock() rounds
    // a requested chunk size DOWN to a whole number of elements before
    // allocating the real buffer (see that method's own comment), so a
    // non-multiple request would deterministically allocate slightly
    // fewer bytes than requested; picking exact multiples here keeps this
    // test's own arithmetic simple. This does NOT make the test depend on
    // that rounding behavior either way: the assertions below compare
    // against GeometryPool's own vertexBufferAllocatedBytes()/
    // indexBufferAllocatedBytes() diagnostic accessors, which report
    // whatever VMA actually charged for this specific chunk, not the
    // nominal request.
    rx::asset::PoolConfig config;
    config.vertexChunkBytes = 48ull * 20000;  // exactly 20000 PoolVertex elements
    config.indexChunkBytes = 4ull * 20000;    // exactly 20000 uint32_t elements

    auto pool = rx::asset::GeometryPool::create(fixture->allocator, fixture->device, fixture->uploader, config);
    REQUIRE(pool != nullptr);

    // One small mesh, guaranteed to fit in the first (and only) block --
    // exactly ONE chunk allocation (one vertex Buffer + one index Buffer)
    // happens as a result of this single upload() call.
    std::vector<rx::asset::PoolVertex> verts(3);
    std::vector<uint32_t> indices{0, 1, 2};
    rx::asset::MeshRange range = pool->upload(verts, indices);
    REQUIRE(pool->blockCount() == 1);
    CHECK(range.blockId == 0);

    const VkDeviceSize realVertexBytes = pool->vertexBufferAllocatedBytes(0);
    const VkDeviceSize realIndexBytes = pool->indexBufferAllocatedBytes(0);
    REQUIRE(realVertexBytes >= config.vertexChunkBytes);
    REQUIRE(realIndexBytes >= config.indexChunkBytes);
    MESSAGE("real vertex chunk bytes = ", realVertexBytes, " (requested ", config.vertexChunkBytes, ")");
    MESSAGE("real index chunk bytes = ", realIndexBytes, " (requested ", config.indexChunkBytes, ")");

    const rx::rhi::RxMemoryReport after = fixture->allocator.report();
    const rx::rhi::MemoryCategoryStats afterStats = after.category(rx::rhi::MemoryCategory::GeometryPool);

    CHECK(afterStats.bytes - beforeStats.bytes == realVertexBytes + realIndexBytes);
    // Two new allocations: one vertex Buffer, one index Buffer.
    CHECK(afterStats.count - beforeStats.count == 2);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
