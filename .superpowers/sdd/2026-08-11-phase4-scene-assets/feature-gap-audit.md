# Feature-Gap Audit — Capability Completeness (Coordinator Blind Spots)

**Date:** 2026-08-11
**Auditor mandate:** find capabilities a production-grade renderer-middleware
needs that appear NOWHERE in the planning universe — distinct from the prior
code-correctness audit (`stage0-audit.md`) and from the spec's own G1-G15
(which fixed underspecification, not absence).

**Artifacts audited (the claimed-complete feature universe):**
`docs/superpowers/specs/*.md` (all 5, incl. the master registry's
deferred-not-dropped list and the Phase 4 seed notes),
`docs/superpowers/plans/*.md` (all 5), the full board (issues #1-#24, bodies
dumped and searched), `.superpowers/sdd/2026-08-11-phase4-scene-assets/*.md`
(research files, briefs, stage0 audit), `docs/threading.md`, `docs/abi.md`,
`README.md`, `MANUAL_VERIFICATION.md`, CLAUDE.md, plus the delivered surface
(samples 01-07, all public headers, `rx_api.h` ABI).

**Method note:** every absence claim below was verified by case-insensitive
extended-regex sweeps over that entire artifact set (multiple synonyms per
capability), then cross-checked against delivered code where the question was
"planned nowhere AND handled nowhere." An initial sweep had a broken
alternation pattern (`\|` under `grep -E` is literal); it was caught against
known-present terms ("device-lost", "cubemap") and rerun correctly — the
findings below come from the corrected sweep only.

---

## VERDICT: GAPS FOUND

**12 gaps: 3 V1-blocking, 7 V1-expected, 2 post-V1.**

The plan is unusually complete on geometry/performance/pipeline axes
(GPU-driven, meshlets, RT, temporal cluster, scheduler sharing, bindings,
loop model are all registered). The blind spots cluster in three areas the
registry never looks at: **the lighting environment** (everything except the
one directional light), **the host-integration operational surface** (what an
embedding engine sees when things degrade, hang, or need runtime content),
and **display-output policy** (AA, HDR, window edge states).

---

## Gaps

| # | Capability | Evidence of absence | Why needed | Phase fit | Priority |
|---|-----------|--------------------|-----------|-----------|----------|
| 1 | **Environment lighting: skybox rendering + image-based ambient (IBL: irradiance + prefiltered specular)** | Sweep for `skybox / environment / IBL / image-based / irradiance / ambient` over all specs/plans/seeds/board: zero planning hits. `cubemap` appears once, as the RT-reflections *fallback* mention (registry line 164). Spec D22's StandardPBR has NO ambient term at all (baseColor/MR/normal/occlusion/emissive/alpha + manual exposure); the occlusion texture has nothing to occlude. Registry's nearest item, "global illumination probes" (layer 9), is dynamic GI — it covers neither a sky pass nor the baseline environment term. | Every PBR evaluation starts with a glTF viewer next to Khronos/three.js/Filament references, all IBL-lit. Metals with no specular environment render near-black; sample 08's DamagedHelmet under one directional light will visibly lose that comparison. Games need a sky. libktx (already committed) loads cubemap KTX2 with mip chains — the ingredient is in-tree with no consumer. | Minimal constant/flat ambient: could land with D22 in Stage 1 (one material term). Skybox pass + prefiltered environment: head of techniques phase, before/with the temporal cluster. Registry entry now. | **V1-blocking** |
| 2 | **Punctual lights (point/spot): light types, attenuation/units model, their shadow paths (cube/spot maps), and `KHR_lights_punctual` import** | Spec D19/plan Task 14 define `DirectionalLightDesc` only; D15 shadow culling is directional-only; D21/registry shadows are "cascades" (directional). Registry's "clustered/deferred lighting grids" is a *scaling structure* that presupposes punctual lights but no artifact ever commits the light types themselves. `KHR_lights_punctual` appears exactly once — in `research-p4-assets.md:20` as a fastgltf capability listing, never adopted; the importer plan (Task 10) imports meshes/materials/textures/skins and never mentions glTF lights (or cameras). | Point/spot lights are a near-universal game requirement (lamps, torches, muzzle flashes). A renderer whose only light is directional is disqualifying at evaluation. Importer side has the same retrofit economics as seed 14's skinning argument: dropping lights at import forces asset re-processing a phase later. | Light types + forward shading: techniques phase (with clustered grids). glTF light (and camera) import: cheap Stage 1/next-phase importer addition — parse-and-preserve now, per the seed-14 precedent. Point/spot shadows: techniques phase. | **V1-blocking** |
| 3 | **Engine-generated dynamic content APIs: runtime mesh submission + per-frame texture updates (the enabler for host UI/text, particles, video textures, procedural geometry)** | Sweeps for `procedural mesh / dynamic mesh / runtime mesh / texture update / dynamic texture / video texture`: zero planning hits anywhere. `RxTextureDesc` is create-once by documented contract ("read synchronously during createTexture2D(), never retained past that call"); `IRxTexture` has no methods; `GeometryPool::upload` is internal and import-fed. The streaming-phase registry blurb covers *file assets* (refcount/streamed, concurrent GPU-object creation); the SDK-phase blurbs cover bindings/scheduler-sharing/loop-model. No artifact defines how a host engine gets ITS OWN per-frame-generated vertices or texel updates onto the GPU. | RendererX's product is middleware for engines that own gameplay. Those engines must render UI/text, particles, video, debug views, procedural/deformed meshes — all of which are per-frame CPU-generated content. bgfx's transient/dynamic buffers and dynamic textures are the precedent and are among its most-used APIs. Without this contract, RendererX can only draw imported static scenes — a middleware disqualifier in the V1 timeframe. Internals (Uploader, ParamArena, pools) mostly exist; what's absent is the designed contract. | Contract design: scene-submission/SDK specs (the ABI projection D23 already schedules is the natural vehicle). Should be named in the registry NOW so the SDK spec is obligated to answer it. | **V1-blocking** |
| 4 | **Device-lost / GPU-hang handling policy + crash diagnostics** | RHI spec (2026-08-09, Error handling) translates `VK_ERROR_DEVICE_LOST` into an engine error code — detection only. Sweeps for `device_fault / aftermath / breadcrumb / TDR / watchdog / hang` (as GPU-hang): zero planning hits. Registry layer 12 lists GPU markers, debug lines, leak tracking, perf counters — no crash diagnostics, no recovery story. | Shipped games hang GPUs. Middleware must define the contract: is device loss fatal-with-diagnostics or recoverable (device recreation + resource reload)? And hosts need actionable artifacts (pass-level breadcrumbs, `VK_EXT_device_fault` payload through the existing D23 log sink) to triage player crash reports. Silence here reads as "your game crashes and we shrug" to an evaluating engine team. | Policy decision + host-facing contract: SDK-phase spec. Breadcrumbs/device-fault capture: tooling phase (layer 12), riding the existing per-pass debug-label machinery. | V1-expected |
| 5 | **Host-facing capability/adapter surface: GPU enumeration + selection, and a caps/feature-degradation report** | Optionality-with-fallback is committed per-feature *internally* (registry: "startup capability query with a raster fallback") but no artifact plans an API for the HOST to (a) enumerate/choose the physical device (vk-bootstrap auto-selects today; `RxMaterialSystemDesc` is a single bridge pointer) or (b) learn what got enabled — RT? mesh shaders? aniso ceiling? which fallbacks engaged? Sweeps for `adapter / GPU selection / physical device selection / caps`: zero planning hits. | Engines build graphics-options menus, telemetry, and iGPU/dGPU laptop policies on exactly this surface (bgfx `caps`, D3D adapter enumeration precedents). The engine-wide optionality principle NEEDS a reporting channel to be honest with the host — "graceful degradation" that the host can't observe is silent degradation. | SDK phase: a POD caps struct + adapter selection in the creation desc, designed with the DLL surface. Registry entry now. | V1-expected |
| 6 | **Anti-aliasing policy: MSAA support-or-rejection decision + resolve-attachment semantics** | Sweep for `MSAA / multisampl / sample count` over all planning artifacts: only plumbing constants (Phase 1 plan hardcodes `VK_SAMPLE_COUNT_1_BIT`; PassSignature/AttachmentDesc carry a `samples` field that nothing ever sets above 1). The graph resource model has no resolve-attachment concept (grep of `rx_graph` headers: "resolve" appears only as name-resolvers). TAA is committed (temporal cluster); MSAA is neither committed nor rejected anywhere. | A forward renderer on the Deck floor is the classic MSAA-friendly configuration, and evaluators will ask. TAA-only is a defensible answer — but it is currently an *accident*, not a decision, and resolve attachments (also needed for any future MSAA-off-into-post path) must be designed into the graph's resource/aliasing model before history+aliasing work ossifies it. | Record the decision in the registry now; design resolve semantics in the techniques-phase spec alongside the temporal cluster. | V1-expected |
| 7 | **Window edge states: minimize/zero-extent swapchain, occluded-window behavior, DPI/content-scale policy** | Sweeps for `minimiz / occluded / zero extent / DPI / content scale`: zero planning hits (only "minimizes redundant transitions"). Delivered code: `Device::recreateSwapchain` (device.cpp:468-525) has no zero-extent guard; `window.cpp` handles no minimize/restore events; resize is tested, minimize is not. The stage0 code audit did not flag it — the path simply doesn't exist to audit. | Alt-tab/minimize is the first thing any playtester does; a 0x0 swapchain recreate fails validation or crashes, and rendering while minimized burns battery (Deck). DPI: SDL3 absorbs most of it, but HUD/overlay scaling on hidpi/Gamescope-scaled displays needs a stated policy before the SDK freezes the surface. | Small platform-hardening item: fits any phase touching present (samples already exercise present paths); SDK phase at the latest. | V1-expected |
| 8 | **HDR display output + swapchain colorspace policy** | "HDR" appears in artifacts only as the internal RGBA16F transient's nickname. Sweeps for `HDR10 / scRGB / colorspace (of swapchain) / color management / EDR / swapchain_colorspace`: zero planning hits. The post cluster (registry layer 9) lists tone mapping/color grading/upscalers — all implicitly SDR-out (phase 3 plan: "linear→sRGB handled by the UNORM/sRGB swapchain"). | Mid-gen 2026 expectation for AA titles, and the project's OWN floor hardware ships it: Steam Deck OLED exposes HDR through Gamescope. Retrofit touches tonemap, swapchain creation, and UI compositing — exactly the "expensive to change later" shape that D13 (reversed-Z) was pulled forward to avoid. | Techniques phase, with the tonemap/post-stack work (output-transform stage + `VK_EXT_swapchain_colorspace` ladder). Registry entry now. | V1-expected |
| 9 | **Renderer-wide VRAM budget + memory-pressure response** | D9/G13 cover the *geometry pool's* growth/stats only. Sweeps for `memory budget / memory pressure / VRAM / VK_EXT_memory_budget`: zero planning hits (the only "memory pressure" hit is about CI runner noise). Registry layer 12 has "memory leak tracking" — a different capability. The streaming-phase blurb (refcounted/streamed assets) never mentions budgets or eviction. | Middleware shares the GPU with the host engine's own allocations; it must report its footprint and respect a host-set budget, and the Deck's UMA makes over-commit a hard failure, not a slowdown. Streaming (already planned) is unimplementable without a budget/eviction policy — the phase exists but its central input is unregistered. | Streaming phase owns it (residency/eviction); `VK_EXT_memory_budget` polling + host-facing stats can precede it cheaply. Registry entry now. | V1-expected |
| 10 | **Surface/window ownership for embedding: host-provided native window handles** | Sweeps for `native window / window handle / HWND / host-provided / embed / surface ownership`: zero planning hits. The SDK-phase registry answers loop ownership (host owns `main()`), scheduler sharing, and bindings — but never who owns the WINDOW. Today rx_platform creates its own SDL3 window; fixed constraint 5 ("SDL3 is the windowing library") can be read as "renderer owns the window," but that reading is nowhere recorded as a decision. | Embedding engines and editors overwhelmingly have their own window management; "can you render into my HWND/X11 window?" is a standard middleware evaluation question (bgfx `platformData.nwh` precedent; SDL3 can wrap foreign handles via `SDL_CreateWindowWithProperties`). Either support it or record renderer-owned-window as a deliberate product constraint — currently the question is unasked. | SDK-phase spec must answer it explicitly (support via SDL3 foreign-window properties, or a recorded rejection). | V1-expected |
| 11 | **Consumer screenshot/readback + capture API** | Readback exists only inside test gates/samples. Sweeps for `screenshot / frame capture / video capture` as a *product feature*: zero planning hits (all hits are CI/test evidence rows). | Photo modes, thumbnails, bug-report attachments, marketing capture. Cheap: the readback machinery already exists; only the public contract is missing. | SDK/tooling phase, riding the ABI projection. | Post-V1 |
| 12 | **Latency control: frames-in-flight configurability + present-wait pacing** | `kFramesInFlight = 2` is a compile-time constant (frame_sync.h:76); no artifact discusses configurable in-flight depth, `VK_KHR_present_wait`, or input-latency management. (FPS cap and Gamescope pacing cooperation ARE recorded — seed 1 — this row is only the uncovered residue.) | Competitive/latency-sensitive titles ask for 1-frame-in-flight or present-wait pacing; middleware normally exposes the knob. Minor scope, but the constant is baked into pool sizing everywhere, so a late retrofit is wide. | Profiling/instrumentation phase (where seed 1 already parks pacing) or SDK phase. | Post-V1 |

---

## Near-misses checked and found already covered

Each of these was a candidate gap; the sweep found it genuinely present in
the planning universe (artifact cited):

- **FPS cap / frame limiter + Gamescope pacing cooperation** — seed note 1
  (profiling phase; public API SDK phase). *Bookkeeping nit:* seed 1 demanded
  re-recording in the Phase 4 spec's deferred list and the spec's list omits
  it — the capability is safe (seed notes are standing artifacts) but the
  carry-forward discipline slipped once.
- **Frame-time HUD** — seed 1 (profiling phase); sample 09's ImGui HUD covers
  the dev-side interim.
- **Pipeline pre-caching / warmup UX** — VkPipelineCache delivered (corrupt-
  cache regression test, plan Task 8); Fossilize/offline variant tooling
  recorded deferred (Phase 3 spec D7 + deferral list).
- **Cascaded shadows** — registry + spec D21 (explicitly techniques-phase).
- **Occlusion culling (HiZ), GPU-driven culling/indirect, meshlets, mesh
  shaders, visibility buffers** — registry, with sequencing.
- **Hardware RT with raster fallbacks** — registry (optionality principle).
- **LOD management, skinning, morph targets** — registry layer 8; skin data
  preserved at import (seed 14, issue #18 closed).
- **Animation playback** — recorded decision: consumer-side or ozz-animation
  (seed 14); renderer contract = joint palettes. Deliberate, not absent.
- **Tiled/clustered light culling** — registry ("clustered/deferred lighting
  grids") — the *grids* are covered; the light types are not (gap 2).
- **TAA, motion blur, upscalers (DLSS/FSR/XeSS), motion vectors, history
  resources** — registry temporal cluster + Phase 4 history resources
  (issue #19 closed).
- **Tone mapping, color grading, auto-exposure** — registry + D22
  (auto-exposure recorded deferred). SDR output gamma decided (sRGB
  swapchain, phase 3 plan).
- **Debug line drawing, GPU markers, RenderDoc/PIX, leak tracking, perf
  counters** — registry layer 12 (tooling).
- **Public log sink, vsync control, hot reload, window resize chain** —
  delivered (issues #17, #13 closed; Phase 2/3).
- **Fallback/error assets, pool budget stats** — D11, D9/G13.
- **Compressed/packed vertex formats** — D8 records packing as a deferred
  optimization; runtime block compression + mip-gen decisions recorded (D10).
- **C-ABI-first multi-language bindings, scheduler sharing with hosts,
  main-loop ownership** — registry SDK-phase commitments (2026-08-10/11).
- **Scene/asset/task ABI projection** — D23 explicitly schedules it
  (SDK phase) — so "no public scene API yet" is a recorded sequencing
  decision, not a gap.
- **Headless rendering** — machinery delivered (every sample's headless
  gate); adequate for the audience until the SDK projection.
- **macOS/MoltenVK** — registry (deferred, recorded).

## Judgment calls (honest notes)

1. **"Post stack" as a bucket.** Registry layer 9's one-liner ("post stack")
   plus the enumerated cluster (tonemap/grading/TAA/blur/upscalers) does NOT
   name bloom, SSAO, or DoF. I ruled this *underspecified, not absent* — the
   bucket exists and the techniques spec will enumerate it — so no gap row.
   Flagging it here so the techniques spec author knows evaluators expect
   bloom + an AO story (RT-AO's raster fallback is implied but unnamed).
2. **Punctual lights (gap 2) despite "clustered grids."** I claim the gap
   because a scaling structure for many lights is not a commitment to the
   light types, units/attenuation model, their shadows, or their import —
   none of which any artifact states. Reasonable people could call this
   "implied"; the importer omission alone (lights/cameras silently dropped,
   with no logged-and-skipped entry like COLOR_0 got) justifies the row.
3. **Gap 3 rated V1-blocking on product grounds.** Nothing *rendered today*
   is wrong; the rating reflects that an engine-evaluation in the SDK
   timeframe fails without a dynamic-content contract, and that the registry
   — which exists precisely to keep such things from being forgotten — has
   no entry for it.
4. **Ruled out as not-gaps for this product/timeframe:** multi-GPU (out of
   V1 scope for middleware; even AAA engines rarely ship it — worth one
   registry line as an explicit non-goal, at most), OIT (sorted blend per
   D14 is the V1 answer), decals and simple fog (expressible by consumers
   through the material system + blend partition at V1; deferred-decal
   *techniques* are post-V1), volumetrics (post-V1 techniques bucket), text
   input/IME (consumer-side; renderer input exists for samples), audio/
   physics/game logic (out of scope BY DESIGN per product intent — not
   audited).
5. **Render-to-texture for gameplay / split-screen** — judged expressible by
   the existing architecture (declared graph passes + per-view
   DrawListBuilder); no dedicated feature card exists, but no design is
   missing either. The SDK projection of the graph should demonstrate a
   camera-to-texture case to prove it; noted, not gapped.
6. **Scope discipline:** this audit did not re-litigate anything
   `stage0-audit.md` covered (code correctness), and treats research files as
   version-fact sources per the spec's own author's note — a capability
   mentioned only in research (e.g. `KHR_lights_punctual`) does not count as
   planned.
