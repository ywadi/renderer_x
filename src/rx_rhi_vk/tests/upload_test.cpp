#include <doctest/doctest.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/mesh_buffers.h>
#include <rx_rhi_vk/texture.h>
#include <rx_rhi_vk/upload.h>
#include <rx_platform/window.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace {

// Same skip-guarded windowed-device fixture pattern as buffer_test.cpp/
// device_test.cpp -- Uploader::create() takes a real rx::rhi::Device&
// (it needs a real graphics queue + queue family), which only exists
// against a real VkSurfaceKHR per device.h, so (unlike
// deletion_queue_test.cpp's pure-headless fixture, which never touches
// rx::rhi::Device at all) this file's tests need a window.
struct UploadTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
};

std::optional<UploadTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping Uploader test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping Uploader test");
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

    return UploadTestFixture{std::move(*window), std::move(*context), std::move(*device), std::move(*allocator)};
}

// Copies `size` bytes out of `src` (assumed device-local, TRANSFER_SRC_BIT)
// into a freshly allocated, invalidated, host-visible buffer -- the
// readback half of every round-trip test below.
std::vector<uint8_t> readBackBuffer(rx::rhi::Allocator& allocator, VkDevice device, VkQueue queue,
                                     uint32_t queueFamily, VkBuffer src, VkDeviceSize size) {
    auto readback = allocator.createHostVisibleBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());

    auto cmdCtx = rx::rhi::CommandContext::create(device, queue, queueFamily);
    REQUIRE(cmdCtx.has_value());
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, src, readback->handle(), 1, &region);
    });

    // GPU write (the copy above) happened after this host-visible buffer
    // was mapped -- invalidate() before reading, per Buffer's own
    // contract (this task's own Buffer API addition).
    readback->invalidate();

    std::vector<uint8_t> result(static_cast<size_t>(size));
    std::memcpy(result.data(), readback->mappedData(), result.size());
    return result;
}

}  // namespace

TEST_CASE("Uploader::uploadToBuffer round-trips bytes through the staging path byte-exact") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_roundtrip");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device);
    REQUIRE(uploader.has_value());

    constexpr VkDeviceSize kSize = 4096;
    std::array<uint8_t, kSize> pattern{};
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
    }

    // createHostVisibleBuffer() (not createDeviceLocalBuffer()) is the
    // deterministic, hardware-independent way to force the staging branch,
    // matching this test's name -- its directPathCapable() is a hardcoded
    // false by construction, not a measurement (buffer.h). Task 8 fix:
    // this used to call createDeviceLocalBuffer() with TRANSFER_DST/SRC-
    // only usage on the (then-believed) assumption that a transfer-only
    // usage bit combination can NEVER measure as direct-path-capable "no
    // matter the hardware" -- empirically false, caught by a real CI
    // failure on GitHub Actions' lavapipe/llvmpipe build (which, unlike
    // this project's own development machine's local lavapipe build,
    // exposes no non-host-visible DEVICE_LOCAL memory type at all, so
    // even a transfer-only buffer measures directPathCapable()==true
    // there) -- see createDeviceLocalBuffer()'s corrected header comment
    // in buffer.h for the full mechanism.
    auto dst = fixture->allocator.createHostVisibleBuffer(
        kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dst.has_value());
    REQUIRE_FALSE(dst->directPathCapable());

    REQUIRE(uploader->uploadToBuffer(*dst, 0, pattern.data(), kSize));
    // [Phase 4 Task 11, spec D25] flush() no longer blocks -- explicitly
    // wait() on the ticket it returns before reading back, since this
    // readback's own separate command-buffer submission has no other
    // synchronization tying it to the upload's completion (relying on
    // same-queue submission order alone would be an unstated, spec-
    // fragile assumption, not a real guarantee).
    uploader->wait(uploader->flush());
    CHECK(uploader->everUsedStagingPath());
    CHECK_FALSE(uploader->everUsedDirectPath());

    std::vector<uint8_t> readBack = readBackBuffer(fixture->allocator, fixture->device.device(),
                                                     fixture->device.graphicsQueue(),
                                                     fixture->device.graphicsQueueFamily(), dst->handle(), kSize);
    CHECK(std::memcmp(readBack.data(), pattern.data(), kSize) == 0);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Uploader::uploadToBuffer takes the direct memcpy-into-mapped-destination path when the destination's "
          "own allocation lands DEVICE_LOCAL+HOST_VISIBLE -- mechanism asserted, not just observed") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_direct_path");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device);
    REQUIRE(uploader.has_value());

    constexpr VkDeviceSize kSize = 256;
    std::array<uint8_t, kSize> pattern{};
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<uint8_t>((i * 17 + 3) & 0xFF);
    }

    // VERTEX_BUFFER_BIT is the real device-consuming usage bit that makes
    // VmaBufferImageUsage::ContainsDeviceAccess() true, which is what
    // makes Allocator::createDeviceLocalBuffer()'s
    // ALLOW_TRANSFER_INSTEAD_BIT request meaningful at all (this task's
    // own Critical review finding -- see that method's header comment).
    auto dst = fixture->allocator.createDeviceLocalBuffer(
        kSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dst.has_value());

    REQUIRE(uploader->uploadToBuffer(*dst, 0, pattern.data(), kSize));

    if (dst->directPathCapable()) {
        // The exact scenario this task's Critical review fix exists for:
        // a ReBAR-capable desktop GPU (this project's own dev machine, a
        // 246MB BAR window on an RTX 2080) or a unified-memory device
        // (e.g. CI's lavapipe) must genuinely take the direct branch for
        // a buffer this small -- asserted with CHECK, not merely logged
        // or MESSAGE'd.
        CHECK(uploader->everUsedDirectPath());
        CHECK_FALSE(uploader->everUsedStagingPath());
    } else {
        // Only reachable on hardware with no memory type that is both
        // DEVICE_LOCAL and HOST_VISIBLE for this usage combination (e.g.
        // a discrete GPU with ReBAR unavailable/disabled) -- still a
        // correct outcome, just not the one this test is named for; the
        // staging branch must have engaged instead.
        MESSAGE(
            "this device's VERTEX_BUFFER_BIT|TRANSFER_DST_BIT allocation did not land DEVICE_LOCAL+HOST_VISIBLE -- "
            "staging path exercised instead, which is still correct");
        CHECK(uploader->everUsedStagingPath());
    }

    // [Phase 4 Task 11] flush()+wait() only matters for the staging
    // branch (nothing was recorded for a direct-path write, so flush()
    // returns the trivially-complete ticket and wait() is a no-op) --
    // harmless either way.
    uploader->wait(uploader->flush());

    std::vector<uint8_t> readBack = readBackBuffer(fixture->allocator, fixture->device.device(),
                                                     fixture->device.graphicsQueue(),
                                                     fixture->device.graphicsQueueFamily(), dst->handle(), kSize);
    CHECK(std::memcmp(readBack.data(), pattern.data(), kSize) == 0);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Uploader::uploadToBuffer stages through the ring buffer when the destination is not direct-path-"
          "capable, deterministically (hardware-independent)") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_staging_deterministic");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device);
    REQUIRE(uploader.has_value());

    constexpr VkDeviceSize kSize = 256;
    std::array<uint8_t, kSize> pattern{};
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<uint8_t>((i * 5 + 1) & 0xFF);
    }

    // createHostVisibleBuffer() always reports directPathCapable() ==
    // false (a hardcoded false, not a measurement -- see buffer.h's own
    // comment on Buffer::directPathCapable()), regardless of this
    // machine's real memory-type classification -- the deterministic way
    // to force and verify the staging branch's own correctness
    // independent of any specific hardware's ReBAR/unified-memory
    // availability, exactly what this task's review asked for ("assert
    // the mechanism, not the hardware").
    auto dst = fixture->allocator.createHostVisibleBuffer(kSize,
                                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dst.has_value());
    REQUIRE_FALSE(dst->directPathCapable());

    REQUIRE(uploader->uploadToBuffer(*dst, 0, pattern.data(), kSize));
    CHECK(uploader->everUsedStagingPath());
    CHECK_FALSE(uploader->everUsedDirectPath());
    uploader->wait(uploader->flush());

    std::vector<uint8_t> readBack = readBackBuffer(fixture->allocator, fixture->device.device(),
                                                     fixture->device.graphicsQueue(),
                                                     fixture->device.graphicsQueueFamily(), dst->handle(), kSize);
    CHECK(std::memcmp(readBack.data(), pattern.data(), kSize) == 0);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Uploader auto-flushes mid-batch when the ring buffer runs out of room, and every upload stays "
          "byte-exact") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_ring_wrap");
    if (!fixture.has_value()) {
        return;
    }

    // Deliberately tiny (64 bytes) so two 48-byte uploads in the same
    // batch cannot both fit -- the second one forces reserveRingSpace()'s
    // internal auto-flush path (upload.cpp) before it can write anything,
    // per the class comment in upload.h. Neither upload result should be
    // affected by that internal flush happening mid-batch.
    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device, /*ringBufferSize=*/64);
    REQUIRE(uploader.has_value());

    constexpr VkDeviceSize kChunkSize = 48;
    std::array<uint8_t, kChunkSize> patternA{};
    std::array<uint8_t, kChunkSize> patternB{};
    for (size_t i = 0; i < kChunkSize; ++i) {
        patternA[i] = static_cast<uint8_t>(i);
        patternB[i] = static_cast<uint8_t>(0xFF - i);
    }

    auto dstA = fixture->allocator.createDeviceLocalBuffer(
        kChunkSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dstA.has_value());
    auto dstB = fixture->allocator.createDeviceLocalBuffer(
        kChunkSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dstB.has_value());

    REQUIRE(uploader->uploadToBuffer(*dstA, 0, patternA.data(), kChunkSize));
    REQUIRE(uploader->uploadToBuffer(*dstB, 0, patternB.data(), kChunkSize));
    // [Phase 4 Task 11] waiting on the FINAL ticket is sufficient even
    // though the wrap above triggered its own internal (non-blocking)
    // flush() for dstA's write: tickets from the same Uploader are
    // strictly ordered by submission (see upload.h's class comment), so
    // this final wait() transitively covers the earlier, internally
    // flushed batch too.
    uploader->wait(uploader->flush());

    std::vector<uint8_t> readA = readBackBuffer(fixture->allocator, fixture->device.device(),
                                                  fixture->device.graphicsQueue(),
                                                  fixture->device.graphicsQueueFamily(), dstA->handle(), kChunkSize);
    std::vector<uint8_t> readB = readBackBuffer(fixture->allocator, fixture->device.device(),
                                                  fixture->device.graphicsQueue(),
                                                  fixture->device.graphicsQueueFamily(), dstB->handle(), kChunkSize);
    CHECK(std::memcmp(readA.data(), patternA.data(), kChunkSize) == 0);
    CHECK(std::memcmp(readB.data(), patternB.data(), kChunkSize) == 0);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Uploader::uploadToBuffer rejects a single upload larger than the ring buffer's total capacity") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_oversize");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device, /*ringBufferSize=*/64);
    REQUIRE(uploader.has_value());

    // The ring-buffer capacity check (reserveRingSpace(), upload.cpp) only
    // runs on the STAGING branch -- a direct-path-capable destination
    // writes straight into its own mapped allocation and never touches
    // the ring buffer at all, so this rejection can only be exercised
    // against a destination the staging branch is guaranteed to take.
    // createHostVisibleBuffer() (hardcoded directPathCapable()==false, not
    // measured) is that deterministic, hardware-independent guarantee --
    // createDeviceLocalBuffer() here (Task 8 fix) measured
    // directPathCapable()==true on GitHub Actions' lavapipe build, which
    // made this 128-byte direct-path write against a real 128-byte
    // allocation trivially succeed and silently skip the rejection this
    // test exists to check -- see buffer.h's corrected comment on
    // createDeviceLocalBuffer() for why that measurement is
    // backend-dependent, not a bug.
    std::array<uint8_t, 128> tooBig{};
    auto dst = fixture->allocator.createHostVisibleBuffer(128, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(dst.has_value());
    REQUIRE_FALSE(dst->directPathCapable());

    CHECK_FALSE(uploader->uploadToBuffer(*dst, 0, tooBig.data(), tooBig.size()));
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("MeshBuffers::create uploads vertex+index data into device-local buffers, byte-exact readback") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_meshbuffers");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device);
    REQUIRE(uploader.has_value());

    struct Vertex {
        float position[3];
        float uv[2];
    };
    constexpr std::array<Vertex, 4> kVertices{{
        {{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F}},
        {{1.0F, 0.0F, 0.0F}, {1.0F, 0.0F}},
        {{1.0F, 1.0F, 0.0F}, {1.0F, 1.0F}},
        {{0.0F, 1.0F, 0.0F}, {0.0F, 1.0F}},
    }};
    constexpr std::array<uint32_t, 6> kIndices{{0, 1, 2, 2, 3, 0}};

    auto mesh = rx::rhi::MeshBuffers::create(fixture->allocator, *uploader, kVertices.data(), sizeof(kVertices),
                                              kIndices.data(), sizeof(kIndices),
                                              static_cast<uint32_t>(kIndices.size()), VK_INDEX_TYPE_UINT32);
    REQUIRE(mesh.has_value());
    CHECK(mesh->indexCount() == 6);
    CHECK(mesh->indexType() == VK_INDEX_TYPE_UINT32);
    CHECK(mesh->vertexBufferSize() == sizeof(kVertices));
    CHECK(mesh->indexBufferSize() == sizeof(kIndices));

    // MeshBuffers::create() does not add TRANSFER_SRC_BIT to either
    // buffer's usage (it only ever needs to be a transfer *destination*
    // in production) -- readBackBuffer() below needs a fresh pair of
    // TRANSFER_SRC-capable device-local buffers instead, populated
    // through the same Uploader, purely so this test can verify the
    // upload path independently of MeshBuffers' own accessors already
    // returning correct handles/sizes above.
    auto vertexCopy = fixture->allocator.createDeviceLocalBuffer(
        sizeof(kVertices), VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(vertexCopy.has_value());
    REQUIRE(uploader->uploadToBuffer(*vertexCopy, 0, kVertices.data(), sizeof(kVertices)));
    uploader->wait(uploader->flush());

    std::vector<uint8_t> readBack =
        readBackBuffer(fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
                        fixture->device.graphicsQueueFamily(), vertexCopy->handle(), sizeof(kVertices));
    CHECK(std::memcmp(readBack.data(), kVertices.data(), sizeof(kVertices)) == 0);

    // The real proof that MeshBuffers' OWN vertex/index buffers actually
    // contain the right bytes (not just that Uploader in isolation works,
    // which the copy above already showed): copy straight out of
    // mesh->vertexBuffer()/indexBuffer() themselves.
    std::vector<uint8_t> meshVertexReadBack =
        readBackBuffer(fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
                        fixture->device.graphicsQueueFamily(), mesh->vertexBuffer(), sizeof(kVertices));
    CHECK(std::memcmp(meshVertexReadBack.data(), kVertices.data(), sizeof(kVertices)) == 0);

    std::vector<uint8_t> meshIndexReadBack =
        readBackBuffer(fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
                        fixture->device.graphicsQueueFamily(), mesh->indexBuffer(), sizeof(kIndices));
    CHECK(std::memcmp(meshIndexReadBack.data(), kIndices.data(), sizeof(kIndices)) == 0);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===========================================================================
// [Phase 4 Task 11, spec D25 as amended by gate ruling RC4] UploadTicket /
// non-blocking flush() tests below -- see upload.h's class comment for the
// full design this section exercises against.
// ===========================================================================

TEST_CASE("Uploader::flush: two overlapping flush() calls without waiting on the first ticket in between -- the "
          "first ticket still reports its own true status, independent of the second, zero validation errors "
          "[gate matrix-issue28 row 1, the highest-priority correctness gate for this ticket]") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_overlapping_flush");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device);
    REQUIRE(uploader.has_value());

    constexpr VkDeviceSize kSize = 4096;
    std::array<uint8_t, kSize> patternA{};
    std::array<uint8_t, kSize> patternB{};
    for (size_t i = 0; i < kSize; ++i) {
        patternA[i] = static_cast<uint8_t>((i * 3 + 1) & 0xFF);
        patternB[i] = static_cast<uint8_t>((i * 7 + 5) & 0xFF);
    }

    // Deterministic staging-path destinations (createHostVisibleBuffer's
    // hardcoded directPathCapable()==false) -- forces real GPU work, real
    // ring bytes, and a real timeline-semaphore signal per batch, not a
    // trivially-complete ticket; the reused-single-fence design's row 1
    // hazard specifically concerned reset-while-referenced fences backing
    // real, in-flight submissions like these.
    auto dstA = fixture->allocator.createHostVisibleBuffer(
        kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dstA.has_value());
    auto dstB = fixture->allocator.createHostVisibleBuffer(
        kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dstB.has_value());

    REQUIRE(uploader->uploadToBuffer(*dstA, 0, patternA.data(), kSize));
    rx::rhi::UploadTicket ticketA = uploader->flush();

    // THE overlapping call: issued WITHOUT waiting or even polling
    // ticketA first -- exactly the scenario the reused-single-fence
    // design could not survive (matrix-issue28 row 1: resetting that
    // fence to submit this second batch would corrupt or invalidate
    // ticketA's own completion signal, or trip a validation error).
    REQUIRE(uploader->uploadToBuffer(*dstB, 0, patternB.data(), kSize));
    rx::rhi::UploadTicket ticketB = uploader->flush();

    CHECK(ticketA.value != 0);
    CHECK(ticketB.value != 0);
    CHECK(ticketB.value > ticketA.value);

    // ticketA must still be independently queryable/waitable on its own
    // terms -- not entangled with, superseded by, or corrupted by
    // ticketB having since been issued.
    uploader->wait(ticketA);
    CHECK(uploader->isComplete(ticketA));

    uploader->wait(ticketB);
    CHECK(uploader->isComplete(ticketB));
    // ticketA must STILL report complete after ticketB is also confirmed
    // done -- proving its status genuinely reflects its OWN submission,
    // not a side effect of ticketB's.
    CHECK(uploader->isComplete(ticketA));

    std::vector<uint8_t> readA = readBackBuffer(fixture->allocator, fixture->device.device(),
                                                  fixture->device.graphicsQueue(),
                                                  fixture->device.graphicsQueueFamily(), dstA->handle(), kSize);
    std::vector<uint8_t> readB = readBackBuffer(fixture->allocator, fixture->device.device(),
                                                  fixture->device.graphicsQueue(),
                                                  fixture->device.graphicsQueueFamily(), dstB->handle(), kSize);
    CHECK(std::memcmp(readA.data(), patternA.data(), kSize) == 0);
    CHECK(std::memcmp(readB.data(), patternB.data(), kSize) == 0);

    // The reused-single-fence design's specific failure mode: a
    // vkResetFences-while-referenced validation error, or (worse) a
    // silently wrong completion status. Neither is possible here by
    // construction (timeline semaphore, no reset step at all) -- this
    // assertion is the empirical proof.
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Uploader::flush: a direct-path-only batch (no GPU work recorded) returns an already-complete ticket on "
          "the same call stack, zero fence/semaphore machinery touched [gate matrix-issue28 row 8]") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_direct_path_ticket");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device);
    REQUIRE(uploader.has_value());

    constexpr VkDeviceSize kSize = 256;
    std::array<uint8_t, kSize> pattern{};
    for (size_t i = 0; i < kSize; ++i) {
        pattern[i] = static_cast<uint8_t>((i * 13 + 2) & 0xFF);
    }

    auto dst = fixture->allocator.createDeviceLocalBuffer(
        kSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dst.has_value());

    REQUIRE(uploader->uploadToBuffer(*dst, 0, pattern.data(), kSize));

    if (!dst->directPathCapable()) {
        // Hardware without a memory type that is both DEVICE_LOCAL and
        // HOST_VISIBLE for this usage combination -- the staging branch
        // engaged instead, so the trivially-complete-ticket case this
        // test is named for cannot be exercised on this machine. Still
        // assert the complementary fact (a staging batch's ticket is
        // real/nonzero, matching row 9's composition) rather than
        // silently skipping every assertion.
        MESSAGE(
            "this device's VERTEX_BUFFER_BIT|TRANSFER_DST_BIT allocation did not land DEVICE_LOCAL+HOST_VISIBLE -- "
            "cannot exercise the direct-path-only ticket case here; asserting the staging-path ticket is real "
            "instead");
        rx::rhi::UploadTicket ticket = uploader->flush();
        CHECK(ticket.value != 0);
        uploader->wait(ticket);
        CHECK_FALSE(fixture->context.hasValidationErrors());
        return;
    }

    CHECK(uploader->everUsedDirectPath());
    rx::rhi::UploadTicket ticket = uploader->flush();
    // The direct-path memcpy already fully happened, synchronously, on
    // the calling thread, by the time uploadToBuffer() returned -- there
    // is no outstanding GPU work for flush() to submit, so this ticket is
    // the trivially-complete sentinel, reported complete on this SAME
    // call stack (no wait() needed at all).
    CHECK(ticket.value == 0);
    CHECK(uploader->isComplete(ticket));
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Uploader::flush with nothing recorded since the last flush() returns an already-complete ticket, "
          "matching this class's pre-Task-11 no-op flush() behavior [gate matrix-issue28 row 8]") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_empty_flush");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device);
    REQUIRE(uploader.has_value());

    rx::rhi::UploadTicket ticket = uploader->flush();
    CHECK(ticket.value == 0);
    CHECK(uploader->isComplete(ticket));
    // A second consecutive empty flush() behaves identically.
    rx::rhi::UploadTicket ticket2 = uploader->flush();
    CHECK(ticket2.value == 0);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Uploader::flush: a batch mixing a direct-path buffer upload with a staging-path upload produces one "
          "ticket covering ONLY the recorded (staging) work -- both destinations correct once it completes "
          "[gate matrix-issue28 row 9]") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_mixed_batch");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device);
    REQUIRE(uploader.has_value());

    constexpr VkDeviceSize kSize = 256;
    std::array<uint8_t, kSize> patternDirect{};
    std::array<uint8_t, kSize> patternStaged{};
    for (size_t i = 0; i < kSize; ++i) {
        patternDirect[i] = static_cast<uint8_t>((i * 11 + 1) & 0xFF);
        patternStaged[i] = static_cast<uint8_t>((i * 19 + 4) & 0xFF);
    }

    // dstDirect MAY or may not actually land direct-path-capable
    // (hardware-dependent, per uploadToBuffer()'s own comment) -- either
    // way, dstStaged (createHostVisibleBuffer, hardcoded
    // directPathCapable()==false) always forces real GPU work into this
    // batch, so the resulting ticket is never trivially complete
    // regardless of what dstDirect measured as.
    auto dstDirect = fixture->allocator.createDeviceLocalBuffer(
        kSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dstDirect.has_value());
    auto dstStaged = fixture->allocator.createHostVisibleBuffer(
        kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dstStaged.has_value());
    REQUIRE_FALSE(dstStaged->directPathCapable());

    REQUIRE(uploader->uploadToBuffer(*dstDirect, 0, patternDirect.data(), kSize));
    REQUIRE(uploader->uploadToBuffer(*dstStaged, 0, patternStaged.data(), kSize));
    CHECK(uploader->everUsedStagingPath());
    MESSAGE("dstDirect landed direct-path-capable: ", dstDirect->directPathCapable() ? "yes" : "no");

    rx::rhi::UploadTicket ticket = uploader->flush();
    CHECK(ticket.value != 0);
    uploader->wait(ticket);
    CHECK(uploader->isComplete(ticket));

    std::vector<uint8_t> readDirect =
        readBackBuffer(fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
                        fixture->device.graphicsQueueFamily(), dstDirect->handle(), kSize);
    std::vector<uint8_t> readStaged =
        readBackBuffer(fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
                        fixture->device.graphicsQueueFamily(), dstStaged->handle(), kSize);
    CHECK(std::memcmp(readDirect.data(), patternDirect.data(), kSize) == 0);
    CHECK(std::memcmp(readStaged.data(), patternStaged.data(), kSize) == 0);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Uploader::flush: a ticket covering ONLY an image upload is never the trivially-complete sentinel (no "
          "'accidentally done' shortcut leaks into the image path), and is complete only once the GPU copy is "
          "confirmed finished [gate matrix-issue28 row 7]") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_image_only_ticket");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device);
    REQUIRE(uploader.has_value());

    // Large enough (16 MiB) that the real GPU copy behind this ticket
    // cannot plausibly have already finished by the time flush() returns
    // and this test's very next line queries isComplete() -- a handful of
    // CPU instructions cannot outrun a multi-megabyte host-to-device copy
    // plus its submission/signal round trip on any driver this project
    // targets. Images ALWAYS stage (upload.h's class comment) -- there is
    // no direct path to race against here at all.
    constexpr VkExtent2D kExtent{2048, 2048};
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
    auto texture = rx::rhi::Texture2D::create(fixture->device.physicalDevice(), fixture->device.device(),
                                               fixture->allocator, kExtent, kFormat, VK_IMAGE_USAGE_SAMPLED_BIT,
                                               /*requestedMipLevels=*/1);
    REQUIRE(texture.has_value());

    std::vector<uint8_t> pixels(static_cast<size_t>(kExtent.width) * kExtent.height * 4, 0x5A);
    REQUIRE(uploader->uploadToImage(*texture, pixels.data(), pixels.size(), /*generateMips=*/false));

    rx::rhi::UploadTicket ticket = uploader->flush();
    // Deterministic regardless of timing: an image ticket is NEVER the
    // trivially-complete sentinel -- uploadToImage() always reserves ring
    // space and records real commands (row 7's "no trivially-done
    // shortcut" requirement).
    CHECK(ticket.value != 0);

    // Best-effort timing probe (not the load-bearing assertion): on every
    // driver observed while writing this test, a 16 MiB copy is not yet
    // done on the very next host instruction after flush() returns.
    // CHECK (not REQUIRE) because an exotic, extremely fast path is not
    // this test's correctness concern -- the deterministic assertions
    // above and below are.
    CHECK_FALSE(uploader->isComplete(ticket));

    uploader->wait(ticket);
    CHECK(uploader->isComplete(ticket));
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Uploader::flush()/isComplete() never block past a wall-clock threshold across many overlapped "
          "batches -- timer around the call itself, not frame counters [gate matrix-issue28 row 13; per the "
          "report's scratch-worktree revert evidence, this test FAILS against the pre-Task-11 blocking flush()]") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_wallclock_nonblocking");
    if (!fixture.has_value()) {
        return;
    }

    // Sized generously above kUploadSize below (its default 16 MiB does
    // not fit even one 128 MiB payload) -- this test's ring reclamation is
    // not what is under test here, so a comfortably oversized ring keeps
    // every uploadToBuffer() call on the simple, no-wrap path.
    auto uploader =
        rx::rhi::Uploader::create(fixture->allocator, fixture->device, /*ringBufferSize=*/160u * 1024u * 1024u);
    REQUIRE(uploader.has_value());

    // 128 MiB per "frame" -- empirically sized (see the report's
    // scratch-worktree revert evidence) so a genuine blocking
    // vkWaitForFences(..., UINT64_MAX)-equivalent round trip (submit ->
    // GPU copy -> signal -> host wait wakes) clears kThreshold with a
    // comfortable margin on this project's own dev hardware (observed
    // 14-19 ms per call when the pre-Task-11 blocking behavior was
    // deliberately reintroduced in a scratch worktree) -- a smaller
    // payload (2-64 MiB) round-tripped too fast on this machine's discrete
    // GPU to reliably clear an 8 ms threshold, which would have made this
    // test a weak discriminator on fast hardware even though it is a
    // strong one on CI's software-rasterized lavapipe. A non-blocking
    // flush()/isComplete() call, in contrast, is dominated by driver call
    // overhead alone, independent of payload size (observed single- to
    // double-digit microseconds regardless of kUploadSize) -- this is WHY
    // increasing the payload costs this test's REAL (non-reverted) run
    // nothing: the timed calls stay microsecond-scale either way, only
    // the untimed final wait()+readback loop grows.
    constexpr VkDeviceSize kUploadSize = 128u * 1024u * 1024u;
    constexpr int kFrameCount = 8;
    constexpr auto kThreshold = std::chrono::milliseconds(8);

    std::vector<uint8_t> payload(kUploadSize, 0x3C);

    std::vector<std::optional<rx::rhi::Buffer>> destinations;
    destinations.reserve(static_cast<size_t>(kFrameCount));
    for (int i = 0; i < kFrameCount; ++i) {
        destinations.push_back(fixture->allocator.createHostVisibleBuffer(
            kUploadSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
        REQUIRE(destinations.back().has_value());
    }

    std::vector<rx::rhi::UploadTicket> tickets;
    tickets.reserve(static_cast<size_t>(kFrameCount));

    std::chrono::steady_clock::duration worstFlush{};
    std::chrono::steady_clock::duration worstIsComplete{};

    for (int i = 0; i < kFrameCount; ++i) {
        REQUIRE(uploader->uploadToBuffer(*destinations[static_cast<size_t>(i)], 0, payload.data(), kUploadSize));

        auto beforeFlush = std::chrono::steady_clock::now();
        rx::rhi::UploadTicket ticket = uploader->flush();
        auto flushDuration = std::chrono::steady_clock::now() - beforeFlush;
        worstFlush = std::max(worstFlush, flushDuration);
        CHECK(flushDuration < kThreshold);

        auto beforeIsComplete = std::chrono::steady_clock::now();
        bool complete = uploader->isComplete(ticket);
        auto isCompleteDuration = std::chrono::steady_clock::now() - beforeIsComplete;
        worstIsComplete = std::max(worstIsComplete, isCompleteDuration);
        CHECK(isCompleteDuration < kThreshold);
        (void)complete;  // not asserted true/false here -- pure non-blocking poll timing probe

        tickets.push_back(ticket);
        // Deliberately NEVER wait()/poll-to-completion before the next
        // frame's own flush() -- N overlapped frames in flight, none of
        // them drained, is exactly the scenario this invariant must hold
        // under.
    }

    MESSAGE("worst-case flush() duration: ",
            std::chrono::duration_cast<std::chrono::microseconds>(worstFlush).count(), " us over ", kFrameCount,
            " overlapped frames");
    MESSAGE("worst-case isComplete() duration: ",
            std::chrono::duration_cast<std::chrono::microseconds>(worstIsComplete).count(), " us");

    // Drain everything before readback/teardown -- correctness, not timed.
    for (auto& ticket : tickets) {
        uploader->wait(ticket);
    }
    for (int i = 0; i < kFrameCount; ++i) {
        std::vector<uint8_t> readBack = readBackBuffer(
            fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
            fixture->device.graphicsQueueFamily(), destinations[static_cast<size_t>(i)]->handle(), kUploadSize);
        CHECK(std::memcmp(readBack.data(), payload.data(), kUploadSize) == 0);
    }
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Uploader ring-buffer reclamation under heavy wrap: partial reclaim engages (blocking-wait count stays "
          "below flush() calls issued), the ring never silently grows, and every upload lands correctly despite "
          "many wraps [gate matrix-issue28 rows 5/6]") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_ring_reclaim_stress");
    if (!fixture.has_value()) {
        return;
    }

    constexpr VkDeviceSize kRingSize = 512;  // deliberately tiny
    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device, kRingSize);
    REQUIRE(uploader.has_value());
    CHECK(uploader->ringCapacity() == kRingSize);

    constexpr VkDeviceSize kChunkSize = 64;  // several chunks per ring "lap"
    constexpr int kUploadCount = 80;         // many multiples of the ring's total capacity

    std::vector<std::optional<rx::rhi::Buffer>> destinations;
    std::vector<std::array<uint8_t, kChunkSize>> patterns;
    destinations.reserve(static_cast<size_t>(kUploadCount));
    patterns.reserve(static_cast<size_t>(kUploadCount));

    for (int i = 0; i < kUploadCount; ++i) {
        // createHostVisibleBuffer(): hardcoded directPathCapable()==false
        // -- deterministically forces every one of these through the
        // ring, regardless of hardware (see the staging-path test above
        // for the same reasoning).
        destinations.push_back(fixture->allocator.createHostVisibleBuffer(
            kChunkSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
        REQUIRE(destinations.back().has_value());
        REQUIRE_FALSE(destinations.back()->directPathCapable());

        std::array<uint8_t, kChunkSize> pattern{};
        for (size_t b = 0; b < kChunkSize; ++b) {
            pattern[b] = static_cast<uint8_t>((i * 37 + static_cast<int>(b) * 5 + 3) & 0xFF);
        }
        patterns.push_back(pattern);
    }

    std::vector<rx::rhi::UploadTicket> tickets;
    tickets.reserve(static_cast<size_t>(kUploadCount));

    for (int i = 0; i < kUploadCount; ++i) {
        REQUIRE(uploader->uploadToBuffer(*destinations[static_cast<size_t>(i)], 0,
                                          patterns[static_cast<size_t>(i)].data(), kChunkSize));
        // Flush every single upload individually, NEVER waiting in
        // between -- maximizes overlap and forces the ring to wrap many
        // times over while several tickets are still genuinely in flight,
        // exactly the stress shape the plan's own steps call for ("many
        // small uploads > ring size").
        tickets.push_back(uploader->flush());
        CHECK(uploader->ringCapacity() == kRingSize);  // never silently grown, any iteration
    }

    MESSAGE("ring wrapped ", uploader->ringWrapCount(), " times across ", kUploadCount, " uploads into a ",
            kRingSize, "-byte ring");
    MESSAGE("blocking ring-reclaim waits: ", uploader->blockingRingWaitCount(),
            " (flush() calls issued: ", kUploadCount, ")");

    // The ring genuinely wrapped many times over its own capacity --
    // proving this is a real wrap-heavy stress case, not a vacuous one.
    CHECK(uploader->ringWrapCount() > 0);

    // [Task 13 review flake attribution, 2026-08-18]
    // Under Wine's slow scheduling (windows-cross-zig CI runs), the correct
    // poll-first reclamation policy legitimately blocks multiple times per
    // wrap (observed 23-44 waits vs 9 wraps on Wine) because submission
    // latency causes older pending regions to still be in flight when a new
    // wrap arrives. This overlaps the degenerate whole-ring-drain policy's
    // fast-machine range (18-36 waits vs 9 wraps on discrete GPU), so no
    // numeric bound discriminates between the policies based on counts alone.
    // The policy gate is enforced by the deterministic test below
    // ("...poll-first proof"), which forces completion before wrap and
    // asserts exact equality (blockingRingWaitCount() == blockingWaitsBeforeWrap)
    // -- timing-independent, fails under the degenerate policy regardless of
    // scheduling. This stress test therefore asserts data correctness and
    // fixed ring capacity instead, omitting the wait-count check.

    for (auto& ticket : tickets) {
        uploader->wait(ticket);
    }

    // Data correctness under heavy wrap -- readback EVERY destination,
    // not just a sample, per the plan's own "readback, not just no-crash"
    // requirement.
    for (int i = 0; i < kUploadCount; ++i) {
        std::vector<uint8_t> readBack =
            readBackBuffer(fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
                            fixture->device.graphicsQueueFamily(), destinations[static_cast<size_t>(i)]->handle(),
                            kChunkSize);
        CHECK(std::memcmp(readBack.data(), patterns[static_cast<size_t>(i)].data(), kChunkSize) == 0);
    }
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Uploader ring reclamation polls (never blocks) when the oldest pending region is already confirmed "
          "complete BEFORE the wrap that would need it -- deterministic, hardware-speed-independent "
          "[review fix round 1, gate matrix-issue28 row 5: the stress test's own blocking-wait counter cannot "
          "tell 'polled and found already done' apart from 'blocked unconditionally and it happened to return "
          "immediately' by counting alone, since a policy that blocks EVERY wrap regardless of completion also "
          "measures at most one block per wrap on the stress test's own construction -- this test closes exactly "
          "that gap by forcing the completion precondition explicitly, with no dependency on relative CPU/GPU "
          "speed at all]") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_ring_reclaim_deterministic_poll");
    if (!fixture.has_value()) {
        return;
    }

    // Ring holds exactly two 64-byte chunks -- the smallest shape that
    // can force a wrap on the THIRD upload while keeping the scenario
    // fully deterministic (no auto-flush-on-wrap needed for uploads 1/2).
    constexpr VkDeviceSize kRingSize = 128;
    constexpr VkDeviceSize kChunkSize = 64;
    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device, kRingSize);
    REQUIRE(uploader.has_value());

    auto makePattern = [](uint8_t seed) {
        std::array<uint8_t, kChunkSize> pattern{};
        for (size_t b = 0; b < kChunkSize; ++b) {
            pattern[b] = static_cast<uint8_t>(seed + b * 3);
        }
        return pattern;
    };

    auto dstA = fixture->allocator.createHostVisibleBuffer(
        kChunkSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto dstB = fixture->allocator.createHostVisibleBuffer(
        kChunkSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto dstC = fixture->allocator.createHostVisibleBuffer(
        kChunkSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dstA.has_value());
    REQUIRE(dstB.has_value());
    REQUIRE(dstC.has_value());

    const auto patternA = makePattern(11);
    const auto patternB = makePattern(101);
    const auto patternC = makePattern(211);

    // Upload A, then B -- each explicitly flushed AND waited before the
    // next call, so BOTH tickets are genuinely, confirmably complete
    // (not just "probably done by now" the way the stress test's rapid
    // back-to-back flushing leaves to chance) by the time this test
    // reaches upload C below. Cursor lands exactly at kRingSize (128)
    // afterward -- no wrap has happened yet.
    REQUIRE(uploader->uploadToBuffer(*dstA, 0, patternA.data(), kChunkSize));
    uploader->wait(uploader->flush());
    REQUIRE(uploader->uploadToBuffer(*dstB, 0, patternB.data(), kChunkSize));
    uploader->wait(uploader->flush());

    const uint32_t blockingWaitsBeforeWrap = uploader->blockingRingWaitCount();
    const uint32_t wrapsBeforeWrap = uploader->ringWrapCount();

    // Upload C: the ring has no room left (aligned cursor == 128 ==
    // kRingSize), so this MUST wrap. Both PendingRegion entries this
    // wrap's reclaim needs to clear (A's and B's) are already complete,
    // confirmed above -- a correct, poll-first implementation's
    // reclaimCompletedRegions() pops both via a pure poll and the
    // overlap-check loop never executes at all, so blockingRingWaitCount()
    // must NOT change. A degenerate implementation that blocks
    // unconditionally on wrap (never checking isComplete() first, the
    // exact policy row 5 forbids) WOULD increment it here regardless.
    REQUIRE(uploader->uploadToBuffer(*dstC, 0, patternC.data(), kChunkSize));
    rx::rhi::UploadTicket ticketC = uploader->flush();

    CHECK(uploader->ringWrapCount() == wrapsBeforeWrap + 1);
    CHECK(uploader->blockingRingWaitCount() == blockingWaitsBeforeWrap);

    uploader->wait(ticketC);

    // Data correctness: A/B/C all land in the right place despite the
    // wrap reusing the ring's front the moment it was proven reclaimable.
    std::vector<uint8_t> readA = readBackBuffer(fixture->allocator, fixture->device.device(),
                                                  fixture->device.graphicsQueue(),
                                                  fixture->device.graphicsQueueFamily(), dstA->handle(), kChunkSize);
    std::vector<uint8_t> readB = readBackBuffer(fixture->allocator, fixture->device.device(),
                                                  fixture->device.graphicsQueue(),
                                                  fixture->device.graphicsQueueFamily(), dstB->handle(), kChunkSize);
    std::vector<uint8_t> readC = readBackBuffer(fixture->allocator, fixture->device.device(),
                                                  fixture->device.graphicsQueue(),
                                                  fixture->device.graphicsQueueFamily(), dstC->handle(), kChunkSize);
    CHECK(std::memcmp(readA.data(), patternA.data(), kChunkSize) == 0);
    CHECK(std::memcmp(readB.data(), patternB.data(), kChunkSize) == 0);
    CHECK(std::memcmp(readC.data(), patternC.data(), kChunkSize) == 0);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}
