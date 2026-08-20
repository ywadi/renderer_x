# Matrix — P5 T26 (issue #62): SSR + probe fallback

**Plan task:** Task 26 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:734-751`), Stage 3.
**Charter binding:** frame-pipeline target (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:447-451`,
SSR sits between clustered/volumetric lighting and the scene-color mip
chain); priority order item (7) (`:452-458`); environment/indirect block
(`:422-427`, "probes as the SSR fallback... SSR itself lands with the
scene-color chain").
**Depends on:** Task 22 (scene-color mip chain — SSR's own roughness-aware
filtering explicitly rides it, per the ticket's own text) and Task 10
(IBL/environment/probe runtime integration — the fallback target on miss).
**Standing corrective binding this ticket specifically:** "Real-GPU
verification... lavapipe-only verification is NOT verification" (plan
`:65-73`) — directly relevant to SSR since ray-marching is the kind of
GPU-behavior-sensitive code this corrective was written for.

**Sources consulted:**
- Ticket body: `gh issue view 62`.
- Plan Task 26 + Global Constraints (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:55-134, 734-751`).
- Charter block (cited above).
- Google Filament @ commit `721ec800093de984cbee155e459298b6b2dbb855`
  (fetched 2026-08-20), Apache-2.0: `shaders/src/surface_light_reflections.fs`
  (`traceScreenSpaceRay()` signature + marching-strategy description,
  miss-handling code, fetched verbatim).
- Render-graph history-resource machinery, read first-hand at HEAD
  (`bf5b853`) by the same in-round research pass feeding the T22 matrix:
  `src/rx_graph/transient_pool.h:118-181,228-263,290-301`
  (`PinnedHistoryEntry`/`PinnedHistorySlot`, ping-ponged-by-name,
  never-swept persistent images — the exact mechanism the ticket's
  "history-resource consumption (Phase 4 Task 1 machinery)" text refers
  to), `src/rx_graph/include/rx_graph/pass.h:67-138`
  (`setHistoryOutput`/`addHistoryInput`).
- Software-rasterizer determinism: general technical consensus on
  Mesa llvmpipe/lavapipe as bit-exact, input-deterministic CPU
  implementations (WebSearch, 2026-08-20 — Mesa's own `docs.mesa3d.org`
  llvmpipe overview plus independent technical summaries; cross-checked
  against this repo's own already-adopted D17 tolerance-pixel-gate
  precedent, which relies on lavapipe determinism as its foundation:
  `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md:301-311`).
- Phase 5 plan's own standing corrective on real-driver verification
  (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:65-73`), read
  first-hand.

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/code support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------------|-------------------------------|
| 1 | Ray-march algorithm choice | Filament `traceScreenSpaceRay(vsOrigin, vsDirection, uvFromViewMatrix, vsZBuffer, vsZThickness, nearPlaneZ, stride, jitterFraction, maxSteps, maxRayTraceDistance, out hitPixel, out vsHitPoint)` (`shaders/src/surface_light_reflections.fs`, fetched verbatim, signature quoted exactly) — a fixed-stride, view-space linear march against the depth buffer, with jitter for temporal dithering. This signature matches Morgan McGuire & Michael Mara's published "Efficient GPU Screen-Space Ray Tracing" reference algorithm's parameter shape (general knowledge — the specific paper is a well-known, publicly documented technique; not independently re-fetched/cited as a separate source in this pass since Filament's own shipped code IS the charter's named port source and already reflects it). | consume-now | VERIFIED, exact signature and marching description (fixed `stride` pixel increments, `jitterFraction` dithering, loop bound by `maxSteps`, hard-capped at screen resolution). No hierarchical-Z acceleration structure is used by this specific function (a simpler, more portable choice — appropriate for a Vulkan-1.3-baseline, Deck-floor target per CLAUDE.md's performance policy, since Hi-Z requires an additional mip-chain-of-depth build). | Mirror-plane alignment probe per the ticket's own acceptance sketch: a known feature's reflection appears at the analytically-predicted pixel (closed-form check against the marched ray's expected screen-space path, not a fuzzy visual match). |
| 2 | Roughness-aware blur/mip selection | Ticket's own text: "Roughness-aware filtering via the Task 22 chain." | **Not a direct Filament port for THIS specific piece — flagged** | **VERIFIED, and this is a load-bearing finding:** Filament's OWN `traceScreenSpaceRay`-consuming code (the SSR-result-sampling call site, fetched verbatim) uses a FIXED mip level: `Fr = vec4(textureLod(sampler0_ssr, reprojected.xy, 0.0).rgb * fade, fade);` — LOD `0.0`, unconditionally, no roughness term anywhere in the cited function. Filament's roughness-aware IBL/reflection blur (`perceptualRoughnessToLod`, cited in the T22 matrix) is a SEPARATE mechanism applied to PREFILTERED CUBEMAP sampling, not to the raw SSR hit. RendererX's ticket explicitly requires roughness-aware SSR filtering via the Task 22 mip chain — this is a DELIBERATE COMPOSITION RendererX is building (SSR hit UV + Task 22's scene-color mip chain + a roughness→LOD formula), not something to literally copy from this one Filament function. The implementer must be told this explicitly, or a naive "port Filament's SSR" reading would ship a non-roughness-aware fixed-LOD-0 result and miss the ticket's own stated requirement. | Roughness-aware blur monotonicity per the ticket's own acceptance sketch: increasing surface roughness strictly increases the SSR result's measured blur, using the SAME pinned roughness→LOD formula the T22/T24 matrices name (Filament's screen-space `evaluateRefraction` LOD formula, `lod = max(0, (2*log2(perceptualRoughness) + offset) * invLog2sqrt5)`, reused here for consistency across every scene-color-mip-chain consumer rather than inventing a fourth formula). |
| 3 | Miss/edge handling — explicit fallback signal | Ticket's own acceptance sketch: "Edge/miss discrimination: off-screen-reflection regions provably show the probe fallback, not garbage/stretch." | consume-now | VERIFIED: `traceScreenSpaceRay()` returns a `bool` hit flag; the caller's own composition returns `vec4(0.0)` (a zero RGB, zero alpha "fade" weight) on miss — `Fr.a` acts as a hit-CONFIDENCE weight, not a binary flag, allowing the caller to BLEND between the SSR result and a fallback rather than hard-switching (Filament's own `fade` variable, present in the cited quote, implies edge-attenuation near the screen border — a soft transition, not a hard cutoff, which is the more robust choice and should be ported as such rather than simplified to a binary hit/miss). | Discrimination test: an off-screen reflection target (geometry visible in the mirror but outside the current view frustum) shows the environment/probe fallback (Task 10's mechanism) cleanly blended in at `Fr.a=0`, not a stretched/clamped edge-sample artifact — the ticket's own named failure mode to positively rule out. |
| 4 | History-resource consumption ("reflects the previous frame") | Ticket's own text names this directly, citing "Phase 4 Task 1 machinery." | consume-now | VERIFIED the exact mechanism: `PinnedHistoryEntry`/`PinnedHistorySlot` (`transient_pool.h:118-181,228-263,290-301`) — two ping-ponged physical images KEYED BY NAME (not shape), never swept, retired only at shutdown; wired via `Pass::setHistoryOutput`/`addHistoryInput` (`pass.h:67-138`). This is a real, working, ALREADY-DELIVERED mechanism this ticket reuses as-is — no new graph-layer work needed for the history piece specifically (unlike T22's mip-chain gap, this dependency is NOT a gap). | A test confirms an SSR pass correctly reads the PREVIOUS frame's history resource (a synthetic two-frame sequence where frame N's history read matches frame N-1's write, not frame N's own in-flight write — the classic history-resource off-by-one-frame bug class). |
| 5 | Lavapipe determinism for ray-march correctness gates | This repo's own D17 precedent (`docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md:301-311`): tolerance-based pixel gates against committed reference PNGs, generated and CI-enforced on lavapipe specifically, with real-driver runs reported as info-only for VISUAL correctness (though the Phase 5 plan's standing corrective, below, changes what "info-only" means for OTHER properties). | consume-now (CI-gateable on lavapipe, WITH the standing corrective's real-driver addition) | VERIFIED: lavapipe/llvmpipe is a CPU (LLVM-JIT-compiled) software rasterizer — bit-exact and input-deterministic BY CONSTRUCTION (no warp-level hardware races, no vendor-specific texture-unit rounding), corroborated by Mesa's own documentation and independent technical sources (WebSearch, 2026-08-20). This means the mirror-alignment probe (row 1), miss-discrimination probe (row 3), and monotonicity probe (row 2) are ALL legitimately committed-reference-PNG-gateable on lavapipe, matching the SAME discipline already proven out for every other Phase 4 pixel gate — SSR does not need a special exemption from CI-lavapipe testing on THIS basis. | Tolerance pixel gates for rows 1-3 run in CI on lavapipe per the existing D17 discipline (±4/255, <0.5% failing-pixel budget — reuse the existing parameters unless the spec rules otherwise). |
| 6 | What genuinely CANNOT be honestly certified on lavapipe alone | Phase 5 plan's own standing corrective, quoted directly (`:65-73`): "lavapipe-only verification is NOT verification. Every GPU-facing task/round includes a real-driver run (default ICD, `--validate`, sustained) alongside the lavapipe suite... lavapipe forgives real-driver limits (descriptor pools were the proof)." | log-don't-drop as a lavapipe-only signal / consume-now as a real-driver-run requirement | This is a POLICY row, not a library-support row — but it is DIRECTLY testable-vs-not distinction the ticket's own research brief asked for: (a) **cost/performance numbers** ("cost measured + published" per the ticket's own acceptance sketch) are MEANINGLESS on lavapipe (a software rasterizer's wall-clock timing has no relationship to any real GPU's — measuring SSR cost on lavapipe and calling it a "performance number" would violate CLAUDE.md's "measured claims only" policy on its face); these MUST be real-driver-labeled. (b) **descriptor-pool/resource-limit exhaustion under a ray-march's texture-sampling pattern** (SSR samples the depth buffer + scene-color mip chain + history resource, potentially several descriptor-bound resources per draw/dispatch) — the plan's own cited precedent ("descriptor pools were the proof") is a DIRECT warning that lavapipe silently tolerates limits real hardware enforces; this is a genuine correctness risk specific to SSR's multi-resource-binding shape, not a generic caveat. (c) **texture-LOD/anisotropic-filtering hardware behavior** at the SSR-mip-sample step (row 2) — lavapipe's software texture sampler and a real GPU's hardware texture unit are not GUARANTEED bit-identical at non-trivial LOD/filtering settings (unlike raw ray-march arithmetic, which IS deterministic on both, texture FILTERING implementation details can legitimately differ between software and hardware samplers) — this specific sub-claim is UNVERIFIED to the same certainty as row 5's broader determinism claim and should be spot-checked on the real driver, not assumed identical. | Cost/performance: real-driver-labeled Tracy numbers only, per the ticket's own text ("cost measured + published"), matching CLAUDE.md. Correctness/value probes (rows 1-3): lavapipe CI gate PLUS a real-driver sustained run asserting zero validation errors (the standing corrective's own baseline requirement for every GPU-facing task) — the real-driver run's PURPOSE here is descriptor-limit/sync-validation health, not re-proving pixel values already lavapipe-gated. Row 6(c)'s texture-filtering-difference risk: a documented MANUAL_VERIFICATION spot-check (real-driver screenshot compared qualitatively against the lavapipe reference) rather than a CI-hard-gated tolerance, since a genuine sampler-implementation difference (not a bug) could legitimately fail a tight tolerance gate. |
| 7 | Deck-floor viability | CLAUDE.md's performance-exit-criterion policy: "Features above the Vulkan 1.3 baseline... are optional capabilities with a fallback, never baseline requirements — but the fallback path is engineered to the same performance bar." SSR itself is expressible at the Vulkan 1.3 baseline (no hardware-RT dependency in the ported algorithm, row 1) — so it is NOT an "above-baseline optional capability" in CLAUDE.md's sense; it is a baseline feature that must simply BE FAST on the Deck floor. | consume-now | N/A — policy application, not a library-support fact. | Deck-tier cost numbers are a genuine Phase-5-exit requirement (per the plan's own Stage/phase-exit criteria, `:996-1004`) — this ticket's own "cost measured + published" criterion should explicitly plan for a Deck row at the Stage 3 checkpoint (T28), even though T26 itself only needs desktop numbers per its own acceptance sketch; flagged so the Deck number isn't dropped between T26's landing and T28's checkpoint. |
| 8 | TAA interplay | Ticket's own text: "TAA interplay noted for Task 33." | N/A-Phase-5 (correctly deferred, cross-ticket) | VERIFIED as correctly out of scope — Task 33 (TAA, Stage 4) is explicitly the consumer of the shared velocity-buffer/history infrastructure (plan `:872-895`), sequenced strictly after T26 (Stage 3) with no circular dependency. | No acceptance criterion owned by this ticket; Task 33's own matrix (out of this gate round's Stage-3 scope) is where the interplay gets tested. |

---

## Conflicts

None found that contradict the plan/charter/ticket text. The one
load-bearing nuance worth flagging prominently (row 2): a literal reading
of "SSR... Filament's SSR is the port source" could lead an implementer
to port Filament's fixed-LOD-0 sampling verbatim and only THEN discover
the ticket's own separately-stated "roughness-aware filtering via the
Task 22 chain" requirement isn't satisfied by that port — this is not a
contradiction in the ticket's text (both statements are present and
correct), but the port-source function itself does not contain the
roughness-aware piece, so an implementer needs both pieces of context
together, which this gate's row 2 makes explicit.

## New gaps

None beyond cross-references already registered in the T22 matrix (the
mip-chain infrastructure this ticket's roughness-aware filtering
depends on) and the T25 matrix's general D27/draw-list observations
(SSR itself does not touch `draw_list.h`, so no direct interaction was
found).

## Verification health

- **Verified first-hand:** Filament's `traceScreenSpaceRay` signature and
  miss-handling code fetched verbatim from the pinned commit.
- **Verified first-hand:** the history-resource mechanism (row 4) via
  direct reading of `transient_pool.h`/`pass.h` at HEAD (same research
  pass that fed the T22 matrix).
- **Corroborated, not first-hand-primary-sourced:** lavapipe/llvmpipe's
  determinism property (row 5) is well-established technical consensus
  (Mesa's own architecture makes it a near-tautological property of
  CPU/LLVM-JIT rasterization), cross-checked via WebSearch rather than
  reading Mesa's llvmpipe source directly — appropriate confidence tier
  for a widely-documented architectural property, not a narrow
  implementation detail requiring source-level verification.
- **Explicitly UNVERIFIED, flagged as a real risk, not asserted either
  way:** row 6(c)'s claim that lavapipe's software texture sampler could
  produce DIFFERENT (not just slower) results than real hardware at
  non-trivial LOD/filtering settings — this is a plausible-but-unproven
  risk based on general graphics-programming knowledge (software and
  hardware texture samplers are not spec-mandated to be bit-identical at
  every filtering mode), not verified against Mesa's specific lavapipe
  texture-sampling implementation in this pass. Recommended treatment
  (row 6) is a spot-check, not a hard CI gate, precisely because this
  uncertainty exists.
- No dead links encountered.
