#include <doctest/doctest.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/descriptor_arena.h>
#include <VkBootstrap.h>
#include <array>
#include <utility>

// Pure headless test -- no rx::platform::Window, no VkSurfaceKHR, no
// swapchain anywhere in this file, exactly like clear_color_test.cpp/
// pipeline_layout_test.cpp/bindless_test.cpp/deletion_queue_test.cpp (safe
// to run in any order alongside the windowed Device/Buffer tests in this
// binary because tests/doctest_main.cpp warms vk-bootstrap's process-wide
// instance-function cache before any TEST_CASE runs -- see the comment
// there). DescriptorArena needs neither descriptor-indexing nor
// dynamic-rendering/sync2 device features (unlike bindless_test.cpp's own
// fixture) -- it only ever creates/resets plain VkDescriptorPools and
// allocates ordinary (non-update-after-bind) VkDescriptorSets -- so this
// file's fixture is deliberately the most minimal headless device in this
// binary: a bare Vulkan 1.3 device, no required-features struct at all.
namespace {

struct HeadlessDescriptorArenaFixture {
    rx::rhi::Context context;
    vkb::Device vkbDevice;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
};

HeadlessDescriptorArenaFixture makeFixture() {
    auto context = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    vkb::PhysicalDeviceSelector selector(context->vkbInstance());
    auto physResult = selector.set_minimum_version(1, 3).select();
    REQUIRE(physResult.has_value());

    auto deviceResult = vkb::DeviceBuilder(physResult.value()).build();
    REQUIRE(deviceResult.has_value());
    vkb::Device vkbDevice = deviceResult.value();
    volkLoadDevice(vkbDevice.device);

    return HeadlessDescriptorArenaFixture{std::move(*context), vkbDevice, vkbDevice.device,
                                           vkbDevice.physical_device.physical_device};
}

// One VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER binding at binding 0 -- the exact
// shape rx_material's own ParamArena allocates against (instance.h), and
// enough to exercise allocate() with a real, non-trivial layout rather than
// an empty (zero-binding) one.
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

TEST_CASE("DescriptorArena::allocate across 3 simulated frames reuses reset pools with zero validation errors") {
    HeadlessDescriptorArenaFixture fixture = makeFixture();

    {
        rx::rhi::DescriptorArena::Capacities capacities{/*maxSets=*/4, /*uniformBuffers=*/4};
        auto arena = rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/2, capacities);
        REQUIRE(arena.has_value());
        CHECK(arena->framesInFlight() == 2);

        VkDescriptorSetLayout layout = makeUboSetLayout(fixture.device);

        // Real host-visible buffer to write a genuine UBO descriptor
        // against, so the sets this test allocates are exercised as more
        // than opaque handles -- proves allocate()'s sets are real,
        // vkUpdateDescriptorSets-legal objects, not just non-null.
        auto allocator =
            rx::rhi::Allocator::createRaw(fixture.physicalDevice, fixture.device, fixture.context.instance());
        REQUIRE(allocator.has_value());
        auto buffer = allocator->createHostVisibleBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        REQUIRE(buffer.has_value());

        auto writeUbo = [&](VkDescriptorSet set) {
            VkDescriptorBufferInfo bufferInfo{buffer->handle(), 0, 256};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo = &bufferInfo;
            vkUpdateDescriptorSets(fixture.device, 1, &write, 0, nullptr);
        };

        // --- Simulated frame 0 (slot 0): allocate up to this arena's own
        // maxSets, then confirm exhaustion is a clean VK_NULL_HANDLE, not a
        // crash or a validation error. [post-release fix, CI lavapipe run
        // acfce89] This is now DescriptorArena's own arena-enforced budget
        // firing (see descriptor_arena.h's class-level BUDGETS ARE
        // ARENA-ENFORCED comment) -- deterministic on every driver,
        // including lavapipe, which legally never enforces
        // VkDescriptorPool's maxSets/pool-size limits itself.
        arena->beginFrame(0);
        for (uint32_t i = 0; i < capacities.maxSets; ++i) {
            VkDescriptorSet set = arena->allocate(layout);
            REQUIRE(set != VK_NULL_HANDLE);
            writeUbo(set);
        }
        CHECK(arena->allocate(layout) == VK_NULL_HANDLE);

        // --- Simulated frame 1 (slot 1): a DIFFERENT pool, so allocation
        // succeeds again immediately -- proves the two frame-in-flight
        // slots are genuinely independent, not one shared pool.
        arena->beginFrame(1);
        VkDescriptorSet frame1Set = arena->allocate(layout);
        REQUIRE(frame1Set != VK_NULL_HANDLE);
        writeUbo(frame1Set);

        // --- Simulated frame 2 (back to slot 0, frameIndex % 2 == 0):
        // beginFrame() resets slot 0's pool, reclaiming the capacity
        // frame 0 fully consumed above -- allocation must succeed again,
        // proving reset (not merely "a fresh arena") is what freed it.
        arena->beginFrame(2);
        VkDescriptorSet frame2Set = arena->allocate(layout);
        REQUIRE(frame2Set != VK_NULL_HANDLE);
        writeUbo(frame2Set);
        for (uint32_t i = 1; i < capacities.maxSets; ++i) {
            REQUIRE(arena->allocate(layout) != VK_NULL_HANDLE);
        }
        CHECK(arena->allocate(layout) == VK_NULL_HANDLE);

        vkDestroyDescriptorSetLayout(fixture.device, layout, nullptr);
        // `buffer`/`allocator`/`arena` (RAII) go out of scope at the end of
        // this block, before the device itself is destroyed below -- same
        // reverse-destruction discipline every other rx_rhi_vk GPU test in
        // this binary documents.
    }

    vkDeviceWaitIdle(fixture.device);
    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}

TEST_CASE("DescriptorArena::create rejects framesInFlight == 0 and zero capacities with a clean error, no crash") {
    HeadlessDescriptorArenaFixture fixture = makeFixture();

    CHECK_FALSE(rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/0).has_value());

    rx::rhi::DescriptorArena::Capacities zeroSets{/*maxSets=*/0, /*uniformBuffers=*/4};
    CHECK_FALSE(rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/2, zeroSets).has_value());

    rx::rhi::DescriptorArena::Capacities zeroUbos{/*maxSets=*/4, /*uniformBuffers=*/0};
    CHECK_FALSE(rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/2, zeroUbos).has_value());

    // A valid call still succeeds -- proves the rejections above are
    // specific to the bad inputs, not a broken fixture.
    CHECK(rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/2).has_value());

    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}

// A set layout with TWO uniform-buffer bindings (binding 0 and binding 1,
// one descriptor each) -- used below to prove allocate()'s own
// uniformBuffers accounting sums the caller-supplied
// `uniformBufferDescriptorCount` per call rather than just counting calls,
// exactly the distinction a single-binding layout (makeUboSetLayout above)
// cannot exercise.
VkDescriptorSetLayout makeTwoUboSetLayout(VkDevice device) {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) == VK_SUCCESS);
    return layout;
}

// [Post-release fix, CI lavapipe run acfce89] Both TEST_CASEs above happen
// to size maxSets == uniformBuffers, so hitting one ceiling always means
// hitting the other at the exact same allocate() call -- neither one can
// tell, on its own, which of the two arena-enforced budgets
// (descriptor_arena.h's class-level BUDGETS ARE ARENA-ENFORCED comment)
// actually fired. This case deliberately sizes the two ceilings DIFFERENTLY
// in both directions, so each sub-case's exhaustion can only be explained
// by the one budget that is actually smaller.
TEST_CASE("DescriptorArena::allocate enforces its own maxSets and per-type uniformBuffers budgets independently, "
          "before ever calling the driver") {
    HeadlessDescriptorArenaFixture fixture = makeFixture();

    {
        VkDescriptorSetLayout oneUboLayout = makeUboSetLayout(fixture.device);

        // --- Sub-case A: uniformBuffers is the SMALLER budget (3 vs. 8) --
        // exhaustion must fire at the 4th allocate() call even though
        // maxSets (8) is nowhere close to used up.
        {
            rx::rhi::DescriptorArena::Capacities capacities{/*maxSets=*/8, /*uniformBuffers=*/3};
            auto arena = rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/1, capacities);
            REQUIRE(arena.has_value());
            arena->beginFrame(0);

            for (uint32_t i = 0; i < capacities.uniformBuffers; ++i) {
                REQUIRE(arena->allocate(oneUboLayout) != VK_NULL_HANDLE);
            }
            // uniformBuffers (3) is exhausted; maxSets (8) has 5 slots left
            // -- this VK_NULL_HANDLE can only be the per-type budget.
            CHECK(arena->allocate(oneUboLayout) == VK_NULL_HANDLE);
        }

        // --- Sub-case B: maxSets is the SMALLER budget (3 vs. 8) --
        // exhaustion must fire at the 4th allocate() call even though
        // uniformBuffers (8) is nowhere close to used up.
        {
            rx::rhi::DescriptorArena::Capacities capacities{/*maxSets=*/3, /*uniformBuffers=*/8};
            auto arena = rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/1, capacities);
            REQUIRE(arena.has_value());
            arena->beginFrame(0);

            for (uint32_t i = 0; i < capacities.maxSets; ++i) {
                REQUIRE(arena->allocate(oneUboLayout) != VK_NULL_HANDLE);
            }
            // maxSets (3) is exhausted; uniformBuffers (8) has 5 descriptors
            // left -- this VK_NULL_HANDLE can only be the maxSets budget.
            CHECK(arena->allocate(oneUboLayout) == VK_NULL_HANDLE);
        }

        // --- Sub-case C: a single allocate() call consuming MORE than one
        // uniformBuffers descriptor (a real two-UBO-binding layout, paired
        // with the matching uniformBufferDescriptorCount=2 argument) --
        // proves the budget sums descriptors per call, not just counts
        // calls. 5 descriptors of budget, 2 consumed per call: two calls
        // fit (4 of 5 used), a third would need 2 more (6 total) and must
        // fail even though 1 descriptor of headroom nominally remains.
        {
            VkDescriptorSetLayout twoUboLayout = makeTwoUboSetLayout(fixture.device);
            rx::rhi::DescriptorArena::Capacities capacities{/*maxSets=*/8, /*uniformBuffers=*/5};
            auto arena = rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/1, capacities);
            REQUIRE(arena.has_value());
            arena->beginFrame(0);

            REQUIRE(arena->allocate(twoUboLayout, /*uniformBufferDescriptorCount=*/2) != VK_NULL_HANDLE);
            REQUIRE(arena->allocate(twoUboLayout, /*uniformBufferDescriptorCount=*/2) != VK_NULL_HANDLE);
            CHECK(arena->allocate(twoUboLayout, /*uniformBufferDescriptorCount=*/2) == VK_NULL_HANDLE);

            vkDestroyDescriptorSetLayout(fixture.device, twoUboLayout, nullptr);
        }

        vkDestroyDescriptorSetLayout(fixture.device, oneUboLayout, nullptr);
    }

    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}

// [P0 regression pin] samples/09_scene/main.cpp crashed running
// `sample_09_scene --present --scene <Sponza.gltf>` on a real, limit-
// enforcing NVIDIA driver: its own hand-rolled "material params"
// VkDescriptorPool was fixed at capacity=8 (justified only for that
// sample's grid/stress modes), while Sponza's own glTF asset has 25
// materials, each needing one set-1 VkDescriptorSet -- vkAllocateDescriptorSets
// failed on the 9th. lavapipe let the SAME oversubscription through clean
// (it never enforces VkDescriptorPool's own maxSets/pool-size ceilings --
// descriptor_arena.h's own BUDGETS ARE ARENA-ENFORCED comment), which is
// exactly why every prior Sponza verification of that sample missed this:
// only a real driver's own pool enforcement ever surfaced it there. The fix
// (samples/09_scene/main.cpp's createMaterialParamArena()) now routes that
// same allocation through THIS class instead of a raw, unaccounted pool, so
// this test pins the two concrete real-world numbers from that incident
// directly against DescriptorArena's own arena-enforced accounting -- which
// discriminates demand > capacity deterministically on every driver,
// including lavapipe, with zero dependence on a real GPU's own enforcement.
TEST_CASE("DescriptorArena::allocate discriminates Sponza-scale demand against an undersized pool (8-set capacity "
          "vs. 25-material demand) deterministically, even under lavapipe") {
    HeadlessDescriptorArenaFixture fixture = makeFixture();

    constexpr uint32_t kPreFixSampleCapacity = 8;    // sample_09_scene's own pre-fix kMaxMaterialParamSets constant.
    constexpr uint32_t kSponzaMaterialCount = 25;    // assets/fetched/Sponza/glTF/Sponza.gltf's real material count.

    {
        VkDescriptorSetLayout layout = makeUboSetLayout(fixture.device);

        // --- The undersized (pre-fix) pool: accepts EXACTLY its declared
        // capacity and refuses every allocation past it -- the arena's own
        // accounting, not driver behavior, is what discriminates
        // demand > capacity, and it must hold identically under lavapipe.
        {
            rx::rhi::DescriptorArena::Capacities undersized{kPreFixSampleCapacity, kPreFixSampleCapacity};
            auto arena = rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/1, undersized);
            REQUIRE(arena.has_value());
            arena->beginFrame(0);

            uint32_t succeeded = 0;
            for (uint32_t i = 0; i < kSponzaMaterialCount; ++i) {
                if (arena->allocate(layout) != VK_NULL_HANDLE) {
                    ++succeeded;
                }
            }
            CHECK(succeeded == kPreFixSampleCapacity);
        }

        // --- The fixed pool: sized from the real material count (exactly
        // what createMaterialParamArena() now does) accepts the FULL
        // demand with zero rejections -- proves the fix's own sizing
        // strategy, not just the enforcement mechanism, is correct.
        {
            rx::rhi::DescriptorArena::Capacities fixed{kSponzaMaterialCount, kSponzaMaterialCount};
            auto arena = rx::rhi::DescriptorArena::create(fixture.device, /*framesInFlight=*/1, fixed);
            REQUIRE(arena.has_value());
            arena->beginFrame(0);

            for (uint32_t i = 0; i < kSponzaMaterialCount; ++i) {
                CHECK(arena->allocate(layout) != VK_NULL_HANDLE);
            }
        }

        vkDestroyDescriptorSetLayout(fixture.device, layout, nullptr);
    }

    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}
