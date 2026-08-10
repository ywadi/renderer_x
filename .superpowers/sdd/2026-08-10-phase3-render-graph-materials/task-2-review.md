# Task 2 review: sync2 barrier derivation in rx_graph (089af68..df40111)

Reviewed: brief, implementer report, full diff, current tree state (`git log`
confirms commit `df4011108a8b1755f3ad2115ef4aba5a14d5b1ff`, author/committer
`Yousef Wadi <ywadi85@gmail.com>`, subject `feat: derive sync2 barriers in
rx_graph compile`, no body). Cross-checked against Granite's actual source
(`renderer/render_graph.cpp`, fetched directly: `build_physical_barriers()`
~L3193-3395, `physical_pass_handle_invalidate_barrier()`/`need_invalidate()`
~L1748-2181, `physical_pass_handle_flush_barrier()` ~L2147-2181) and the
2017-08-15 blog post, not just the brief's paraphrase of them. Re-ran the
existing `rx_graph_tests` binary (already built, `ninja: no work to do`):
20/20 cases, 149/149 assertions pass, matching the report. Wrote two
standalone probes (kept in `/tmp/.../scratchpad/`, not the repo) linking
directly against the built `librx_graph.a` to exercise behavior no required
test covers; both are reproducible with the commands below.

## Spec compliance

| # | Requirement | Verdict |
|---|---|---|
| 1 | Files created/modified exactly as listed (`barriers.h/.cpp`, `render_graph.{h,cpp}`, `test_barriers.cpp`, CMakeLists) | ✅ |
| 2 | Interface: `ImageBarrier`/`BufferBarrier`/`PassBarriers` + `CompiledGraph::passBarriers()`/`finalBarriers()`, exactly as specified | ✅ (plus additive, brief-exceeding `ResourceBarrierState`/`applyAccess` — see Important finding 2) |
| 3 | Algorithm faithfully ports **Granite's actual** per-resource invalidate/flush state machine (global constraint: "consult [Granite] to check the model was ported faithfully, not approximately") | ❌ — see Critical finding 1 |
| 4 | `shadow-then-sample` test, exact 6-field barriers | ✅ |
| 5 | `no-redundant-read` test, proves absence | ✅ |
| 6 | `hdr-tonemap` test, exact 6-field barriers | ✅ |
| 7 | `war-execution-only` test, srcAccess=0 | ✅ (only via `applyAccess()` directly, not through `RenderGraph`'s public API — see Important finding 2) |
| 8 | `compute-to-draw-buffer` test, BufferBarrier only | ✅ |
| 9 | `present-final` test, finalBarriers() | ✅ |
| 10 | `culled-contributes-nothing` test | ✅ |
| 11 | sync2 vocabulary only (`Vk*Flags2`, `*Barrier2`-shaped payload structs) | ✅ |
| 12 | Device-free; `vulkan_core.h` only in headers; no volk in headers | ✅ (grep-verified, no `volk`/`vulkan/vulkan.h`/`rx_rhi_vk` include or link) |
| 13 | No AI attribution anywhere | ✅ (grep across every changed file + commit message/author; clean) |
| 14 | Existing rx_graph style/conventions | ✅ |
| 15 | Coordinator rule: first use = srcStage NONE/srcAccess 0/oldLayout UNDEFINED | ✅ |
| 16 | Coordinator rule: WAR = execution-only + layout transition | ✅ in isolation (see finding 2 on public-API reachability) |
| 17 | Coordinator rule: WAW makes prior write available | ✅ |
| 18 | Coordinator rule: storage buffers → BufferBarrier only, no layouts | ✅ |
| 19 | Coordinator rule: finalBarriers = backbuffer currentLayout→PRESENT_SRC_KHR, dst NONE/0 | ✅ |
| 20 | Coordinator rule: culled passes contribute nothing | ✅ (verified by test + code reading: `buildBarriers()` only walks `executionOrder()`, and `passAccesses()` is empty for culled passes) |
| 21 | Coordinator rule: redundant read-after-read emits nothing | ✅ for same-stage re-reads (the only case any required test exercises) — **not true in general**, see Critical finding 1 |
| 22 | `compile()` deterministic/repeatable | ✅ (probed empirically, see below) |

**Overall spec verdict: ❌** — requirement 3 (Granite-fidelity of the state
machine) and, as a direct consequence, the general form of requirement 21
(read-after-read/read-after-flush correctness across different pipeline
stages) fail. Everything else is compliant.

## Critical finding: the state machine under-synchronizes a second reader in a different pipeline stage, and this compounds into WAR writes

Granite's actual model (not just the brief's one-paragraph summary of it)
keeps **two** separate pieces of state per resource that `rx_graph` collapses
into one:

- `to_flush_access` — cleared as soon as *any* invalidate barrier resolves it.
- `pipeline_barrier_src_stages` — the stage of the last actual write,
  **not** cleared when `to_flush_access` is cleared; it persists as the
  correct `srcStageMask` for any *later* invalidate that targets a stage not
  yet covered.
- `invalidated_in_stage[64]` — a genuinely **per-pipeline-stage-bit** array
  (`need_invalidate()`, `render_graph.cpp` L1748-1756), so a reader in stage
  S1 being invalidated never marks stage S2 as invalidated too.

`rx_graph`'s `ResourceBarrierState` (`barriers.h`) instead has one
`pendingFlushStages/Access` pair that gets **fully cleared** the moment any
read resolves it, and one **aggregate** `invalidatedStages/Access` pair (not
per-stage-bit). Because `applyAccess()`'s read-needs-barrier clause is
`!write && state.hasPendingFlush && !coveredByInvalidated(...)`
(`barriers.cpp` line ~140), once `hasPendingFlush` flips false after the
*first* resolving read, **no subsequent read can ever get a barrier again**
(regardless of its own stage's actual coverage) until the next write. This
is not a corner case — it is the ordinary "same resource read by two
different passes in two different pipeline stages" pattern (e.g. a shadow
map or G-buffer texture sampled by one pass's fragment shader and later by a
compute pass, with no intervening write).

Verified directly against the built `librx_graph.a` (not by re-reading source
a second time):

```
write (COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_WRITE)
read1 (VERTEX_SHADER / SHADER_SAMPLED_READ)   -> has_barrier=1 (correct)
read2 (COMPUTE_SHADER / SHADER_SAMPLED_READ)  -> has_barrier=0  <-- BUG
```

`read2` gets **zero** barriers: `COMPUTE_SHADER`'s cache is never named as a
`dstStage` in any sync2 barrier for this resource, so it has no visibility
guarantee at all — a genuine GPU synchronization hazard, not merely a
missed optimization.

It compounds: a subsequent WAR write after `read1`+`read2` produces
`srcStage = VERTEX_SHADER` only — `COMPUTE_SHADER` is silently omitted, so the
write is not even guaranteed to wait for `read2`'s in-flight compute-shader
access to finish:

```
WAR write after read1+read2: has_barrier=1 srcStage=8 (VERTEX_SHADER=8 COMPUTE_SHADER=2048)
 -- write's srcStage OMITS COMPUTE_SHADER
COMPOUNDING BUG CONFIRMED: the WAR write does not wait on read2's stage at all.
```

None of the brief's seven required tests exercise this pattern: every test
that has multiple stages touching one resource combines them into a single
`ResourceAccess` *within one pass* (`combineByResource()` unions stages
before `applyAccess()` ever sees them, e.g. `compute-to-draw-buffer`'s
`VERTEX_SHADER|FRAGMENT_SHADER` combined read), and `no-redundant-read`'s two
readers use the *same* stage. So this ships completely uncaught by the
specified suite.

This also contradicts the report's own claim that "the purpose of
`pendingFlush` vs `invalidated`... as two distinct pieces of state... [is]
both Granite's core insight and this task's brief's own framing of it" —
the two pieces of state exist in `rx_graph`, but they don't carry the
information Granite's actually do, so the "shape of the decision" was not,
in fact, ported faithfully, only approximately.

Reproduction (kept outside the repo tree):
```
/media/ywadi/second/renderer_x/cmake/zig-wrappers/zig-cxx-linux \
  -I src/rx_graph/include -I src/rx_core/include \
  -isystem .deps-cache/Vulkan-Headers-89af49d8f66fdf91/include \
  -std=gnu++20 -O0 -g probe_multistage.cpp \
  build/linux-native/src/rx_graph/librx_graph.a \
  build/linux-native/src/rx_core/librx_core.a -o probe_multistage
```
(probe source available on request; both scenarios above were run against
the actual shipped `applyAccess()`, not a reimplementation.)

**Fix direction** (not prescribing the exact shape, since Task 3 may want a
say): either (a) track invalidation per-stage-bit as Granite does (an
array/bitset keyed by stage, not a single aggregate `stages` mask), or (b)
at minimum decouple "is there still unflushed data" from "what srcStage
should a barrier to an uncovered stage use," so a resolved flush's original
write-stage survives long enough to seed a later, different-stage read's
barrier, and make the read-needs-barrier check depend only on
`!coveredByInvalidated(...)`, not additionally on `hasPendingFlush`.

## Important finding: Deviation 1's added public surface is justified but not clearly scoped

Verified independently (not taking the report's word for it):
`render_graph.cpp` line 220, `uint32_t writer = it->second.back();` —
confirms every reader of a resource name binds to that name's *final*
declared writer, so a `write, read, write` sequence for one physical
resource can never survive `compile()`'s topological sort as `write, read,
write`; it becomes `write, write, read`. The report's empirical claim is
correct, and `ResourceBarrierState`/`applyAccess()` genuinely are the real
per-resource decomposition `buildBarriers()` is built from, not scaffolding
bolted on purely for testability. The reads-observe-final-version semantic
*is* documented in a public header (`barriers.h`'s comment on `applyAccess()`,
lines ~368-382), satisfying the coordinator's stated condition for accepting
this deviation.

That said: the brief's "Interfaces (produces)" section is a locked contract
(presumably for Task 3's executor to consume), and this adds two more public
names to it without coordinator sign-off ahead of time, placed directly
alongside the brief-specified types with no namespace or naming convention
marking them as a lower-level, more-likely-to-change seam (e.g. `detail::`).
Functionally this is fine today; recommend either explicit coordinator
ratification of the expanded interface, or moving `ResourceBarrierState`/
`applyAccess()` under a clearly-marked internal namespace so Task 3 and
future consumers don't treat them as equally stable, general-purpose API.

## Minor finding: the report's Deviation 2 write-up overstates its own scope

The report claims "six of the seven required tests" needed the "reader pass
doubles as present" restructuring. Direct inspection of
`test_barriers.cpp` shows only **four** test cases use that pattern at all
(`shadow-then-sample`, `hdr-tonemap`, `compute-to-draw-buffer`,
`culled-contributes-nothing`), and only **three** of those actually needed
the list-size relaxation the report describes (`shadow-then-sample`,
`hdr-tonemap`, `culled-contributes-nothing`); `compute-to-draw-buffer`'s
"draw" pass also writes `bb`, but the contamination lands in `imageBarriers`,
a list that test never asserts on at all (it only checks `bufferBarriers`),
so no relaxation was needed there. `no-redundant-read`, `war-execution-only`,
and `present-final` don't use the pattern at all. This doesn't change the
correctness of any shipped test, but the self-audit in the report is
inaccurate and should be corrected for the record — exactly the kind of
detail that should be verified against the diff, not taken on trust.

None of the seven scenarios are vacuous: `no-redundant-read` still asserts
`imageBarriers.empty()` (a genuine absence proof, not weakened at all — it
was never part of the relaxed set), and the three relaxed tests still assert
every one of the six fields exactly, just located by `physicalIndex` first.

## Minor finding: relaxed tests find "at least one" barrier for the resource under test, not "exactly one"

`findImageBarrier()` (`test_barriers.cpp` line 86) returns the *first* list
entry matching `physicalIndex`. Combined with only asserting the *total*
list size (e.g. `== 2`), a hypothetical regression that emitted a duplicate
barrier for the resource under test *and* dropped the pass's own unrelated
first-use barrier would still pass (`size == 2`, first match still correct).
I verified this is not a live bug: `combineByResource()`'s
`std::find_if`-based per-pass merge structurally guarantees at most one
combined entry per `physicalIndex` per pass, so `buildBarriers()` cannot
currently produce two `ImageBarrier`s for the same resource within one
pass's list. But the test's defense-in-depth against a future regression in
that merge is weaker than the coordinator explicitly asked for. Recommend
adding an exact-count assertion (e.g.
`std::count_if(barriers, matches physicalIndex) == 1`) alongside the
existing `find`, in the three tests that use this pattern.

## Verified correct (no finding)

- **Ordering/minimality**: for `shadow-then-sample` and `hdr-tonemap`, the
  emitted barrier count for the actual constructed graph topology (3 image
  barriers each across the whole compiled graph, plus the final present
  barrier) is the true minimum given that topology — no spurious/duplicate
  barriers beyond the first-use transitions and the one under test.
- **Determinism**: probed directly — `compile()` called twice on the same
  `RenderGraph` (no `reset()`), and `reset()` + rebuild from scratch, both
  produce byte-identical `passBarriers()`/`finalBarriers()` on a
  shadow-then-sample-shaped graph. `buildBarriers()` has no hash-map
  iteration or other ordering-sensitive construct in its own logic;
  `combineByResource()` walks a `std::vector` in declaration order.
- **Culled passes**: `buildBarriers()` only walks `graph.executionOrder()`,
  and `CompiledGraph::passAccesses()` is empty for any culled pass (Task 1
  guarantee), so a culled pass's declared accesses never reach
  `combineByResource()`/`applyAccess()` at all — matches the
  `culled-contributes-nothing` test and the coordinator's rule.
- **WAW / WAR reset semantics**: a write unconditionally clears
  `invalidatedStages/Access` and sets a fresh `pendingFlush`
  (`barriers.cpp` lines 175-180) — correct per Granite and the brief. WAW
  correctly makes the prior write available (uses `pendingFlushStages/Access`
  as `srcStage/Access` when `hasPendingFlush` is still true at the next
  write). WAR correctly emits `srcAccess = 0` with `srcStage` taken from
  `invalidatedStages` — but see the Critical finding for why that
  accumulated `invalidatedStages` can itself be incomplete.
- **Storage buffers**: `BufferBarrier` has no layout fields; `applyAccess()`
  never touches `oldLayout/newLayout` for `isBuffer == true`; `buildBarriers()`
  only ever produces a `BufferBarrier` (never an `ImageBarrier`) for a
  buffer resource.
- **finalBarriers()**: backbuffer's `pendingFlushStages/Access` at the end of
  the walk becomes `srcStage/Access`, dst is `NONE`/`0`, `newLayout` is
  `PRESENT_SRC_KHR` — matches brief and coordinator rule exactly, confirmed
  by the `present-final` test.
- **Header hygiene / device-freedom / no AI attribution**: all grep-verified
  clean across every changed file and the commit itself.

## Test-run evidence

Re-ran the already-built `rx_graph_tests` binary directly (build was already
up to date — `ninja: no work to do`), rather than trusting the report's
pasted output alone:

```
[doctest] test cases:  20 |  20 passed | 0 failed | 0 skipped
[doctest] assertions: 149 | 149 passed | 0 failed |
[doctest] Status: SUCCESS!
```

This matches the report's claim exactly and confirms the working tree is in
the state the report describes. Did not re-run the full multi-preset
regression (`windows-cross-zig` under Wine) — no concrete reason from the
diff to doubt it, and it's expensive; the two standalone probes above were
targeted specifically at behavior no required test covers, per the review's
own instructions.

## Quality verdict

**4 findings**: 1 Critical, 1 Important, 2 Minor. Not approved as-is —
the Critical finding is a real, verified GPU synchronization hazard for a
common access pattern (same resource, two readers in different pipeline
stages, no intervening write) that every one of the brief's required tests
happens to avoid exercising. Recommend a fix round targeting
`ResourceBarrierState`/`applyAccess()`'s per-stage invalidation tracking
before this barrier deriver is trusted by Task 3's executor.
