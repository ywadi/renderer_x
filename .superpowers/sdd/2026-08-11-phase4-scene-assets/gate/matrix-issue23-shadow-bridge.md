# Completeness matrix — issue #23: Shadow quality bridge: fitted frustum, slope-scaled bias, PCF

**Plan task:** Task 22, "Shadow quality bridge (D21)"
(`docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:419-423`).

**Spec decisions binding this ticket:** D21 (shadow quality bridge,
design doc:315-320) is the primary decision. D13 (reversed-Z for the
main camera, :225-235) explicitly binds this ticket per its own plan
text (*"Reversed-Z main-camera migration lands here for the scene
path"*) and, critically, D13 itself states shadow maps stay
**standard-Z** in Phase 4 — this shapes several rows below. D15
(culling: frustum + shadow-caster, :247-261) supplies the light-frustum-
fitting *input* (visible bounds from `DrawListBuilder`, Task 19) this
ticket consumes rather than derives itself. D16 (test content strategy,
:263-270) and D17 (tolerance pixel gates, :272-281) bind the GPU-probe
methodology. D24/D25/D26/D27 (memory-budget/eviction, UploadTicket,
GPU-driven readiness, main-thread pre-resolution) are the binding-rule
invariants this gate must enforce wherever they touch this ticket.

**Ticket body (`gh issue view 23`):** *"Production-credible single
shadow map for the scene path (spec D21): light ortho fitted to visible
bounds, slope-scaled depth bias (vkCmdSetDepthBias), 3x3 PCF;
reversed-Z migration of the scene path's main depth lands here.
Acne/peter-panning/softness GPU probes."* No comments/amendments on the
issue as of this session.

**Sources consulted (in-repo):**
- `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:1-24`
  (Global Constraints), `:350-367` (Task 18, scene proxies + reversed-Z
  camera — the upstream dependency), `:368-407` (Task 19,
  `DrawListBuilder` — supplies the "visible bounds" this ticket fits
  against), `:419-423` (Task 22, this ticket).
- `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md`
  D13, D15, D16, D17, D21, D24-D27 (full text read).
- Delivered code (full files/sections read, verified 2026-08-18):
  `samples/05_multipass/main.cpp` (the existing, explicitly-superseded
  shadow baseline — camera/light projection setup :212-320, fixed pipe
  state :892-910/:1034-1050/:1155-1160, `kShadowMapSize`/`kDepthFormat`
  :189-190), `shaders/multipass/lit.frag.slang` (manual single-tap
  comparison, full file), `shaders/multipass/shadow.vert.slang` (full
  file), `shaders/multipass/scene_types.slang` (full file),
  `src/rx_graph/executor.cpp` (depth clear-value sites :639-653,
  :1110-1125), `src/rx_material/material_system.cpp` (hardcoded
  `depthCompareOp`/rasterization state, `getPipeline()` :1694-1824),
  `src/rx_graph/include/rx_graph/pass_signature.h` (full file —
  attachment-shape-only scope, explicit fixed-function-state exclusion),
  `src/rx_material/include/rx_material/material_system.h` (`bindInstance`/
  `PipelineRequest` — no fixed-function-state or compare-op axis).
  Grep sweeps (2026-08-18, full-repo, non-test files): `depthCompareOp`/
  `VK_COMPARE_OP` (hits: `samples/03_bindless_mesh/main.cpp:614`,
  `samples/05_multipass/main.cpp:904,1046`,
  `samples/07_stress/main.cpp:642`, `material_system.cpp:1771` — five
  independent hardcoded `VK_COMPARE_OP_LESS` sites, zero
  `VK_COMPARE_OP_GREATER*` anywhere); `depthBias`/`DepthBias` (zero
  hits anywhere in `src/`/`samples/` — no `vkCmdSetDepthBias` call
  exists in this codebase today); `depthClamp`/`VK_EXT_depth_clip`
  (zero hits); `compareEnable` (zero hits — no comparison sampler
  exists anywhere today); `src/rx_scene`/`src/rx_asset` directories
  (confirmed absent — Stage 2 is specified but not yet dispatched, per
  the gate research brief's own framing).

**Sources consulted (external, fetched/searched 2026-08-18):**
- Fitted light frustum / crop-matrix: NVIDIA GPU Gems 3 Chapter 10
  ("Parallel-Split Shadow Maps on Programmable GPUs", Dimitrov),
  LearnOpenGL's "Cascaded Shadow Maps" guest article
  (`learnopengl.com/Guest-Articles/2021/CSM`) — search digests.
- Texel snapping: search digest citing the standard
  "quantize-frustum-size-and-snap-center-to-texel-increments" technique
  (commonly attributed to Microsoft's "Common Techniques to Improve
  Shadow Depth Maps" DirectX tech article,
  `learn.microsoft.com/.../common-techniques-to-improve-shadow-depth-maps`
  — surfaced by search, not independently re-fetched and quoted this
  session).
- Depth bias: D3D formula (`learn.microsoft.com/.../direct3d11/d3d10-graphics-programming-guide-output-merger-stage-depth-bias`,
  search-digest-quoted: *"Offset = m × D3DRS_SLOPESCALEDEPTHBIAS +
  D3DRS_DEPTHBIAS, where m = max(|∂z/∂x|, |∂z/∂y|)"*); Vulkan's
  parameter names/semantics fetched directly from
  `github.com/KhronosGroup/Vulkan-Guide` `chapters/depth.adoc` and the
  `VkPipelineRasterizationStateCreateInfo` member docs (the literal
  Vulkan-spec bias equation text could not be retrieved through the
  fetch tool across three attempts — see Verification health).
- Reversed-Z: Nathan Reed, "Depth Precision Visualized"
  (`reedbeta.com/blog/depth-precision-visualized`) — search digest.
- Depth clip/clamp: Vulkan spec `VK_EXT_depth_clip_enable` refpage and
  `VkPipelineRasterizationStateCreateInfo::depthClampEnable`
  (`docs.vulkan.org/spec/latest/chapters/primsrast.html`, fetched
  directly, quoted below).
- Comparison samplers / hardware PCF: Vulkan spec
  `docs.vulkan.org/spec/latest/chapters/textures.html` (depth-compare-
  operation section, fetched directly, quoted below).
- PCF tap patterns, acne/peter-panning, front-face culling:
  LearnOpenGL "Shadow Mapping" (`learnopengl.com/Advanced-Lighting/Shadows/Shadow-Mapping`),
  Matt Pettineo, "A Sampling of Shadow Techniques"
  (`therealmjp.github.io/posts/shadow-maps`) — search digests.
- Shadow-map resolution precedent: Unity HDRP/URP shadow-resolution
  docs, general Steam Deck settings-guide community sources — search
  digests, no Deck-specific first-party number found (see Verification
  health).

---

## The matrix

| Feature | First-tier precedent (named, cited) | Phase-4 disposition | Library support (verified, cited) | Proposed acceptance criterion |
|---|---|---|---|---|
| Light ortho frustum fitted to visible bounds | Universal CSM/single-map precedent: project the camera frustum's (or its visible slice's) corners into light space, take the AABB, use it as the crop/ortho extent (GPU Gems 3 ch.10, Dimitrov; LearnOpenGL CSM article — both search-digested, fetched/confirmed 2026-08-18). | consume-now | Already **decided**, not merely precedented: D15 (design doc:247-255) commits to exactly this variant — *"directional light gets an ortho frustum fitted to the camera frustum's world bounds, with casters extruded conservatively along the light direction so off-screen casters still cast."* This is the "fit-to-camera-frustum-bounds + caster extrusion" family, not "fit-to-tight-scene-AABB" (a different, tighter-but-costlier variant some engines use) — worth naming explicitly since the ticket body says "fitted to visible bounds" without specifying which family. VERIFIED absent today: sample 05's light frustum is a hand-tuned, scene-content-independent CONSTANT (`kLightOrthoHalfSize = 9.0F`, `samples/05_multipass/main.cpp:222`, sized only to exceed `kGroundHalfSize * sqrt(2)` — never fitted to anything). | This ticket's own acceptance criterion is narrower than it may first read: the AABB-fitting computation itself is `DrawListBuilder`'s (Task 19) `buildShadow()` output (`ShadowLists`, per the plan's Task 19 interface text) — Task 22 CONSUMES a fitted extent, it does not compute one. GPU test (this ticket's scope): given a `ShadowLists`-supplied extent, the shadow pass's ortho projection is built from EXACTLY that extent (no additional hand-tuned margin reintroduced) and a caster just outside the camera's screen-space view but within the extruded light-space bound still appears in the shadow map (the D15 "off-screen caster" case, `research-p4-scene.md:133` cites this as "the classic correctness trap"). |
| Texel snapping (shimmer prevention) | Standard technique: quantize the light-frustum's world-space size to a fixed step and snap its center to shadow-map-texel-size increments as the camera/frustum moves, so texel-to-world mapping stays constant frame-to-frame (search digest, commonly attributed to Microsoft's CSM technique article, fetched 2026-08-18 via search — not independently re-fetched and quoted verbatim this session). | consume-now | UNVERIFIED whether this specific technique is named anywhere in this project's own planning artifacts — grepped `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md` and the plan for `texel\|snap\|shimmer`: zero hits. D21's "production-credible" framing (*"will not survive Sponza"*, contrasting with sample 05's fixed single tap) implies this is in scope by intent, but it is not literally named in D21's own text. | GPU test: render the SAME static scene from two slightly different camera positions that produce two different (but overlapping) fitted light extents; without snapping, a static caster's shadow edge would shift by a sub-texel amount between the two renders (shimmer); with snapping, the shadow edge for the shared visible region is pixel-identical across both renders. This is a genuinely new test methodology for this codebase — sample 05 has no camera movement to shimmer in the first place (fixed top-down camera, `main.cpp:212-214`). |
| Slope-scaled depth bias — formula and Vulkan mechanism | Standard technique: `bias = constantBias + slopeBias × m`, where `m` is the surface's maximum depth slope relative to the light (D3D's own documented formula, quoted above: *"Offset = m × SlopeScaleDepthBias + DepthBias"*, `learn.microsoft.com` D3D11 docs, search-digest-quoted 2026-08-18). Vulkan exposes the identical shape dynamically via `vkCmdSetDepthBias(constantFactor, clamp, slopeFactor)` on `VkPipelineRasterizationStateCreateInfo`'s three named fields (`depthBiasConstantFactor`/`depthBiasClamp`/`depthBiasSlopeFactor`, `Vulkan-Guide/chapters/depth.adoc`, fetched directly 2026-08-18: *"the depth bias can be calculated"* from these three, and `depthBiasClamp` requires the `depthBiasClamp` device feature or must be `0.0`). | consume-now | VERIFIED absent: zero `vkCmdSetDepthBias`/`depthBias*` occurrences anywhere in `src/`/`samples/` (grep, 2026-08-18) — sample 05 uses a fixed SHADER-CONSTANT bias instead (`kShadowBias = 0.0025`, `lit.frag.slang:21`, applied as a manual `fragmentDepth - kShadowBias > storedDepth` comparison, `:54`), explicitly NOT the rasterizer-level `vkCmdSetDepthBias` mechanism D21/this ticket calls for. This is genuinely new plumbing: `VK_DYNAMIC_STATE_DEPTH_BIAS` must be added to the shadow pipeline's dynamic-state list (today's dynamic states are only `VIEWPORT`/`SCISSOR` everywhere this was checked — `material_system.cpp:1785`, and each sample's own `:2`-element `dynamicStates` arrays). | GPU test: a fixed-slope test plane (e.g. a ramp at a known angle to the light) renders with NO acne at that specific known slope using the slope-scaled formula's predicted bias value, verified against a closed-form expectation (not just "looks clean") — the acne/peter-panning probe row below covers the qualitative pass/fail; this row is about the formula being wired correctly (constant-only bias would under-bias steep slopes and over-bias shallow ones, the exact defect slope-scaling exists to fix). |
| Slope-scaled bias sign convention under reversed-Z — does it flip? | The ticket/brief explicitly asks this; the general principle (from Nathan Reed's "Depth Precision Visualized" and the D3D/Vulkan bias formula above): under standard-Z (`compare = LESS`, depth increases away from the viewer), a POSITIVE bias pushes a caster's stored depth FARTHER from the light (the intended "push the occluder back" direction); under reversed-Z (`compare = GREATER`, depth DECREASES away from the viewer), achieving the identical "push away from the light" physical effect would require the bias's EFFECTIVE sign to invert, since larger depth values now mean CLOSER, not farther. | **N/A-Phase-4 — the premise does not apply to Phase 4's actual shadow pass.** | VERIFIED via D13's own explicit text (design doc:230-232): *"Shadow maps keep standard-Z in Phase 4 (ortho depth is less precision-critical; bias tuning is calibrated for it; the cascades work in the techniques phase revisits)."* Only the MAIN camera migrates to reversed-Z this phase (confirmed: `rx::scene::Camera`'s projection helpers are the D13 migration's sole surface, plan Task 18 text, :350-367) — the shadow-caster pass this ticket builds stays standard-Z (`compare = LESS`, clear = 1.0), so `vkCmdSetDepthBias`'s constant/slope factors keep their ordinary, non-inverted sign throughout Phase 4. | This row exists to PREVENT a plausible but wrong "fix": an implementer aware of reversed-Z elsewhere in the same task (D13's main-camera migration lands in this SAME ticket) could reasonably but incorrectly assume the shadow pass's bias needs an inverted sign too, since both changes land together. Acceptance criterion: a code comment at the shadow pipeline's `vkCmdSetDepthBias` call site states explicitly that the shadow pass is standard-Z (citing D13) and the bias sign is the ordinary (non-reversed) convention — pre-empting exactly this confusion for whoever revisits it at the cascades-phase shadow-map reversed-Z migration D13 itself defers. |
| Depth clip vs. clamp for casters behind the light's near plane | Standard technique: enabling `depthClampEnable` on the shadow-caster pipeline prevents near-plane-clipped casters from vanishing from the shadow map entirely (clamping their depth to the near/far planes instead of discarding the primitive), trading a small depth-precision cost at the clamped extremes for correctness — a common shadow-caster-pass setting (search-digested precedent, LearnOpenGL/general shadow-mapping literature). | consume-now | VERIFIED Vulkan mechanism, fetched directly (`docs.vulkan.org/spec/latest/chapters/primsrast.html`, 2026-08-18), quoted: *"depthClampEnable controls whether to clamp the fragment's depth values... If the pipeline is not created with `VkPipelineRasterizationDepthClipStateCreateInfoEXT` present then enabling depth clamp will also disable clipping primitives to the z planes of the frustum."* — i.e. `depthClampEnable=VK_TRUE` alone (no extension needed) already gets both effects (clamp + implicit no-clip) for a device that supports the `depthClamp` feature. `VK_EXT_depth_clip_enable` (registry refpage, search-digested 2026-08-18) exists only for the DECOUPLED case (clamp without disabling clipping, or vice versa) — not required here since the shadow pass wants BOTH together, which core `depthClampEnable` already provides. Zero `depthClamp`/`VK_EXT_depth_clip` occurrences anywhere in this codebase today (grep, 2026-08-18) — the `depthClamp` PHYSICAL DEVICE FEATURE must also be verified enabled/requested at device-creation time (not checked as part of this ticket's own grep — that's `Device::create`'s scope, cross-referenced, not independently re-verified this session). | GPU test: a caster geometrically positioned so part of it extends behind the light's near plane still casts its FULL shadow silhouette (not a clipped/truncated one) with `depthClampEnable=VK_TRUE` on the shadow pipeline; a regression variant with clamp disabled demonstrates the defect this setting fixes (missing/truncated shadow), proving the test actually exercises the setting rather than passing vacuously. |
| 3×3 PCF tap pattern — box vs. Poisson | Standard trade-off: a regular NxN grid is simpler and faster but produces visible banding at shadow edges; Poisson-disk (or blue-noise) distributed taps reduce banding for the same or fewer samples but cost more setup complexity (rotation-per-pixel, precomputed disk) (LearnOpenGL/Pettineo "A Sampling of Shadow Techniques", search-digested 2026-08-18). | consume-now (regular 3×3 box grid, per the ticket's own literal text) | The ticket body and D21 both specify "3×3 PCF" literally (a fixed regular grid, 9 taps) — not a Poisson pattern, which would typically use a different (often non-square, e.g. 8- or 12-tap) count. This is a SCOPE match to the ticket's own text, not a first-tier-precedent gap: 3×3 box PCF is itself a legitimate, common baseline (the technique LearnOpenGL's own introductory shadow-mapping article teaches before Poisson as a refinement). | GPU test named directly by the plan (`plans/2026-08-11-phase4-scene-assets.md:423`): *"PCF softness probe (edge gradient spans ≥2 texels)"* — a shadow edge sampled across several adjacent screen pixels shows a smooth gradient (not a hard binary transition) spanning at least 2 shadow-map texels' worth of screen-space distance, distinguishing real filtering from a single-tap comparison (sample 05's current approach, which produces a hard 0/1 edge by construction — `lit.frag.slang:54`'s `? 0.0 : 1.0` ternary). |
| Comparison-sampler PCF (hardware-filtered) vs. manual multi-tap | Standard technique: a `VkSampler` with `compareEnable=VK_TRUE` turns each texture fetch into a depth comparison, and Vulkan explicitly permits BILINEAR filtering of the per-texel comparison RESULTS (not the raw depth values) — one linear-filtered tap already approximates a 2×2 PCF footprint "for free," and a 3×3 box of such taps gets a smoother 4×4-ish effective footprint at 9 samples instead of the 9 hard single-texel comparisons a non-comparison sampler would need for the same box size. | consume-now (recommended; not yet a ruling — see Conflicts) | VERIFIED via Vulkan spec, fetched directly (`docs.vulkan.org/spec/latest/chapters/textures.html`, 2026-08-18), quoted: *"If the image view has a depth/stencil format... a depth comparison is performed. The result is 1.0 if the comparison evaluates to true, and 0.0 otherwise"* and, on filtering: *"If the value of `magFilter` is `VK_FILTER_LINEAR`... then D may be computed in an implementation-dependent manner... proportional to, or a weighted average of, the number of comparison passes or failures"* — confirming linear-filtered comparison sampling is spec-legal and gives a free small-footprint blend per tap. VERIFIED absent today: zero `compareEnable` occurrences anywhere in this codebase (grep, 2026-08-18) — sample 05's shadow sampler is explicitly documented as NOT using this mechanism (`lit.frag.slang:8-13`: *"no `VK_COMPARE_OP`-enabled `VkSampler`"*, doing the comparison manually in float arithmetic instead). | This is CLAUDE.md's fast-path-as-default policy applied directly: a comparison sampler moves per-tap filtering into fixed-function hardware instead of the shader's own arithmetic, and is the standard production mechanism (not sample 05's manual-comparison teaching-example approach). Acceptance criterion: the shadow-sampling helper for the scene path creates and uses a `VkSampler` with `compareEnable=VK_TRUE`, `compareOp=VK_COMPARE_OP_LESS` (matching the shadow pass's own standard-Z convention — see the reversed-Z-does-not-apply row above), and the 3×3 PCF loop's 9 taps each read the hardware-filtered comparison result directly (`.SampleCmp()`-equivalent in Slang) rather than manually comparing a raw depth sample as `lit.frag.slang` does today. |
| Acne-vs-peter-panning probe methodology | LearnOpenGL's shadow-mapping article names the standard diagnostic shape directly: a large, grazing-angle-lit ground plane exposes acne (self-shadowing striping from insufficient bias); a long, thin, elevated caster with a nearby receiver exposes peter-panning (shadow detaches from its caster's base when bias is too large) (search-digested 2026-08-18). | consume-now | The plan's own text (`plans/2026-08-11-phase4-scene-assets.md:423`) already commits to this exact test shape: *"acne scene (large ground plane at grazing light) renders without acne (probe variance check) and without peter-panning (contact probe)."* Sample 05 already has a WORKED PRECEDENT for probe-point derivation to build from: `kShadowProbeWorld`/`kLitProbeWorld` (`main.cpp:231-232`) are analytically-derived world points guaranteed inside/outside a known shadow, with `worldToPixel()` (`:295-320`) converting them to exact screen pixels for a closed-form (not visual/fuzzy) assertion — this pattern generalizes directly to the new scene-path probes. | Concrete scene/probe spec (deepening the plan's one-line description per this gate's charter): (1) **acne probe** — a ground plane large enough that the light's grazing angle at its far edge exceeds ~80° from the surface normal (the angle regime where insufficient slope-scaling produces visible striping); assert PER-PIXEL depth-test-pass VARIANCE across a small neighborhood of otherwise-uniformly-lit pixels stays below a fixed threshold (acne shows up as high-frequency variance in an otherwise flat-lit region — a variance check catches it where a single-pixel probe would not). (2) **peter-panning probe** — a thin vertical caster (e.g. a pole) with a receiver plane immediately at its base; assert the shadow's rendered edge is CONTINUOUS with the caster's own base silhouette within a small pixel tolerance (peter-panning shows up as a visible GAP between caster-base and shadow-edge, measurable as a horizontal pixel offset at the contact point). |
| Shadow-map resolution/format policy | Community/industry precedent (Unity HDRP/URP docs, general guidance, search-digested 2026-08-18): 1024×1024 is a broadly-cited "safe" baseline; 2048×2048 is a common "high" tier; handheld/mobile guidance generally recommends the LOW end of that range or lower. No Deck-specific first-party number was found (see Verification health). | consume-now (continue the existing convention) | VERIFIED existing in-repo convention: `kShadowMapSize = 1024`, `kDepthFormat = VK_FORMAT_D32_SFLOAT` (`samples/05_multipass/main.cpp:189-190`) — this resolves the ticket text's shorthand "D16/D32 precedent": D16 is the design-doc decision governing test-content/asset strategy (design doc:263-270, no resolution/format number of its own), and "D32" is not a spec-decision number in this project's numbering (checked: the design doc has no D28-D32) but the existing `VK_FORMAT_D32_SFLOAT` literal already adopted at `samples/05_multipass/main.cpp:190` — i.e. the brief's "D32" almost certainly refers to this format constant, not a fixed-function decision number. Community precedent (1024 "safe," 2048 "high," handheld skews low) is consistent with keeping this project's existing 1024/D32_SFLOAT choice as the floor-hardware default rather than requiring new research to justify a change. | Acceptance criterion: the scene-path shadow map continues `1024×1024`/`D32_SFLOAT` as the DEFAULT (matching the existing sample-05 precedent, satisfying the Deck floor-hardware framing without inventing a new number), with the resolution exposed as an overridable parameter (not a re-hardcoded literal in Task 22's own new code) so a later phase's cascades work is not blocked by a second hardcoded constant to hunt down. |
| Front-face culling for shadow casters (bias alternative) | Standard alternative/complementary technique: cull FRONT faces when rendering into the shadow map (so the stored depth is the caster's BACK surface), which eliminates most acne without any bias at all for closed/solid geometry — LearnOpenGL states this directly: *"Front face culling can be used to solve most of the peter panning issue"* (search-digested 2026-08-18). Known trade-off: fails for thin, open, or non-closed geometry (a single-sided plane caster has no back face to cull to, so this techique alone cannot handle it — the SAME geometry class the ticket's own doubleSided/thin-caster considerations touch). | N/A-Phase-4 | D21's own text commits explicitly to the bias-based approach (*"slope-scaled depth bias"*) as the Phase-4 mechanism, not a culling-based alternative — this is an already-made design choice, not an omission. Retrofit economics: choosing NOT to build front-face culling now is safe to defer because it is a genuinely independent, additive technique (a `cullMode` flip on the shadow pipeline) that doesn't touch data layout, vertex format, or any other call site — adding it later (if bias tuning alone proves insufficient on thin/complex Sponza-style geometry) costs nothing at existing call sites. | Not this ticket's acceptance criterion; noted for the coordinator as a low-cost fallback if the acne/peter-panning probes (row above) can't find a bias value that satisfies both simultaneously on real content — a known, named escape hatch rather than a silent gap. |
| Sample 05's simpler shadow path staying untouched | — (project-internal boundary, not an external precedent). | N/A-Phase-4 (explicit scope boundary) | VERIFIED: Task 22's own file list states this boundary directly (*"sample 05 keeps its own simpler shaders untouched — documented"*, plan:421). Confirmed nothing else in the plan lists `samples/05_multipass/` as a Task 22 file. | No acceptance criterion — a boundary, not a deliverable. Flagged only to confirm sample 05's manual-comparison/fixed-bias/no-texel-snapping approach remains a deliberately-preserved SIMPLER reference implementation, not an oversight this ticket must "fix." |
| Reversed-Z migration — depth CLEAR VALUE touch points (D13) | — (enumeration of this codebase's own call sites, not an external precedent). | consume-now | VERIFIED two DISTINCT hardcoded clear-value sites, both `VkClearDepthStencilValue{1.0F, 0}`, neither parameterized per-pass: (1) `src/rx_graph/executor.cpp:646` — an explicit `vkCmdClearDepthStencilImage` call for first-use/transient-resource initialization; (2) `src/rx_graph/executor.cpp:1119` — the dynamic-rendering `VkRenderingAttachmentInfo::clearValue.depthStencil` used for EVERY depth attachment's `LOAD_OP_CLEAR` across EVERY declared render-graph pass, confirmed by reading the surrounding code: no parameter threads a per-pass clear value in from `AttachmentDesc` (`rx_graph/include/rx_graph/resources.h:49-55` — `AttachmentDesc` has NO `clearValue` field at all). Additional non-render-graph sites: `samples/03_bindless_mesh/main.cpp:1112,1498` (that sample's own direct dynamic-rendering setup, same hardcoded literal). | This is a RENDER-GRAPH-LEVEL gap (`rx_graph`, not `rx_material`/this ticket's own library) that this ticket's scene-path migration is blocked on: the executor has no mechanism to clear a depth attachment to `0.0` for one pass (the reversed-Z main-camera pass) while clearing another to `1.0` (a standard-Z shadow pass, per the row above) in the SAME frame — today's process-wide constant cannot express both simultaneously, which the scene path needs from Stage 2 onward. Acceptance criterion: `AttachmentDesc` (or `Pass::setDepthStencilOutput`) gains a per-pass clear-value field (or a `DepthConvention{REVERSED, STANDARD}` enum that both the clear-value AND compare-op rows below derive from consistently), and the render-graph executor reads it at both cited call sites instead of the current literal. This is new `rx_graph` scope discovered by this gate, not named in Task 22's own file list (which only mentions `shaders/multipass/` and the scene shadow path) — see New gaps below. |
| Reversed-Z migration — depth COMPARE-OP touch points (D13) | — (enumeration). | consume-now | VERIFIED five independent hardcoded `VK_COMPARE_OP_LESS` sites, zero `GREATER*` anywhere (grep, 2026-08-18): `samples/03_bindless_mesh/main.cpp:614`, `samples/05_multipass/main.cpp:904,1046`, `samples/07_stress/main.cpp:642`, `src/rx_material/material_system.cpp:1771`. The LAST of these is the one that matters most for this ticket: `MaterialSystem::getPipeline()` (`material_system.cpp:1694-1824`) hardcodes ONE `depthCompareOp` for EVERY material-driven pipeline it ever builds, with no per-request axis to vary it (`PipelineRequest` — `material_system.h:52-61` — carries only `material`/`pass`/`specializationBits`, none of which can select a compare op) — and StandardPBR/Unlit (issue #8, the OTHER material ticket) draw through exactly this path for the scene's main camera pass. | This mirrors issue #8's own "Architecture gap: fixed-function pipeline-state as a variant-cache axis" finding almost exactly (both blend/cull and compare-op are `VkPipeline` fixed-function fields absent from every existing cache-key axis) — cross-referenced there, restated here because it is THIS ticket's actual blocker for D13. Two resolutions exist, and the design doc does not settle which: **(a)** if the new scene-path shadow-caster pass is built via a SEPARATE, direct `vkCreateGraphicsPipelines` call (mirroring sample 05's own shadow pipeline, which never goes through `MaterialSystem`), then `getPipeline()` only ever needs to represent the reversed-Z MAIN-camera case going forward, and the fix is a single literal flip (`VK_COMPARE_OP_LESS` → `VK_COMPARE_OP_GREATER_OR_EQUAL`) once Stage 2 lands — no new axis needed. **(b)** if StandardPBR/Unlit materials are ever expected to ALSO render into the depth-only shadow pass (reusing their own vertex logic for a depth pre-pass or the shadow map itself), `getPipeline()` needs a real compare-op axis, exactly like the blend/cull gap. Acceptance criterion: the coordinator/implementer explicitly decides (a) vs (b) before dispatch — this gate surfaces the fork, does not resolve it (see Conflicts below) — and whichever is chosen, a GPU test asserts the reversed-Z main-camera pass rejects/accepts fragments per `GREATER_OR_EQUAL` (a fragment exactly at the far plane, depth=0.0 under reversed-Z, must still pass against a cleared 0.0 background per the clear-value row above). |
| Reversed-Z migration — PROJECTION touch points (D13) | — (enumeration). | consume-now | VERIFIED: every sample's camera projection today uses an un-reversed `glm::perspective`/`glm::ortho` (with only the standard Vulkan Y-flip applied, `applyVulkanYFlip()`/equivalent — `samples/03_bindless_mesh/main.cpp:181`, `05_multipass/main.cpp:263,281`, `07_stress/main.cpp:881`, `06_materials/main.cpp:361`) — none remap the near/far mapping to `[1,0]`. Task 18 (`rx::scene::Camera`, plan:350-367) is the SOLE new source of a reversed-Z-aware projection helper (*"reversed-inf-far projection helpers... near→1/far→0 mapping"*) — this ticket does not build that helper itself, it CONSUMES it for the scene path's main camera. | Acceptance criterion (this ticket's own scope, given Task 18 supplies the matrix math): the scene path's main-camera render pass is built from `rx::scene::Camera::proj()`/`viewProj()` (Task 18's output), never a locally-reconstructed projection matrix — a code-review/grep-gateable criterion (no `glm::perspective`/`glm::ortho` call for the main camera anywhere under the scene-path sample's own `main.cpp`, only inside `rx_scene`). |
| 3×3 PCF — shadow map read path for the scene-path shadow-caster pass | Mirrors issue #8's D26.1 finding, applied to the depth-only shadow-caster pass. | consume-now | VERIFIED sample 05's existing shadow-caster VERTEX shader (`shaders/multipass/shadow.vert.slang:28-33,45-49`) reads its per-caster transform via a PUSH-CONSTANT `transformIndex` indexing `gTransforms[0][...]` — the exact push-constant-per-draw pattern D26.1 (design doc:396-401) names as the anti-pattern to avoid ("never per-draw push constants... binds StandardPBR/Unlit **and the `recordDrawList` helper**"). Task 22's own file list explicitly includes modifying "shadow path shared pieces" for the scene path — meaning this new shadow-caster vertex shader is squarely in scope for the SAME D26.1 requirement issue #8's materials carry, not exempt from it just because it has no fragment stage. | Acceptance criterion: the new scene-path shadow-caster vertex shader indexes its per-caster transform via `SV_VulkanInstanceID` into a `firstInstance`-addressed bindless storage buffer (identical mechanism and identical Slang `SV_InstanceID`-vs-`SV_VulkanInstanceID` pitfall as issue #8's own D26.1 row — cross-referenced there for the full Vulkan/Slang/D3D citation chain), NOT a push-constant `transformIndex` like `shadow.vert.slang`'s current (explicitly-superseded) approach. |
| D27 (main-thread pipeline pre-resolution) applied to the shadow-caster pipeline | Binding rule. | consume-now | The shadow-caster pipeline is, like every StandardPBR/Unlit pipeline, a `getPipeline()`-cached `VkPipeline` (or a directly-built one, per the compare-op row's unresolved fork above) — either way it is subject to the SAME D27 constraint: `DrawListBuilder`'s pre-resolution pass (Task 19) must resolve the shadow pipeline's handle on the main thread BEFORE any worker-chunk fan-out records shadow-caster draws, exactly like the lit-pass materials. | Not a new mechanism this ticket builds (Task 19 owns the pre-resolution pass itself) — this ticket's obligation is that its shadow-caster pipeline-build path is CALLABLE from that pre-resolution point (i.e. does not, itself, require per-chunk/worker-thread state). Code-review checklist item, not a runtime probe unique to this ticket. |
| D24 (memory-budget/eviction invariant) applied to shadow-caster draws | Binding rule. | consume-now | Shadow-caster draws resolve the SAME mesh/geometry handles the main lit pass does (both read from `GeometryPool`/`DrawListBuilder`'s `ViewLists`/`ShadowLists`) — D24's residency-tolerant handle resolution therefore applies identically; a mesh evicted between the main pass and the shadow pass (or vice versa) must resolve to a fallback, never a crash or raw-pointer escape. | No new test unique to this ticket — satisfied by construction as long as the shadow-caster draw path reads geometry through the same handle-mediated `DrawListBuilder`/`GeometryPool` accessors the lit pass uses, never a cached raw buffer/pointer captured across frames. |
| D25 (UploadTicket / non-blocking flush) applied to this ticket | Binding rule. | N/A-Phase-4 for this ticket specifically | The shadow-caster pass performs no uploads of its own (it only reads already-resident geometry uploaded by the import/GeometryPool path, Task 11/12's scope) — correctly N/A by the same retrofit-economics test the brief requires: nothing at this ticket's own call sites would need revisiting if D25's ticket mechanism changes shape later, since this ticket never calls `Uploader::flush()`/consumes an `UploadTicket` directly. | No acceptance criterion owned by this ticket. |

---

## Conflicts

- **The depth compare-op fork (main-camera-only vs. shared
  material-pipeline) is not settled by any existing artifact.** See the
  "Reversed-Z migration — depth COMPARE-OP touch points" row above: the
  design doc, the plan, and issue #23's own text all describe "reversed-Z
  migration of the scene path's main depth" as if it were a single,
  localized change, but whether `MaterialSystem::getPipeline()` (issue
  #8's own library) ever needs to emit a NON-reversed-Z pipeline for a
  depth-only shadow-caster use is genuinely undetermined from any
  artifact read this session. Not resolving; the coordinator should
  decide (and state explicitly in either this ticket's or issue #8's
  acceptance criteria) whether the shadow-caster pass is built through
  `MaterialSystem` at all, since the answer changes whether a new
  compare-op axis must be built or a one-line literal flip suffices.
- **The depth CLEAR-VALUE gap is `rx_graph` (executor-level) scope, not
  named in Task 22's own file list.** Task 22's file list
  (`plans/2026-08-11-phase4-scene-assets.md:421`) names only
  `shaders/multipass/` and unspecified "primary target... Stage-2 scene
  shadow path" files — it does not name `src/rx_graph/executor.cpp` or
  `rx_graph/include/rx_graph/resources.h`, yet the "depth CLEAR VALUE
  touch points" row above shows the actual blocking constant lives
  there, in a library neither this ticket nor issue #8 owns. Not
  resolving; flagged so the coordinator can either widen Task 22's file
  list explicitly or spin this off as its own small `rx_graph` task
  ahead of Task 22's dispatch (the render-graph layer has no other
  ticket in this gate pass that would naturally pick it up).
- **`vkCmdSetDepthBias` requires `VK_DYNAMIC_STATE_DEPTH_BIAS` to be
  added to the shadow pipeline's dynamic-state list, which is a small
  but real change to pipeline-creation code Task 22's own text does not
  explicitly call out** (it names the *runtime call*, `vkCmdSetDepthBias`,
  but not the pipeline-creation-time dynamic-state declaration that
  makes that call legal). Not a disagreement, just a discovered
  implementation-completeness detail worth stating so it is not
  rediscovered mid-implementation.

## New gaps

- **No render-graph mechanism exists for per-pass depth clear values or
  per-pass depth compare-ops** (both rows above) — this is a gap in
  `rx_graph`'s own `AttachmentDesc`/`PassSignature` model, not named in
  the master registry (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md`,
  grepped for `clear value\|compare op\|depth convention`, fetched
  2026-08-18: no hit) despite being a direct, structural prerequisite
  for D13's reversed-Z decision — which the design doc DOES already
  commit to. This is the shadow-bridge-ticket-specific instance of the
  SAME class of gap issue #8's own matrix names generically
  ("fixed-function pipeline-state as a variant-cache axis") — the two
  matrices independently converge on the same underlying architectural
  hole from two different tickets, which is itself signal that it is
  real and not an artifact of either ticket's own framing. Proposed
  fit: land once, ahead of or alongside Task 18 (the first task that
  needs `rx::scene::Camera`'s reversed-Z convention to actually reach a
  real `VkPipeline`/render pass), consumed by both issue #8 and this
  ticket.
- **No shadow-map-format/resolution "policy" artifact exists beyond the
  one hardcoded sample-05 constant** — D16 governs test CONTENT
  (which assets), not shadow-map SIZE/FORMAT policy; there is no
  decision doc row that would tell a future cascades-phase author what
  desktop-vs-Deck resolution tiers this project intends, only the one
  inherited literal. Low urgency (Phase 4 can continue the existing
  1024/D32_SFLOAT default without a new decision), but worth a registry
  note so the cascades-phase spec inherits an explicit number rather
  than archaeology through sample 05's source.

## Verification health

- **Verified first-hand this session:** every in-repo file/line citation
  above was read directly from the working tree on 2026-08-18 —
  `samples/05_multipass/main.cpp` (camera/light setup, pipeline-creation
  sections, `kShadowMapSize`/`kDepthFormat`), all three
  `shaders/multipass/{lit.frag,shadow.vert,scene_types}.slang` files in
  full, `src/rx_graph/executor.cpp`'s two clear-value sites with
  surrounding context, `src/rx_graph/include/rx_graph/pass_signature.h`
  and `resources.h` in full, `src/rx_material/material_system.cpp`'s
  `getPipeline()` in full, `material_system.h`'s `PipelineRequest`/
  `bindInstance()`. The five-site `VK_COMPARE_OP_LESS` grep, the
  zero-hit `depthBias`/`compareEnable`/`depthClamp` greps, and the
  confirmed absence of `src/rx_scene`/`src/rx_asset` were all run
  directly this session, not inherited from an earlier pass. The Vulkan
  spec quotes for `depthClampEnable` and the depth-compare-operation/
  linear-filtering interaction were fetched and quoted directly from
  `docs.vulkan.org` (unlike issue #8's matrix, these two specific
  sub-pages DID return usable content through the fetch tool).
- **Search-digest, not primary-source-quoted:** texel snapping's
  standard formulation, the D3D depth-bias formula's exact wording,
  Nathan Reed's reversed-Z precision argument, PCF box-vs-Poisson
  trade-offs, and the LearnOpenGL acne/peter-panning/front-face-culling
  material were all synthesized from WebSearch result digests rather
  than fetched and quoted verbatim from their own pages this session
  (budget/scope trade-off for this gate pass, consistent with issue
  #8's matrix). These are well-established, multiply-corroborated
  techniques (each digest drew from 2-3 independent search hits pointing
  at the same well-known sources), not single-source claims.
- **The literal Vulkan-spec depth-bias equation (the exact formula
  relating `depthBiasConstantFactor`/`depthBiasSlopeFactor`/
  `depthBiasClamp`/the format's minimum resolvable depth difference `r`
  to the final per-fragment bias) could NOT be retrieved** despite three
  direct-fetch attempts against `docs.vulkan.org/spec/latest/chapters/primsrast.html`
  (the fetch tool's HTML-to-markdown conversion appears to truncate
  before reaching that specific subsection every time) and one attempt
  against `registry.khronos.org` (HTTP 403). The D3D-equivalent formula
  (quoted above) and Vulkan's own parameter-NAME documentation
  (`Vulkan-Guide/chapters/depth.adoc`, fetched successfully) together
  give high confidence the two formulas are equivalent in shape, but
  this is inference from convergent secondary sources, not a
  directly-quoted primary-source equation — flagged explicitly per this
  gate's "never present an assumption as fact" rule. A follow-up fetch
  attempt using a tool that doesn't summarize/truncate (e.g. downloading
  the spec HTML/PDF directly rather than through the AI-summarizing
  fetch tool) would resolve this if the exact equation text is needed
  before implementation.
- **No Deck-specific first-party shadow-resolution guidance was found**
  — Valve does not appear to publish a recommended shadow-map-size
  tier for the Deck specifically (searched); the "1024 safe / 2048
  high, handheld skews low" framing is general community/industry
  guidance, not a Deck number. This project's own existing 1024
  precedent (sample 05) is reasonable but not independently validated
  against Deck hardware by this gate pass.
- **No dead links encountered** among directly-fetched sources.
