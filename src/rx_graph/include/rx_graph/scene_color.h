#pragma once
// Vulkan-Headers only -- same device-free header-hygiene rule as every
// other public rx_graph header this file sits alongside (resources.h/
// pass_signature.h): a plain VkFormat/string constant declaration touches
// no live Vulkan handle, so it carries no volk/rx_rhi_vk dependency either.
#include <vulkan/vulkan_core.h>

namespace rx::graph {

// [Task 3 (#39), gate ruling -- rulings-2026-08-20.md "T3 (#39)": "HDR
// scene-color format B10G11R11 (UFLOAT) as the process-wide default with a
// documented A16B16G16R16F escape hatch where precision demands"; matrix
// rows 2/3/6/9, matrix-p5t03-hdr-scene-color.md]
//
// THE ONE ENGINE-OWNED HDR SCENE-COLOR CONVENTION -- replaces four
// independent `constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;`
// copies that used to live one per sample (samples/05_multipass,
// 07_stress, 08_gltf_viewer, 09_scene -- verified identical-by-copy-paste,
// not by any shared source, before this task) [matrix row 2/9]. Every
// sample now declares its opaque-lit working color target against this
// constant, never its own.
//
// FORMAT: `VK_FORMAT_B10G11R11_UFLOAT_PACK32` -- a packed 32-bit-per-texel
// value: 11-bit unsigned float R (5-bit exponent, 6-bit mantissa), 11-bit
// unsigned float G (same layout), 10-bit unsigned float B (5-bit exponent,
// 5-bit mantissa) [Khronos `VkFormat` spec, bit-layout section, fetched
// directly -- matrix row 10's "Verified first-hand" citation]. NO sign bit
// (radiance is never negative, so this costs nothing real) and NO alpha
// channel (a sampled read of this format always returns alpha == 1.0).
//
// PRECISION CHARACTERISTIC THAT MUST BE HANDLED HONESTLY, NOT GLOSSED
// OVER: the blue channel's mantissa is ONE BIT NARROWER than red/green's
// (5 bits vs 6) -- for the SAME magnitude value written to all three
// channels, blue quantizes to a COARSER grid (step size 2x red/green's, at
// the same exponent) and can end up with up to 2x red/green's worst-case
// rounding error. This is proven, not just documented, by
// rx_graph_gpu_tests' `SceneColorGpu` cases (test_scene_color_gpu.cpp),
// which write an identical non-power-of-two radiance value to all three
// channels of a real `VK_FORMAT_B10G11R11_UFLOAT_PACK32` attachment via a
// real draw, read the raw packed texel back, and decode it with
// `glm::unpackF2x11_1x10` (GLM's own implementation of this exact packed
// layout -- reused, not reimplemented, per this repo's "don't reinvent the
// wheel" rule) to show blue's larger quantization error directly.
//
// WHY B10G11R11 OVER RGBA16F AS THE DEFAULT [matrix row 3's decisive
// recommendation, adopted verbatim by the ruling]: half the per-texel
// footprint (32 bits vs 64) on every read/write this buffer sees for its
// entire consumer chain (mip chain -- Task 22; SSR -- Task 26; glass read
// -- Tasks 23/24; bloom -- Task 31; tonemap -- this task + Task 32; TAA --
// Task 33), which matters most on the Steam Deck's UMA bandwidth floor
// this phase's benchmark gates are required to prove against (repo
// CLAUDE.md, "Performance is an exit criterion"). Google Filament's own
// `FRenderer::getHdrFormat()` (`filament/src/details/Renderer.cpp`, v1.75.0
// pin) independently makes the identical choice for its OPAQUE views at
// its low/medium quality tier (`TextureFormat::R11F_G11F_B10F`, bit-
// identical layout) -- only Filament's TRANSLUCENT views ever get an
// alpha-bearing RGBA16F-class buffer. The commonly-raised "but transmission
// blending needs alpha on this buffer" counter-argument does not survive
// contact with this codebase's own charter: glass is real screen-space-
// refraction that READS this buffer as a texture input, never a
// fixed-function alpha-blend WRITE onto it (charter,
// `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:398-411`)
// -- an opaque buffer's alpha channel is definitionally always 1.0 and was
// never going to carry information either way.
inline constexpr VkFormat kHdrFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32;

// [Task 3 (#39), gate ruling] THE DOCUMENTED ESCAPE HATCH -- use this,
// never a fifth ad hoc format, whenever a scene-color-shaped resource
// genuinely needs what `kHdrFormat` structurally cannot provide:
//   - SIGNED values (kHdrFormat is unsigned-only; a resource that can go
//     negative -- e.g. some post-process intermediate, never plain
//     radiance -- cannot be stored in it at all, not just imprecisely).
//   - An ALPHA channel that must carry real information (kHdrFormat's
//     alpha is definitionally 1.0 always -- see kHdrFormat's own comment).
//   - Symmetric, higher per-channel precision than kHdrFormat's narrower
//     (and blue-asymmetric) mantissas can offer.
// `VK_FORMAT_R16G16B16A16_SFLOAT`: four IEEE-754-binary16-compatible
// channels (1 sign + 5 exponent + 10 mantissa bits each), including alpha,
// symmetric across every channel -- proven by
// `SceneColorGpu::EscapeHatchPreservesNegativeAndSymmetricPrecision`
// (test_scene_color_gpu.cpp), which writes a negative value through this
// format and confirms R/G/B decode with IDENTICAL, symmetric rounding
// error via `glm::unpackHalf4x16`, unlike kHdrFormat's proven asymmetry
// above. This was RGBA16F's status quo before this task (all four samples
// used it as their sole HDR format) -- kept alive as the documented
// fallback rather than removed, exactly per the ruling's wording.
inline constexpr VkFormat kHdrFormatHighPrecision = VK_FORMAT_R16G16B16A16_SFLOAT;

// [Task 3 (#39), matrix row 6] The canonical name every pass that
// establishes or consumes the opaque-lit scene-color working image should
// declare (`Pass::addColorOutput(kSceneColorResourceName, ...)` /
// `Pass::addTextureInput(kSceneColorResourceName)`). Not a *requirement*
// enforced anywhere in rx_graph itself (a graph can still name a resource
// whatever it wants -- this constant only removes the reason to ever type
// the literal "hdr" by hand) -- every one of this repo's existing samples
// already happened to agree on this exact name by copy-paste convention
// before this task; this constant is that convention's single source now.
inline constexpr const char* kSceneColorResourceName = "hdr";

// ===========================================================================
// SCENE-COLOR SEAM -- the documented consumer table [matrix row 6's own
// acceptance criterion: "a table mapping each frame-pipeline-target stage
// after 'opaque lighting' to the task that will attach to it"].
//
// Charter frame-pipeline target (binding order,
// `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:447-451`,
// restated at `docs/superpowers/plans/2026-08-20-phase5-techniques.md:20-24`):
//
//   depth -> shadows -> clustered light assignment -> opaque lighting
//     [[[ writes kSceneColorResourceName / kHdrFormat -- THIS TASK ]]]
//   -> volumetrics (froxel march + apply)
//   -> SSR                              (reads scene color -- Task 26, #62)
//   -> scene-color mip chain            (Task 22, #58 -- the seam's own
//                                         downstream mip-generation owner)
//   -> glass/transmission                (reads scene color as a texture
//                                         input for screen-space refraction,
//                                         never an alpha-blend write onto
//                                         it -- Tasks 23/#59 and 24/#60)
//   -> particles/transparency
//   -> bloom                             (Task 31, #67)
//   -> tone mapping (AgX/ACES, FG8 HDR output)
//                                         [reads kSceneColorResourceName;
//                                         this task's own tonemap-hook
//                                         documentation below; the actual
//                                         AgX/ACES algorithm lands in
//                                         Task 32, #68]
//   -> TAA                               (Task 33, #69 -- history/velocity
//                                         resources are NEW work per RC6,
//                                         not something this seam pre-pays)
//
// Every one of the above tasks CONSUMES this seam; none of them may
// establish a second, independently-formatted "scene color"-shaped
// resource of their own [plan global constraint: "samples are pure
// consumers... no sample/task hand-rolls what the engine provides"].
// ===========================================================================

// ===========================================================================
// FG6 MSAA POLICY RULING -- DOCUMENTED REJECTION, NOT AN IMPLEMENTATION
// [Task 3 (#39), gate ruling -- rulings-2026-08-20.md "T3 (#39)": "FG6
// ruled no-MSAA/TAA-first (a documented rejection with rationale,
// revisitable post-TAA)"; matrix rows 4/5, matrix-p5t03-hdr-scene-color.md].
//
// FG6 registry text
// (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:211-213`):
// "(FG6) MSAA policy decision + resolve-attachment semantics in the graph
// (decide in techniques-phase spec, before aliasing/history ossify the
// resource model)". This task DECIDES the policy and records the decision
// here; it does NOT implement MSAA or resolve-attachment semantics.
//
// DECISION: no-MSAA, TAA-first. Real hardware multisample resolve is
// explicitly rejected for this phase's default anti-aliasing story in
// favor of Task 33's TAA (already the charter's own committed, LAST stage
// of the frame-pipeline target above) -- REVISITABLE after TAA lands, if
// real evidence later demands hardware MSAA specifically (e.g. a
// forward-only content class TAA serves poorly). Not a permanent
// architectural wall, a sequencing call.
//
// RATIONALE:
//   1. TAA is already the committed, scheduled temporal-stability
//      mechanism for this entire phase (frame-pipeline target, last
//      stage) -- this is not a new commitment this task invents, only one
//      it declines to duplicate with a second, competing AA path.
//   2. The render graph's history-resource machinery already exists
//      specifically to support a ping-ponged temporal consumer
//      (`isHistory`/`PinnedHistoryEntry`, `transient_pool.h`) -- Task 33
//      is not starting from zero the way a from-scratch MSAA resolve path
//      would be.
//   3. Real MSAA resolve requires NEW work in TWO currently-incomplete
//      layers, verified directly this task (both citations below still
//      hold as of this task's own HEAD):
//
// GAP (a) -- the `samples` axis is real per-attachment metadata, correctly
// threaded from `AttachmentDesc::samples` (resources.h) through
// `PassSignature::samples` (pass_signature.h, hashed) to
// `TransientPool::acquireImage()`'s own pooling key (transient_pool.h),
// and `rx::material::MaterialSystem`'s pipeline creation DOES correctly
// derive `multisampleState.rasterizationSamples` from a declared pass's
// sample count (`src/rx_material/material_system.cpp:1997`,
// `multisampleState.rasterizationSamples = req.pass.samples;`) -- but it
// dead-ends at the one place that actually allocates the GPU image:
// `rx::rhi::Texture2D::create()`'s signature (`src/rx_rhi_vk/include/
// rx_rhi_vk/texture.h`) takes no `samples` parameter at all, and BOTH of
// its image-creation paths hardcode `imageInfo.samples =
// VK_SAMPLE_COUNT_1_BIT` unconditionally
// (`src/rx_rhi_vk/src/texture.cpp:133,213`, verified unchanged as of this
// task). Declaring `samples > 1` on an `AttachmentDesc` today would build
// a pipeline correctly expecting N samples bound against an ACTUALLY
// 1-sample image -- an immediate dynamic-rendering validation error, not
// silent misbehavior.
//
// GAP (b) -- zero resolve-target wiring exists anywhere in this codebase:
// no field on `AttachmentDesc`/`PhysicalResource` (resources.h) carries a
// resolve-target reference, and `Executor`'s `VkRenderingAttachmentInfo`
// construction (`src/rx_graph/executor.cpp`) never sets
// `resolveMode`/`resolveImageView` (whole-file grep, zero hits, verified
// this task).
//
// A future MSAA ticket's actual starting scope is therefore: (1) thread a
// real `samples` parameter through `Texture2D::create()`/
// `createForPresuppliedMips()` and `TransientPool::acquireImage()`'s
// actual VkImage creation, (2) add a resolve-target field to
// `AttachmentDesc`/`PhysicalResource` and wire `resolveMode`/
// `resolveImageView` into `Executor`'s attachment-info construction --
// NOT "wire up an already-working axis" (FG6's own wording could be
// misread that way; it is not the case -- see GAP (a) above).
// ===========================================================================

// [Task 3 (#39), matrix row 10] STORAGE-IMAGE CAVEAT, forward-looking note
// for Task 2 (#38)'s compute-pass consumers (SSR -- Task 26; froxel
// volumetrics -- Task 30) that may need to read-modify-write scene color
// as a storage image, not only sample it: `kHdrFormat`'s
// `VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT` support is empirically confirmed
// (both lavapipe and the real NVIDIA driver, this task's own
// `SceneColorGpu::FormatSupportsColorAttachmentAndLinearSampling` test
// logs it) but is NOT guaranteed the same way `kHdrFormatHighPrecision`'s
// is (RGBA16F's storage-image/UAV read+write support has no known vendor
// caveat on any graphics API; packed-float formats like `kHdrFormat`
// historically do on the closest cross-API analogue, DirectX's
// `DXGI_FORMAT_R11G11B10_FLOAT` UAV typed-load). Any future compute pass
// that targets `kHdrFormat` as a READ-WRITE storage image must re-query
// `VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT` on its own target hardware before
// assuming it -- never assumed from this comment alone.

}  // namespace rx::graph
