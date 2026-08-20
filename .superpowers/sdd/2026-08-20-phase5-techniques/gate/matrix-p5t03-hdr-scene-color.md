# Matrix — Issue #39 (P5 T03): HDR scene-color infrastructure + MSAA policy decision (FG6)

**Plan task:** Task 3 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:207-230`, Stage 0).

**Spec/charter decisions binding this ticket:**
- FG6 registry line — MSAA policy decision + resolve-attachment semantics
  in the graph, "decide in techniques-phase spec, before aliasing/history
  ossify the resource model"
  (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:211-213`).
- Techniques-phase charter's **Frame pipeline target** (binding order this
  ticket's "scene color" seam must serve): depth → shadows → clustered
  light assignment → opaque lighting → volumetrics → SSR → **scene-color
  mip chain** → glass/transmission → particles/transparency → bloom → tone
  mapping (AgX/ACES, FG8 HDR output) → TAA
  (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:447-451`,
  restated verbatim at
  `docs/superpowers/plans/2026-08-20-phase5-techniques.md:20-24`). Also the
  charter's own glass ruling: "Glass is REAL transmission, never alpha
  blending" — transmissive BTDF reads a scene-color source via
  screen-space refraction, environment/probe fallback on miss
  (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:398-411`)
  — see Conflicts below, this bears directly on the format ruling's alpha
  premise.
- Global constraints binding every Stage-0 ticket (real-GPU verification,
  reference-vs-ground-truth discipline, no-deferred-fixes, samples-are-
  pure-consumers) — `docs/superpowers/plans/2026-08-20-phase5-techniques.md:55-111`.
- D22 (Phase 4, grounding only — StandardPBR's manual tonemap-side
  `--exposure`, the predecessor Task 4 [ticket #40, a different
  researcher's ticket] formally replaces):
  `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md:360-373`.

**Sources consulted (all first-hand, this session, HEAD `07774a3` for the
files below; repo tip at research time `54f92ed1`):**
- `gh issue view 39` — full ticket text.
- Plan file: "Global Constraints" (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:55-111`)
  and "### Task 3" (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:207-230`), full read.
- `samples/05_multipass/main.cpp:192,1067,1522`; `samples/07_stress/main.cpp:132,669,1137`;
  `samples/08_gltf_viewer/main.cpp:133,567,1659`; `samples/09_scene/main.cpp:146,542,2348`
  — each site's own `kHdrFormat` declaration + `.addColorOutput("hdr", swapchainRelativeDesc(kHdrFormat))`
  call, read in surrounding context.
- `shaders/multipass/tonemap.frag.slang` (19 lines) and
  `shaders/multipass/tonemap.vert.slang` (45 lines) — full read.
- `shaders/stress/tonemap.frag.slang` (19 lines) and
  `shaders/stress/tonemap.vert.slang` (47 lines) — full read, diffed
  against the multipass pair (`diff` — only header-comment lines differ,
  shader logic is byte-for-byte identical in both frag and vert).
- `shaders/material/material.slang:320-348` (`RxMaterialGlobals::exposure`)
  and `shaders/material/forward_entry.slang:213-220`
  (`color.rgb *= exp2(gMaterialGlobals.exposure);`) — the actual exposure
  attachment point samples 08/09 use, applied INSIDE the forward-lit pass,
  not inside the tonemap shader.
- `samples/08_gltf_viewer/main.cpp:38-50` (header comment: reuses
  `shaders/multipass/tonemap.{vert,frag}.slang` VERBATIM, binding
  constraint "you may NOT touch shaders/multipass/"), `:179-186,1544,1765,2143`
  (`--exposure` CLI flag plumbing). `samples/09_scene/main.cpp` — grepped
  for `exposure`/`Exposure`: **zero hits**; that sample has no `--exposure`
  flag at all and runs at the material system's default (0.0, neutral).
- `src/rx_graph/include/rx_graph/pass_signature.h` (102 lines, full read).
- `src/rx_graph/include/rx_graph/resources.h` (172 lines, full read).
- `src/rx_graph/transient_pool.h` (344 lines, full read) and
  `src/rx_graph/transient_pool.cpp` (`acquireImage()`, lines 16-52, full read).
- `src/rx_graph/render_graph.cpp:720-735` (backbuffer's `samples` forced
  to `VK_SAMPLE_COUNT_1_BIT` at compile time).
- `src/rx_graph/executor.cpp:1063-1151` (`realize()`'s `acquireImage()`
  call site, threading `physical.attachment.samples` through);
  `:1310-1420` (`PassSignature` construction from `PhysicalResource`,
  including the comment on `ResolvedResource` carrying "no sample-count
  field at all"); grepped the whole file for
  `resolveImageView`/`resolveMode`/`VK_RESOLVE_MODE`/`pResolveAttachment`/
  `VkRenderingAttachmentInfo` — zero hits for any resolve-target wiring.
- `src/rx_rhi_vk/include/rx_rhi_vk/texture.h:104-107` (`Texture2D::create()`
  signature — no `samples` parameter) and `src/rx_rhi_vk/src/texture.cpp:126-137,206-217`
  (`VkImageCreateInfo.samples` hardcoded to `VK_SAMPLE_COUNT_1_BIT` in
  BOTH `create()` and `createForPresuppliedMips()`).
- `src/rx_material/material_system.cpp:1997` (`multisampleState.rasterizationSamples = req.pass.samples;`
  — the pipeline side DOES honor a declared sample count).
- Repo-wide grep for `VK_SAMPLE_COUNT_[248]` outside default-value
  declarations, and for `samples` in `src/rx_graph/tests/*.cpp` — zero
  non-1 sample count anywhere in the codebase, including tests.
- `samples/08_gltf_viewer/references/{loading_state,loaded_scene}.png`,
  `samples/09_scene/references/grid_scene.png` (`find`, confirmed present);
  `samples/08_gltf_viewer/CMakeLists.txt:114-115,137,148`,
  `samples/09_scene/CMakeLists.txt:109,128,140` (`add_test` registrations);
  `samples/09_scene/main.cpp:2542-3035` (the D17 gate mechanism:
  `rx::samples::loadRgba8Png`/`compareToReference`/`GateResult`, the
  backbuffer-format restriction to R8G8B8A8/B8G8R8A8 families at line
  2566, and the C1 shadow discrimination re-proof pattern at lines
  3015-3035 as the template for a new HDR-value discrimination proof).
  `samples/05_multipass` and `samples/07_stress` were checked for the same
  `references/` pattern — **neither has one**; both instead use in-frame
  probe-pixel assertions (`kShadowProbeWorld`/`kLitProbeWorld`,
  `samples/05_multipass/main.cpp:60-64`), not committed reference PNGs —
  so "existing sample pixel gates" concretely scopes to 08 and 09 only.
- `samples/09_scene/main.cpp:2813-2827` (`Allocator::createHostVisibleBuffer`
  + `vkCmdCopyImageToBuffer` + `Buffer::invalidate()`/`mappedData()`) — the
  existing generic GPU-to-host readback pattern, already used for the
  final RGBA8 backbuffer capture; directly reusable, unmodified in
  mechanism, for a raw-HDR-texel readback (row 1) by pointing it at the
  "hdr" resource instead of the backbuffer and reinterpreting the copied
  bytes per the ruled format.
- `third_party/CMakeLists.txt` — grepped case-insensitively for
  "filament": **zero hits**, confirming Filament is not yet vendored.
- `gh search code "R11G11B10F" repo:google/filament` and
  `gh search code "hdrFormat" repo:google/filament` (fetched 2026-08-20)
  — surfaced `filament/src/details/Renderer.cpp`,
  `filament/src/details/Renderer.h`, `filament/src/RendererUtils.{h,cpp}`,
  `filament/include/filament/Options.h`.
- WebFetch, `https://raw.githubusercontent.com/google/filament/main/filament/src/details/Renderer.cpp`
  (fetched 2026-08-20) — `FRenderer::getHdrFormat()`: opaque scene color
  branches on `view.getRenderQuality().hdrColorBuffer`, LOW/MEDIUM →
  `mHdrQualityMedium`, HIGH/ULTRA → `mHdrQualityHigh`; translucent views
  short-circuit to `mHdrTranslucent`. `gh search code "mHdrQualityMedium"
  repo:google/filament` confirms the field is constructed as
  `mHdrQualityMedium(TextureFormat::R11F_G11F_B10F)`
  (`filament/src/details/Renderer.cpp`), with a runtime
  `driver.isRenderTargetFormatSupported()` fallback guard. Quoted
  fallback-chain description (medium: R11F_G11F_B10F→RGB8; high:
  RGB16F→RGBA16F→R11F_G11F_B10F→RGB8) is WebFetch's own summarization of
  the constructor body, not a line-numbered direct quote — flagged in
  Verification health.
- Attempted (failed/inconclusive, flagged honestly): WebFetch against
  `docs.vulkan.org/spec/latest/chapters/formats.html` and the
  `KhronosGroup/Vulkan-Docs` `formats.adoc`/`required_format_support.adoc`
  raw sources, seeking the exact mandatory-format-support table row for
  `VK_FORMAT_B10G11R11_UFLOAT_PACK32` vs `VK_FORMAT_R16G16B16A16_SFLOAT`
  — the page/file is too large for the fetch tool's summarizer to reach
  the table section (truncated before it; the required-support appendix
  file also 404'd at the guessed path). Not re-attempted further after
  three tries to conserve budget; the row-3 format ruling below treats the
  precise mandatory-feature-bit table as UNVERIFIED THIS SESSION and
  routes the actual acceptance criterion through an in-task
  `vkGetPhysicalDeviceFormatProperties` empirical query instead of a
  memorized spec citation — see row 3.
- Khronos `VkFormat` spec page (`docs.vulkan.org/spec/latest/chapters/formats.html`,
  fetched independently 2026-08-20) — direct read of the
  `VK_FORMAT_B10G11R11_UFLOAT_PACK32`/`VK_FORMAT_R16G16B16A16_SFLOAT`
  bit-layout descriptions (row 10); the mandatory-`STORAGE_IMAGE`-support
  table itself hit the same truncation limit already noted above for the
  `COLOR_ATTACHMENT` case (five attempts at different anchors, all
  truncated) — not re-litigated as a separate failure, folded into the
  same known tool limitation.
- WebSearch synthesis (2026-08-20) corroborating `B10G11R11_UFLOAT_PACK32`'s
  membership in Vulkan's guaranteed-`STORAGE_IMAGE_BIT`-without-extended-
  formats set — secondary-tier, flagged as such in row 10.
- Microsoft DirectX-Specs / community sources (fetched via WebSearch
  2026-08-20) — `DXGI_FORMAT_R11G11B10_FLOAT` UAV typed-store (mandatory
  FL11_0+) vs. typed-load (optional) distinction, the cross-API
  corroboration in row 10.

---

## The matrix

| # | Criterion | Verification method & evidence expectation | Current code state (verified, cited) | Disposition | Proposed binding acceptance criterion |
|---|-----------|----------------------------------------------|-----------------------------------------|-------------|------------------------------------------|
| 1 | HDR intermediate proven by VALUE: a >1.0 radiance input survives to the tonemap input | GPU test w/ driver labels (lavapipe + real NVIDIA per the plan's real-GPU-verification constraint), exact-value readback gate: render a synthetic radiance value >1.0 (e.g. 4.0) into the named HDR scene-color image via a controlled draw, copy the RAW texel (not the tonemapped backbuffer) to a host-visible buffer, and assert the decoded float is the expected value within the ruled format's own precision tolerance. Discrimination proof required: the same test run against a deliberately wrong UNORM8-family target must show the value clamped to ~1.0, proving the test actually discriminates rather than passing by construction. | No sample or test today asserts a raw HDR value round-trips — every existing gate (row 8) compares the FINAL tonemapped RGBA8 backbuffer only, which cannot distinguish "HDR preserved then correctly tonemapped" from "HDR silently clamped, coincidentally similar after tonemap." The readback MECHANISM already exists and is reusable unmodified: `samples/09_scene/main.cpp:2813-2827` — `Allocator::createHostVisibleBuffer` + `vkCmdCopyImageToBuffer` + `Buffer::invalidate()`/`mappedData()`, today aimed at the backbuffer; pointing the same copy at the "hdr" resource (via `PassContext::image("hdr")`/`Executor::resolveImage`) is a mechanical retarget, not new infrastructure. | consume-now | A device-free-adjacent GPU test asserts a >1.0 radiance value read back from the HDR resource matches the expected value within the ruled format's ULP/mantissa precision, and a companion mutant (wrong-format target) is shown to fail the same assertion (pasted evidence, per the plan's discrimination-proof discipline). |
| 2 | The chosen format's precision characteristics are documented, with a test pinning the format | Device-free test: a single named engine constant (not 4 independent sample-local ones) is asserted `== VK_FORMAT_<ruled>` via `static_assert`/unit test; doc comment at the declaration states bit layout (mantissa/exponent per channel, alpha presence, signed/unsigned range) per row 3's ruling. | No shared constant exists today. Verified: `samples/05_multipass/main.cpp:192`, `samples/07_stress/main.cpp:132`, `samples/08_gltf_viewer/main.cpp:133`, `samples/09_scene/main.cpp:146` each independently declare `constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;` — four copies, zero engine-owned source of truth; a grep of `src/rx_graph`/`src/rx_scene` for any `kSceneColorFormat`-shaped symbol returns nothing. | consume-now | One engine-owned named constant (location per Task 1 spec — `rx_graph` or `rx_scene` is the natural home given the "graph treats scene color as the documented seam" scope text) replaces all 4 sample-local declarations; a unit test pins its value; its doc comment states the ruled format's precision facts. |
| 3 | **Format ruling: B10G11R11 (`VK_FORMAT_B10G11R11_UFLOAT_PACK32`) vs RGBA16F (`VK_FORMAT_R16G16B16A16_SFLOAT`)** | Facts-plus-recommendation ruling row (coordinator decision, not resolved here) + an in-task empirical `vkGetPhysicalDeviceFormatProperties` query (both drivers) confirming `COLOR_ATTACHMENT_BIT`/`COLOR_ATTACHMENT_BLEND_BIT`/`SAMPLED_IMAGE_FILTER_LINEAR_BIT` for whichever format is ruled — NOT a memorized spec-table citation (see Verification health: the exact mandatory-support table row could not be independently re-fetched this session). | **RGBA16F** is the STATUS QUO: all 4 samples already use it (row 2's citations); zero output-format churn if kept, though the plan's own text already schedules gate regeneration regardless (row 8). 64 bits/texel, full alpha, signed range, 2x the memory/bandwidth of B10G11R11 on every read/write this buffer sees for its entire consumer chain (mip chain, SSR, glass read, bloom, tonemap, TAA history) — Steam Deck is an explicit hardware floor for this phase's benchmark gates (repo CLAUDE.md "Performance is an exit criterion"; plan `docs/superpowers/plans/2026-08-20-phase5-techniques.md:65-73`'s vendor-matrix note names the Deck explicitly), where UMA bandwidth is the tighter constraint than desktop VRAM capacity. **B10G11R11** is 32 bits/texel (half the footprint), unsigned-only (radiance is never negative — no loss there), no alpha channel, and is Google Filament's own OWN DEFAULT choice for exactly this buffer at its LOW/MEDIUM quality tier — `FRenderer::getHdrFormat()` (`filament/src/details/Renderer.cpp`, fetched 2026-08-20) returns `mHdrQualityMedium`, constructed as `TextureFormat::R11F_G11F_B10F` (bit-identical layout to Vulkan's `B10G11R11_UFLOAT_PACK32`), for OPAQUE views; Filament's HIGH/ULTRA opaque tier steps up to plain `RGB16F` (still alpha-free), and only the TRANSLUCENT path uses `RGBA16F` — i.e. Filament's own precedent draws the SAME opaque/alpha-needs-a-separate-buffer split this ticket is deciding, rather than using one RGBA16F buffer for everything. | needs-coordinator-decision | **Recommendation: B10G11R11 for the opaque-lit scene-color working image**, matching Filament's own precedent tier-for-tier and taking the bandwidth win on the Deck floor the performance mandate requires proving. The commonly-cited counter-argument ("transmission blending needs alpha on this buffer," per this ticket's own research brief) does NOT hold under the charter's own transmission model — see Conflicts below: real transmission is a screen-space REFRACTION READ of scene color as a texture input, not a fixed-function alpha-blend WRITE onto it, so the opaque buffer's alpha channel would sit at a constant 1.0 and never carry information regardless of format. If the coordinator instead wants one buffer serving both opaque and later transparent/glass composite work (simpler graph, Filament explicitly does NOT do this), RGBA16F is the correct fallback. Whichever is ruled, an in-task format-support query against both the lavapipe and real-NVIDIA ICDs is still required before treating it as portable. |
| 4 | **FG6 MSAA policy ruling: resolve-attachment semantics in the graph vs a recorded no-MSAA/TAA-first ruling** | Facts-plus-recommendation ruling row (coordinator decision). | The charter's frame-pipeline target places TAA at the very end of the pipeline, after tone mapping — TAA is already the plan's committed temporal-stability mechanism for this phase (frame-pipeline target citation above). The render graph's history-resource machinery (`isHistory`/`PinnedHistoryEntry`, `src/rx_graph/transient_pool.h:118-181`) already exists specifically to support ping-ponged temporal resources, i.e. the TAA-first path is not starting from zero. Real hardware MSAA resolve, by contrast, requires threading a genuinely new axis through TWO currently-broken layers before it could work at all — see row 5's structural findings: the pass-signature/pooling layer already carries `samples` as live metadata, but it dead-ends unused at actual image creation. | needs-coordinator-decision | **Recommendation: rule FG6 no-MSAA/TAA-first for this ticket**, recording the rejection with rationale (TAA is already the committed temporal AA path per the charter; adding real MSAA resolve support requires the row-5 structural fixes regardless of whether MSAA ships this phase, so it is better sequenced as its own later ticket than bundled into the scene-color seam's first landing) and documenting the graph's current single-sample assumption explicitly (row 5's citations) so a future MSAA ticket has a named, correct starting gap rather than rediscovering it. |
| 5 | FG6 resolve semantics implemented+tested (if row 4 rules MSAA in), OR the graph's current single-sample assumption is explicitly documented (if row 4 rules MSAA out) | Either: GPU test w/ driver labels rendering a real multisampled attachment and asserting the resolved output matches a known-good single-sample reference within an anti-aliasing-consistent tolerance (if ruled in); or: a doc comment/spec section stating the graph's `samples` field is presently metadata-only and citing exactly where it dead-ends (if ruled out, per row 4's recommendation). | **Two independent, concrete structural facts, both verified first-hand:** (a) `VkSampleCountFlagBits samples` IS a real per-attachment axis with correct plumbing through `AttachmentDesc` (`resources.h:83`) → `PassSignature` (`pass_signature.h:58`, hashed) → `TransientPool::acquireImage()`'s pooling key (`transient_pool.h:222-223`, `transient_pool.cpp:16-24`) → `MaterialSystem`'s pipeline creation, where `multisampleState.rasterizationSamples = req.pass.samples;` (`material_system.cpp:1997`) DOES correctly derive the pipeline's own multisample state from a declared sample count. (b) But it dead-ends at actual GPU resource creation: `Texture2D::create()`'s signature (`texture.h:104-107`) has NO `samples` parameter at all, and both of `Texture2D`'s image-creation paths hardcode `imageInfo.samples = VK_SAMPLE_COUNT_1_BIT` unconditionally (`texture.cpp:133`, `texture.cpp:213`) — `TransientPool::acquireImage()` receives and stores a `samples` value (`transient_pool.cpp:31-43`) but never passes it to `Texture2D::create()`. **Consequence**: declaring `samples > 1` on an `AttachmentDesc` today would build a pipeline correctly expecting N samples (per material_system.cpp:1997) bound against an ACTUALLY 1-sample image — a pipeline/attachment sample-count mismatch, which dynamic rendering's own valid-usage rules make an immediate validation error, not silent misbehavior. Separately, zero resolve-target wiring exists anywhere: no field on `AttachmentDesc`/`PhysicalResource` (`resources.h`, full read) carries a resolve-target reference, and `executor.cpp`'s `VkRenderingAttachmentInfo` construction (`:1310-1420`) never sets `resolveMode`/`resolveImageView` (grepped the whole file, zero hits). Repo-wide, no test anywhere exercises `samples != VK_SAMPLE_COUNT_1_BIT` (grepped `src/rx_graph/tests/*.cpp`); `render_graph.cpp:730` explicitly forces the backbuffer's own resolved `samples` to 1 (correct, since the backbuffer is never multisampled), but nothing else in the codebase ever sets it to anything but the default either. | consume-now (documentation branch, per row 4's recommendation) — **would be needs-coordinator-decision if row 4 is overridden to rule MSAA in**, since the row-5(a) pipeline mismatch would then need fixing as PART of this same ticket, a materially larger scope than the ticket's current "Files" list anticipates | If row 4's no-MSAA/TAA-first recommendation stands: document, in the scene-color conventions doc this ticket already owns, that (1) `samples` is real per-attachment metadata correctly threaded to `PassSignature`/pipeline creation but (2) is NOT currently honored by `Texture2D`/`TransientPool` image allocation (cite `texture.cpp:133,213`) and (3) no resolve-attachment field exists on `AttachmentDesc`/`PhysicalResource` — so a future MSAA ticket's actual scope starts at "thread samples through Texture2D::create + add a resolve-target field," not "wire up an already-working axis." This closes the finding the ticket asked for ("the graph's assumption documented") with exact, re-discoverable citations. |
| 6 | The graph treats "scene color" as the documented seam later tasks (mip chain — Task 22, SSR — Task 26, transmission — Tasks 23-24, bloom, TAA) attach to | Review/doc-audit criterion (not a runtime test): a committed conventions doc names the resource, its format, its declaring pass, and enumerates every downstream consumer named in the charter's frame-pipeline target, with each consumer's own future task cited. | Verified absence of any such convention today: all 4 samples (row 2's citations) independently invent their own `"hdr"`-named resource via `.addColorOutput("hdr", swapchainRelativeDesc(kHdrFormat))` — same resource NAME by coincidence of copy-paste, not by any engine contract enforcing it; nothing prevents a 5th sample from naming it differently or sizing/formatting it differently. The charter's frame-pipeline target (citation above) already lists every downstream consumer this seam must serve, in binding order. | consume-now | A short conventions doc (co-located with wherever row 2's named constant lands) states: the resource name (`"hdr"` or a ruled successor), its format (row 3's ruling), its size class (swapchain-relative), and a table mapping each frame-pipeline-target stage after "opaque lighting" to the task that will attach to it (mip chain → Task 22, SSR → Task 26, glass/transmission → Tasks 23-24, bloom/tonemap → this ticket + Task 32, TAA → its own task). All 4 samples migrate to declare against the shared constant instead of their own `kHdrFormat` (ties into row 9). |
| 7 | Tonemap shader's current exposure/tonemap attachment point is documented as the point Task 4 (exposure) and Task 32 (AgX/ACES) attach to — NOT redesigned by this ticket | Doc-audit criterion: the conventions doc (row 6) or a dedicated shader-header comment states the exact current hook shape. | Verified: `shaders/multipass/tonemap.frag.slang` and `shaders/stress/tonemap.frag.slang` are functionally byte-identical (diff shows only header-comment text differs) — both hardcode plain Reinhard, `float3 mapped = hdr / (1.0 + hdr);`, with NO exposure/uniform/push-constant input of any kind beyond a bindless `hdrTextureIndex`/`hdrSamplerIndex` pair (`tonemap.vert.slang:11-14` in both copies). Samples 05/07 have NO exposure control anywhere (grepped both files + their shaders — zero hits). Samples 08/09 instead apply exposure UPSTREAM of tonemap, inside the forward-lit material pass: `shaders/material/forward_entry.slang:220`, `color.rgb *= exp2(gMaterialGlobals.exposure);`, sourced from `RxMaterialGlobals::exposure` (`shaders/material/material.slang:344`). Sample 08 exposes this via `--exposure` (`samples/08_gltf_viewer/main.cpp:179-186`); sample 09 has NO `--exposure` flag and runs at the material system's default (0.0, neutral) — confirmed by an exhaustive grep of `samples/09_scene/main.cpp` for "exposure" (zero hits). Sample 08's own header comment (`main.cpp:38-50`) explicitly documents this split and a prior task's binding constraint: "you may NOT touch shaders/multipass/." | consume-now | Document (not redesign) that: (1) the tonemap SHADER's own hook is a bindless-texture-index push constant only, hardcoded Reinhard, identical logic in both `shaders/multipass` and `shaders/stress` copies; (2) exposure is applied upstream, in the material forward pass, via `RxMaterialGlobals::exposure`, for samples 08/09 only; (3) samples 05/07 have no exposure path at all. This is the exact, cited attachment point Task 4 (physical-units exposure, a DIFFERENT ticket) and Task 32 (AgX/ACES) must read before making any change — this ticket does not alter tonemap.slang's algorithm. |
| 8 | Existing sample pixel gates regenerated with provenance + discrimination floors intact; zero validation errors both drivers | Exact-value/tolerance readback gate (existing D17 mechanism) re-run after this ticket's changes land, PLUS a provenance note (what changed, why the new baseline is more correct, verified against what) for every regenerated PNG, PLUS the existing discrimination re-proof pattern (e.g. `samples/09_scene/main.cpp:3015-3035`'s C1 shadow-disabled re-proof) confirmed still discriminating. Both drivers (lavapipe + real NVIDIA) per the plan's real-GPU-verification constraint. | Concretely scoped: `samples/08_gltf_viewer/references/{loading_state,loaded_scene}.png` (registered via `add_test(NAME sample_08_gltf_viewer_headless ...)`, `CMakeLists.txt:137`) and `samples/09_scene/references/grid_scene.png` (`add_test(NAME sample_09_scene_headless ...)`, `CMakeLists.txt:128`, plus `sample_09_scene_stress_headless`, `:140`) are the ONLY image-diff pixel gates in the samples this ticket touches — `samples/05_multipass` and `samples/07_stress` have no `references/` directory at all and instead assert fixed WORLD-SPACE PROBE pixels in-frame (`kShadowProbeWorld`/`kLitProbeWorld`, `samples/05_multipass/main.cpp:60-64`), a different (still valid, but non-image-file) gate mechanism this criterion should also re-verify but cannot "regenerate a PNG" for. The D17 gate itself (`samples/09_scene/main.cpp:2542-3035`) reads the committed reference via `rx::samples::loadRgba8Png`, compares via `rx::samples::compareToReference` into a `rx::samples::GateResult` (failing-pixel count/fraction), and its backbuffer-format check is restricted to the R8G8B8A8/B8G8R8A8 families (`main.cpp:2566`) — i.e. these gates compare the FINAL tonemapped, backbuffer-format output, not the HDR intermediate; a pure internal-format/naming consolidation with NO change to the tonemap algorithm should in principle be BYTE-IDENTICAL on these gates (a stronger, more useful assertion than "regenerate and hope it looks similar"), while an actual B10G11R11-vs-RGBA16F format SWITCH (row 3) is expected to shift LOW-DYNAMIC-RANGE post-tonemap pixels by a small, precision-bounded amount and legitimately needs new baselines. | consume-now | Whichever of row 3/4's rulings change actual rendered output, regenerate exactly the two files named above with a provenance note distinguishing "byte-identical, no visual change expected" (pure refactor case) from "new baseline, bounded precision-shift documented" (format-switch case); re-run the `sample_09_scene` C1 discrimination re-proof and confirm it still fails-on-purpose against a broken build; zero validation-layer messages on both the default lavapipe CI driver and a real-NVIDIA `--validate` run, per the plan's standing real-GPU-verification corrective. |
| 9 | `kHdrFormat`-duplication finding — ownership boundary | Review criterion: whichever ticket claims it must show the OTHER 3 sample sites migrated too, not just its own. | Verified duplication (row 2's citations): 4 independent `constexpr VkFormat kHdrFormat` declarations across `samples/05_multipass`, `07_stress`, `08_gltf_viewer`, `09_scene`. This ticket's own scope text ("the graph treats scene color as the documented seam") is naturally the CONSUMING/promoting end of this fix (row 2/6 already require creating the one shared constant); Task 5 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:256-284`, ticket #41 by the +36 offset every other ticket in this round follows) is a separately-dispatched "sample-driven API-gap audit" whose OWN scope text is general ("sweep samples 07-09 for hand-rolled engine facilities") and does not name `kHdrFormat` specifically, but structurally the fix belongs wherever the shared constant is INTRODUCED, i.e. here — see Open Questions, this needs an explicit one-line coordinator call regardless of my read. | needs-coordinator-decision | Whichever ticket lands first introduces the shared constant AND migrates all 4 call sites in the same commit (per the plan's own "Samples are pure consumers... no sample hand-rolls what the engine provides" global constraint, `plan.md:88-93`) — grep-enforced zero remaining `kHdrFormat` sample-local declarations is the acceptance bar, matching Task 5's own stated methodology for its other audit rows. |
| 10 | Row 3's format ruling, supplementary factor: `COLOR_ATTACHMENT` + `STORAGE_IMAGE` usage on core Vulkan 1.3 — relevant because Task 2/#38's compute passes (SSR, froxel volumetrics) may need to read-modify-write scene color as a storage image, not only sample it | Khronos `VkFormat` bit-layout facts read directly this session; the mandatory-format-support/`STORAGE_IMAGE` table itself could NOT be fetched intact this session (five separate WebFetch attempts against `docs.vulkan.org/spec/latest/chapters/formats.html` at different anchors, all truncated before reaching that table — same tool limitation row 3's own Verification-health note already flags for the `COLOR_ATTACHMENT_BIT` case) — corroborated instead via WebSearch synthesis and a DirectX cross-API precedent, both lower-confidence than a direct table read. | Bit-layout facts VERIFIED FIRST-HAND (Khronos `VkFormat` spec page, fetched 2026-08-20, independent of row 3's own citation): `VK_FORMAT_B10G11R11_UFLOAT_PACK32` = 11-bit R + 11-bit G + 10-bit B, unsigned float only (no negative values — irrelevant for radiance), no alpha. `VK_FORMAT_R16G16B16A16_SFLOAT` = 4×16-bit signed float including alpha. **Storage-image support, SECONDARY-SOURCE ONLY:** WebSearch synthesis of Khronos spec content places `B10G11R11_UFLOAT_PACK32` among the formats Vulkan guarantees `STORAGE_IMAGE_BIT` for without the optional `shaderStorageImageExtendedFormats` feature — consistent across independent search hits but NOT confirmed by directly reading the primary table this session. **Cross-API corroboration, DIRECT READ** (Microsoft DirectX-Specs / community sources): the DirectX equivalent (`DXGI_FORMAT_R11G11B10_FLOAT`) requires UAV typed-STORE only from Feature Level 11_0 onward; UAV typed-LOAD is explicitly OPTIONAL — a real, historically-hit hardware gap for packed-float formats specifically in read-modify-write compute paths, the exact shape a compute SSR/volumetrics pass reading-then-writing scene color would need. `R16G16B16A16_FLOAT`/RGBA16F carries no such caveat on either API — full read+write UAV/storage-image support is universal, with no known vendor exception. | needs-coordinator-decision — folds into row 3's ruling as an ADDITIONAL factor, not a separate axis | This does not by itself overturn row 3's B10G11R11 recommendation (compute-writable scene color is Task 2/#38's concern, not yet scheduled against this exact resource), but it is a real, asymmetric risk row 3 does not currently weigh: RGBA16F's storage-image/UAV read+write support has no known vendor caveat on any API surveyed, while B10G11R11's does (secondary-sourced for Vulkan directly; directly-sourced and historically real on the closest cross-API analogue). If row 3 is ruled to B10G11R11, `VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT` (not just `COLOR_ATTACHMENT_BIT`) must be empirically confirmed via `vkGetPhysicalDeviceFormatProperties2`/`vulkaninfo` on both the dev NVIDIA GPU and real Deck/RADV hardware BEFORE any later task (SSR, volumetrics) writes a compute pass against it as a storage image — never assumed, exactly row 3's own "in-task empirical query, not a memorized spec claim" discipline, extended to cover the storage-image feature bit specifically, not only color-attachment. |

---

## Conflicts

- **Row 3's format ruling vs. this research brief's own stated premise.**
  The STEP-2 instructions handed to this research explicitly frame the
  RGBA16F case as needed because the opaque scene-color buffer "must
  later... be consumed by transmission blending which needs alpha (Tasks
  23-24)." This is in direct tension with the techniques-phase charter's
  own, already-committed glass ruling: "Glass is REAL transmission, never
  alpha blending... Refraction samples a scene-color source: screen-space
  refraction first, environment/probe fallback on miss"
  (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:398-411`).
  Under that model, Tasks 23-24's glass geometry READS the opaque
  scene-color buffer as a texture input (for refraction) and WRITES its
  own composited result elsewhere (or via its own shader-computed blend,
  not a hardware alpha-blend equation reading a meaningful destination
  alpha) — the opaque buffer's own alpha channel is never load-bearing
  either way, since an "opaque" buffer's alpha is definitionally always
  1.0. Filament's own precedent (row 3) independently confirms this split:
  its OPAQUE views never use an alpha-bearing format even at its highest
  quality tier (`RGB16F`, not `RGBA16F`); only TRANSLUCENT views get
  `RGBA16F`. Flagged as a conflict because it directly undercuts the
  strongest argument for defaulting to RGBA16F — the coordinator should
  resolve this explicitly rather than defer to the brief's original
  framing by default.
- **Ticket's own "Files" list omits `shaders/stress/`.** The ticket text
  names `shaders/multipass`/`shaders/material` tonemap entry as the touch
  point, but `shaders/stress/tonemap.{vert,frag}.slang` is a THIRD,
  currently byte-identical-logic copy (row 7's citations) that is not
  mentioned anywhere in the ticket's file list. If this ticket's
  implementation edits `shaders/multipass/tonemap.frag.slang` for the new
  scene-color read convention without also touching `shaders/stress`'s
  copy, the two copies silently diverge in LOGIC for the first time (they
  already diverge in comments only). Not a contradiction of anything
  already ruled — just an omission worth surfacing before implementation
  starts, since the ticket's acceptance criteria (zero validation errors,
  gates intact) implicitly cover sample 07 too.

## New gaps

- **The MSAA image-creation dead-end (row 5) is a genuinely new finding,
  not previously named by the ticket, plan, or FG6 registry text.** FG6's
  own wording ("MSAA policy decision + resolve-attachment semantics in the
  graph") reads as though the only missing piece is resolve semantics —
  i.e. it assumes the `samples` axis itself already works end-to-end and
  only the RESOLVE step is unbuilt. That assumption is false: `samples` is
  correctly plumbed through `PassSignature`/pipeline creation but silently
  dropped at the one place that actually allocates the GPU image
  (`Texture2D::create()`, hardcoded to 1 sample, no parameter to override
  it). A future ticket that rules MSAA IN without first discovering this
  would hit an immediate dynamic-rendering validation error (pipeline
  `rasterizationSamples` vs. attachment's real sample count mismatch) the
  first time it tried `samples > 1`. Recorded here so it does not need
  rediscovering.
- No other capability gap surfaced beyond what the ticket/plan/FG6 already
  scope.

## Open Questions

- **Format ruling (decisive, my recommendation): B10G11R11
  (`VK_FORMAT_B10G11R11_UFLOAT_PACK32`)** for the opaque-lit scene-color
  working image — matches Filament's own precedent at the matching
  quality tier, halves bandwidth on the Steam Deck floor the performance
  mandate requires benchmarking against, and the "needs alpha for
  transmission" counter-argument does not survive contact with the
  charter's own screen-space-refraction (not alpha-blend) transmission
  model — see Conflicts.
- **MSAA policy (decisive, my recommendation): rule FG6 no-MSAA/TAA-first**
  for this ticket, recording the rejection with rationale and documenting
  the two concrete structural gaps row 5 found (samples axis dead-ends at
  `Texture2D::create`; no resolve-target field anywhere) as the correctly-
  scoped starting point for whatever future ticket takes MSAA on for
  real — bundling that fix into this ticket would materially expand its
  "Files" list beyond what's currently scoped.
- **Storage-image factor (row 10), one line:** this does not change my
  agreement with the B10G11R11 recommendation above, but it is the one
  piece of evidence in this matrix that cuts the OTHER way and deserves
  the coordinator's explicit attention rather than being absorbed
  silently into row 3's already-decisive framing — RGBA16F's
  storage-image/UAV read+write support is universal with no known vendor
  caveat on any API, while B10G11R11's is corroborated only secondarily
  for Vulkan and has a directly-verified historical gap on the closest
  cross-API analogue (DirectX UAV typed-load, optional not mandatory).
  If B10G11R11 is ruled per row 3, this risk is fully retired by an
  in-task empirical `STORAGE_IMAGE_BIT` query (row 10's own acceptance
  criterion) before Task 2/#38's compute passes ever target this
  resource — it is a verification-order note, not a reason to overturn
  row 3's recommendation.
- **Ownership, one line:** the `kHdrFormat`-duplication finding (row 9)
  reads as this ticket's job by construction (it's the natural
  side-effect of rows 2/6's "one shared constant" requirement), but Task 5
  / ticket #41's sample-hand-roll audit is the OTHER plausible owner by
  its general "sweep samples 07-09" mandate — needs one explicit
  coordinator line assigning it before dispatch, so the two tickets don't
  either both skip it or both redundantly do it.

## Verification health

**Verified first-hand (primary source read directly, in full or in the
cited region, this session):**
- All four `kHdrFormat` sample declarations and their `.addColorOutput`
  call sites.
- `shaders/multipass/tonemap.{vert,frag}.slang` and
  `shaders/stress/tonemap.{vert,frag}.slang` — full reads, directly
  diffed against each other (not inferred).
- `shaders/material/material.slang`'s `RxMaterialGlobals` and
  `shaders/material/forward_entry.slang`'s exposure application line.
- `pass_signature.h`, `resources.h`, `transient_pool.h` — full reads, all
  172/102/344 lines each.
- `transient_pool.cpp`'s `acquireImage()` — full function read.
- `texture.h`'s `create()`/`createForPresuppliedMips()` signatures and
  `texture.cpp`'s corresponding `VkImageCreateInfo` construction — direct
  reads confirming the hardcoded `VK_SAMPLE_COUNT_1_BIT` in both paths.
- `material_system.cpp:1997`'s `rasterizationSamples` line — direct read.
- `executor.cpp`'s `realize()` (1063-1151) and the `PassSignature`
  derivation region (1310-1420) — direct reads; the whole-file grep for
  resolve-related symbols is exhaustive (zero hits, not an inference from
  a partial read).
- `render_graph.cpp:720-735` — direct read.
- The D17 gate mechanism in `samples/09_scene/main.cpp` (2542-3035) and
  both samples' `CMakeLists.txt` `add_test`/reference-PNG registrations —
  direct reads.
- The readback pattern at `samples/09_scene/main.cpp:2813-2827` — direct
  read.
- `third_party/CMakeLists.txt` — grepped in full, zero Filament hits,
  confirming non-vendored status directly rather than assuming it.
- Filament's `FRenderer::getHdrFormat()` and the `mHdrQualityMedium`
  construction — fetched directly from `raw.githubusercontent.com`
  (the actual source file, not documentation prose, matching this phase's
  own "port from Filament's CURRENT shaders/source, never docs prose"
  discipline extended here to the precedent-citation research itself) and
  confirmed via a second, independent `gh search code` hit for the
  constructor initializer.

**Inferred / lower-confidence, flagged explicitly:**
- The exact Vulkan mandatory-format-support table row for
  `VK_FORMAT_B10G11R11_UFLOAT_PACK32` (which `VkFormatFeatureFlagBits` are
  spec-GUARANTEED vs. merely commonly-supported) could NOT be
  independently re-fetched this session after three attempts (the
  `docs.vulkan.org` HTML page and two `Vulkan-Docs` raw-source guesses
  were either truncated before the table or 404'd) — row 3's proposed
  acceptance criterion deliberately routes around this by requiring an
  in-task empirical `vkGetPhysicalDeviceFormatProperties` query against
  both real drivers instead of trusting a memorized spec claim. This is a
  gap in THIS matrix's own verification, not a gap in the engine.
- Filament's exact fallback-CHAIN wording (medium: R11F_G11F_B10F→RGB8;
  high: RGB16F→RGBA16F→R11F_G11F_B10F→RGB8) is WebFetch's own
  summarization of the constructor body's surrounding logic, not a raw
  line-by-line quote — the core fact this matrix leans on (opaque default
  = R11F_G11F_B10F at low/medium, alpha-bearing format reserved for
  translucent views) IS independently corroborated by the `gh search
  code` field-initializer hit, so treated as reliable; the exact fallback
  ORDER is treated as directionally reliable but secondary.
- `gh search code` results are GitHub's own code-search index, not a
  guaranteed-exhaustive enumeration — absence of a hit (e.g. no other
  `mHdr*` format field found) is not proof of absence, only of "not
  indexed under this exact query."
- Sample 05/07's probe-pixel gate mechanism was confirmed to exist and to
  NOT be PNG-based, but its own pass/fail tolerance semantics were not
  independently re-verified line-by-line here (out of this ticket's
  direct scope — row 8 flags it as a gate that also needs re-verification
  after this ticket's changes, without claiming to have audited its
  internals).

- Row 10's `STORAGE_IMAGE_BIT` membership claim for
  `VK_FORMAT_B10G11R11_UFLOAT_PACK32` is WebSearch-synthesized (secondary),
  not a direct read of the primary Vulkan mandatory-support table —
  consistent across independent search hits, but explicitly NOT treated
  as equivalent in confidence to the bit-layout facts (which WERE read
  directly from Khronos's own `VkFormat` page). Row 10's own acceptance
  criterion routes around this the same way row 3 already does for
  `COLOR_ATTACHMENT_BIT`: an in-task empirical driver query, not a
  memorized/secondary-sourced claim.
- The DirectX `DXGI_FORMAT_R11G11B10_FLOAT` UAV typed-load-optional fact
  (row 10) is a real, directly-sourced cross-API precedent, but it is an
  ANALOGUE, not a Vulkan spec citation — offered as corroborating evidence
  for why the storage-image risk asymmetry is plausible and historically
  grounded, not as proof of Vulkan's own exact behavior.

No dead links encountered among sources actually read successfully.
