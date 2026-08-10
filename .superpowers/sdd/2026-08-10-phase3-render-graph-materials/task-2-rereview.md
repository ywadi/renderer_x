# Task 2 fix round 1 re-review: per-stage barrier visibility (df40111..77f42cf)

Scope: verify the four findings from `task-2-review.md` are addressed by
commit `77f42cf` and that the fix introduces no new defects. Not a full
re-review of Task 2 (already reviewed in `task-2-review.md`).

Reviewed: the four findings, the fix diff
(`review-df40111..77f42cf.diff`), the implementer's fix-round report
section (`task-2-report.md`, "Fix round 1"), and the current tree state
(`git log` confirms `HEAD` is `77f42cf2ea10d6f25f4ec3367c9b7b7967cdf544`,
author/committer `Yousef Wadi <ywadi85@gmail.com>`, no AI attribution in
subject/body — grep-verified clean). `git diff --stat df40111..77f42cf`
matches the diff package exactly: only `barriers.cpp`, `barriers.h`,
`test_barriers.cpp` changed — `render_graph.{h,cpp}`/CMakeLists untouched,
as the report claims.

## Finding 1 (Critical): per-stage invalidation tracking

**Addressed.** `detail::ResourceBarrierState` (`barriers.h`) now separates
what Granite keeps separate:
- `lastWriteStages` — persists across reads (Granite's
  `pipeline_barrier_src_stages`), only overwritten by a new write.
- `hasPendingFlush`/`pendingFlushAccess` — cleared by any resolving
  barrier (Granite's `to_flush_access`).
- `invalidatedInStage` — `std::array<VkAccessFlags2, 64>`, indexed by
  stage-bit position, plus `invalidatedStagesUnion` (an explicitly
  non-Granite convenience field, documented as such).

`coveredByInvalidated()` (`barriers.cpp`) now checks per-stage-bit via
`forEachStageBit`, and the read-needs-barrier clause is
`!write && !coveredByInvalidated(...)` — the `hasPendingFlush` gate the
prior version had is gone, exactly per the review's fix direction.

Re-ran the prior review's exact probe against the rebuilt library
(commit 77f42cf, `librx_graph.a` rebuilt and current — `ninja: no work to
do`), from the scratchpad, not the repo:

```
write has_barrier=1
read1 (VERTEX_SHADER) has_barrier=1 srcStage=1024 srcAccess=256
read2 (COMPUTE_SHADER) has_barrier=1 srcStage=1024 srcAccess=0 dstStage=2048
FIXED: read2 got its own barrier (dstStage should be COMPUTE_SHADER=2048)
WAR write after read1+read2: has_barrier=1 srcStage=2056 (expect union=2056, VERTEX=8 COMPUTE=2048)
COMPOUNDING BUG FIXED: WAR write's srcStage is the union of both reader stages.
```

`read2` (the previously-silenced second reader in an uncovered stage) now
gets a real barrier — `srcStage` correctly chained off the persisted
`lastWriteStages`, `srcAccess=0` (already flushed by read1), `dstStage`
correctly `COMPUTE_SHADER`. The compounding WAR write now unions both
reader stages (`8|2048=2056`) instead of omitting `COMPUTE_SHADER`. The
Critical finding is confirmed dead against the actual rebuilt binary, not
just by re-reading source.

The two required new tests exist and match spec:
- `multi-stage-read-gets-own-barrier` (test_barriers.cpp:222) — API-level,
  builds `W` (write) → `A` (Graphics-class, FRAGMENT_SHADER read, doubles
  as present) → `B` (Compute-class, COMPUTE_SHADER read, side-effect-only),
  asserts `A`'s flush barrier and `B`'s own barrier via
  `checkImageBarrier()` with all six fields exact (`B`'s: srcStage =
  `COLOR_ATTACHMENT_OUTPUT_BIT`, srcAccess = `NONE`, dstStage =
  `COMPUTE_SHADER_BIT`, dstAccess = `SHADER_SAMPLED_READ_BIT`, no layout
  change).
- `war-unions-all-reader-stages` (test_barriers.cpp:271) — unit-level,
  direct `detail::applyAccess()` reproduction of the reviewer's own
  scenario (write → read(VERTEX_SHADER) → read(COMPUTE_SHADER) → write).
  Every one of the four steps' six fields is asserted exactly via
  individual `CHECK`s, including the WAR write's `srcStage ==
  (VERTEX_SHADER_BIT | COMPUTE_SHADER_BIT)`.

Both tests pass (confirmed by running the rebuilt `rx_graph_tests` binary
directly: 22/22 test cases, 203/203 assertions, matching the report).

Also checked the WAR-union divergence against the Vulkan sync2 model, as
instructed: a WAR hazard's execution dependency must cover the pipeline
stage of *every* outstanding reader, not just the most recent one — a
`srcStageMask` that omits a reader's stage does not guarantee that
reader's work completes before the writer's `dstStageMask` begins
executing, which is a real hazard, not a missed optimization. Granite's
own literal behavior (chaining a WAR write's `srcStage` off
`pipeline_barrier_src_stages`, i.e. the *original write's* stage) would
not wait on any reader at all and is therefore not a valid substitute
here. The union approach (`invalidatedStagesUnion`) is the correct fix,
and it is documented in the code itself, not only the report:
`barriers.h`'s field comment on `invalidatedStagesUnion` and the WAR
branch's inline comment in `barriers.cpp` (`applyAccess()`) both call out
the divergence from Granite's literal source and why it's required.

Sanity-checked the per-stage model for a new defect class: whether the
64-entry `invalidatedInStage` array/`forEachStageBit` loop mishandles
sync2's extended stage bits (positions >31, e.g. `VK_PIPELINE_STAGE_2_
COPY_BIT`=bit 32, `..._BLIT_BIT`=bit 34, up to `..._COPY_INDIRECT_BIT_KHR`
=bit 46 in the vendored Vulkan-Headers). `VkPipelineStageFlags2` is
`typedef VkFlags64` (a 64-bit type) by definition, so no stage bit any
Vulkan version can define will ever fall outside `[0, 63]` — the 64-entry
array is exactly, not approximately, sized, and `forEachStageBit`'s loop
correctly iterates the full `0..63` range (not truncated to 32). Verified
this empirically too, not just by inspection: a second scratchpad probe
using `VK_PIPELINE_STAGE_2_TRANSFER_BIT` (bit 12) as a write stage, then
`VK_PIPELINE_STAGE_2_COPY_BIT` (bit 32) and `..._BLIT_BIT` (bit 34) as two
different-stage reads:

```
Extended-stage-bit probe: write(bit12) has_barrier=0; read(COPY_BIT, bit32) has_barrier=1 srcStage=4096
Same-stage re-read (COPY_BIT again) has_barrier=0 (expect 0, i.e. redundant-read elided)
Different extended stage (BLIT_BIT, bit34) has_barrier=1 srcStage=4096 (expect nonzero, its own barrier)
EXTENDED-STAGE-BIT HANDLING CORRECT: bits >31 tracked per-stage exactly like low bits.
```

Bits >31 are tracked per-stage exactly like low bits — same-stage re-read
correctly elided, different extended stage correctly gets its own
barrier. No new defect class here. (Separately, current declarations in
`render_graph.cpp`'s `resolveAccess` table only ever produce low
single-digit stage bits, so this is a robustness property of the model,
not something today's callers currently exercise — but the model itself
is correct for the full 64-bit range, which is what the finding asked to
check.)

## Finding 2 (Important): scoped to `rx::graph::detail`

**Addressed.** `ResourceBarrierState`, `BarrierTransition`, and
`applyAccess` are now inside `namespace detail` in both `barriers.h` and
`barriers.cpp`, with an explicit comment immediately above the namespace
opening in `barriers.h`: "Not API-stable: ... exposed only so
barriers.cpp's own unit tests and Task 3's executor can drive the
per-resource state machine directly -- buildBarriers() is the one
brief-specified, stable entry point," and a forward pointer to this fix
round's Important finding. `test_barriers.cpp` updated to `using
rx::graph::detail::applyAccess;` / `using rx::graph::detail::
ResourceBarrierState;`. Grepped the tree: no file outside
`barriers.h/.cpp`/`test_barriers.cpp` references either name, so nothing
else needed updating.

## Finding 3 (Minor): exact-one-barrier assertion before field checks

**Addressed.** `countMatchingImageBarriers()` (test_barriers.cpp, new
helper) is called with `REQUIRE(... == 1)` immediately before
`findImageBarrier()`/`checkImageBarrier()` in all three relaxed tests:
`shadow-then-sample` (line ~145), `hdr-tonemap` (line ~196),
`culled-contributes-nothing` (line ~424). Order in each case is: list-size
check → exact-one-match check → locate → six-field check, matching what
the finding asked for.

## Finding 4 (Minor): report deviation-2 counts corrected

**Addressed.** `task-2-report.md`'s deviation-2 section now reads "Four of
the seven required tests build the scenario through a pass that 'doubles
as the present pass'... but only in three of those four... does that
contamination land in a list the test actually asserts on," with an
inline note "(Corrected in fix round 1 below -- the original text here
said 'six of the seven' needed the pattern, which was wrong; see that
section.)" The "Fix round 1" section restates the corrected 4/3 counts
again under its own Minor-finding heading.

## New-defect check on the fix itself

Read the full diff line-by-line, not just the parts covering the four
findings:
- `combineByResource()`/`CombinedAccess` (unchanged logic, just moved out
  from between the two `detail`-namespace blocks and now calling
  `detail::isWriteAccess`) — untouched, no behavior change.
- `buildBarriers()`'s final-barrier construction now reads
  `s.lastWriteStages` instead of the removed `s.pendingFlushStages` for
  the backbuffer's src stage — correct rename, same field's old role
  (last write's stage), still only reached once per compile since nothing
  reads the backbuffer after its final write.
- The write branch's three-way split (WAW / WAR / first-use) and the read
  branch's `srcAccess = hasPendingFlush ? pendingFlushAccess : NONE` were
  checked against a partial-coverage case (a read requesting stages
  `S1|S2` where only `S1` was previously invalidated): `needBarrier` fires
  correctly (coverage fails on `S2`), the emitted barrier's `dstStage`
  covers both `S1|S2`, and the post-barrier update
  (`forEachStageBit(stages, ...)`) marks *both* `S1` and `S2` invalidated
  for that access — consistent with the barrier just inserted actually
  covering both stages. No defect found.
- Rebuilt and ran the full `rx_graph_tests` binary directly (not trusting
  the report's pasted output alone): `22/22` test cases, `203/203`
  assertions, `SUCCESS`, matching the report exactly.
- Commit `77f42cf`'s diff stat matches the review package's file list
  exactly (`barriers.cpp`/`barriers.h`/`test_barriers.cpp` only); grepped
  the commit for AI attribution — clean; author/committer is
  `Yousef Wadi <ywadi85@gmail.com>`.

No new defects found.

## Verdict

All four findings addressed. Approved.
