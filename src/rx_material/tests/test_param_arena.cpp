// Standalone ParamArena tests [Fix round 1, task-7-review.md F2]:
// instance.h's own header comment claims ParamArena "[l]ives at namespace
// scope... so it can be unit-tested on its own" -- this file is that test,
// exercising ParamArena directly (no MaterialSystem, no BindlessTable,
// no RenderGraph/Executor) against a bare VkDevice + rx::rhi::Allocator,
// mirroring rx_rhi_vk/tests/descriptor_arena_test.cpp's own headless
// fixture pattern for the identical reason: ParamArena needs neither
// descriptor-indexing nor dynamic-rendering/sync2 device features, so this
// is deliberately the most minimal fixture that can build one.
//
// Every case here specifically exercises MORE THAN frameInFlightIndex == 0
// -- the exact gap the review flagged (every existing bindInstance()-level
// test only ever called `beginFrame(0, ...)`).
#include <doctest/doctest.h>
#include <rx_material/instance.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/context.h>
#include <VkBootstrap.h>

#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace {

struct HeadlessParamArenaFixture {
    rx::rhi::Context context;
    vkb::Device vkbDevice;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
};

HeadlessParamArenaFixture makeFixture() {
    auto context = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    vkb::PhysicalDeviceSelector selector(context->vkbInstance());
    auto physResult = selector.set_minimum_version(1, 3).select();
    REQUIRE(physResult.has_value());

    auto deviceResult = vkb::DeviceBuilder(physResult.value()).build();
    REQUIRE(deviceResult.has_value());
    vkb::Device vkbDevice = deviceResult.value();
    volkLoadDevice(vkbDevice.device);

    return HeadlessParamArenaFixture{std::move(*context), vkbDevice, vkbDevice.device,
                                      vkbDevice.physical_device.physical_device};
}

// One VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER binding at binding 0 -- exactly the
// shape ParamArena::writeAndAllocate() allocates against (instance.cpp).
VkDescriptorSetLayout makeUboSetLayout(VkDevice device) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings = &binding;

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) == VK_SUCCESS);
    return layout;
}

}  // namespace

TEST_CASE("ParamArena: writes into different frame-in-flight slots are isolated, and beginFrame()'s reset (not "
          "merely a fresh arena) is what reclaims a slot's capacity on wraparound") {
    HeadlessParamArenaFixture fixture = makeFixture();

    {
        auto allocator =
            rx::rhi::Allocator::createRaw(fixture.physicalDevice, fixture.device, fixture.context.instance());
        REQUIRE(allocator.has_value());

        auto arena = rx::material::ParamArena::create(fixture.device, *allocator, /*framesInFlight=*/2);
        REQUIRE(arena.has_value());
        CHECK(arena->framesInFlight() == 2);

        VkDescriptorSetLayout layout = makeUboSetLayout(fixture.device);

        std::array<uint8_t, 16> blobA{};
        blobA.fill(0xAA);
        std::array<uint8_t, 16> blobB{};
        blobB.fill(0xBB);

        // --- Frame 0 (slot 0): write blob A.
        arena->beginFrame(0);
        VkDescriptorSet setA = arena->writeAndAllocate(layout, blobA.data(), blobA.size());
        REQUIRE(setA != VK_NULL_HANDLE);

        // --- Frame 1 (slot 1, a DIFFERENT frame-in-flight index than any
        // other case in this task's test suite exercises): write blob B.
        arena->beginFrame(1);
        VkDescriptorSet setB = arena->writeAndAllocate(layout, blobB.data(), blobB.size());
        REQUIRE(setB != VK_NULL_HANDLE);

        // Slot 0's bytes must be completely untouched by slot 1's own bump
        // allocation -- the two frame-in-flight slots are backed by
        // entirely separate host-visible buffers, never a shared one.
        const void* slot0Data = rx::material::detail::debugFrameBufferData(*arena, 0);
        REQUIRE(slot0Data != nullptr);
        CHECK(std::memcmp(slot0Data, blobA.data(), blobA.size()) == 0);

        const void* slot1Data = rx::material::detail::debugFrameBufferData(*arena, 1);
        REQUIRE(slot1Data != nullptr);
        CHECK(std::memcmp(slot1Data, blobB.data(), blobB.size()) == 0);

        // --- Frame 2 (frameIndex % 2 == 0 -- wraps back to slot 0):
        // beginFrame() must RESET slot 0's cursor -- write a third blob and
        // confirm it lands at the START of slot 0's buffer again (byte-
        // exact at offset 0), proving the reset actually happened rather
        // than this just being a fresh, still-empty arena coincidentally
        // agreeing.
        arena->beginFrame(2);
        std::array<uint8_t, 16> blobC{};
        blobC.fill(0xCC);
        VkDescriptorSet setC = arena->writeAndAllocate(layout, blobC.data(), blobC.size());
        REQUIRE(setC != VK_NULL_HANDLE);

        const void* slot0DataAfterWrap = rx::material::detail::debugFrameBufferData(*arena, 0);
        REQUIRE(slot0DataAfterWrap != nullptr);
        CHECK(std::memcmp(slot0DataAfterWrap, blobC.data(), blobC.size()) == 0);

        // Slot 1 (frame 1's own blob B) must remain completely untouched by
        // frame 2's write into slot 0 -- confirms isolation holds across a
        // wraparound too, not just on the first pass through each slot.
        const void* slot1DataStillB = rx::material::detail::debugFrameBufferData(*arena, 1);
        REQUIRE(slot1DataStillB != nullptr);
        CHECK(std::memcmp(slot1DataStillB, blobB.data(), blobB.size()) == 0);

        vkDestroyDescriptorSetLayout(fixture.device, layout, nullptr);
        // `allocator`/`arena` (RAII) go out of scope at the end of this
        // block, before the device itself is destroyed below.
    }

    vkDeviceWaitIdle(fixture.device);
    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}

TEST_CASE("ParamArena: byte-arena exhaustion and descriptor-pool exhaustion at a non-zero frame-in-flight index "
          "fail cleanly (VK_NULL_HANDLE), never corrupting an already-written blob; cursor does NOT advance on failure") {
    HeadlessParamArenaFixture fixture = makeFixture();

    {
        auto allocator =
            rx::rhi::Allocator::createRaw(fixture.physicalDevice, fixture.device, fixture.context.instance());
        REQUIRE(allocator.has_value());

        auto arena = rx::material::ParamArena::create(fixture.device, *allocator, /*framesInFlight=*/2);
        REQUIRE(arena.has_value());

        VkDescriptorSetLayout layout = makeUboSetLayout(fixture.device);

        // --- Byte-arena exhaustion, at frameInFlightIndex == 1 (not 0) ---
        // A single request larger than the whole frame's documented byte
        // budget must fail cleanly.
        arena->beginFrame(1);
        std::vector<uint8_t> tooBig(static_cast<size_t>(rx::material::ParamArena::kBytesPerFrame) + 1, 0x11);
        CHECK(arena->writeAndAllocate(layout, tooBig.data(), tooBig.size()) == VK_NULL_HANDLE);

        // The failed oversized request must not have corrupted this slot:
        // a normal-sized write right after it still succeeds and lands
        // with byte-exact content at the start of the (still-empty) slot.
        std::array<uint8_t, 16> blob{};
        blob.fill(0x22);
        VkDescriptorSet normalSet = arena->writeAndAllocate(layout, blob.data(), blob.size());
        REQUIRE(normalSet != VK_NULL_HANDLE);
        const void* slot1Data = rx::material::detail::debugFrameBufferData(*arena, 1);
        REQUIRE(slot1Data != nullptr);
        CHECK(std::memcmp(slot1Data, blob.data(), blob.size()) == 0);

        // --- Descriptor-pool exhaustion: prove the cursor genuinely does NOT
        // advance on a failed allocate() (instance.cpp: `cursor = end;` now
        // sits AFTER the `set == VK_NULL_HANDLE` check, not before it).
        //
        // Why this needs more than "one more call fails cleanly": once the
        // descriptor arena's arena-enforced maxSets budget
        // (kMaxInstancesPerFrame, matched 1:1 against ParamArena's own
        // constant -- see the [Post-release fix, CI lavapipe run acfce89]
        // comment further down) is exhausted, EVERY subsequent
        // writeAndAllocate() call on this frame slot fails on the
        // descriptor check -- permanently, until the next beginFrame(),
        // which would also rewind the byte cursor back to 0 and destroy
        // the very evidence being looked for. So there is no way to get a
        // SUCCESSFUL write after the failure to prove the point; the
        // discriminator instead has to come from TWO CONSECUTIVE FAILING
        // calls.
        //
        // writeAndAllocate()'s memcpy() always runs (into the byte arena,
        // at the offset the CURRENT cursor computes) before the descriptor
        // check, on every call, success or failure alike -- that hasn't
        // changed by the fix, only what happens to the cursor VARIABLE
        // afterward has:
        //   - Fixed code (cursor advances only on success): the cursor
        //     stays parked at the same value across repeated failures, so
        //     failing call #2's memcpy lands at the EXACT SAME offset as
        //     failing call #1's and overwrites it.
        //   - Pre-fix/reverted code (cursor advances unconditionally,
        //     before the descriptor check): failing call #1 still moves
        //     the cursor forward by one blob width despite failing, so
        //     failing call #2 lands one blob width FURTHER ALONG instead,
        //     leaving call #1's bytes untouched.
        // Reading back the bytes at the offset both calls compute from
        // therefore reads as failing-call-#2's sentinel with the fix, and
        // failing-call-#1's (unchanged) sentinel with the bug -- an
        // objective, deterministic distinguisher that never depends on
        // uninitialized memory content.
        //
        // Every blob below is exactly kUniformBufferAlignment (256) bytes,
        // so each successful write's OWN aligned offset advances the
        // cursor by exactly one blob width with zero rounding waste:
        // write i (0-indexed) always lands at i * kUniformBufferAlignment.
        arena->beginFrame(1);

        std::array<uint8_t, static_cast<std::size_t>(rx::material::ParamArena::kUniformBufferAlignment)> fillBlob{};
        fillBlob.fill(0x22);

        // (a) One successful writeAndAllocate -- record where its bytes land
        // (offset 0, the start of the freshly-reset slot).
        VkDescriptorSet firstSet = arena->writeAndAllocate(layout, fillBlob.data(), fillBlob.size());
        REQUIRE(firstSet != VK_NULL_HANDLE);
        const void* slot1DataFirst = rx::material::detail::debugFrameBufferData(*arena, 1);
        REQUIRE(slot1DataFirst != nullptr);
        CHECK(std::memcmp(slot1DataFirst, fillBlob.data(), fillBlob.size()) == 0);

        // [Post-release fix, CI lavapipe run acfce89] This now exercises
        // rx::rhi::DescriptorArena's own arena-enforced maxSets/uniformBuffers
        // budget (descriptor_arena.h's class-level BUDGETS ARE
        // ARENA-ENFORCED comment), not driver-side VkDescriptorPool
        // enforcement -- ParamArena::create() sizes both of that budget's
        // fields to kMaxInstancesPerFrame (instance.cpp), so filling to that
        // ceiling hits the SAME arena-enforced limit deterministically on
        // every driver, including lavapipe (which legally never enforces
        // VkDescriptorPool's own limits itself -- see
        // rx_rhi_vk/tests/descriptor_arena_test.cpp for that class's own
        // direct, per-budget coverage). Fill the remaining budget (one
        // allocation already done above).
        uint32_t successfulAllocations = 1;
        for (; successfulAllocations < rx::material::ParamArena::kMaxInstancesPerFrame; ++successfulAllocations) {
            VkDescriptorSet s = arena->writeAndAllocate(layout, fillBlob.data(), fillBlob.size());
            REQUIRE(s != VK_NULL_HANDLE);
        }
        CHECK(successfulAllocations == rx::material::ParamArena::kMaxInstancesPerFrame);

        // The offset the NEXT writeAndAllocate() call will compute from,
        // PROVIDED the cursor has not moved since the last success --
        // exactly kMaxInstancesPerFrame full-width blobs in. Byte-arena
        // headroom check: kMaxInstancesPerFrame * kUniformBufferAlignment =
        // 512 * 256 = 131072 bytes used; kBytesPerFrame is 1 MiB
        // (1048576 bytes), leaving ~896 KiB free -- ample room for two more
        // 256-byte writes to land inside the buffer under EITHER cursor
        // behavior below, so this test measures descriptor-pool exhaustion
        // only, never byte-arena exhaustion.
        constexpr VkDeviceSize kExpectedStuckOffset =
            static_cast<VkDeviceSize>(rx::material::ParamArena::kMaxInstancesPerFrame) *
            rx::material::ParamArena::kUniformBufferAlignment;

        // (b) One writeAndAllocate call that FAILS (descriptor budget
        // exhausted). Its own memcpy still executes at whatever offset the
        // cursor is CURRENTLY at -- kExpectedStuckOffset, identical in old
        // and new code for THIS specific call, since no divergence has
        // happened yet.
        std::array<uint8_t, static_cast<std::size_t>(rx::material::ParamArena::kUniformBufferAlignment)> sentinel1{};
        sentinel1.fill(0x77);
        CHECK(arena->writeAndAllocate(layout, sentinel1.data(), sentinel1.size()) == VK_NULL_HANDLE);

        // (c) ANOTHER writeAndAllocate call on the same frame slot -- also
        // fails on the descriptor check (maxSets stays exhausted until the
        // next beginFrame()), but its memcpy's own destination offset is
        // exactly where the discrimination happens: with the fix, the
        // cursor never moved after (b)'s failure, so this write re-enters
        // at kExpectedStuckOffset and overwrites sentinel1 with sentinel2;
        // with the pre-fix bug, (b)'s failure already advanced the cursor
        // past kExpectedStuckOffset once, so this write lands one blob
        // width further along instead, leaving sentinel1 untouched.
        std::array<uint8_t, static_cast<std::size_t>(rx::material::ParamArena::kUniformBufferAlignment)> sentinel2{};
        sentinel2.fill(0x99);
        CHECK(arena->writeAndAllocate(layout, sentinel2.data(), sentinel2.size()) == VK_NULL_HANDLE);

        const void* slot1DataAfterExhaustion = rx::material::detail::debugFrameBufferData(*arena, 1);
        REQUIRE(slot1DataAfterExhaustion != nullptr);
        const auto* stuckOffsetBytes = static_cast<const uint8_t*>(slot1DataAfterExhaustion) + kExpectedStuckOffset;

        // THE discriminator: with the fix, sentinel2 (the LATER failing
        // write) is what's actually sitting at the stuck offset, because
        // both failing calls kept landing on top of each other -- the
        // cursor never advanced on failure. With the pre-fix
        // cursor-advance-before-allocate bug reinstated, sentinel1 would
        // still be sitting there untouched instead (failing call (c) would
        // have landed one blob width further along), and this CHECK fails.
        CHECK(std::memcmp(stuckOffsetBytes, sentinel2.data(), sentinel2.size()) == 0);

        vkDestroyDescriptorSetLayout(fixture.device, layout, nullptr);
    }

    vkDeviceWaitIdle(fixture.device);
    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}
