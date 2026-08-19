# Task 23 review — Executor per-frame allocation elimination (card #29)

Reviewer: independent (did not write this code). Commits under review:
`477a511` (rx_rhi_vk DeletionQueue in-place compaction), `1197d7b`
(rx_graph executor zero-alloc rework), `23e5c69` (report). Authority order
followed: `gate/rulings-2026-08-18.md` §#29 > spec/CLAUDE.md >
`gate/matrix-issue29-executor-allocations.md` > ticket #29.

## Verdict 1 — Spec compliance: PASS

Every binding acceptance criterion in the brief, as amended by
rulings-2026-08-18.md §#29, is met:

- `execute()` is zero-heap-alloc in steady state, proven by the bound
  methodology (capacity-snapshot via `detail::allocationCapacitiesForTesting`,
  no global operator-new interposition) — independently re-run, green.
- Sites 1-4 (tracking vectors + the two maps), 5-8 (per-pass/barrier
  scratch), 9 (debug-label buffer), 10-11 (chunk command-buffer scratch),
  12 (`nameToIndex` transparent lookup) all converted per the matrix's
  shapes, `Impl`-persistent, no reconstruction.
- Hidden 8th allocation site (`combineAccessesByResource`) found and fixed
  in-round, matching CLAUDE.md's no-deferred-fixes rule — legitimate,
  in-scope (both call sites are inside `execute()`'s own call graph).
- `DeletionQueue::onFrameFenceSignaled` folded in per the ruling's explicit
  "SCOPE GROWS" instruction — not scope creep (see adjudication 3 below).
- `compile()`/`realize()` exempted and documented as setup/resize-only in
  code.
- Byte-identical rendering: full 26/26 ctest green, including the D29
  mixed-depth-convention test (`Task 22`'s logic verified untouched, both
  behaviorally and diff-wise — `executor.cpp:646`/`:1119`-area convention
  logic is not part of this diff at all).
- Zero unfiltered validation errors: confirmed directly (only
  "known false positive"-tagged lines appear in any GPU test run; every
  test case's own `CHECK_FALSE(hasValidationErrors())` passes).
- No public API change: `Executor`'s public method surface is untouched;
  every new symbol is `detail::`-scoped, following the established
  `debugChunkStats()`/`debugLastFrameFinalStages()` carve-out convention.

## Verdict 2 — Code quality: APPROVED, with 2 minor findings (non-blocking)

### Findings

1. **[Minor, code quality]** `ExecutorAllocationCapacitiesForTesting`'s
   zero-alloc test (`test_execute_gpu.cpp:2294-2301`) only `REQUIRE`s
   nonzero on 2 of the 13 tracked fields before trusting the steady-state
   comparison. I independently instrumented and confirmed all 13 fields
   are genuinely nonzero after warm-up (`firstBarrierSeen=64`,
   `vkBufferBarriersScratch=1`, `chunkBuffersScratch[0]=7`, etc.) — the
   test is not vacuous — but the anti-vacuousness guard itself only
   covers 2 fields, so a future graph-fixture edit that silently stops
   exercising, say, `vkBufferBarriersScratch` would degrade one of the
   13 `CHECK`s to a silent `0==0` pass without the test noticing. Widening
   the two `REQUIRE`s to cover all 13 fields (or at least the least-common
   ones: `vkBufferBarriersScratch`, `colorPhysIdxScratch`) would close
   this durably.
2. **[Minor, process]** The two disclosed concerns (vector\<bool\> gap,
   dev-container benchmark venue) are flagged only in prose (report +
   code comments), not as durable registry/errata entries. Given
   CLAUDE.md's repo policy that implementer/reviewer agents don't
   maintain the board/registry, this is the correct scope boundary for
   the implementer to have stayed inside — flagging for the coordinator,
   not self-authorizing a registry edit — but it means the concerns rely
   on this review (and the coordinator reading it) to actually reach the
   registry. No action needed from the implementer; noting for the
   coordinator's own follow-up.

No correctness, safety, or discipline defects found. The
`combineAccessesByResource`/`applyBarriers` shared-scratch-buffer
non-reentrancy claims were checked against the actual call graph and hold
(both call sites per pass are strictly sequential within one per-pass
loop iteration, never concurrent). `DeletionQueue::onFrameFenceSignaled`'s
in-place compaction is a textbook-correct erase-remove-with-manual-side-effect
(destructor runs exactly once per due item; `items_.resize()` only ever
shrinks, so no double-invocation or capacity-loss hazard).

## Empirical verification performed

- **Build**: clean from `/media/ywadi/second/renderer_x` (real path),
  `cmake --build build/linux-native -j8` — zero errors, and a full
  force-rebuild of all 4 touched translation units produced zero
  warnings (confirmed via `touch` + rebuild + grep).
- **Full ctest, linux-native, serial, under `xvfb-run -a`**: **26/26
  passed**, run three times across this review (baseline, and twice more
  after each revert-probe was restored) — always 26/26, always clean.
- **Zero validation errors**: every GPU test binary run directly (not
  just via ctest) showed only lines explicitly tagged
  `(known false positive: ...)`; zero unfiltered validation
  errors/warnings anywhere.
- **D29 (Task 22) regression check**: ran `D29*` test case directly — 1
  test case, 21/21 assertions passed, confirming Task 22's mixed-depth-
  convention logic is behaviorally untouched.
- **New tests run directly**: `rx_graph_gpu_tests` full suite (11 cases,
  959/959 assertions) and `rx_rhi_vk_tests` DeletionQueue suite (6 cases,
  495/495 assertions), both green.
- **Independent revert-discrimination (2 of 2, both reproduced exactly,
  numbers byte-for-byte matching the report)**:
  1. Reverted `executor.cpp`'s sites 1-4 to fresh-reconstruct-into-the-
     same-field every call (report's "Probe 2" shape). Result: **310
     assertions, 290 passed, 20 failed** — exact match to the report.
     Capacity-only `CHECK`s on the 3 `std::vector<bool>` fields did
     **not** fail (confirming the disclosed gap is real); `.data()`
     pointer-identity on `finalStageThisExecute`/`finalAccessThisExecute`
     failed on every one of the 10 steady frames (2 assertions × 10).
     Restored byte-identically (`git diff` empty after `git checkout`);
     rebuild green, 324/324 assertions passing again.
  2. Reverted `Executor::Impl::nameToIndex`'s declared type back to plain
     `std::unordered_map<std::string, uint32_t>` (non-transparent).
     Result: **compile failure**, at exactly the two sites the report
     names — `impl.nameToIndex = std::move(nameToIndex)` in `realize()`
     (no viable `operator=` between the two map types) and
     `impl.nameToIndex.find(name)` in `lookupResolvedIndex()` (no
     `find` overload accepts `std::string_view` against a
     non-transparent map) — proving the compile-time regression-guard
     claim directly, not merely trusting it. Restored byte-identically;
     rebuild green.
  3. Also independently reverted `DeletionQueue::onFrameFenceSignaled`
     to the old fresh-vector two-pass (report's "Probe 1"). Result:
     **495 assertions, 480 passed, 15 failed** — exact match. No
     capacity-only `CHECK` failed; `itemDataForTesting` pointer-identity
     failed repeatedly across the 30 steady iterations. Restored
     byte-identically; rebuild green, 495/495 passing again.
- **Vacuousness sweep**: instrumented the zero-alloc test to print all 13
  tracked `before`-snapshot fields; confirmed every one is genuinely
  nonzero after warm-up (not a trivial `0==0` pass anywhere) — see
  Finding 1 above for the one durability gap found (only 2 of 13 fields
  have an explicit `REQUIRE > 0` guard, though all 13 are empirically
  nonzero today). Removed instrumentation, restored byte-identically,
  rebuild green.
- **Commit hygiene**: all 3 commits authored/committed by
  `Yousef Wadi <ywadi85@gmail.com>` (matches local git config); grepped
  all 3 commits' full diffs for AI-attribution strings
  (`claude|anthropic|co-authored|ai assist|generated by`) — the only
  hits are references to the repo's own `CLAUDE.md` policy file by name,
  which is expected and not attribution. `git log origin/main..HEAD`
  shows all 3 commits are local-only (nothing pushed). Final
  `git status`: only the pre-existing `.../progress.md` modification
  remains (untouched by this review, as instructed) — every temporary
  review edit was restored byte-identically (`git diff` empty on every
  touched file after each probe).
- **Not independently re-verified by me** (outside this review's
  required scope, per the task's explicit "full linux-native serial
  ctest" instruction): the windows-cross-zig (Wine) build/test results
  the report also claims 26/26 for; the actual sample 07 dev-container
  timing numbers (re-running a 5-repetition A/B benchmark was judged
  out of scope for a correctness/discrimination review and is addressed
  by adjudication 2 below on process grounds instead).

## Adjudications

### 1. The `std::vector<bool>` capacity-only gap (3 fields: `firstBarrierSeen`, `attachmentEverWritten`, `touchedThisExecute`)

**Verdict: the accepted-gap disclosure is sufficient; the type choice is
not a defect.**

The matrix's row for sites 1-2 is the *specific, binding* acceptance
criterion for exactly these fields, and it is unambiguous on both the
type and the test shape: *"Both become `Impl`-persistent `std::vector<bool>`
members, resized (not reconstructed) only when `resources.size()` changes,
and cleared (`std::fill`, not reallocated) at the top of every `execute()`.
Test: capacity is unchanged across N steady-state frames"* — no `.data()`/
pointer-identity requirement, and `std::vector<bool>` named explicitly as
the required shape. Per the rulings doc's own stated precedence rule
("Matrix criteria not mentioned here are adopted verbatim" — the §#29
ruling entry does not touch this row), this is binding as written. Since
`std::vector<bool>` genuinely has no `.data()` member in the standard (not
an oversight, not a convenience gap — it is the bit-packed specialization,
architecturally incapable of exposing a pointer), a `.data()`-based second
signal is *structurally* unavailable for these 3 fields without changing
their element type — and changing the element type would itself violate
the matrix's own named binding shape for these two sites. The implementer
correctly identified this as outside Task 23's authority to change
unilaterally and disclosed it prominently instead (code comments on
`ExecutorAllocationCapacitiesForTesting`, report Deviations §3, report
self-review) rather than silently shipping a false sense of full coverage.

That said, this review independently confirms the substance of the
implementer's own finding is real and worth flagging to the coordinator:
the matrix's own justification for accepting capacity-only as adequate —
*"astronomically unlikely for std::vector's doubling growth policy, and
not a realistic false-negative risk in practice"* — is empirically false
for exactly the regression shape this ticket's own steady-state premise
produces (a fixed-element-count-every-call reconstruction reliably lands
on the *same* capacity every time, confirmed both by the report's Probe 2
and by my own independent re-run of it, `310 | 290 passed | 20 failed`,
zero of those 20 failures coming from a capacity check). This is a sound
candidate for an errata entry against the matrix (in the spirit of the
rulings doc's existing E1 errata for issue #21), not a defect in this
task's delivery — the implementer does not have standing to add that
errata themselves, and correctly deferred it to prose disclosure instead.

### 2. Sample 07 benchmark venue (dev container, not Deck/desktop-floor hardware)

**Verdict: acceptable for this task's own criterion.**

Task 23's brief text sets the bar as "sample 07 numbers unchanged or
better" — a task-level constraint, distinct from CLAUDE.md's phase-level
mandate ("every phase exits with published benchmark numbers (desktop AND
Steam Deck)"). The rulings doc's own §15 entry for Task 24 ("Task 23 lands
before stress-v2 numbers (sequencing already binding)") confirms Task 24,
not Task 23, is the designated owner of the phase-exit-grade publication.
Measured single-thread-unchanged / multi-thread-faster-and-more-consistent
on the shared dev container satisfies "unchanged or better" as a same-
machine, same-methodology A/B (explicitly labeled as such, not
overclaimed as an absolute/hardware-floor number) — this is honest and
adequately scoped. The report's own recommendation to re-measure on
dedicated/Deck hardware before phase-exit publication is the correct
call and does not need to be enforced within Task 23 itself. I did not
re-run the benchmark myself (judged out of scope for a correctness
review of an already-honestly-labeled, non-gating measurement); flagging
only that this recommendation currently lives in prose and should reach
Task 24's own gate explicitly (coordinator follow-up, not an implementer
gap).

### 3. `DeletionQueue::onFrameFenceSignaled` scope (477a511)

**Verdict: authorized, not scope creep.**

The matrix names this exact function and file by name as a "new finding,"
explicitly proposing it be either folded into Task 23 or spun out as a
follow-up ticket. The binding ruling resolves that proposal directly and
unambiguously: *"SCOPE GROWS (ruled): `DeletionQueue::onFrameFenceSignaled`'s
non-empty-path fresh-vector allocation is folded into Task 23 (in-place
compaction; the pending-item test variant is mandatory) — same
discipline, one function, cross-file reach noted."* This is a named site,
explicitly ruled in-scope by the highest-authority document governing
this task, with a mandatory test variant that was in fact delivered and
independently re-verified by me (Probe 1 reproduction, exact match). Not
adjacent/opportunistic scope expansion — it is bound-in scope.

## Files (all absolute paths)

- `/media/ywadi/second/renderer_x/src/rx_graph/executor.cpp`
- `/media/ywadi/second/renderer_x/src/rx_graph/include/rx_graph/executor.h`
- `/media/ywadi/second/renderer_x/src/rx_graph/tests/test_execute_gpu.cpp`
- `/media/ywadi/second/renderer_x/src/rx_rhi_vk/include/rx_rhi_vk/deletion_queue.h`
- `/media/ywadi/second/renderer_x/src/rx_rhi_vk/src/deletion_queue.cpp`
- `/media/ywadi/second/renderer_x/src/rx_rhi_vk/tests/deletion_queue_test.cpp`
- `/media/ywadi/second/renderer_x/.superpowers/sdd/2026-08-11-phase4-scene-assets/task-23-report.md`
