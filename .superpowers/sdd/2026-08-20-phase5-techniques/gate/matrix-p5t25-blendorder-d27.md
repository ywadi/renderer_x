# Matrix — P5 T25 (issue #61): Transparency ordering — blendOrder tier + D27 partition revisit

**Plan task:** Task 25 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:713-732`), Stage 3.
**Charter binding:** registry note (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:354-356`):
"Per-primitive blendOrder sort tier (techniques phase, with the
translucency work; Phase 4 documents reserved sort-key bits and delivers
determinism via the creation-index tie-break instead)." Phase-4
exit-review registry item (a) (`:510-513`): "D27 pre-resolution's
'resolve once per distinct key' currently holds only for the opaque
partition — blend-partition interleaving re-fires resolution per run
(cost, not correctness); revisit with the techniques phase's
transparency work."
**Spec decisions binding this ticket:** D14 (draw lists with sort keys),
D26.3 (instancing collapse), D27 (main-thread pipeline pre-resolution) —
`docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md:236-261, 441-490`.

**Sources consulted:**
- Ticket body: `gh issue view 61`.
- Plan Task 25 + Global Constraints (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:55-134, 713-732`).
- Charter registry note + exit-review item (cited above).
- Design doc D14/D26/D27 (`docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md:236-261, 441-490`),
  read first-hand.
- Delivered code, read first-hand at HEAD (`bf5b853`) by a parallel
  in-round research pass covering `src/rx_scene/draw_list.h`/`.cpp` in
  full: sort-key layout + reserved-bits comment (`draw_list.h:220-390`,
  quoted directly by this matrix's own reading of the same file, offset
  220-380), `DrawListBuilder::Impl::partitionAndSort()`
  (`draw_list.cpp:543-562`), `resolveDrawGroups()` (`draw_list.cpp:855-893`),
  `collapseAndSortOpaque()` (`draw_list.cpp:595-660`), `ViewLists`
  (`draw_list.h:148-166`), `MaterialDisposition`/`AlphaMode`
  (`src/rx_asset/include/rx_asset/mesh_asset.h:115-121`).
- `MaterialSystem::getPipeline()`'s own `PipelineKey`/pipeline cache
  (`src/rx_material/material_system.cpp:824, 1921-1944`), confirming the
  cost-vs-correctness framing.
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/phase4-exit-review.md:201-205`
  (item M3 — the original finding this ticket closes), read first-hand.
- `src/rx_scene/tests/draw_list_test.cpp` (existing test inventory,
  lines 442, 466, 481, 539, 589, 948, 1081, 1355, 1420).
- `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:520`
  (the "BtF" comment this ticket's research brief referenced, confirming
  it means Back-to-Front sort direction, not an instancing term).

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/code support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------------|-------------------------------|
| 1 | Reserved sort-key bits for per-primitive `blendOrder` | bgfx's own `SortKey`/`RenderPass::CommandKey` discipline (cited by the header comment itself, `draw_list.h:224-230`, as the precedent for "one uint64_t, several shapes"). | consume-now (populate the existing reservation) | **VERIFIED, exact bits.** BLEND sort-key shape (`draw_list.h:275-276`): `[priority:3][reservedBlendOrder:13][depthBucket:32][tieBreak:16]`, bits 63-61/60-48/47-16/15-0. `kBlendReservedBlendOrderBits=13` (`draw_list.h:336`), shift `kBlendReservedBlendOrderShift` (`draw_list.h:343`). Currently ALWAYS ZERO — the header comment cites its own origin: "RC5/#5 ruling: 'per-primitive blendOrder is NOT added... reserved bits documented in the key layout'" (`draw_list.h:271-273`), and `draw_list.cpp:114-116` explicitly hardcodes it to 0 with a comment naming this exact deferral. Decode round-trip already exists and is tested (`DecodedBlend::reservedBlendOrder`, `draw_list.h:395`; test at `draw_list_test.cpp:466`, asserting the field is currently always zero — this specific test's assertion INVERTS once this ticket lands, a deliberate, must-be-updated regression point). | Round-trip test extended: `encodeBlend()`→`decodeBlend()` with a NON-zero `blendOrder` value preserves it exactly (the existing test at `draw_list_test.cpp:466` currently asserts zero — this ticket must update that assertion's premise, not just add a new test, per the "no deferred fixes" standing directive covering test debt too). |
| 2 | `blendOrder` overrides depth WITHIN its tier (not globally) | Ticket's own acceptance sketch: "blendOrder overrides depth within its documented tier (order test vs glTF/Filament semantics)." glTF's own `KHR_materials_blend` conventions and Filament's `RenderableManager::setPriority`/`Renderable::Builder::blendOrder` place blendOrder ABOVE depth as a sort tier but BELOW any coarser priority/pass tier — i.e. it's a secondary sort key, not a replacement for depth sorting (general industry shape; not independently re-verified against Filament's C++ scene-graph source in this pass, which is out of this ticket's shader/graph-code scope — see Verification health). | consume-now | The EXISTING bit layout already encodes exactly this hierarchy structurally: `priority` (3 bits, coarsest) sits ABOVE `reservedBlendOrder` (13 bits), which sits ABOVE `depthBucket` (32 bits) — i.e. the bit POSITIONS already express "blendOrder overrides depth, but priority overrides blendOrder," matching the ticket's own "within its documented tier" phrasing exactly. This is a favorable finding: the ticket's acceptance criterion is a NATURAL consequence of bits already correctly ordered in Phase 4, not new key-shape design. | Order test: two overlapping transparent draws at the SAME depth-bucket-adjacent range but DIFFERENT `blendOrder` values sort by `blendOrder` (not tie-break/creation-index); two draws with the SAME `blendOrder` but different depth sort by depth (proves blendOrder doesn't globally override depth, only breaks depth ties within its own bit position — exactly the "within its tier" semantics). |
| 3 | Determinism across `--threads` preserved | Ticket's own acceptance sketch, referencing the ALREADY-EXISTING determinism guarantee. | consume-now (regression preserved, not new) | VERIFIED existing test: `draw_list_test.cpp:1081`, "build() produces byte-identical ViewLists across --threads 1/2/8" — this test ALREADY covers the BLEND partition's sort (the sort key, including the now-populated `reservedBlendOrder` bits, is a pure function of per-primitive data, not thread-scheduling-dependent) — `std::sort` on a fully-determined 64-bit key (per the tie-break design's own stated goal, `draw_list.h:307-313`) is thread-count-invariant by construction. | Re-run the existing `--threads 1/2/8` determinism test with `blendOrder`-varying fixture data added — the test's EXISTING shape already proves the property; this ticket's job is to confirm it doesn't regress when the previously-always-zero field starts carrying real, per-primitive-varying values. |
| 4 | D27 resolve-once-per-distinct-key across BOTH partitions | Phase-4 exit-review item (a) + item M3, quoted exactly: *"`resolveDrawGroups()` (`draw_list.cpp:846-884` at review time, now `:855-893`) groups by materialIndex **adjacency**; the blend partition is depth-sorted, so materials interleave and `resolvePipeline` re-fires per run (cache hits, main thread — cost only, not correctness)."* (`.superpowers/sdd/2026-08-11-phase4-scene-assets/phase4-exit-review.md:201-205`, read first-hand). | consume-now | **VERIFIED, exact mechanism.** `resolveDrawGroups()` (`draw_list.cpp:855-893`) does a SINGLE linear scan over the CONCATENATED `lists.commands` (opaque then blend together), grouping by `materialOf(i) != currentMaterial` ADJACENCY (`draw_list.cpp:865-891`) — a new group (and a fresh `resolvePipeline()` call) fires only when the material CHANGES from the immediately-previous command. Opaque's own sort key places `materialKey` ABOVE `depthBucket` (bits 53-39 above 38-16, `draw_list.h:245-246`), so same-material commands stay contiguous (modulo `priority` fragmentation — see New gaps). BLEND's key (`draw_list.h:275-276`) has NO material tier at all — `[priority][reservedBlendOrder][depthBucket][tieBreak]` — so materials interleave freely by depth order; the SAME `materialIndex` produces MULTIPLE non-adjacent runs across a depth-sorted blend list, each re-invoking `resolvePipeline()`. `MaterialSystem::getPipeline()`'s own `unordered_map<PipelineKey, VkPipeline>` cache (`material_system.cpp:824, 1939-1944`) makes each re-invocation a CACHE HIT, confirming "cost, not correctness" precisely as both the registry note and the exit-review item frame it. | **Exact testable invariant (the ticket's own acceptance sketch, made concrete):** key = `PipelineRequestKey{materialIndex, passSignatureHash, specializationBits}` (`draw_list.h:535-541`); instrument `resolveDrawGroups()`'s `PipelineResolveFn` call count per distinct key on an INTERLEAVED-material blend scene (e.g. 3 materials alternating by depth across 30 draws); assert invocation count == 1 per distinct key. **Discrimination proof: this test FAILS on pre-task code** (current adjacency-only grouping re-invokes per run — with 3 interleaved materials across 30 draws, a naive alternating pattern could re-fire up to 30 times instead of 3), exactly the "test fails on pre-task code" discrimination the plan's own acceptance sketch requires. |
| 5 | Fix mechanism: replace adjacency-grouping with a memoized key→token map | N/A — internal implementation-shape row; the fix is fully scoped within `draw_list.cpp`. | consume-now | VERIFIED: `getPipeline()`'s own cache is ALREADY correct and cheap (row 4) — the fix does NOT require any `rx_material` change. It is confined to `resolveDrawGroups()` (`draw_list.cpp:855-893`): replace pure adjacency detection with an `unordered_map<PipelineRequestKey, ResolvedGroupToken>` scan-local cache, so a materialIndex/pass/specialization key resolves ONCE regardless of how many times its run fragments across BOTH partitions (opaque's own adjacency already avoids most redundancy per row 4's finding, but a small map costs little and closes the gap uniformly rather than leaving opaque's "mostly fine" and blend's "reliably bad" as two different code paths). | Cost measurement per the ticket's own acceptance sketch ("cost, not correctness — measured"): Tracy-measured `resolveDrawGroups()` wall time on a blend-heavy interleaved scene, before/after, published — this is a genuine perf-regression-guard candidate, not just a correctness test, per the plan's own binding performance-exit-criterion policy. |
| 6 | Instancing-collapse interaction — blend draws must stay uncollapsed | Ticket's own file list implies sort-key-layout-only changes; the "BtF uncollapsed" framing this ticket's research brief used. | N/A-Phase-5 (already correctly the case — confirm, don't change) | **VERIFIED "BtF" = Back-to-Front** (the sort DIRECTION, per `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:520`'s own comment: `/* opaque FtB ... then instancing-collapsed; blend BtF, uncollapsed */`), NOT an instancing-collapse mechanism name. `collapseAndSortOpaque()` (`draw_list.cpp:595-660`, `sameDrawIdentity()` at `:337-349`) applies ONLY to the opaque partition; blend is explicitly `// Blend is never collapsed [D14]` (`draw_list.cpp:669`, `draw_list.h:152-153,269-270`) — matching correct transparency-ordering discipline (world-position-dependent draws must not merge). Adding `blendOrder` does not change this; a `blendOrder`-bearing draw is still a distinct per-instance record. | Regression test: after `blendOrder` lands, a scene with two identical-mesh/identical-material transparent instances at different `blendOrder` values still produces TWO separate draw records (not collapsed into one instanced draw) — proves the D14 "never collapsed" invariant survives the new field, an easy accidental regression if a future implementer's collapse-adjacency key forgets to account for `blendOrder` as a distinguishing field. |
| 7 | Sort-key layout doc + decode() round-trip updated | Ticket's own acceptance sketch: "Sort-key layout doc + decode() round-trip updated for the new bits." | consume-now | The doc IS the header comment (`draw_list.h:220-390`) — already exhaustively precise (verified by direct reading, quoted extensively above); "updating" it means changing the PROSE describing `reservedBlendOrder` from "always zero, reserved" to "populated by per-primitive blendOrder," not restructuring the bit layout itself (the 13-bit field/shift constants are already correctly sized and positioned). | Doc-diff review criterion (not a runtime test): the updated comment block accurately describes the NEW behavior with the same rigor as the current one (the existing comment is unusually thorough — bit diagrams, named precedents, cross-references to D14/D26.3/RC5 — the replacement text should match that bar, not regress to a one-line note). |
| 8 | Per-primitive `blendOrder` DATA field — does it exist anywhere yet? | Ticket's own text: "per-primitive `blendOrder` populates the sort-key bits." | **Genuinely new — not yet built anywhere** | **VERIFIED ZERO HITS.** `grep -rn "blendOrder" src/` returns only a test-name STRING LITERAL (`draw_list_test.cpp:467`) and the header COMMENT (`draw_list.h:271`) — no actual data field exists in `draw_list.h` structs, the glTF importer, or `ImportedScene`/`MaterialAsset`. Unlike T21/T23/T24's extension gaps (where fastgltf already parses the data and only RendererX-side plumbing is missing), `blendOrder` has NO glTF-spec equivalent to parse at all — glTF core has no `blendOrder`/`renderOrder` concept; this would be either (a) a RendererX-specific authoring convention (e.g. a mesh/primitive extras field or a scene-authoring API), or (b) sourced from a specific glTF vendor extension if one exists for this purpose (not checked in this pass — out of scope, see Verification health). | This ticket's own scope must define WHERE `blendOrder` values come from before they can populate the sort key — a data-source decision the ticket's current text does not name. Recommendation: expose it as a `Scene`/renderable-authoring API parameter (matching Filament's `RenderableManager::Builder::blendOrder()` precedent, general-knowledge citation only) rather than inventing a glTF-import path with no spec backing — this keeps the feature usable by hand-authored/procedural scenes (RendererX's non-glTF consumers) as well as imported ones. |
| 9 | Existing test inventory this ticket extends | N/A — internal coverage-audit row. | consume-now | VERIFIED test names/lines: round-trip — `draw_list_test.cpp:442` (opaque), `:466` (blend, MUST be updated per row 1), `:481` (shadow); partition direction — `:539` ("Opaque partition sorts front-to-back... blend partition sorts back-to-front"); MASK-in-opaque — `:589`; shadow exclusion — `:948`; determinism — `:1081`; worker-thread proofs — `:1355`, `:1420`. | N/A | This ticket's new tests should sit alongside this existing suite, reusing its fixture/harness conventions (confirmed to already exist and be well-structured) rather than building a parallel test harness. |

---

## Conflicts

None found that contradict the plan/charter/ticket text. The plan's
phrasing "D27 pre-resolution currently re-fires per blend-partition run"
is CONFIRMED accurate and precisely mechanistic (row 4) — this is a
well-specified ticket relative to its Stage-3 siblings; the main
open item is row 8 (data-source decision), which the ticket's own text
does not address and which the coordinator should resolve before
dispatch since it changes the ticket's actual scope (importer/API work,
not just `draw_list.cpp` sort-key work).

## New gaps

- **`blendOrder`'s data source is undefined** (row 8) — not registered
  anywhere in the master registry or this plan's text beyond "per-
  primitive blendOrder populates the sort-key bits," which describes the
  MECHANISM (bits) without the SOURCE (where the per-primitive value
  comes from). Proposed fit: this ticket itself, since it's the ticket
  chartered to consume the reserved bits — recommendation given in row 8.
- **Opaque partition's own adjacency-grouping fragility under `priority`
  tiers** (noted in row 4's parenthetical, not separately verified in
  depth this pass): the opaque sort key places `priority` (3 bits) ABOVE
  `pipelineKey`/`materialKey` (`draw_list.h:245-246`), so a scene using
  multiple `priority` values could in principle fragment same-material
  runs across non-adjacent priority bands the SAME way blend fragments
  across depth — meaning row 4's fix (a memoized map, not pure adjacency)
  closes this latent opaque-side gap too, as a side effect, without
  needing separate registration. Flagged for the coordinator's awareness
  that the fix's benefit is not blend-only, in case the ticket's scope
  gets narrowed to "blend partition only" during implementation planning.
- **Interaction with T23's open transmission-partition question** (see
  the T23 matrix, row 8): if a THIRD `ViewLists` partition is added for
  transmission draws, this ticket's D27 fix (a scan-wide memoized map
  over `lists.commands`) should be designed to cover however many
  partitions exist at the time it lands, not hardcoded to "two." Cross-
  referenced, not re-registered as a separate gap — the coordinator
  should sequence T23's partition decision and T25's fix consistently
  (both matrices flag this from their own side).

## Verification health

- **Verified first-hand, exhaustively:** every `draw_list.h`/`.cpp`
  citation in this matrix was read directly from the working tree at
  HEAD (both by this matrix's own direct reading of `draw_list.h:220-390`
  and by a parallel same-round research pass covering `draw_list.cpp`'s
  function bodies in full) — line numbers cited are current, not
  inherited from the Phase-4 exit-review document (which itself is
  slightly stale, `:846-884` vs. current `:855-893`, a ~9-line drift
  explicitly noted and reconciled in row 4).
- **Verified first-hand:** the exit-review's exact original finding text
  (item M3) was read directly from
  `.superpowers/sdd/2026-08-11-phase4-scene-assets/phase4-exit-review.md:201-205`,
  not paraphrased from the plan's own summary of it.
- **Inferred / lower-confidence, flagged explicitly:** row 2's
  "blendOrder sits below priority, above depth" INDUSTRY-CONVENTION
  claim (Filament's `blendOrder` API, glTF ecosystem norms) is general
  knowledge, NOT independently re-verified against Filament's own C++
  scene-graph source in this pass — the STRUCTURAL argument (the existing
  bit-position hierarchy already matches this shape) is verified
  first-hand from RendererX's own code and stands on its own regardless
  of the external precedent's exact re-verification status.
- Row 8's "is there a glTF vendor extension for blendOrder" question was
  explicitly NOT researched in this pass (out of the assigned scope of
  reconciling THIS ticket's own stated mechanism-vs-source gap) — flagged
  as an open sub-question for whoever resolves row 8, not asserted either
  way.
- No dead links encountered.
