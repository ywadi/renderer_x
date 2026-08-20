#include <doctest/doctest.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>

#include <algorithm>

namespace {

// One `Texture2D[]` unbounded array + one `SamplerState` + one
// `ConstantBuffer<T>`, each with an explicit `[[vk::binding(binding, set)]]`,
// plus a `[[vk::push_constant]]` global -- exactly the shape Task 2's brief
// asks the reflection test to cover. Hand-computed expected table (verified
// directly against `spirv-dis` on this exact source's compiled SPIR-V, and
// against a throwaway reflection probe against this shipped Slang build --
// see reflection.h's comment on reflect() for the discrepancy that probing
// found against [R:A3]'s assumed API):
//
//   global           | category         | set | binding | count      | VkDescriptorType
//   ---------------- | ---------------- | --- | ------- | ---------- | -----------------------------
//   gTextures[]      | DescriptorTable  |  0  |    0    | unbounded  | VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
//   gSampler         | DescriptorTable  |  1  |    0    |     1      | VK_DESCRIPTOR_TYPE_SAMPLER
//   gFrame           | DescriptorTable  |  1  |    1    |     1      | VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
//   gPush            | PushConstant     |  -  |    -    |     -      | offset=0, size=80 bytes
//                                                                       (2x uint32 = 8B, padded to the
//                                                                       16B alignment float4x4 needs,
//                                                                       + 64B matrix = 80B std430 total)
//
// `vsMain` reads gFrame+gPush; `fsMain` reads gTextures+gSampler+gPush --
// every entry point in the linked program touches at least one global, so
// reflect()'s conservative "every binding/push-range gets every entry
// point's stage OR'd in" merge (see reflection.h) means every one of the
// four entries above must carry VERTEX|FRAGMENT.
const char* kLayoutTestSource = R"(
struct PushConstants {
    uint textureIndex;
    uint samplerIndex;
    float4x4 mvp;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(0, 0)]]
Texture2D gTextures[];

[[vk::binding(0, 1)]]
SamplerState gSampler;

struct FrameData {
    float4x4 viewProj;
};

[[vk::binding(1, 1)]]
ConstantBuffer<FrameData> gFrame;

struct VSOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VSOut vsMain(uint vertexID : SV_VertexID)
{
    VSOut o;
    o.position = mul(gFrame.viewProj, mul(gPush.mvp, float4(0, 0, 0, 1)));
    return o;
}

[shader("fragment")]
float4 fsMain() : SV_Target
{
    return gTextures[gPush.textureIndex].Sample(gSampler, float2(0, 0));
}
)";

// A single oversized push-constant global (192 bytes: three float4x4
// members, std430-packed with no inter-member padding since each 64-byte
// matrix is already 16-byte aligned) -- reflection must still succeed and
// report the real size; only PipelineLayoutBuilder (rx_rhi_vk,
// pipeline_layout_test.cpp) enforces the 128-byte guaranteed-minimum budget
// [spec Fixed decision #5, R:B2]. Verified directly against this shipped
// Slang build: elementTypeLayout->getSize() == 192, offset == 0.
const char* kOversizedPushConstantSource = R"(
struct BigPushConstants {
    float4x4 a;
    float4x4 b;
    float4x4 c;
};

[[vk::push_constant]]
ConstantBuffer<BigPushConstants> gBigPush;

[shader("vertex")]
float4 vsMain(uint vertexID : SV_VertexID) : SV_Position
{
    return mul(gBigPush.a, float4(0, 0, 0, 1));
}
)";

const rx::shader::ShaderLayoutInfo::Binding* findBinding(const rx::shader::ShaderLayoutInfo& info, uint32_t set,
                                                          uint32_t binding) {
    auto it = std::find_if(info.bindings.begin(), info.bindings.end(), [&](const auto& b) {
        return b.set == set && b.binding == binding;
    });
    return it != info.bindings.end() ? &*it : nullptr;
}

constexpr VkShaderStageFlags kBothStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

}  // namespace

TEST_CASE("reflect() reports exact set/binding/type/count/stage for an unbounded array + sampler + "
          "constant buffer + push constants") {
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());

    rx::shader::CompileResult compileResult =
        compiler->compileFromSource("LayoutModule", kLayoutTestSource, {"vsMain", "fsMain"});
    INFO("diagnostics: " << compileResult.diagnostics);
    REQUIRE(compileResult.ok);

    auto layout = rx::shader::reflect(compileResult);
    REQUIRE(layout.has_value());

    REQUIRE(layout->bindings.size() == 3);

    const auto* textures = findBinding(*layout, /*set=*/0, /*binding=*/0);
    REQUIRE(textures != nullptr);
    CHECK(textures->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    CHECK(textures->unboundedArray);
    CHECK(textures->count == 0);
    CHECK(textures->stages == kBothStages);

    const auto* sampler = findBinding(*layout, /*set=*/1, /*binding=*/0);
    REQUIRE(sampler != nullptr);
    CHECK(sampler->type == VK_DESCRIPTOR_TYPE_SAMPLER);
    CHECK_FALSE(sampler->unboundedArray);
    CHECK(sampler->count == 1);
    CHECK(sampler->stages == kBothStages);

    const auto* frame = findBinding(*layout, /*set=*/1, /*binding=*/1);
    REQUIRE(frame != nullptr);
    CHECK(frame->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    CHECK_FALSE(frame->unboundedArray);
    CHECK(frame->count == 1);
    CHECK(frame->stages == kBothStages);

    REQUIRE(layout->pushRanges.size() == 1);
    CHECK(layout->pushRanges[0].offset == 0);
    CHECK(layout->pushRanges[0].size == 80);
    CHECK(layout->pushRanges[0].stages == kBothStages);
}

TEST_CASE("reflect() succeeds on a >128-byte push constant block (budget enforcement is PipelineLayoutBuilder's "
          "job, not reflect()'s)") {
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());

    rx::shader::CompileResult compileResult =
        compiler->compileFromSource("BigPushModule", kOversizedPushConstantSource, {"vsMain"});
    INFO("diagnostics: " << compileResult.diagnostics);
    REQUIRE(compileResult.ok);

    auto layout = rx::shader::reflect(compileResult);
    REQUIRE(layout.has_value());

    REQUIRE(layout->pushRanges.size() == 1);
    CHECK(layout->pushRanges[0].offset == 0);
    CHECK(layout->pushRanges[0].size == 192);
    CHECK(layout->pushRanges[0].size > 128);
}

TEST_CASE("reflect() returns nullopt for a failed CompileResult") {
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());

    rx::shader::CompileResult failed =
        compiler->compileFromSource("BadModule", "this is not valid slang source {{{", {"main"});
    CHECK_FALSE(failed.ok);

    auto layout = rx::shader::reflect(failed);
    CHECK_FALSE(layout.has_value());
}

TEST_CASE("reflect() reports elementStride for storage-buffer bindings (StructuredBuffer element size)") {
    const char* storageBufferSource = R"(
        struct ObjectData {
            float4x4 transform;
            float4 color;
        };

        [[vk::binding(0, 0)]]
        StructuredBuffer<ObjectData> gObjects;

        [shader("vertex")]
        float4 main(uint vertexID : SV_VertexID) : SV_Position {
            // Use the storage buffer to ensure it's included in reflection
            ObjectData data = gObjects[0];
            return data.color;
        }
    )";

    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());

    rx::shader::CompileResult compileResult =
        compiler->compileFromSource("StorageBufferModule", storageBufferSource, {"main"});
    INFO("diagnostics: " << compileResult.diagnostics);
    REQUIRE(compileResult.ok);

    auto layout = rx::shader::reflect(compileResult);
    REQUIRE(layout.has_value());

    // Storage buffer should report its element stride.
    // ObjectData is: float4x4 (64 bytes, std430) + float4 (16 bytes) = 80 bytes total
    const auto* readBuffer = findBinding(*layout, /*set=*/0, /*binding=*/0);
    REQUIRE(readBuffer != nullptr);
    CHECK(readBuffer->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    CHECK(readBuffer->elementStride == 80);
}

// [Task 2 (#38), gate ruling RC2, matrix row 2's binding acceptance
// criterion: "a compute module declaring one RWStructuredBuffer<uint> and
// one RWTexture2D<float4> global reflects both correctly (types,
// set/binding, stage flags incl. VK_SHADER_STAGE_COMPUTE_BIT) via the
// EXISTING reflect() path unmodified"] Also the regression coverage for a
// REAL crash this ticket's own work found and fixed: a compute-only
// linked program's reflect() call used to crash inside a process that had
// already initialized Vulkan/vk-bootstrap (see CompileResult::cachedLayout's
// own doc comment, compiler.h, and rx_rhi_vk/tests/compute_pipeline_test.cpp
// for the device-backed half of this regression's coverage) -- this
// specific TEST_CASE, run standalone (no Vulkan Context anywhere in this
// binary), never itself exercised that failure mode, but is kept exactly
// as the matrix's own row 2 asks: proof the EXISTING reflect() path needs
// no compute-specific code of its own, only the shared cachedLayout fix
// every caller (graphics included) now benefits from.
TEST_CASE("reflect() reports exact set/binding/type/stage for a compute module's RWStructuredBuffer + RWTexture2D") {
    const char* src = R"(
        [[vk::binding(0, 0)]]
        RWStructuredBuffer<uint> gOutBuffer;

        [[vk::binding(1, 0)]]
        RWTexture2D<float4> gOutImage;

        [shader("compute")]
        [numthreads(8, 8, 1)]
        void csMain(uint3 id : SV_DispatchThreadID)
        {
            gOutBuffer[id.x] = id.x;
            gOutImage[id.xy] = float4(1, 1, 1, 1);
        }
    )";

    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());

    rx::shader::CompileResult compileResult = compiler->compileFromSource("ComputeLayoutModule", src, {"csMain"});
    INFO("diagnostics: " << compileResult.diagnostics);
    REQUIRE(compileResult.ok);
    REQUIRE(compileResult.entryPointCode.size() == 1);
    CHECK(compileResult.entryPointCode[0].stage == VK_SHADER_STAGE_COMPUTE_BIT);

    auto layout = rx::shader::reflect(compileResult);
    REQUIRE(layout.has_value());
    REQUIRE(layout->bindings.size() == 2);

    const auto* outBuffer = findBinding(*layout, /*set=*/0, /*binding=*/0);
    REQUIRE(outBuffer != nullptr);
    CHECK(outBuffer->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    CHECK(outBuffer->count == 1);
    CHECK(outBuffer->stages == VK_SHADER_STAGE_COMPUTE_BIT);
    CHECK(outBuffer->elementStride == 4);  // uint

    const auto* outImage = findBinding(*layout, /*set=*/0, /*binding=*/1);
    REQUIRE(outImage != nullptr);
    CHECK(outImage->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    CHECK(outImage->count == 1);
    CHECK(outImage->stages == VK_SHADER_STAGE_COMPUTE_BIT);
}
