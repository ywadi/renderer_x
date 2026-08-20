# Matrix — P5 T22 (issue #58): Scene-color HDR mip chain

**Plan task:** Task 22 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:653-671`), Stage 3.
**Charter binding:** frosted-glass model (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:403-413`,
"the opaque scene color renders into an HDR mip chain and transmission
roughness selects the mip (sharp→blurred→frosted) — Filament's
refractive-scatter model"); frame-pipeline target (`:447-451`, mip chain
sits between SSR and glass/transmission); Task 3's scene-color seam is
this task's stated foundation (plan `:207-230`, not yet built as of this
gate round — Task 3 is Stage 0, sequenced before Stage 3 but its own
delivery state was not re-verified in this pass, out of this ticket's
scope to audit).
**Spec decisions:** none yet exist for Phase 5 (Task 1/this gate is the
first pass); the render-graph facts below are Phase 3/4 delivered code,
read first-hand, not Phase 5 spec decisions.

**Sources consulted:**
- Ticket body: `gh issue view 58`.
- Plan Task 22 + Global Constraints + Task 3 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:55-134, 207-230, 653-671`).
- Charter block (cited above).
- Delivered code, read first-hand at HEAD (`bf5b853`) by a parallel
  in-round research pass covering the render graph in full:
  `src/rx_graph/include/rx_graph/resources.h` (`AttachmentDesc`,
  `PhysicalResource`, `ResourceAccess` — no mip field anywhere),
  `src/rx_graph/include/rx_graph/pass.h` (whole-resource-only
  input/output declaration, no subresource parameter),
  `src/rx_graph/include/rx_graph/pass_signature.h:33-58` (attachment
  shape only, explicit header-comment disclaimer of fixed-function-state
  coverage — the same disclaimer the Phase 4 gate already found for
  blend/cull state applies here to mip-count/subresource state),
  `src/rx_graph/include/rx_graph/barriers.h` + `src/rx_graph/executor.cpp`
  (whole-image `VK_REMAINING_MIP_LEVELS` barriers, no per-subresource
  transition machinery — lines 534-535, 705-706, 807-808, 831-832),
  `src/rx_graph/transient_pool.h`/`.cpp` (persistent/history vs. transient
  resource classes, both creating images with `requestedMipLevels=1`
  today — lines 32, 137), `src/rx_rhi_vk/src/texture.cpp`
  (`Texture2D::create`/`createForPresuppliedMips` support arbitrary
  `mipLevels` on the underlying `VkImage`, lines 131/211, but each
  creates only ONE whole-chain `VkImageView` covering the full mip
  range, lines 165/240; `Texture2D::recordMipChainBlit`, lines 257-310,
  a real per-level `vkCmdBlitImage` downsample chain with correct
  per-level barrier transitions, but invoked ONLY from
  `Uploader::uploadToImage` at upload time, `src/rx_rhi_vk/src/upload.cpp:332`,
  entirely outside the render graph), `docs/superpowers/specs/2026-08-10-phase3-render-graph-materials-design.md`
  (zero `mip`/`subresource` hits, confirmed by direct grep).
- Google Filament, `google/filament` @ commit `721ec800093de984cbee155e459298b6b2dbb855`
  (fetched 2026-08-20), Apache-2.0: `shaders/src/surface_light_indirect.fs`
  (`perceptualRoughnessToLod`, `evaluateRefraction`'s screen-space LOD
  branch, fetched verbatim).
- Khronos glTF Sample Renderer @ commit `863b981fb755359063e370ff7b6e956bda0716e2`
  (fetched 2026-08-20), Apache-2.0: `source/Renderer/shaders/ibl.glsl`
  (`getIBLVolumeRefraction`'s `framebufferLod` formula, fetched verbatim).

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/code support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------------|-------------------------------|
| 1 | Mip-level-count declaration on a graph resource | Every graph-based renderer with a mip-chain pass (Filament's `FrameGraph`, generic precedent, not independently re-verified against Filament's C++ graph code in this pass — the ported ARTIFACT is the shader-side LOD math, not Filament's own graph implementation, which this project's `rx_graph` does not port). | consume-now (new mechanism) | **VERIFIED ABSENT.** `AttachmentDesc` (`resources.h:78-88`) and `PhysicalResource` (`resources.h:121-170`) carry `format`/`sizeClass`/`width`/`height`/`samples`/`depthConvention` — no mip-count field. Every graph-driven image creation call site (`transient_pool.cpp:32,137`) hardcodes `requestedMipLevels=1`. | A device-free test: a resource declared with `mipLevels > 1` is accepted by the graph's compile step and the resulting `PhysicalResource`/`Texture2D` reports the correct level count — this is new struct surface, not a config flip. |
| 2 | Per-subresource (mip level) pass input/output declaration | Same category as row 1 — no existing `rx_graph` precedent to build on; this is genuinely new graph API surface. | consume-now (new mechanism) | **VERIFIED ABSENT.** `Pass::addColorOutput`/`setDepthStencilOutput`/`addTextureInput`/`addHistoryInput`/`setHistoryOutput` (`pass.h:55-138`) take only a resource NAME — no mip/layer parameter exists anywhere in the `Pass` API. `ResourceAccess` (`resources.h:107-112`) has no subresource-range field. | A mip-generation pass declares "write mip N, read mip N-1" as two DISTINCT subresource accesses on the SAME named resource; the graph's dependency/barrier derivation treats them as sequential (not aliased/parallel) — test asserts the derived barrier count and ordering match a hand-computed expectation for a 4-level chain (3 write→read transitions). |
| 3 | Per-mip barrier derivation | N/A — internal graph-machinery row, no external precedent needed (Vulkan's own subresource-range barrier model is the only "precedent," and it's the spec itself, not a renderer choice). | consume-now (new mechanism) | **VERIFIED ABSENT.** Every current barrier construction site in `executor.cpp` hardcodes `baseMipLevel = 0; levelCount = VK_REMAINING_MIP_LEVELS` (lines 534-535, 705-706, 807-808, 831-832) — i.e. the barrier system today can only ever transition an ENTIRE image's mip range at once, never a single level. A mip-chain-generation pass needs level-N write → level-N read → level-(N+1) write in strict sequence, each a distinct subresource-range barrier. | GPU test with sync validation ON: a 4-level mip-generation pass chain produces zero validation errors AND the barrier count matches the expected per-level sequence (not one coarse whole-image barrier silently serializing everything, which would be correct-but-undetected-regression-prone — the test must distinguish "correct because coarse" from "correct because precise"). |
| 4 | Downsample/blit (or compute) pass TYPE usable from the graph | Filament's mip/blur post-process passes (charter's own named port source, `:655-656` — "filter ported from Filament's mipmap/blur passes"); this project's OWN existing `Texture2D::recordMipChainBlit` (`texture.cpp:257-310`) as a structurally-similar but not graph-integrated precedent. | consume-now | Filament's shader-level filter (the actual blur/downsample KERNEL) is a portable artifact; RendererX's OWN `recordMipChainBlit` is real, working, per-level-barrier-correct `vkCmdBlitImage` code but is invoked exclusively from `Uploader::uploadToImage` (`upload.cpp:332`) — entirely outside `rx_graph`, and its own header comment (`texture.h:162-177`) flags an sRGB caveat NOT correctness-safe for an HDR/float scene-color target's filtering needs without adaptation (a linear-space HDR mip chain has different correctness requirements than an sRGB-encoded asset-texture mip chain: no gamma-aware averaging bug is possible on linear data, but the existing code's box-filter-vs-blit distinction was never audited for HDR precision/overflow). | Mip VALUES asserted per the ticket's own acceptance sketch: a known HDR test pattern (values >1.0, some pixels near float precision extremes) produces expected filtered values at mips 1..N — not merely "chain exists, looks blurred." |
| 5 | Per-mip `VkImageView` for pass I/O | N/A — Vulkan-API-level requirement, not a renderer-precedent row: a compute/graphics pass writing into mip level N as a render target or storage image needs a `VkImageView` scoped to `{baseMipLevel: N, levelCount: 1}`. | consume-now (new mechanism) | **VERIFIED ABSENT.** `Texture2D::view()` (`texture.h:144`) exposes exactly ONE `VkImageView` per texture, created with `levelCount = mipLevels` (the WHOLE chain) at both `texture.cpp:165` and `:240`. `PassContext::imageView(name)` (`executor.h:268`) mirrors this — one view per named resource. There is no factory/accessor for a per-mip-level view anywhere in the RHI or graph layers today. | A pass targeting mip level 2 of a 4-level chain writes ONLY that level (readback confirms levels 0/1/3 are untouched by that pass, level 2 changes) — proves per-mip view scoping is real, not accidentally whole-chain. |
| 6 | Transient vs. persistent resource-class decision for the chain | Ticket's own text (`:660-661`) flags this as an open question: "Graph integration respects history/transient rules (persistent-vs-transient class per the Phase 4 sequencing constraint)." | **Genuinely open — flagged, not resolved by this gate** | VERIFIED: "Transient" = discard-per-frame, shape-keyed, pooled/aliased across a 2-frame staleness window (`transient_pool.h:36-45,183-194,279-302`). "Persistent/history" = `PinnedHistoryEntry`/`PinnedHistorySlot`, two ping-ponged images keyed by NAME not shape, never swept (`transient_pool.h:118-181,228-263,290-301`), designed Phase 4 Task 1 for whole SINGLE-MIP ping-ponged images — neither existing path is multi-mip-aware. A scene-color mip chain is almost certainly TRANSIENT in nature (rebuilt every frame from that frame's opaque pass, not carried across frames like TAA history) — but the CURRENT transient path's own creation call hardcodes `requestedMipLevels=1` (`transient_pool.cpp:32`), so "transient" alone doesn't resolve the gap; the transient path itself needs multi-mip support added, independent of which class is chosen. | The spec (Task 1's own coordinator-authored output, not this matrix) must rule transient-vs-persistent explicitly; whichever is chosen, this ticket's acceptance criterion is that the CHOSEN path supports a real multi-mip `Texture2D` end to end (not just accepts a `mipLevels` parameter that then silently gets ignored downstream). |
| 7 | Frosted-glass roughness→LOD mapping (Filament, IBL-cubemap variant) | Filament `shaders/src/surface_light_indirect.fs`, fetched verbatim 2026-08-20: `float perceptualRoughnessToLod(float perceptualRoughness) { return frameUniforms.iblRoughnessOneLevel * perceptualRoughness * (2.0 - perceptualRoughness); }` — a quadratic curve, empirically tuned for a 256×256 cubemap with 5 mip levels (per the function's own source comment, as summarized from the fetch). | consume-now (as the T23/T24 downstream reference — this ticket builds the chain, does not itself select mips for transmission) | VERIFIED — this is Filament's CUBEMAP/IBL-specific curve, used for prefiltered-environment sampling, not literally the scene-color-framebuffer case. | Not this ticket's own acceptance criterion (T23/T24 consume the chain) — recorded here because the chain's MIP COUNT and roughness-range must be chosen jointly with whichever formula T23/T24 pick (row 8), or the chain either wastes levels or runs out of range before reaching the roughest materials. |
| 8 | Frosted-glass roughness→LOD mapping (Filament, screen-space-refraction variant) — **the testable relationship for THIS chain** | Filament `evaluateRefraction()`, same file, fetched verbatim: `#if REFRACTION_MODE == REFRACTION_MODE_CUBEMAP` uses row 7's formula; `#else const float invLog2sqrt5 = 0.8614; float lod = max(0.0, (2.0 * log2(perceptualRoughness) + frameUniforms.refractionLodOffset) * invLog2sqrt5);` for the SCREEN-SPACE (scene-color-framebuffer) case — this is the directly-relevant formula since T22/T23/T24's mip chain is a screen-space scene-color buffer, not a cubemap. | consume-now (the recommended reference formula) | VERIFIED, quoted verbatim from the pinned commit. A SECOND independent reference exists: Khronos Sample Renderer's `getIBLVolumeRefraction()` (`ibl.glsl`, fetched verbatim 2026-08-20): `float framebufferLod = log2(float(u_TransmissionFramebufferSize.x)) * applyIorToRoughness(roughness, ior);` — a simpler `log2(bufferSize) * f(roughness, ior)` form, with `applyIorToRoughness`'s own body not independently confirmed in this pass (defined elsewhere in the same file, not fetched to depth). Two named, cited, testable formulas exist; the coordinator/Task-1-spec should pick ONE as the pinned reference (Filament's, per the charter's explicit "Filament's refractive-scatter model" framing, `:411-413`) rather than an implementer improvising a third. | Monotonicity + closed-form test (feeds T24's own acceptance sketch): increasing `perceptualRoughness` strictly increases the selected LOD per the pinned formula, verified at ≥4 roughness values against the exact formula output (not just "looks blurrier"). |
| 9 | Chain build point in the frame pipeline | Charter's frame-pipeline target (`:447-451`): "...volumetrics... → SSR → scene-color mip chain → glass/transmission → particles/transparency → bloom..." — an explicit, ordered position. | consume-now | VERIFIED as stated text (not independently re-derivable from code, since no frame-pipeline orchestration exists yet in Phase 5 — this is a Task-1-spec-level sequencing commitment, not a code fact). | The graph's pass-dependency declarations enforce this order structurally (the mip-chain pass consumes opaque+SSR output, glass/transmission passes declare the mip chain as an input) — a test asserts the compiled pass order matches, and that reordering (a regression) is structurally rejected or at minimum loudly flagged (a dependency-cycle/missing-input error), not silently mis-ordered. |
| 10 | Cost measurement (1080p/1440p, per ticket's own acceptance sketch) | CLAUDE.md performance-exit-criterion policy; the standing corrective in this same plan's Global Constraints (`:65-73`, real-driver verification mandatory). | consume-now | N/A — policy row, not a library-support row. | Tracy-measured cost at both resolutions, REAL-DRIVER-labeled (lavapipe numbers are not performance evidence per the standing corrective) — published per the plan's own "Cost measured at 1080p/1440p and published" text. |

---

## Conflicts

None found that contradict the plan/charter/ticket text outright. The
ticket's own phrasing ("Task 3's scene-color seam grows mips") reads as
if this is an incremental addition to existing infrastructure; the
render-graph findings above show it is closer to a NEW subsystem (mip-
count on resources, per-subresource pass declarations, per-mip barriers,
per-mip image views) layered under a genuinely small piece of reused
infrastructure (Task 3's named HDR scene-color resource itself). This is
not a contradiction of the ticket's TEXT but is exactly the kind of
scope-magnitude gap this gate exists to surface before dispatch — the
ticket's one-line "Task 3's scene-color seam grows mips" undersells the
five distinct new mechanisms rows 1-5 identify.

## New gaps

- **No mip/subresource concept anywhere in `rx_graph`** (rows 1-5): not
  previously named in the master registry
  (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md`,
  grepped for "mip"/"subresource", zero hits outside this charter block
  itself) or in the Phase 3 render-graph design doc (also zero hits,
  confirmed by direct grep). This is the single largest piece of new
  graph-layer surface Stage 3 requires, and every later mip-consumer
  (T24's frosted glass, Task 31's bloom per the plan's own text
  `:837-838` "reuses or mirrors the Task 22 chain") depends on it
  landing correctly here first. Proposed fit: this ticket is correctly
  positioned as the FIRST to need it (already so scheduled); flagged so
  the coordinator sizes Task 22 accordingly rather than treating it as a
  small addition.
- **`Texture2D::recordMipChainBlit`'s sRGB caveat vs. HDR/float
  correctness** (row 4): the existing upload-time mip-blit code was
  built and audited for asset-texture semantics (sRGB decode/encode
  correctness), not scene-color HDR semantics (overflow/precision at
  float extremes, linear-space filtering only). Not registered anywhere
  as a risk today. Proposed fit: this ticket's own test suite must
  positively exercise the HDR-specific case (row 4's acceptance
  criterion), not assume the existing blit code's correctness transfers.

## Verification health

- **Verified first-hand:** all `rx_graph`/`rx_rhi_vk` file:line citations
  (rows 1-6) were read directly from the working tree at HEAD by a
  parallel in-round research pass, not inferred from the ticket's own
  description or the Phase 4 gate's prior findings.
- **Verified first-hand:** both roughness→LOD formulas (rows 7-8) were
  fetched verbatim from the pinned Filament/Khronos-Sample-Renderer
  commits, not paraphrased from documentation or search digests.
- **Lower-confidence, flagged explicitly:** `applyIorToRoughness()`'s
  exact body (Khronos Sample Renderer's LOD formula, row 8) was not
  independently confirmed — the fetch tool reported it as "defined
  elsewhere in the same file" without returning that definition; treat
  the Khronos formula as a secondary/corroborating reference only, not a
  fully independently-verified alternative to Filament's cited formula.
- Task 3's actual delivery state (does the named HDR scene-color
  resource this ticket "grows mips" on already exist in code?) was
  explicitly NOT audited in this pass — out of this ticket's assigned
  scope (Task 3 is a Stage 0 ticket outside Stage 3 T21-T28) and, as of
  this gate round, Phase 5 implementation has not yet dispatched, so no
  such code exists to check. This dependency is structural (Task 22
  cannot start before Task 3 lands), not a research gap.
