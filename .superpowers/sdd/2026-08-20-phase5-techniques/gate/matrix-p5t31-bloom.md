# Completeness matrix — P5 T31 (issue #67): Bloom

**Plan task:** Task 31, Stage 4 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:834-848`).
**Charter binding:** frame-pipeline slot *"...glass/transmission →
particles/transparency → bloom → tone mapping..."*
(`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:447-451`);
no separate charter priority-list entry (bloom is not one of the 12 named
priorities — it is pipeline plumbing feeding tonemap, consistent with the
plan's own "feeding the tonemapper" framing).

**Sources consulted (in-repo, 2026-08-20):**
- Plan Task 22 text (`plan.md:653-671`, Stage 3, not yet landed) — "the
  opaque scene color renders into an HDR mip chain... Stage 4's bloom
  reuses the same chain" is the PLAN's own claim of a reuse relationship;
  verified nothing exists yet to reuse (`shaders/post/` does not exist,
  confirmed via `ls shaders/` — only `material/`, `multipass/`, `shadow/`,
  `stress/`, `tests/`, and two top-level triangle shaders are present
  today).
- `src/rx_core/include/rx_core/profile.h` — Tracy, confirmed real (see T29
  matrix).

**Sources consulted (external, fetched 2026-08-20, `google/filament`,
pinned tag `v1.75.0`):**
- `filament/src/materials/bloom/bloom.cpp` (41 lines, full file) — Apache-2.0
  (Copyright 2025 The Android Open Source Project), material registration
  only.
- `filament/src/materials/bloom/{bloomDownsample.mat,bloomDownsample2x.mat,
  bloomDownsample9.mat,bloomUpsample.mat}` — the actual downsample/upsample
  kernels. `bloomDownsample9.mat` fetched in full (65 lines): a 6×6-tap
  downsample implemented via 9 bilinear samples, weights `o=1.5+0.261629`,
  `wa=7.46602/32`, `wb=1-2·wa`, citing `https://www.shadertoy.com/view/cslczj`
  in its own source comment — the same technique Jorge Jiménez presented in
  "Next Generation Post Processing in Call of Duty: Advanced Warfare"
  (SIGGRAPH 2014), which the plan's own text names ("COD-style").
- GitHub code search for `karisAverage`/`quenching` across
  `google/filament`: zero matches — Filament's shipped downsample kernel
  does NOT implement an explicit Karis-average firefly clamp; its only
  firefly damping is the incidental smoothing of the weighted 9-tap
  bilinear average itself.
- `filament/src/PostProcessManager.cpp` — orchestrates the bloom chain
  (thresholding, mip-chain iteration count, dirt-texture compositing) as
  C++ pass-graph glue around the `.mat` kernels above; not fetched in full
  for this pass (the `.mat` kernels are the load-bearing port artifacts;
  the orchestration is straightforward N-level ping-pong the render graph
  already expresses via ordinary transient/attachment declarations, no new
  primitive needed).

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/port-source support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------|-------------------------------|
| 1 | Downsample kernel (6×6-tap-via-9-bilinear-sample, weighted) | Filament `bloomDownsample9.mat` (Apache-2.0, `v1.75.0`), itself citing the public Shadertoy/Jiménez 2014 technique the plan already names. | consume-now — primary port source | Verified present, full kernel read (weights + sample offsets cited above). | Port-parity test: known input pattern → filtered VALUES at each mip match the ported kernel's own weights (not just "looks blurred") — same discipline as the plan's own Task 22 acceptance sketch for its mip chain. |
| 2 | Upsample kernel (tent filter, additive composite into the coarser mip) | Filament `bloomUpsample.mat` (Apache-2.0, `v1.75.0`). | consume-now | Present in the same directory; not fetched in full this pass (lower risk than the downsample kernel — a standard 3×3 or 9-tap tent filter is a well-known, easily-verified shape) — **implementer should fetch and cite it directly at port time**, same discipline as row 1. | Same VALUE-asserted discipline as row 1, plus an energy-conservation check across the up-chain (row 4). |
| 3 | Reuse vs. mirror of Task 22's HDR mip chain (spec ruling named directly in the plan's own ticket text: "reuses or mirrors the Task 22 chain per spec ruling") | N/A — internal architecture question the plan explicitly defers to the Task 1 spec. | Task-1-spec decision point (Open Question below) | N/A. | Whichever the spec rules, the acceptance criterion must state which: REUSE means bloom's downsample chain and Task 23/24's transmission-roughness mip selection read the SAME physical mip chain (single generation cost, shared invalidation); MIRROR means bloom owns its own independently-sized/thresholded chain (Filament itself does the latter — its bloom chain is a SEPARATE resource from any transmission-roughness mip selection, since Filament's own transmission/refraction path reads `ssr_history`/`structure` buffers, not the bloom chain — see Conflicts). |
| 4 | Energy bound: bloom never adds net energy beyond its documented weight | N/A — Filament's own `bloom.strength`/dirt-texture-intensity parameters are user-authored, not auto-normalized; the ticket's "energy bound" criterion is stricter than what Filament itself guarantees (Filament allows arbitrary strength/threshold user tuning that CAN add unbounded energy). | consume-now (ticket's own, stricter contract) | N/A — this is RendererX's own invariant, not inherited from the port source. | An integrated-value probe (ticket's own text) measures total scene radiance before/after the bloom composite for a fixed input, asserting the delta stays within the pass's own documented weight parameter — this is a NEW test this codebase must author; Filament provides no equivalent guarantee to lean on. |
| 5 | Firefly behavior (single hot pixel → bounded, stable spread) | Filament's downsample kernel provides only INCIDENTAL firefly damping via its weighted bilinear average (verified: no explicit Karis-average/clamp function found in the pinned source — see Sources). | consume-now, WITH A NAMED GAP | Verified absent as an explicit mechanism in the port source. | The ticket's own "measured, bounded, stable spread" criterion should be read as requiring RendererX to verify the INHERITED (not separately engineered) damping from the 9-tap weighted average is sufficient — if the single-hot-pixel test shows unbounded/unstable spread, an explicit Karis-average clamp (well-documented industry technique, Karis 2014 "Physically Based Shading in Call of Duty: Black Ops") is the fallback, added on top of the ported kernel rather than assumed present in it. |
| 6 | HDR input value test through the full chain | N/A — test discipline, not a precedent row. | consume-now | N/A. | Ticket's own text stands: a >1.0 radiance input survives distinguishably through the full bloom chain into the tonemap input. |
| 7 | Cost measured at 1080p/1440p | N/A — tooling precedent already delivered. | consume-now | Tracy confirmed real. | Ticket's own text stands. |

---

## Conflicts

**Row 3's "reuse or mirror" framing understates a real architectural
tension.** Filament's OWN bloom chain is a dedicated resource, separate
from anything a transmission pass would read for roughness-selected
refraction blur — i.e., Filament's actual precedent is closer to MIRROR
(a bloom-owned chain) than REUSE (a chain shared with Task 22's
transmission consumer). This does not block T31 (the plan already frames
it as an open spec question, not a settled fact), but the coordinator's
ruling should be made with this fact in hand rather than assuming
Filament's own architecture endorses REUSE.

## New gaps

None.

## Open Questions (for the coordinator's binding ruling)

1. **Reuse vs. mirror the Task 22 HDR mip chain (row 3).**
   **Recommendation: REUSE**, not mirror — despite Filament's own
   precedent leaning MIRROR (row 3/Conflicts), RendererX's own stated
   design goal is different from Filament's: CLAUDE.md's performance
   posture ("pooled global geometry buffers, bindless access, minimal
   derived barriers... per-object state churn and retrofit-later designs
   are rejected") favors a single generated HDR mip chain serving BOTH
   Task 23/24's transmission-roughness lookup and Task 31's bloom
   downsample source, halving mip-generation cost and avoiding two
   independently-thresholded scene-color derivatives drifting apart
   visually. The one real cost is a shared invalidation/lifetime contract
   between two Stage-3/Stage-4 consumers — acceptable, and exactly the
   kind of seam Task 1's spec exists to fix in one place rather than let
   each task assume its own answer.
