#pragma once
// Vulkan-Headers only -- same header-hygiene rule as resources.h/barriers.h:
// no volk, no vulkan/vulkan.h, no rx_rhi_vk. PassSignature is a plain,
// device-free value type derived purely from a compiled pass's own
// attachment formats/sample count (Executor::execute() populates it -- see
// executor.h's PassContext::passSignature() and executor.cpp), so it lives
// alongside pass.h/resources.h/barriers.h rather than needing anything a
// real VkDevice produced.
#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>

namespace rx::graph {

// Phase 3 Task 5 (rx_material): the render-graph-derived half of a pipeline
// variant's cache key [spec Phase 3 design, D7 -- "Pass signature is
// derived from the render graph pass declaration: color attachment
// formats, depth format, sample count... the graph is the source of
// per-pass variability, so the key is generated, not hand-enumerated"].
//
// Lives in `rx::graph`, not `rx::material`, even though its only consumer
// this task is rx_material: PassSignature is derived purely from a
// compiled RenderGraph pass's own state (PassContext, defined in
// executor.h, is what populates one -- see passSignature() there), and
// rx_material already depends on rx_graph (never the reverse) [spec
// architecture diagram]. Putting this type in rx_material instead would
// force rx_graph to depend on rx_material just to let PassContext return
// one, an actual dependency cycle -- rx_material aliases this type instead
// (`rx::material::PassSignature`, material_system.h) so callers never need
// to spell the rx::graph name.
//
// Deliberately NOT the full fixed-function pipeline state a real variant
// key could in principle include (blend state, rasterization state, ...) --
// Phase 3's material pipelines fix every other piece of state themselves
// (see rx_material's material_system.cpp), so the render graph only needs
// to contribute what IT actually varies per pass: attachment shape.
struct PassSignature {
    // Phase 4 Task 1 (carried final-review finding from Phase 3): the
    // ceiling on how many color attachments any single declared pass may
    // have -- RenderGraph::compile() throws, naming the offending pass, if
    // a Pass::addColorOutput()/setHistoryOutput() call would push a single
    // pass's own color-attachment count past this (render_graph.cpp).
    // Named here (not a bare magic number) precisely because this array's
    // own size is the ONE existing place that limit was already implicit
    // but unenforced -- every other reference to "8" for this purpose
    // (there were none outside this array before this task) should use
    // this constant instead of repeating the literal.
    static constexpr uint32_t kMaxColorAttachments = 8;

    // VK_FORMAT_UNDEFINED-padded: only the first `colorCount` entries are
    // meaningful. Fixed-size (not a vector) so PassSignature stays a
    // trivially-comparable, trivially-hashable value type, matching every
    // other rx_graph value type's own device-free-header discipline.
    std::array<VkFormat, kMaxColorAttachments> colorFormats{};
    uint32_t colorCount = 0;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    bool operator==(const PassSignature&) const = default;

    // FNV-1a 64-bit over every field above, in declaration order (the
    // unused, VK_FORMAT_UNDEFINED-padded tail of `colorFormats` past
    // `colorCount` is included too -- default-constructed to 0 and never
    // mutated independently of `colorCount`, so two PassSignatures that
    // compare equal via operator== always hash equal here as well; this is
    // NOT a raw memory scan of `*this`, precisely so that guarantee holds
    // regardless of any compiler-inserted padding between fields).
    //
    // FNV-1a is implemented inline here (not imported from a shared
    // utility) because none exists yet in rx_core (checked before writing
    // this), and the algorithm is small, canonical, and fully specified by
    // its two well-known 64-bit constants below -- not the kind of
    // "nontrivial subsystem" this repo's ready-made-library-first policy is
    // aimed at. rx_material's own moduleHash() (material_system.cpp) needs
    // the identical algorithm over a byte buffer instead of struct fields
    // and, for the same reason, implements its own equally small copy
    // rather than pulling this one header into a cross-library dependency
    // for ten lines of arithmetic.
    [[nodiscard]] uint64_t hash() const {
        constexpr uint64_t kOffsetBasis = 0xcbf29ce484222325ULL;
        constexpr uint64_t kPrime = 0x100000001b3ULL;

        uint64_t h = kOffsetBasis;
        auto mix32 = [&h](uint32_t v) {
            for (int byteIndex = 0; byteIndex < 4; ++byteIndex) {
                h ^= static_cast<uint64_t>((v >> (byteIndex * 8)) & 0xFF);
                h *= kPrime;
            }
        };

        for (VkFormat format : colorFormats) {
            mix32(static_cast<uint32_t>(format));
        }
        mix32(colorCount);
        mix32(static_cast<uint32_t>(depthFormat));
        mix32(static_cast<uint32_t>(samples));
        return h;
    }
};

}  // namespace rx::graph
