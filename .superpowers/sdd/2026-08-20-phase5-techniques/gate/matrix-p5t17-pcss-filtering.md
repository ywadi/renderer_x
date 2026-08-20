# Completeness matrix — P5 T17 (issue #53): PCSS filtering (first-quality filter)

**Plan task:** Task 17, Stage 2 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:553-571`).
Depends on T16 (`T16→T17 sequential`, plan:986). **Charter binding**:
*"first-quality filter = PCSS (visible varying penumbra), EVSM later as
the scalable alternative"* (`docs/superpowers/specs/
2026-08-09-toolchain-platform-rhi-design.md:428-430`).

**Sources consulted (in-repo, 2026-08-20):** `shaders/material/
material.slang:127-330` (delivered `rx_sampleShadowPCF` — 3×3 hardware
comparison-sampler PCF, `SamplerComparisonState`/`SampleCmp`, the existing
production shadow-read call site this ticket layers PCSS OVER, per the
ticket's own "layered over the existing hardware-PCF path which remains
the quality-ladder fallback"); `src/rx_shadow/include/rx_shadow/
shadow_frustum.h:44-75` (`ShadowFrustumFit::worldTexelSize` — the
world-space-per-texel value a real blocker-search radius conversion needs,
already computed and available); `docs/superpowers/plans/
2026-08-20-phase5-techniques.md:65-70` (the standing real-GPU-verification
corrective — binding on this ticket's own gating strategy, see Open
Questions).

**Sources consulted (external, fetched 2026-08-20, pinned commit
`721ec800093de984cbee155e459298b6b2dbb855`, `google/filament`):**
`shaders/src/surface_shadowing.fs` (full file, 552 lines — every
shadow-sampling variant read: `ShadowSample_PCF_Hard`/`_PCF_Low`/`_PCF`
lines 1-98, `ShadowSample_VSM` lines 187-208, `ShadowSample_EVSSM` lines
210-375, dispatch switch lines 508-552); `shaders/src/
surface_shadowing.glsl` (full file — anisotropic normal-bias derivation).

---

## CRITICAL FINDING — Filament's "PCSS-class" filter is EVSM-moment-based, not classic depth-blocker-search PCSS

Filament exposes THREE runtime shadow-sampling modes
(`SHADOW_SAMPLING_RUNTIME_{PCF,EVSM,EVSSM}`, surface_shadowing.fs:6-8),
verified by direct source read:

1. **PCF** (`ShadowSample_PCF_Hard`/`_PCF_Low`, lines 24-86) — hardware
   comparison-sampler PCF against a PLAIN depth map. `_Hard` is a single
   `texture()` call against a `sampler2DArrayShadow` (i.e. exactly
   RendererX's OWN already-delivered `compareEnable`/`SampleCmp`
   mechanism). `_Low` adds a 4-tap bilinear-weighted blend (cited in-file:
   *"Castaño, 2013, 'Shadow Mapping Summary Part 1'"*) approximating a
   wider box filter from 4 real taps via bilinear-filtering math — this is
   a genuinely NEW, cheap technique RendererX's own Phase-4 3×3
   9-tap PCF does not yet use (see matrix row below).
2. **EVSM/VSM** (`ShadowSample_VSM`, lines 187-208) — classic Variance
   Shadow Maps: a MOMENT texture (mean + mean² depth, pre-filtered/
   mipmapped), Chebyshev's-inequality-derived soft shadow estimate. The
   charter explicitly defers this ("EVSM later as the scalable
   alternative") and the plan records the same deferral (plan:559,
   registry note at Task 17's close).
3. **EVSSM** (`ShadowSample_EVSSM`, lines 210-375) — Filament's actual
   HIGH-QUALITY soft-shadow filter, and the one that superficially matches
   the charter's "blocker search → penumbra estimation → variable PCF
   disc" description most closely. But it is **intrinsically built on top
   of the EVSM moment texture**, not a plain depth map: its own
   "STEP 1: DYNAMIC O(1) BLOCKER SEARCH" (lines 243-272) is a SINGLE
   `textureLod()` fetch of pre-filtered VSM moments at a computed search
   LOD — an O(1) approximate blocker estimate via mip-chain averaging, NOT
   a real per-texel search loop over individual depth samples. Classic
   Fatahalian/Lauritzen PCSS (the charter's own named "blocker search"
   technique, and the technique most engines mean by "PCSS") does a REAL
   multi-tap search over a plain depth/PCF map — it does not require a
   variance/moment texture at all.

**Consequence: a literal port of `ShadowSample_EVSSM` would silently pull
in EVSM shadow-map generation (moment textures + a mip chain) THIS task —
directly contradicting the plan's own explicit deferral of EVSM to a later
point.** See Open Questions #1 for the recommended resolution.

---

## The matrix

| Feature | First-tier precedent (cited) | Disposition | Library/source support (verified) | Acceptance criterion |
|---|---|---|---|---|
| Blocker search (classic, depth-map-based, NOT Filament's EVSSM) | Standard PCSS technique (Fatahalian/Lauritzen, NVIDIA GPU Gems — the charter's own named shape: "blocker search → penumbra estimation → variable PCF disc"; not independently re-fetched this session, see Verification health — a well-established, textbook technique). RendererX's OWN `ShadowFrustumFit::worldTexelSize` (shadow_frustum.h:67-74) already provides the exact world-to-texel conversion a search-radius-in-texels computation needs, per-cascade (T16's own output). | consume-now (recommended primary path — see Open Questions #1) | The delivered `rx_sampleShadowPCF` (material.slang:295-330) already establishes the exact shadow-map-read call-site SHAPE (per-tap `SampleCmp` against `gShadowCompareSamplers`) a blocker search's OWN taps reuse — but a blocker search needs RAW depth comparisons (is this texel's stored depth CLOSER to the light than the receiver → it's a blocker), which a `compareEnable` sampler's boolean/filtered PASS-FAIL result cannot directly give (see next row). | GPU test: a fixed-geometry scene (occluder + receiver at KNOWN, analytically-derived distances) — blocker search over a small fixed grid (e.g. a 5×5 or Poisson-disc pattern, RendererX's own choice, not Filament's, since Filament's own equivalent is moment-based) returns an average blocker depth matching the analytic expectation within tolerance. |
| A SECOND, non-comparison sampler is needed for blocker search | Vulkan mechanism fact, not an external precedent: a `compareEnable=VK_TRUE` sampler's `SampleCmp` returns a pass/fail (or filtered pass/fail RATIO) result, never the RAW stored depth value — but blocker search needs the raw depth to average multiple blockers' depths together (Fatahalian/Lauritzen's own algorithm). | **new work, in-scope** | VERIFIED absent: `material.slang`'s ONLY shadow-map-adjacent sampler is `gShadowCompareSamplers` (comparison-enabled, material.slang:143-144) — there is no plain (non-comparison) sampler bound to the shadow map texture anywhere in the delivered Phase-4/16 code. | Acceptance criterion: a plain `SamplerState` (non-comparison, e.g. `SamplerState` + ordinary `.Sample()`/`.SampleLod()`) is added alongside the existing comparison sampler, bound to the SAME shadow-map texture — the blocker-search step reads raw depth through this NEW sampler; the existing PCF-disc step continues reading through the EXISTING comparison sampler unchanged (byte-stable fallback path, per the plan's own "PCF fallback path byte-stable vs pre-task gates" criterion, plan:567). |
| Penumbra-width estimation formula | Filament's `pureGeometricRatio` derivation IS legitimately portable math, independent of the EVSSM-specific moment-fetch mechanism around it: `pureGeometricRatio = (position.z - zBlocker) * projectionParam` for directional lights (surface_shadowing.fs:277-284) — the standard SIMILAR-TRIANGLES penumbra estimate (`penumbra ∝ (receiverDepth - blockerDepth) / blockerDepth × lightSize`, the same relation Fatahalian/Lauritzen's own paper derives), just expressed in Filament's own linear-light-space-Z parameterization. | consume-now | VERIFIED via direct source read (surface_shadowing.fs:274-304, the "STEP 2: PENUMBRA ESTIMATION"/"STEP 3" sections) — this math does NOT depend on `zBlocker` having come from a moment-texture O(1) fetch specifically; it works identically if `zBlocker` instead comes from a real multi-tap depth-average blocker search (the row above). | Device-free unit test: given known (receiverDepth, blockerDepth, lightSize/bulbRadius) inputs, the computed penumbra width matches the closed-form similar-triangles prediction exactly — no GPU required, runs identically on every CI runner (see Open Questions #2's gating discussion). |
| Variable-radius PCF disc (the filtering step) | Filament's own STEP 3 (surface_shadowing.fs:306-354, structurally: compute a base LOD/tap-radius from the estimated penumbra width, then take several taps across a scaled disc/grid — the noise-jittered-rotation variant is OPTIONAL, `SHADOW_SAMPLING_EVSSM_NOISE`, off by default in Filament's own config, line 17) — the SHAPE of "scale the tap pattern by the estimated penumbra, then filter" is the portable part; the specific mip-based VSM moment READ is not (see CRITICAL FINDING). | consume-now (adapted, not literal) | The delivered 3×3 fixed-radius PCF (material.slang:295-330) is the STARTING tap PATTERN this step scales — instead of a fixed `1.0/shadowMapResolution` UV step (material.slang, existing), the new variable disc scales that same step by the estimated penumbra width (row above), reusing the EXISTING comparison-sampler taps for the actual filtering (only the RADIUS varies, not the sampling mechanism). | GPU test (the plan's own named acceptance criterion, plan:564-566): penumbra width MEASURED from rendered output widens monotonically with occluder-receiver distance at ≥3 distances — see Open Questions #2 for this specific test's driver-gating recommendation. |
| PCF fallback stays byte-stable | Plan's own explicit criterion (plan:567): *"PCF fallback path byte-stable vs pre-task gates."* | consume-now | N/A — a regression requirement; directly checkable since the existing `rx_sampleShadowPCF` call site (material.slang:295) is UNCHANGED by this ticket per the row above's own acceptance criterion (PCSS is a NEW, additional call path, not a modification of the existing one). | Regression test: every existing Phase-4/16 shadow pixel gate re-run with the quality tier set to "PCF" (not PCSS) is byte-identical to its pre-Task-17 reference. |
| PCF_Low's 4-tap bilinear-optimized box filter (a genuinely new, cheap technique) | Filament `ShadowSample_PCF_Low` (surface_shadowing.fs:35-86), cited in-file: *"Castaño, 2013, 'Shadow Mapping Summary Part 1'"* — 4 real hardware-comparison taps, bilinear-weighted to approximate a wider box filter than 4 taps alone would give (the weight-computation math at lines 51-68 is the portable part). | log-don't-drop | Not named anywhere in the plan/charter/ticket text for THIS phase — but directly relevant to the Deck-tier cost target Task 16's own resolution-tier ruling names (matrix-p5t16's own resolution-tier row): 4 taps at Deck-appropriate cost vs. the existing 9-tap 3×3, at comparable visual quality (bilinear-weighted). | Not this ticket's acceptance criterion — flagged as a candidate Deck-tier PCF variant worth a registry note if not built now (the plan's own quality-tier language, "filter selectable per quality tier," plan:568, already anticipates MORE than a binary PCF-vs-PCSS choice — PCF_Low could be a THIRD tier between them). |
| Cost measured + published, both filters, both policy tiers | Plan's own named criterion (plan:569). Standing CLAUDE.md rule (measured claims only). | consume-now | N/A — measurement requirement. | Acceptance criterion: Tracy-zoned cost numbers for {PCF, PCSS} × {desktop-tier resolution, Deck-tier resolution} published in the ledger, driver-labeled per the standing real-GPU-verification corrective. |

---

## Open Questions

1. **Should Task 17 build classic depth-based PCSS (this matrix's
   recommended primary path) or attempt a literal port of Filament's
   EVSSM (which requires EVSM shadow-map generation the plan explicitly
   defers)?** **Recommendation: classic depth-based PCSS.** This matches
   the charter's OWN literal description ("blocker search → penumbra
   estimation → variable PCF disc") more precisely than Filament's actual
   EVSSM code does (EVSSM's "blocker search" is an O(1) moment-mip
   approximation, not a real search), avoids silently pulling EVSM-map
   generation into this task against the plan's own explicit deferral, and
   reuses RendererX's existing standard-Z D32_SFLOAT shadow-map format
   unchanged (no new texture format/mip-chain-generation infrastructure
   needed). Port Filament's penumbra-estimation SIMILAR-TRIANGLES formula
   (verified portable, matrix row above) as the shared math with a
   genuinely new (not Filament-literal) real multi-tap blocker-search
   loop over the existing plain-depth shadow map. When EVSM lands later
   (registry item, charter's own "later as the scalable alternative"),
   Filament's `ShadowSample_EVSSM` becomes the direct, now-accurate port
   target at that point — this is a scope CORRECTION for Task 17, not a
   capability loss.

2. **Blocker-search/penumbra "correctness" gating across lavapipe vs. a
   real driver — how to gate honestly per this project's own standing
   corrective (plan:65-70, "lavapipe-only verification is NOT
   verification").** **Recommendation: split the acceptance criteria by
   what they actually test.** (a) The penumbra-width FORMULA itself
   (matrix row above, "Penumbra-width estimation formula") is a pure,
   device-free closed-form computation — gate it as a CPU unit test,
   runs identically everywhere, no driver label needed. (b) The plan's own
   named "measured penumbra widens... at ≥3 distances" criterion
   (plan:564-565) is a MONOTONICITY assertion (value at distance N+1 >
   value at distance N), which is robust to small cross-implementation
   floating-point/texture-filtering differences — safe to run on lavapipe
   AS A SMOKE TEST (catches gross logic errors, e.g. an inverted sign,
   cheaply and in CI), but the numeric VALUES themselves (and any
   published cost/quality claim) must additionally be measured on a real
   driver, labeled, per the standing rule — lavapipe's own software
   rasterizer is not guaranteed to reproduce a real GPU's exact texture-
   filtering/derivative behavior at the sub-texel precision PCSS's own
   penumbra math is sensitive to. Concretely: two test variants — one
   gated on EVERY CI run (any driver, monotonicity-only), one gated on
   the real-driver-required run (numeric values, published in the ledger)
   — never conflate the two into a single "PCSS works" claim backed only
   by lavapipe.

## Verification health

**Verified first-hand this session:** `surface_shadowing.fs` (full 552
lines) and `surface_shadowing.glsl` (full 60 lines) were fetched and read
in FULL from `google/filament` at commit
`721ec800093de984cbee155e459298b6b2dbb855` — every shadow-sampling
function's actual code (not a summary) was read, including the exact
`ShadowSample_EVSSM` blocker-search/penumbra-estimation/filtering steps
this matrix's CRITICAL FINDING and recommended-portable-math row are based
on. In-repo `material.slang`'s shadow-sampling section and
`shadow_frustum.h` were re-confirmed directly against the working tree
(same citations already verified for matrix-p5t16, re-checked here for
this ticket's own specific claims about them).

**Not independently re-verified:** the classic Fatahalian/Lauritzen PCSS
paper itself (NVIDIA GPU Gems, "Percentage-Closer Soft Shadows") was not
re-fetched this session — cited from general, well-established knowledge
as the charter's own named technique family; Filament's actual EVSSM code
(fetched and read in full) independently corroborates the SAME
similar-triangles penumbra-estimation relation, which is the specific
piece this matrix relies on being accurate.
