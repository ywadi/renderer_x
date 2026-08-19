# Task 23 report — Executor per-frame allocation elimination (card #29)

BASE=f9169e4. Order of authority followed: rulings-2026-08-18.md §#29 >
task-23-brief.md > gate/matrix-issue29-executor-allocations.md > `gh issue
view 29`.

## Per-criterion proof

### Criterion 1 — `execute()` performs zero heap allocations in steady state

Methodology (bound by ruling): capacity-snapshot via a test-only accessor
(`rx::graph::detail::allocationCapacitiesForTesting`), **not** global
operator-new interposition — this project's `rx_graph_gpu_tests` /
`rx_rhi_vk_tests` binaries link volk, the Vulkan validation layers, and (via
`rx_shader_deploy_runtime_libs`) Slang's runtime, matching the ruling's own
stated linkage-risk reasoning.

New GPU test: `src/rx_graph/tests/test_execute_gpu.cpp`, `"Executor::execute
performs zero heap allocations in steady state..."`. Graph: `produce`
(AsyncCompute storage-buffer producer), `fillWhole` (whole-pass color
output), `probe` (bare pass resolving two real names via `PassContext`),
`fillChunked` (chunked pass into the backbuffer) — one instance of every
site this task touched, in one frame. 4 warm-up `execute()` calls, then 10
further steady-state calls, snapshotting `.capacity()` **and** `.data()`
pointer identity of every Impl-persistent buffer before/after each. All
`CHECK`s pass; `rx_graph_gpu_tests` green both presets.

### Criterion 2 — sites 1-4 (per-execute tracking, incl. the two maps)

`firstBarrierSeen`/`attachmentEverWritten` → `Executor::Impl`-persistent
`std::vector<bool>`, `.assign(resources.size(), false)` at the top of every
`execute()` call. `finalStageThisExecute`/`finalAccessThisExecute` →
`Executor::Impl`-persistent `std::vector<VkPipelineStageFlags2>`/
`std::vector<VkAccessFlags2>`, dense-indexed by `physicalIndex` (no hashing
needed — matrix's own point). A new `touchedThisExecute` `std::vector<bool>`
replaces the two old maps' implicit "key present" signal, load-bearing for
`TransientPool::sweepStale()`'s "only a genuinely-accessed-this-call
resource looks freshly used" contract (`executor.cpp:1482-1508`).

### Criterion 3 — sites 5-8 (per-pass scratch, barrier vectors)

`colorPhysIdx`/`colorAttachments` (`executor.cpp` per-pass loop) and
`vkImageBarriers`/`vkBufferBarriers` (`applyBarriers()`) all became
`Executor::Impl`-persistent, `.clear()`d at the top of their own scope,
referenced via a local `&` alias at each call site to minimize the diff
against the surrounding logic (unchanged).

### Criterion 4 — site 9 (debug-label string)

`beginDebugLabel()` now writes into `Impl::debugLabelScratch`
(`std::vector<char>`, `.resize(name.size()+1)` + `memcpy` + manual null
terminator) instead of constructing a `std::string` per pass.

### Criterion 5 — sites 10-11 (chunk command-buffer vectors)

`chunkBuffers`/`validChunkBuffers` in `recordChunkedPass()` →
`Impl::chunkBuffersScratch[frameSlot]`/`validChunkBuffersScratch[frameSlot]`,
frame-slot indexed (`std::array<..., rx::rhi::FrameSync::kFramesInFlight>`)
per the ruling's explicit instruction ("per-frame-slot, since chunked
recording is concurrent with other frame-in-flight state") — a defensive
match for `ChunkCommandPool`'s own `[frameSlot]` shape, even though every
in-tree caller only ever has one `recordChunkedPass()` call active at a
time (documented in the field comment).

### Criterion 6 — site 12 (`nameToIndex` transparent lookup)

`Executor::Impl::nameToIndex` is now
`std::unordered_map<std::string, uint32_t, TransparentStringHash,
std::equal_to<>>`. `TransparentStringHash` has **one** overload
(`operator()(std::string_view)`), so every key — real `std::string` (via
implicit conversion) or a heterogeneous `std::string_view` lookup — hashes
through the identical call; this is **stronger** than the matrix's own
proposed shape (two separate `std::hash` specializations relying on
cross-type hash-equivalence) — see Deviations.

`lookupResolvedIndex()` now calls `impl.nameToIndex.find(name)` directly,
no `std::string(name)` temporary. Regression-guard is **compile-time**, not
merely test-observed: `std::string_view` has no implicit conversion to
`std::string` (that constructor is `explicit`, C++17 [string.cons]), so a
future edit that silently reverted `nameToIndex`'s type back to
non-transparent would make this exact call **fail to compile**, not
silently reintroduce the allocation. New correctness test: `"PassContext
resolvers: nameToIndex's transparent string_view lookup resolves a real
name (hit) and throws std::out_of_range naming a nonexistent one (miss)"`
(`test_execute_gpu.cpp`).

### Criterion 7 — hidden site found in-task: `combineAccessesByResource()`

Not one of the ticket's 7 named sites: this free function returned a fresh
`std::vector<std::pair<uint32_t, CombinedAccess>>` by value, called TWICE
per pass every `execute()` call (from `synthesizeFirstUseBufferBarrierIfNeeded()`
and from `execute()`'s own per-pass union step) — squarely inside
`execute()`'s call graph. Per CLAUDE.md's "no deferred fixes" rule, fixed
in-round: now takes an out-parameter, backed by
`Impl::combinedAccessScratch`.

### Criterion 8 — `DeletionQueue::onFrameFenceSignaled` (ruling: scope grows)

`src/rx_rhi_vk/src/deletion_queue.cpp`: replaced the "build a fresh
`remaining` vector every call" two-pass with in-place compaction (classic
erase-remove idiom, per-element side effect run by hand since
`std::remove_if`/`std::erase_if`'s predicate isn't guaranteed exactly-once).
`items_.resize(writeIndex)` only ever shrinks size, never capacity.
Mandatory pending-item test variant: `deletion_queue_test.cpp`,
`"DeletionQueue::onFrameFenceSignaled compacts items_ IN PLACE..."` — a
steady "kSlots=2 items always pending" retire/signal cadence (mirrors how
`Executor::execute()` itself drives this queue), never touching the
`items_.empty()` fast path.

### Criterion 9 — GPU-zone (`VkCtxScope`) allocation trace (matrix's open question)

Completed this task, as required. Traced `RX_GPU_ZONE_DYNAMIC` →
`TracyVkZoneTransient` → the dynamic-name `VkCtxScope` constructor in the
**actually-linked** vendored copy (`.deps-cache/tracy-d3259c11d7efada2`,
confirmed via `Tracy_DIR` in `build/linux-native/CMakeCache.txt`, not the
matrix's own citation of a different cache hash) — it calls
`Profiler::AllocSourceLocation(...)`, a real allocation path, gated
identically to the CPU-side `ZoneName()` macro: `m_active = is_active &&
GetProfiler().IsConnected()` under `TRACY_ON_DEMAND` (this project's build
flag). With no profiler connected — the state every test run in this repo
is actually in — the constructor returns before ever reaching
`AllocSourceLocation()`. Documented in `executor.cpp`'s new zero-alloc
test's own top comment.

### Constraints

- Byte-identical rendering: full `rx_graph_gpu_tests` suite (2 new cases +
  all pre-existing, incl. the Task 22 D29 mixed-convention test) green both
  presets; sample 05/06 headless pixel gates green both presets.
- `compile()`/`realize()` exempt: unchanged except `nameToIndex`'s local
  declared type (must match `Impl`'s for the `std::move()` to compile).
- No public API change: `Executor`'s public surface is untouched; every new
  symbol lives in `detail::`, documented "NOT part of the stable public
  contract," matching `debugChunkStats()`/`capacitiesForTesting()` precedent.
- Zero validation errors: every touched/new test case asserts
  `CHECK_FALSE(hasValidationErrors())`; full suites green with `--validate`.

## Before/after (capacity-snapshot proof)

Ran with the fix: `rx_graph_gpu_tests` zero-alloc case — 10 steady frames,
every capacity **and** pointer-identity field bit-for-bit unchanged after
warm-up. `rx_rhi_vk_tests` pending-item case — 30 steady iterations, same
result.

## Revert-probe evidence (TDD discrimination)

Capacity-snapshot cannot be written against the literal pre-fix code (the
`Impl` fields it snapshots don't exist there) — TDD discrimination was
proven via **temporary, git-stash-backed revert probes** instead, exactly
matching this repo's own established discipline for allocation/behavior
regressions the acceptance test itself defines (D26/Task 19 precedent).
Every probe was reverted immediately after capturing the failing run; final
`git status`/`git diff` (below) confirm the tree matches only the intended
fix.

**Probe 1 — `DeletionQueue::onFrameFenceSignaled` reverted to the old
fresh-vector version.** Capacity-only assertion: **passed** (a genuine,
empirically-discovered methodology gap — see Deviations). Adding a
`.data()` pointer-identity check (`itemDataForTesting`) then caught it: 15
of 30 steady iterations failed (`doctest`: `1 |  0 passed |  1 failed`,
`125 assertions: 110 passed | 15 failed`).

**Probe 2 — `executor.cpp` sites 1-4 reverted to fresh-reconstruct-into-
the-same-field every call.** Capacity-only assertion on the two
`std::vector<bool>` fields did **not** discriminate (documented, disclosed
gap — see Deviations); the pointer-identity check on
`finalStageThisExecute`/`finalAccessThisExecute` (real element types, real
`.data()`) caught it on **every** steady frame: `310 assertions: 290 passed
| 20 failed`.

Both probes were reverted immediately after capture; `diff -q` against the
pre-probe backup confirmed byte-identical restoration before proceeding.

## sample 07 numbers (measured, not asserted)

Machine: this dev container (shared/virtualized — not the Deck hardware
floor CLAUDE.md's phase-exit policy ultimately wants; no dedicated
benchmark rig available in this environment. Numbers below are a
same-machine, same-run-methodology A/B, not an absolute performance claim).
`--draws 30000`, `cpu_record_ms` at headless frame 2 (the closest to
steady-state the sample's 3-frame headless loop offers), 5 repetitions
each, BASE=f9169e4 vs. this fix (via `git stash`/`git stash pop`, rebuilt
between each side):

| config | BASE avg (ms) | fix avg (ms) | BASE range | fix range |
|---|---|---|---|---|
| `--threads 1` | 9.031 | 9.170 | 8.75–9.24 | 8.91–9.77 |
| default (7 workers) | 3.660 | 3.528 | 2.74–5.11 | 3.44–3.69 |

Single-thread: statistically unchanged (within this machine's own ~10%
run-to-run noise). Multi-thread: fix averages ~3.6% **faster** and is
markedly **more consistent** (BASE's spread is ~2x; fix's is ~7%) —
mechanistically expected, since eliminating cross-thread heap churn during
chunked recording reduces allocator-lock contention. `draws`/`chunkCount`
identical in every run (rendering unaffected, only CPU timing measured).
Both configurations' `stress headless gate PASSED` in every run.
"Unchanged or better" is satisfied; a dedicated-hardware/Deck re-measurement
is recommended before the phase-exit publication (registry note, not this
task's own gate).

## Command tails

```
$ ctest -R "rx_graph_tests|rx_graph_gpu_tests|rx_rhi_vk_tests"   # linux-native
100% tests passed, 0 tests failed out of 3

$ ctest -R "rx_graph_tests|rx_graph_gpu_tests|rx_rhi_vk_tests"   # windows-cross-zig (Wine)
100% tests passed, 0 tests failed out of 3

$ ctest   # linux-native, full suite
100% tests passed, 0 tests failed out of 26

$ ctest   # windows-cross-zig, full suite (Wine)
100% tests passed, 0 tests failed out of 26
```

Both presets built with zero compiler warnings on the 4 touched
translation units (force-recompiled + grepped for `warning|error`).

## Deviations from the matrix's literal proposed shapes

1. **`TransparentStringHash`**: matrix proposed two separate `std::hash`
   specializations relying on `std::hash<std::string>`/
   `std::hash<std::string_view>` agreeing for equal content (a real,
   practically-universal but still cross-type assumption). Shipped design
   uses ONE overload (`std::string_view`), so every key — inserted or
   looked-up — hashes through the identical function; no cross-type
   assumption exists at all. Stronger, not weaker, than what was asked.
2. **`combineAccessesByResource()`**: not one of the 7 named sites; found
   directly in `execute()`'s call graph while implementing this task and
   fixed in-round (CLAUDE.md's "no deferred fixes").
3. **Pointer-identity strengthening**: the ruling's methodology
   (capacity-snapshot alone) is a **documented, binding-approved**
   limitation ("blind to a same-sized reallocation... not a realistic
   false-negative risk in practice"). This task's own revert-probes
   (above) found that limitation is NOT merely theoretical for a
   fixed-element-count-every-call scenario (this ticket's own "unchanged
   graph" steady-state premise) — capacity alone provably cannot
   distinguish genuine reuse from "freshly reconstruct to the identical
   final size" for exactly that shape. Added `.data()` pointer-identity as
   a second, independent signal everywhere a real (non-`vector<bool>`)
   element type allows it, matching `rx_scene::DrawListBuilder`'s own
   "both checked, neither sufficient alone" test precedent. **Disclosed,
   not fully closed**: `std::vector<bool>`'s 3 fields
   (`firstBarrierSeen`/`attachmentEverWritten`/`touchedThisExecute`) have
   no `.data()` at all (absent from the standard, not merely
   inconvenient) — capacity is their only available signal, and per
   Probe 2, is not proven discriminating for that specific fixed-count
   shape. This is flagged, not silently assumed away, exactly as the
   ruling's own text requires. Changing those 3 fields off
   `std::vector<bool>` was judged out of this task's authority (the
   matrix's own acceptance criterion for sites 1-2 names `std::vector<bool>`
   explicitly as the binding shape).

## Self-review

- Read every one of the 4 binding documents in the specified order before
  writing any code; re-verified the GPU-zone trace against the ACTUAL
  vendored Tracy copy this build links (`Tracy_DIR` in CMakeCache), not
  the matrix's own (different-hash) citation — a genuine independent
  re-verification, not a trust-the-matrix pass-through.
- TDD: capacity-snapshot cannot be written against literal pre-fix code
  (fields don't exist yet); satisfied the "fails on current code" intent
  via two targeted, git-stash-isolated revert probes instead, each
  captured and immediately reverted, with an honest accounting of where
  the bound methodology genuinely does and does not discriminate.
- Found and fixed one additional per-pass allocation site
  (`combineAccessesByResource`) not in the original 7 — did not defer it.
- No public API change; every new symbol is `detail::`-scoped and
  documented as a test-only seam, matching this codebase's own
  established carve-out convention exactly (3 precedents cited by name in
  the code comments: `debugChunkStats`, `debugLastFrameFinalStages`,
  `rx::scene::detail::capacitiesForTesting`).
- No AI attribution anywhere in code, comments, or commit messages;
  author left as local git config (Yousef Wadi).
- Did not touch `.superpowers/sdd/.../progress.md` (pre-existing
  modification, not mine) or any board/issue/plan/spec/ledger file.
- Concern carried forward openly (not hidden): the 3 `std::vector<bool>`
  fields' capacity-only signal is not proven discriminating for a
  fixed-element-count-every-call regression shape; recommend this as a
  registry watch-item if a future reviewer wants it fully closed (would
  require either accepting a `std::vector<uint8_t>` type change to those
  3 fields — a call outside this task's own binding acceptance criterion
  — or a different methodology entirely).
