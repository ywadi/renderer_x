### Task 23: Executor per-frame allocation elimination (card #29)

**Files:** Modify `src/rx_graph/executor.cpp` (+`Impl` in its header if
split); tests in the rx_graph targets.
**Scope (2026-08-18 claim-validation finding — 7 verified per-frame
heap-allocation sites in `execute()`; small, local, no public API
change):**
1. `execute()` performs **zero heap allocations in steady state**
   (unchanged graph, unchanged resource count) — asserted by a counting
   allocator hook or capacity-snapshot check across N frames.
2. The four per-execute tracking containers (`firstBarrierSeen`,
   `attachmentEverWritten`, `finalStageThisExecute`,
   `finalAccessThisExecute`) become `Impl`-persistent, cleared per
   frame; the two `unordered_map`s become index-addressed vectors
   (physical indices are dense).
3. Per-pass scratch (`colorPhysIdx`, `colorAttachments`, both barrier
   vectors, chunk command-buffer vectors) becomes reusable `Impl`
   scratch buffers.
4. Debug-label path stops constructing a `std::string` per pass
   (reusable null-terminated buffer); `nameToIndex` gains heterogeneous
   `string_view` lookup so per-pass resolver calls stop allocating.
**Constraints:** byte-identical rendering (existing GPU suite +
sample 05/06 pixel gates prove it); sample 07 numbers unchanged or
better; zero validation errors. `compile()`/`realize()` are exempt
(setup/resize-only paths, documented as such in the code).
**Steps:** allocation-count test first (fails on current code) →
implement → suite green both presets → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue29-executor-allocations.md` as amended by
`gate/rulings-2026-08-18.md` §#29. Key deltas: methodology =
CAPACITY-SNAPSHOT via test-only accessors (per the in-repo
`debugChunkStats()` seam convention) — NO global operator-new
interposition (volk/validation-layer/Tracy-rpmalloc linkage risk);
test runs the representative Tracy-ON config, with Tracy's own
dynamic-zone-name allocations (verified: `tracy_malloc` per
RX_ZONE_DYNAMIC_NAME call at executor.cpp:1024/:1359) documented as
third-party-attributed exceptions (capacity snapshots are naturally
blind to them); the GPU-zone `VkCtxScope` allocation trace is
completed in-task; SCOPE GROWS: `DeletionQueue::onFrameFenceSignaled`'s
non-empty-path fresh-vector allocation (deletion_queue.cpp:40-41) is
folded in (in-place compaction; the pending-item test variant is
mandatory); `acquireChunkCommandBuffer` (executor.cpp:704-745) is the
named in-repo amortization template; `nameToIndex` gains a transparent
hash functor (documented libstdc++/libc++/MSVC string/string_view
hash-equivalence assumption) with a hit+miss overload-selection test.

