# Task 19 review: `DrawListBuilder` — parallel culling + sort keys (closes #6, #7)

Reviewer: independent (did not implement). Commits under review: `afd8a0d`
(implementation) + `e0fe8c4` (report), base `a178969`. Authority order used:
`gate/rulings-2026-08-18.md` §#6/§#7 + RC3/RC5 > spec D14/D15/D24/D26/D27 >
`gate/matrix-issue06-drawlists-culling.md` + `gate/matrix-issue07-layer-masks.md`
> tickets #6/#7.

## Verdict 1 — Spec compliance: PASS

Every row of both gate matrices, as amended by the rulings, has a
corresponding implementation and a directly-mapped, non-vacuous test.
No matrix row is silently unaddressed except one minor documentation gap
(D26.1's literal "count `vkCmdPushConstants` calls" GPU-only criterion —
see findings). RC3 (BLEND excluded from `ShadowLists`, MASK casts full
silhouette) and RC5 (coupled channels, `castsShadows` opt-out, all-ones
defaults) are both correctly implemented and tested. D24 residency
tolerance (mesh + material) is real and tested both ways.

## Verdict 2 — Code quality: Approved, with 4 low/medium findings (no blockers)

Well-documented, idiomatic, consistent with the codebase's established
conventions (span accessors, `RX_ASSERT_MAIN_THREAD`, `RX_ZONE`/`RX_ZONE_NAMED`,
pImpl, caller-owned reused storage). No dirty-tracking creep. No raw-pointer
escape. Sort-key bit layout is genuinely bgfx/Filament-disciplined (named
constants, round-trip decode, direction/priority/reserved-bits tests).

## Findings

1. **[LOW] Report's own test-count arithmetic is wrong.** The report claims
   "61 test cases (57 unconditional + 4 gated behind `RX_DEBUG_CHECKS`)".
   Actual: `grep -c '^TEST_CASE' src/rx_scene/tests/*.cpp` totals exactly
   **57** across the whole suite (draw_list_test.cpp 31, scene_test.cpp 16,
   camera_test.cpp 8, thread_guard_test.cpp 2), of which 4 (2 in
   draw_list_test.cpp:1291/1355, 2 pre-existing in thread_guard_test.cpp)
   are behind `#ifdef RX_DEBUG_CHECKS`. With `RX_DEBUG_CHECKS=ON` (both dev
   presets) all 57 compile and run — my own from-scratch build/run confirms
   doctest reports `test cases: 57 | 57 passed`, matching the assertion
   count (6199) and 0-failures outcome the report claims. The 61 figure
   double-counts the 4 gated cases. Cosmetic only — the underlying
   pass/fail evidence is accurate and independently reproduced.
2. **[LOW] Adversarial cross-group depth-ordering case is undocumented by
   test.** Constructed mentally per the review brief: two distinct
   collapse-identity groups (different meshes, same pipeline/material),
   each with multiple instances whose individual depths *interleave*
   (e.g. group A at depths {0.95 near, 0.10 far}, group B at {0.90, 0.05}).
   `collapseAndSortOpaque()`'s "representative depth = nearest member"
   design (draw_list.cpp:619-626) places group A's whole batch (0.95, then
   0.10) before group B's whole batch (0.90, 0.05) — so far-member 0.10 is
   submitted before near-member 0.90, i.e. per-*instance* front-to-back is
   not preserved across the group boundary. This is **not a correctness
   bug**: opaque z-testing is submission-order-independent, D14's own text
   frames front-to-back as an early-Z *optimization* not a hard ordering
   guarantee, and the design is the only one that can simultaneously
   satisfy D26.3's "1000 scattered identical instances → 1 draw" collapse
   requirement (the shared fixture direction test at draw_list_test.cpp:539
   and the D26.3 tests only exercise single-instance groups or
   same-depth-instance groups, never this specific overlap). A dedicated
   test would strengthen the documentation of this intentional,
   already-commented tradeoff; no matrix row requires it, so this doesn't
   block.
3. **[MEDIUM] `PipelineResolveFn`'s `uint32_t` return type cannot carry a
   real `VkPipeline` handle.** `VkPipeline` is a non-dispatchable Vulkan
   handle — `VK_DEFINE_NON_DISPATCHABLE_HANDLE` resolves to a pointer type
   (8 bytes) on this project's target platforms (verified against the
   vendored `vulkan_core.h`), but `PipelineResolveFn = std::function<uint32_t(uint32_t)>`
   (draw_list.h:519) is fixed at 32 bits. `MaterialSystem::getPipeline()`
   (material_system.h:381) also takes a `PipelineRequest` struct (material +
   pass-signature + specialization), not a bare `materialIndex` — the
   report's own design note acknowledges this is fine only because
   pass-signature/specialization are constant per `build()` call, which is
   a reasonable but unstated assumption. Task 22/24's real binding will
   need either an index-indirection table (materialIndex/token →
   `VkPipeline`) or a widened seam — neither the header nor the report's
   "known limitations" section flags this specific width mismatch. Not a
   defect in Task 19's own delivered scope (device-free, no `VkPipeline`
   exists here), but exactly the kind of signature drift the review brief
   asked to watch for, and worth a one-line note for whoever picks up
   Task 22/24 so it isn't a surprise.
4. **[LOW] D26.1's literal GPU-observable acceptance criterion has no
   corresponding row in the report's per-criterion proof table.**
   matrix-issue06's "D26.1 — per-draw addressing... zero per-draw push
   constants" row asks for "a GPU test... counting `vkCmdPushConstants`
   calls... O(1) per pass, not O(draws)" — literally unsatisfiable in a
   device-free binary with no `VkCommandBuffer`. The underlying data-layout
   requirement (per-draw payload addressed via `firstInstance`, no
   per-draw push-constant path in this library) is genuinely delivered and
   implicitly covered by the D26.2/D26.3 rows, but the report's table
   silently omits this specific row rather than marking it
   deferred/N-A-with-reasoning the way it explicitly did for the
   `getLayers`/`getChannels` row in the layer-masks table. Documentation
   completeness gap only.

## Adjudications (as requested)

1. **`Scene::aliveSpan()`/`generationsSpan()` — legitimate extension, not
   scope creep.** Verified directly in the diff (scene.h/scene.cpp):
   read-only, additive, `RX_ASSERT_MAIN_THREAD`-guarded, mirrors
   `castsShadowsSpan()`'s existing `uint8_t`-not-`vector<bool>` convention
   exactly. `destroyRenderable()` genuinely does not re-zero other columns
   (confirmed: only `submeshOverrides_` is cleared, scene.cpp), so a bulk
   SoA consumer has no other way to skip dead slots by bare index. This is
   the same class of small, cheap, necessary gap-fill the #5/#18 gate
   ruling already blessed for `getLayers`/`setChannels`. No rework needed.
2. **`recordDrawList`/`resolveDrawGroups` genericized via injected
   callbacks — justified in principle, real integration risk
   unacknowledged.** The plan text (plan:713, quoted accurately in the
   report) does put the real GPU binding at Task 24 ("Registry→Scene→
   DrawListBuilder→graph"), and Task 19's own file list is `draw_list.{h,cpp}`
   only (confirmed: no rx_graph/rx_material dependency in CMakeLists.txt),
   so deferring the bind is architecturally sound and plan-supported. BUT:
   the plan's own interface sketch (plan:526) shows `recordDrawList(PassContext&,
   chunkIndex, chunkCount, const ViewLists&, ...)` — a per-chunk-invoked,
   externally-driven shape — while the delivered `recordDrawList()` instead
   owns its own internal `scheduler.parallelFor()` fan-out and never
   receives a `PassContext&` at all; a production `RecordChunkFn` will have
   to capture whatever per-chunk GPU resource it needs from its own closure,
   and Task 22/24 will need to reconcile this with rx_graph's existing
   `recordChunkedPass` chunking model rather than plug in directly. Combined
   with finding 3 above (pipeline-token width), this is a real, if
   non-blocking, execution-model and signature gap between the seam and the
   real APIs it stands in for — flagged for Task 22/24 planning, not a
   rework trigger for Task 19.
3. **No wall-clock benchmark published — correct, matrices don't demand
   one.** Verified directly: neither matrix-issue06 nor matrix-issue07
   contains a wall-clock/benchmark acceptance criterion for Task 19 (every
   row is a unit-test-shaped criterion). CLAUDE.md's "every phase exits
   with published benchmark numbers" binds phase exits and stress/exit
   samples (Task 23's executor zero-alloc work, Task 24's sample 09
   stress-v2 A/B numbers) — not this device-free library task. D27's own
   "shown to scale linearly... in the published stage-exit numbers" phrase
   is satisfiable at that same later checkpoint. The implementer's
   position is correct.

## Empirical verification performed

- **Build**: `cmake --preset linux-native` + `cmake --build ... --target
  rx_scene_tests` from a forced-clean object rebuild of `draw_list.cpp`
  and `draw_list_test.cpp` — zero warnings, zero errors.
- **Test run**: `./rx_scene_tests` — `test cases: 57 | 57 passed`,
  `assertions: 6199 | 6199 passed`, 0 failures — matches the report's
  claimed assertion count and pass/fail outcome exactly (see finding 1 for
  the test-*count* label discrepancy).
- **Revert-and-restore, 2 of the 4 claimed discriminations, both
  reproduced exactly as claimed:**
  - *Zero-alloc counter*: changed `out.commands.clear()` →
    `out.commands = std::vector<DrawCommand>()` in `build()`. Result:
    baseline delta jumped from 1 to **10** operator-new calls/frame;
    `CHECK(baselineDelta <= 4)` failed as claimed (`10 <= 4` false).
  - *D26.3 lockstep*: dropped the `firstIndex` field from
    `sameDrawIdentity()`. Result: `lists.commands.size() == 2` failed
    (`1 == 2`, i.e. two genuinely distinct meshes wrongly merged into one
    collapsed command) — reproduced exactly as claimed, and caught before
    the lockstep payload-range cross-check even runs, matching the
    report's own account.
  - Both reverts restored via `cp` from a pre-edit backup; `md5sum`
    confirmed byte-identical restore each time
    (`6a1efa3f7a657931e429010c04dbbe2e`); full suite re-run green after
    each restore (57/57, 6199/6199).
  - D27 worker-guard and cross-thread-determinism discriminations were
    **not** independently re-verified this session (2-of-4 selected per
    task instructions); their test code was read and judged
    non-vacuous (genuine rendezvous-barrier construction, deliberate
    adversarial control test proving the guard itself fires) but not
    empirically re-broken.
- **Vacuousness sweep**: read every `TEST_CASE` body in
  `draw_list_test.cpp` (all 31). All assert specific, falsifiable
  outcomes (exact counts, exact indices, cross-checked identity sets,
  independent camera-only confirmation for the off-screen-caster test to
  rule out a trivially-true assertion, adversarial fixtures for
  block-contiguity and lockstep). No `CHECK(true)`-style or
  tautological assertions found.
- **Commit hygiene**: author `Yousef Wadi <ywadi85@gmail.com>` on both
  commits, matching git config; no AI attribution/co-author lines in
  either commit body; `git status --porcelain` shows only the
  pre-existing, untouched `progress.md` modification (left alone per
  instructions); `git log --oneline` confirms local `main` is 2 commits
  ahead of `origin/main` with nothing pushed; diffstat confirms the
  changed-file set is exactly `draw_list.{h,cpp}` + its test +
  `CMakeLists.txt` (2 files) + the documented `scene.h`/`scene.cpp`
  extension — no stray files.

## Not independently re-verified (disclosed per instructions)

- The report's windows-cross-zig/Wine test run (11 tests, 100% pass) —
  not re-run this session; only linux-native was rebuilt/re-run.
- The D27 worker-guard and cross-thread-determinism revert-discrimination
  claims (2 of the 4) — read and judged plausible/non-vacuous, not
  empirically re-broken-and-restored this session.
- GeometryPool's actual `blockId` stability guarantee (Stage 1, outside
  this task's and this review's scope — matrix-issue06 itself flags this
  as unverified).

---

## Fix-round-1 re-review (scoped): ALL 4 FINDINGS ADDRESSED

Commits: `969fe55` (code) + `cd5e4c5` (report delta), diff package
`review-e0fe8c4..cd5e4c5.diff`. Scope: verify closure of the 4 round-1
findings only.

1. **[Medium, seam width] ADDRESSED.** `PipelineRequestKey{uint32_t
   materialIndex; uint64_t passSignatureHash; uint32_t specializationBits;}`
   verified field-for-field against the REAL `rx::material::PipelineRequest`
   (`src/rx_material/include/rx_material/material_system.h:56-65`:
   `MaterialHandle material; PassSignature pass; uint32_t
   specializationBits = 0;`) — exact three-input correspondence, and
   `passSignatureHash`'s `uint64_t` type verified against the real
   `PassSignature::hash()` (`src/rx_graph/include/rx_graph/pass_signature.h:80`:
   `[[nodiscard]] uint64_t hash() const`) — exact match, not approximate.
   `VkPipeline` width verified directly against the vendored
   `vulkan_core.h`: `VK_DEFINE_NON_DISPATCHABLE_HANDLE` is a pointer
   typedef when `VK_USE_64_BIT_PTR_DEFINES==1` (true for every platform
   this project targets — all are 64-bit), i.e. 8 bytes; the widened
   `uint64_t` token carries it without truncation, unlike the old
   `uint32_t`. `resolveDrawGroups()`/`recordDrawList()` now take
   caller-supplied `passSignatureHash`/`specializationBits`, folded into
   `PipelineRequestKey` per-call; the design rationale (a built `ViewLists`
   is pass-agnostic, so these are caller- not `ViewLists`-owned) is sound.
   All 3 D27 tests updated to the new signature; the linear-scan test is
   genuinely strengthened (asserts the two new fields thread through
   `resolveCalls` unchanged, not just that it still compiles). No
   dependency on `rx_material`/`rx_graph` added — still device-free,
   confirmed via unchanged `CMakeLists.txt` (not touched this round).
2. **[Low, cross-group depth test] ADDRESSED, genuinely discriminating.**
   Independently recomputed the fixture's NDC math (`near=0.1`,
   `ndc = near/distance`): group A (distances 0.5, 50) → depths
   {0.2, 0.002}, representative (nearest-member/max) = 0.2; group B
   (distances 1, 80) → depths {0.1, 0.00125}, representative = 0.1. A's
   representative (0.2) beats B's (0.1), so A's whole 2-instance command
   sorts entirely first — matches the test's asserted output exactly.
   Confirmed this discriminates against the specific alternative the
   coordinator named ("a naive per-command depth sort"): a sort-first/
   collapse-adjacent-only design would sort the 4 raw instances by pure
   depth into [A-near(0.2), B-near(0.1), A-far(0.002), B-far(0.00125)] —
   A's two instances are no longer adjacent, so nothing collapses,
   producing **4** commands in a fully interleaved order, not 2. The
   test's own `REQUIRE(lists.commands.size() == 2)` alone already fails
   under that alternative, before the `firstIndex`/`instanceCount`
   assertions even run — a real, non-vacuous discrimination. The
   companion code comment at `collapseAndSortOpaque()` (draw_list.cpp)
   accurately states the tradeoff (early-Z rejection-rate optimization,
   not an ordering guarantee; opaque correctness comes from the
   per-fragment depth test) — matches finding 2's own framing.
3. **[Low, report count] ADDRESSED.** `task-19-report.md` now states 58
   total `TEST_CASE`s (4 gated, included not additional) in every place
   the old "61 (57+4)" figure appeared. Independently re-counted:
   `grep -c '^TEST_CASE' src/rx_scene/tests/*.cpp` → draw_list_test.cpp
   32, scene_test.cpp 16, camera_test.cpp 8, thread_guard_test.cpp 2 = 58,
   exactly matching the report's corrected figure (the +1 over the prior
   round's 57 is the new finding-2 test).
4. **[Low, D26.1 N/A row] ADDRESSED.** The matrix-issue06 per-criterion
   proof table in `task-19-report.md` now carries two new rows immediately
   above the D26.2 row: a PASS row for the data-layout requirement
   (`firstInstance`-only addressing, no push-constant-shaped `DrawPayload`
   field) and an explicit N/A row for the literal
   `vkCmdPushConstants`-count criterion, with rationale (device-free,
   unsatisfiable without a `VkCommandBuffer`) and a forward pointer to
   Task 22/24 — same pattern as the layer-masks table's own
   `getLayers`/`getChannels` N/A row, as claimed.

**Empirical re-verification:** forced-clean rebuild of
`draw_list.cpp`/`draw_list_test.cpp` (deleted `.o` files, rebuilt from
scratch) — zero warnings, zero errors. `./rx_scene_tests` →
`test cases: 58 | 58 passed`, `assertions: 6205 | 6205 passed`, 0
failures — matches the claimed post-fix numbers exactly.

**Scope check:** `git show --stat` on both commits confirms only
`draw_list.cpp`/`draw_list.h`/`draw_list_test.cpp` (code) and
`task-19-report.md` (docs) changed — nothing outside the 4 findings'
footprint (no `scene.h`/`scene.cpp`/`CMakeLists.txt` touched this round,
correctly, since none of the 4 findings required it).

**Commit hygiene:** both commits authored by `Yousef Wadi
<ywadi85@gmail.com>`, matching git config; no AI-attribution strings in
either commit body; `git status --porcelain` shows only the pre-existing,
still-untouched `progress.md` modification; local `main` is 4 commits
ahead of `origin/main` (afd8a0d, e0fe8c4, 969fe55, cd5e4c5) with nothing
pushed.

**Verdict: ALL ADDRESSED.** No new findings raised in this scoped pass
(none requested; none found). Task 19 stands at spec-compliance PASS,
quality Approved, closing cards #6 and #7.
