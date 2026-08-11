# Stage 0 Exit-Gate Foundation Audit

Auditor: fresh maximum-capability pass over the whole system at HEAD `a5e7dbf` (39 unpushed
commits, `origin/main..HEAD`). Method: adversarial + empirical — every prior conclusion
re-derived from source; probes run where possible (full suite re-run, forced-lavapipe +
validation-layer-1.4.357 sync-validation runs, windowed present probes on both drivers,
stress-benchmark reproduction, and a ThreadSanitizer build of the real `rx_task` scheduler +
pinned enkiTS v1.12). Nothing in the tree was modified except this report; nothing was
committed. Scratch experiments live outside the repo and the pre-existing `build/` trees only
gained ordinary ctest artifacts.

---

## Executive verdict: FINDINGS REQUIRE CLOSURE

The foundation is broadly sound — materially sounder than a typical stage of this size:
17/17 gates reproduced locally, the executor's three-way first-use synchronization machinery
survives a current-generation (1.4.357) synchronization-validation layer on both lavapipe and
NVIDIA with zero messages, the 2.7x parallel-recording claim reproduces at steady state, the
attribution sweep of all 39 unpushed commits is clean, and `docs/threading.md`'s guard claims
match the code line-for-line.

However, the audit found **one High** finding — a TSAN-reproducible use-after-free race in
`rx::task::Scheduler::runOnIoThread()`'s task-lifetime scheme, reachable under fully legal,
documented usage (the exact D5 handoff pattern Stage 1's asset streaming will hammer) — plus
**three Medium** findings (a latent bindless slot-reuse UB on the texture-release path, an
ABI-boundary exception leak in `release()`, and a two-layer silent-staleness path in the
dep-cache keys). Stage 0 should not close until the High is fixed and the Mediums are fixed
or explicitly ruled.

Finding counts: **Critical 0 · High 1 · Medium 3 · Low 8.**

---

## Findings table

### F1 — HIGH: use-after-free race on `runOnIoThread()`'s IoTask lifetime (TSAN-reproduced)

- **Where:** `src/rx_task/scheduler.cpp:59` (`~IoTask` via the trash reap at `:118`) racing
  `src/rx_task/scheduler.cpp:354` (`AddPinnedTask`) → enkiTS v1.12
  `TaskScheduler.cpp:902-908` (`AddPinnedTaskInt`).
- **Mechanism:** `AddPinnedTaskInt` *publishes* the task to the IO thread's lock-free pinned
  list at line 902 (`WriterWriteFront(pTask_)`) and then — lines 904 and 907 — **reads
  `pTask_->threadNum` again** to decide which semaphore to signal. The moment the task is
  published, the IO thread may dequeue it, run `fn_()`, push it to the trash list, drain the
  list, and `delete` it (scheduler.cpp:117-120) — all before the submitting thread executes
  line 904. The submitter then reads freed memory; a garbage `threadNum` indexes
  `m_pThreadDataStore[]` out of bounds (misdirected semaphore signal at best, segfault at
  worst). The wrapper's own careful comment (scheduler.cpp:33-58) closes the *consumer-side*
  post-`Execute()` dereference exactly right, but missed the *submitter-side* post-publication
  dereference inside enkiTS itself.
- **Reachability:** fully legal use. `scheduler.h:169-183` documents `runOnIoThread()` as
  "safe to call from any thread, including concurrently from several threads at once and from
  within a parallelFor() chunk" — precisely the reproduction shape. It does **not** require
  the destructor race; even single-threaded main-loop submission has the window (submitter
  preempted between publish and line 904).
- **Reproduction:** scratch TSAN build of unmodified `scheduler.cpp` + the pinned enkiTS
  v1.12 source; a loop of `parallelFor` chunks calling `runOnIoThread()` produces **3-4
  distinct TSAN "data race … heap block freed" reports per run, every run (3/3)**, each pairing
  `AddPinnedTaskInt (TaskScheduler.cpp:904)` on the submitter against
  `delete task / ~IoTask (scheduler.cpp:118/59)` on the IO thread. Full traces in the probe
  appendix. The in-tree suite could never see this: `scheduler_test.cpp` has no concurrent
  multi-submitter case and no TSAN job exists; the review-time "200 trials / 125k submissions,
  zero drops" probe measured *drops*, not lifetime.
- **Failure scenario:** rare, timing-dependent memory-unsafety in the engine's single task
  scheduler; frequency scales with `runOnIoThread()` call volume — i.e. it will get *worse*
  exactly when Stage 1 asset streaming starts using the documented pattern in anger.
- **Suggested fix scope (small, `rx_task`-local or vendored one-liner):**
  1. Check upstream enkiTS master — if `AddPinnedTaskInt` has since been fixed to copy
     `threadNum`/`m_Priority` into locals before `WriterWriteFront`, bump/patch the pin
     (repo rule: prefer the ported upstream fix).
  2. Otherwise, two-phase delete in the wrapper: submitter sets an atomic `published` flag on
     the IoTask *after* `AddPinnedTask()` returns; the IO-thread reaper only deletes tasks
     with `published == true`, deferring the rest to the next wake cycle (and the destructor's
     final drain). Deleting only when *both* "executed" and "submitter done" hold removes the
     race completely.

### F2 — MEDIUM: `releaseTexture()` frees the bindless slot immediately → descriptor-rewrite UB window

- **Where:** `src/rx_material/material_system.cpp:1850` (`impl.bindless->release(...)`),
  vs. the deferred texture teardown at `:1856-1859`.
- **Mechanism:** the *texture* is correctly retired through the fence-gated `DeletionQueue`,
  but the *bindless slot* returns to the free list right away. A subsequent
  `createTexture2D()` (same frame or next) can be handed that slot and
  `vkUpdateDescriptorSets` it (`bindless.cpp` registers write immediately) while a still
  in-flight frame's draws dynamically index the **same slot**. `UPDATE_AFTER_BIND` +
  `UPDATE_UNUSED_WHILE_PENDING` only permit updating descriptors **not used** by pending
  command buffers; a slot the previous frame actually sampled is *used*, so this is a Vulkan
  spec violation (silent on some drivers, garbage/fault on others). The comment at
  `:1846-1849` cites bindless.h's release-safety contract, but that contract
  (`bindless.h:140-149`) covers destroying the *resource*, and explicitly warns the slot must
  not be treated as reusable "until … no in-flight frame can still reference that slot".
- **Reachability:** no in-tree caller currently releases + re-registers within the
  frames-in-flight window, so it is **latent** — but it is a public ABI path
  (`IRxTexture::release()` → `TextureImpl` dtor → here), and release-then-create-within-2-frames
  is the canonical texture-streaming/hot-swap pattern Stage 1 introduces.
- **Suggested fix scope (2 lines):** move `impl.bindless->release(record->bindlessHandle)`
  *into* the retired closure at `:1858` (it runs on the main thread from
  `onFrameCompleted()`, so the `RX_ASSERT_MAIN_THREAD` guard inside `BindlessTable::release`
  stays satisfied).

### F3 — MEDIUM: ABI boundary — `release()` can propagate C++ exceptions across the ABI

- **Where:** `src/rx_material/api_impl.cpp:139-145` (`RxUnknownBase::release()` — no
  try/catch); secondary instance at `api_impl.cpp:534` (`std::filesystem::path` constructed
  *outside* `loadMaterial`'s try block, plus `path.string()` inside the catch at `:548`).
- **Mechanism:** `release()` on the last reference deletes the derived object;
  `TextureImpl::~TextureImpl` → `MaterialSystem::releaseTexture` → `DeletionQueue::retire`
  allocates (`std::function` + `vector::push_back`) — a real (if rare) `std::bad_alloc` path
  that unwinds straight across the ABI, violating `docs/abi.md` rule 3 and `api_impl.cpp`'s
  own header claim ("every virtual method reachable from a caller … is exception-safe").
  The `path` constructor case additionally throws `std::system_error` on malformed narrow →
  wide conversion on the **windows-cross-zig target** (where `path::value_type` is `wchar_t`),
  i.e. on untrusted caller input, not only under OOM.
- **Suggested fix scope:** wrap `release()`'s delete in try/catch (log, swallow — the object
  contract is already "refcount hit zero"); move the `path` construction inside the try and
  keep the raw `const char*` for the catch-side log line. One file.

### F4 — MEDIUM: dep-cache keys under-capture their inputs (two-layer silent staleness)

- **Where:** `cmake/DepCache.cmake:11-24` (key = `name|tag|triple|zig-version|CMAKE_ARGS`
  only) and `.github/workflows/ci.yml:135-139` (Actions cache keyed on
  `hashFiles('third_party/CMakeLists.txt')` only).
- **Failure scenario:** `CMAKE_BUILD_TYPE` is *passed into* every dependency build
  (`DepCache.cmake:61`) but is **not in the key**; the zig wrapper scripts'
  (`cmake/zig-wrappers/*`) own hard-coded flags, the toolchain files, and `DepCache.cmake`'s
  logic itself are likewise invisible to both keys. Changing any of these silently reuses
  dependency binaries built under the old configuration — locally via `.deps-cache` markers,
  and in CI via the restored Actions cache whose key never changed either. Latent today (both
  presets are RelWithDebInfo and the wrappers are stable), but it is exactly the class of
  "works until the day it corrupts a build in a way nobody can reproduce" the dep cache was
  supposed to prevent.
- **Suggested fix scope:** fold `CMAKE_BUILD_TYPE`, `hashFiles` of `cmake/DepCache.cmake`,
  `cmake/toolchains/*`, and `cmake/zig-wrappers/*` into both keys (one hash string each).

### F5 — LOW: main-thread-only surface with no guard: `pumpMain()` (plus execute/realize/beginFrame)

- **Where:** `src/rx_task/scheduler.cpp:362` (`pumpMain` — unguarded),
  `rx_graph/executor.cpp` `execute()`/`realize()` (documented main-thread-only in
  `executor.h:29-33`, unguarded), `material_system.cpp:1439/1445`
  (`beginFrame`/`onFrameCompleted` — mutate ParamArena cursors / run retirement destructors,
  unguarded).
- **Failure scenario:** a worker calling `pumpMain()` runs posted GPU-mutation closures on a
  worker thread. Closures that call *guarded* APIs abort loudly (good); closures or methods in
  the documented-but-unguarded set (`beginFrame`'s ParamArena cursor reset,
  `onFrameFenceSignaled`'s destructor drain) corrupt silently — the exact failure mode the
  Task 7 fix round set out to eliminate. This is the one worker-reachable hole left in the
  otherwise-verified D5 guard net (all 13 documented guard sites confirmed present and
  correctly placed).
- **Suggested fix scope:** add `RX_ASSERT_MAIN_THREAD` to `pumpMain()` (highest value),
  `Executor::execute()/realize()`, and `MaterialSystem::beginFrame()/onFrameCompleted()`.
  Mechanical, mirrors fix round 1.

### F6 — LOW: exceptions from chunk ≥ 1 callbacks are process-fatal, undocumented

- **Where:** `executor.cpp:1420-1421` (`pass.invokeExecuteChunked` inside `recordOneChunk`,
  no catch) dispatched via `scheduler.cpp:300-316` (`enki::TaskSet` lambda, no catch).
- **Failure scenario:** `PassContext`'s resolvers *document* throwing `std::out_of_range`
  (executor.h:148-157) and are legal to call from worker chunks; a typo'd resource name in a
  chunked callback unwinds into enkiTS's `TaskingThreadFunction` → `std::terminate`. On chunk
  0 the same typo is a catchable exception. The documented "throws" contract silently changes
  meaning per-thread.
- **Suggested fix scope:** try/catch around the chunk callback in `recordOneChunk` (log pass +
  chunk, end the secondary, drop the chunk — the existing "this chunk recorded nothing" degrade
  path already exists for acquire failure), or one paragraph in pass.h making termination
  explicit.

### F7 — LOW: ABI misc (from rule-by-rule audit)

- `rx_api.h:138-141` (`RxTextureDesc`) and `:266-268` (`RxMaterialSystemDesc`) have `sizeof`
  pins but **no `alignof` static_asserts** — docs/abi.md requires both; no live padding today.
- No GUID-uniqueness test exists across the five `kIID_*` constants (manually verified unique).
- Exception-boundary tests are structural only (`isDocumentedResult`) — genuine fault injection
  would have caught F3. MSVC consumability is design-clean (empty `RX_CALL` is correct on x64,
  no GCC-isms) but never compiled by any toolchain in the repo — a truthfully-scoped claim, not
  a verified one.

### F8 — LOW: driver-dependent branches without forced evidence (card #24 directive)

See the full matrix below. Concretely actionable: the Tracy **NVIDIA single-begin** time-domain
path and the present-ladder **IMMEDIATE** branch are only ever exercised by manual dev-machine
runs (CI structurally cannot reach them — no discrete GPU); the FIFO-double-fallback,
mip-blit-fallback (`texture.cpp:90-113` silently drops to 1 mip), bindless capacity-rejection,
and host-coherence-exit branches have **never executed anywhere**. None is a known bug; all are
unwitnessed code.

### F9 — LOW: test/CI honesty gaps (none invalidate the 17/17)

- `tools/toolchain_check` Wine smoke step (`ci.yml:334-335`) asserts only exit-code 0 — never
  checks the printed `target=` string, so a cross-compile that failed to define `_WIN32` passes.
- `src/rx_platform/tests/window_test.cpp:16-28`: the only CHECK is unreachable-false (guarded
  by `if (empty) return;`) — the test's real value is "doesn't crash".
- Every GPU fixture shares a "no display/Vulkan → early-return pass" skip guard; real today
  (verified: forced-lavapipe run does real work, 614 assertions in rx_graph_gpu alone), but no
  meta-check (e.g. minimum assertion count) prevents a future CI-environment regression from
  turning the whole GPU suite into silent green skips.
- `ci.yml:190` chains `dpkg -l | grep … && xvfb-run ctest` — a grep miss fails the step before
  ctest runs (misattribution, fails loud not silent).
- Sample 06's "chunked migration" records **all draws in chunk 0**; chunks ≥ 1 are intentional
  (and disclosed, main.cpp:964-1011) no-ops. The 05/06 byte-identical A/B therefore proves the
  chunked *infrastructure*, not parallel material drawing — sample 07 is the only gate whose
  draws genuinely span chunks. Worth remembering when quoting the headline.
- Sample 07's ctest gate is *stronger* than its own description ("counters only") — it also
  runs 4 analytic pixel probes and a validation check. Inaccuracy in the safe direction.

### F10 — LOW: executor self-pacing is caller-discipline, unenforced

- **Where:** `executor.cpp:963-981` (chunk-pool reset at `frameCounter % kFramesInFlight`,
  deletion-queue drain at `frameCounter - kStaleAfterExecutes - 1`).
- The whole cross-frame safety argument (pool reset, secondary reuse, deletion pacing) rests on
  "≤ 1 `execute()` per real fence-bounded frame". Every in-tree caller complies (samples via
  `FrameSync` fence-wait-then-record; GPU tests via synchronous submit), but nothing asserts
  it. A future caller running two graphs per frame resets pools the GPU is still reading.
  Suggested: one sentence in `executor.h`'s execute() contract naming the bound explicitly
  (the executor.cpp comment has it; the public header does not), or a debug counter
  cross-check. — Also resolves the audit question "do history parity and chunk-slot parity
  compose": both derive from the same `frameCounter` (`%2` and `%kFramesInFlight==2`), so they
  cannot disagree; phase alignment with FrameSync's own slot index is irrelevant because the
  guarantee consumed is "submission N-2 complete", which any 1:1 pacing provides.

### F11 — LOW: log-sink performance posture

- Uninstalled fast path: thread-local read + one atomic load (x86: plain load) — compliant
  with "fast path as default". Installed path: **one process-wide mutex per log line, held
  across the host callback** (`log_forward_sink.cpp:96-211`) — this is what buys the verified
  uninstall-drain guarantee (a genuinely correct design, stress-tested in `log_test.cpp:216-274`).
  Acceptable now because no hot path logs per-draw (verified: executor/scheduler hot loops log
  only on failure), but an embedding engine that logs from workers at streaming volume will
  serialize on it. Flag for the SDK phase, not this one.

### Ledger watch-items — dispositions

- **kMaxChunksPerPass = 16** (`executor.h:97`): sound. Pool coverage (`workerCount + 1`) is
  independent of the cap, so high-core-count boxes are correct (chunk indices ≤ 16, pool
  indices ≤ workerCount, both bounded); enkiTS's split arithmetic guarantees width-1 partitions
  for `SetSize < numThreads` (verified against `AddTaskSetToPipeInt`,
  `TaskScheduler.cpp:599-607`), so the `begin + 1` chunk-index mapping never drops chunks.
  The cap is a perf ceiling only; revisit when a frame carries multiple heavy chunked passes.
- **Haiku false-verification (Task 8 item 5):** independently confirmed closed. The escalated
  test (`test_param_arena.cpp:179-298`, commit `2b38b7c`) genuinely discriminates: two
  consecutive *failing* `writeAndAllocate()` calls with distinct sentinels, read back through
  `detail::debugFrameBufferData` — only a cursor-advance-on-failure regression can make the
  first sentinel survive. The integrity lesson (independent reproduction required) held.
- **Commit hygiene (8b96e9a sweep):** re-checked; content-only, no attribution issues.
- **Attribution sweep:** `git log --format='%an %ae %s %B' origin/main..HEAD` over all 39
  commits — zero matches for any AI attribution pattern. Clean.

---

## Driver-dependent-branch evidence matrix (card #24)

Empirical basis: `vulkaninfo` on both in-machine devices (NVIDIA RTX 2080 discrete, driver
580.82.07; lavapipe/llvmpipe Mesa 25.1.5), surface-mode queries under Xvfb (CI's display),
forced-ICD runs this audit performed, plus test/CI cross-referencing.

| Branch | Where | Default path | Non-default path | Evidence status |
|---|---|---|---|---|
| Calibrated-timestamps ext present | `device.cpp:259` → `tracy_gpu.cpp:98-105` | Calibrated ctx (both drivers report ext) | Plain `TracyVkContext` | Fallback **NEITHER** — never executed anywhere |
| Tracy time-domain (single- vs multi-begin) | `tracy_gpu.cpp:16-60` | lavapipe multi-begin (CI, every run) | NVIDIA single-begin | **DEFAULT-ONLY in CI**; NVIDIA path manual-dev-machine only (the original hotfix's root-cause class — still true) |
| Present ladder: MAILBOX | `device.cpp:144-170` | lavapipe has MAILBOX | — | Exercised in CI test + **this audit's windowed lavapipe probe (clean under sync validation)** |
| Present ladder: IMMEDIATE | same | — | NVIDIA lacks MAILBOX → IMMEDIATE | Manual-only; CI cannot reach (no discrete GPU) |
| Present ladder: FIFO double-fallback | same | — | neither optional mode present | **NEITHER** (self-disclosed in `device_test.cpp:243-249`); `FIFO_RELAXED` named but never selectable — dead |
| BC/compressed formats | — | — | — | Not implemented yet (Stage 1); plan already earmarks lavapipe BC verification |
| Mip-blit format-feature fallback | `texture.cpp:90-113` | full chain (all used formats) | silent 1-mip degrade | Fallback **NEITHER** |
| Descriptor-indexing features | `device.cpp:216-241` | hard requirement, both drivers satisfy | gap-diagnostic log | Diagnostic **NEITHER** (fine — it's an error path) |
| Bindless capacity vs UAB limits | `bindless.cpp:83-118` | within-limit | clean-nullopt rejection | Rejection **NEITHER** against a real device limit |
| Descriptor-pool exhaustion | `descriptor_arena.cpp:108-149` | arena-enforced ceiling | raw driver failure | **BOTH exercised historically** (the real NVIDIA-vs-lavapipe CI divergence that motivated the arena); residual driver-failure branch NEITHER |
| ReBAR direct vs staging upload | `buffer.cpp:98-103` / `upload.cpp:143-188` | direct (both drivers measure direct today) | staging on non-ReBAR discrete | Staging *code* forced by tests every run; staging-as-measured-hardware-outcome never observed on any tested device |
| debug-utils labels | `executor.cpp:805` | flag-driven, not driver-driven | — | Both paths exercised (validate vs not) |
| Backbuffer channel order BGRA/RGBA | `05_multipass/main.cpp:346-374` | BGRA (only order either driver reports) | RGBA / fail | RGBA branch **NEITHER** |
| Host-coherent assumption | `01/main.cpp:591`, `02/main.cpp:690` | coherent everywhere tested | error-exit | **NEITHER** |

Structural takeaway: CI = lavapipe-only, dev machine = NVIDIA-default; the windows-cross job
runs no GPU tests at all. Any branch whose non-default side needs a discrete GPU has *no
automated* evidence, and four defensive fallbacks have never executed at all. Suggested
closure: a once-per-stage manual "forced-driver sweep" checklist (both ICDs × validate ×
present), which this audit effectively piloted.

---

## Sync-matrix enumeration (first-use srcStage/srcAccess override machinery)

Three first-use paths × two barrier sources × two recording modes, plus the carry-forward
writeback. `FBS` = `firstBarrierSeen[]`.

| # | Cell | Handling | Status / evidence |
|---|---|---|---|
| 1 | Pooled **image** × compile-time barrier × whole-pass | First barrier per physIdx overridden with pool's `lastFrameFinalStages/Access` (`executor.cpp:308-315`) | ✅ GPU test asserts tracked value feeds the next execute (`test_execute_gpu.cpp:1030-1032`); clean under VVL 1.4.357 sync validation, lavapipe + NVIDIA |
| 2 | Pooled image × compile-time × chunked | Identical — barriers recorded on the primary before `vkCmdBeginRendering(SECONDARY_…)` (`executor.cpp:1050,1471-1480`); recording mode cannot affect barrier placement | ✅ samples 05/06/07 + chunked GPU tests clean under sync validation |
| 3 | Pooled image × synthesized | **N/A by construction** — an image's first access always layout-differs, so compile always emits a barrier to override (`barriers.cpp:81`) | ✅ dead cell, correctly not handled |
| 4 | Pooled **buffer** × compile-time | First-use compile barrier never exists (`barriers.cpp:94`: `needBarrier` false on first buffer write). Later compile barriers are protected from a wrong override because the synthesized barrier at `firstUsePass` sets `FBS` first (`executor.cpp:425,452`) — the ordering that makes cell 5 and this one compose | ✅ verified by inspection + sync validation clean |
| 5 | Pooled buffer × synthesized × whole | `synthesizeFirstUseBufferBarrierIfNeeded` (`executor.cpp:419-454`), src from pool tracking, dst from the pass's combined declared access | ✅ dedicated cross-execute GPU regression test |
| 6 | Pooled buffer × synthesized × chunked | Same primary-side call at `executor.cpp:1051`, before fan-out | ✅ chunked bare/compute-pass GPU test (commit `8f8e173`); clean under sync validation |
| 7 | **History** × either source × whole | Compile barriers skipped entirely (`executor.cpp:304`); per-slot persistent `ResourceBarrierState` drives real barriers from the unmerged access list (`executor.cpp:480-531`); init-clear seeds the state machine honestly (`:589-656`) | ✅ history GPU tests; clean under sync validation |
| 8 | History × chunked | Same primary-side path; write-slot attachment view resolved before fan-out (`resolveAttachmentView`), read-slot resolvers safe from worker chunks (read-only during the blocking fan-out) | ⚠️ **handled by construction, no dedicated test** — no in-tree pass combines history with `setExecuteChunked()` (samples don't use history at all yet). Cheap gap to close when Stage 2's first history consumer lands |
| 9 | **Backbuffer** × compile-time × whole+chunked | UNDEFINED-first-transition srcStage forced to `COLOR_ATTACHMENT_OUTPUT` to chain the acquire semaphore (`executor.cpp:316-331`); `finalBarriers` → PRESENT_SRC from the tracked last write (`barriers.cpp:242-251`) | ✅ probed this audit: windowed `--present --validate` under VVL 1.4.357 on **NVIDIA/FIFO** and **lavapipe/MAILBOX** — zero messages over sustained frames |
| 10 | Backbuffer × synthesized | N/A — backbuffer is never a buffer | ✅ dead cell |
| 11 | Carry-forward writeback (all cells' input) | Per-execute **union** across all touching passes, write-masked access (`executor.cpp:1232-1238,1248-1260`); history/backbuffer correctly excluded | ✅ the fix-round-1 Critical's union semantics re-verified from source; regression test observes it via `debugLastFrameFinalStages` |
| 12 | Chunk-pool reset vs all of the above | Reset at `frameCounter % 2` before any pass (`executor.cpp:963-964`) | ✅ correct under the 1-execute-per-fenced-frame discipline; see F10 for the unenforced assumption |

Also probed for the D5 contract: the chunk-0 guarantee is real (chunk 0 runs to completion on
the calling thread *before* the `parallelFor` for chunks 1..N-1 is even submitted —
`executor.cpp:1442-1455` — so no scheduler behavior can violate it); nested `parallelFor`
inside a chunk is safe including the thread-local command-buffer scope's save/restore
(`executor.cpp:769-780`); a chunked callback calling `pumpMain()` is the guard hole recorded
as F5. The theoretical "IO thread steals a taskset partition before adopting its pinned loop"
window was checked against enkiTS source and is closed by construction (`TryRunTask` runs
pinned tasks before taskset pipes at every priority, and `IoLoopTask` is queued before any
taskset can exist).

---

## Probe evidence appendix

All probes on this machine (8 hw threads; NVIDIA RTX 2080 + lavapipe Mesa 25.1.5), HEAD
`a5e7dbf`, existing `build/linux-native` (RelWithDebInfo, RX_DEBUG_CHECKS=ON, RX_TRACY=ON).

1. **Full suite, default (NVIDIA) device:** `ctest -j4` → **17/17 passed** (19.7 s).
2. **Sync validation, forced lavapipe, layer 1.4.357** (`VK_ICD_FILENAMES=lvp_icd.json`,
   `VK_LAYER_PATH=/home/ywadi/sponza/vvl`; loader-debug run confirmed the 1.4.357 layer is the
   one inserted):
   - `sample_05_multipass --validate` → gate PASSED, 0 validation messages;
   - `sample_06_materials --validate` → PASSED, 0; `sample_07_stress --validate --draws 64` →
     PASSED, 0;
   - `rx_graph_gpu_tests` (validation always on in its fixture) → 614/614 assertions, 0
     messages.
3. **Windowed present under sync validation:** `07_stress --present --validate` ~12 s on
   NVIDIA (FIFO) and forced-lavapipe (`--vsync off` → MAILBOX) → 0 validation messages each —
   the backbuffer acquire-chaining and both reachable present-mode branches, under a current
   layer.
4. **Performance claim:** headless 3-frame gate: 9.14 ms @ `--threads 1` (frame 2 — matches
   the claimed baseline to 3 decimals) vs 4.86 ms @ `--threads 7` (~1.9x, cold).
   Steady-state `--present --vsync off --draws 30000`: **9.35-9.54 ms single-thread vs
   3.46-4.21 ms @ 7 workers → 2.3-2.7x**, fps 84.7 → ~175. The 2.7x headline reproduces at
   the favorable end of the band; methodology (direct cpu_record_ms measurement) is honest.
5. **TSAN probe (F1):** scratch build (`-fsanitize=thread -O1 -g`) of unmodified
   `src/rx_task/scheduler.cpp` + pinned enkiTS v1.12 (`git describe` → `v1.12`) + shim
   `rx_core/log.h|profile.h` no-op headers (include-path shadowing only — repo untouched).
   - Harness A (executor-shaped fan-out: per-(slot,thread) arenas, chunk-0-then-parallelFor,
     postToMain/pumpMain, nested parallelFor, io traffic, 30 schedulers × 40 frames): **1 race
     report** — `AddPinnedTaskInt(TaskScheduler.cpp:904)` vs `~IoTask(scheduler.cpp:59)` via
     the trash reap (`scheduler.cpp:118`). Everything else — the entire chunked-recording
     sharing pattern — **clean**.
   - Harness B (minimal legal repro: `runOnIoThread` from parallelFor chunks, scheduler alive
     throughout): **3-4 reports per run, 3/3 runs**, identical stacks. Logs in scratch
     (`tsan/repro_*.log`).
   - Limitation, stated: no TSAN over the *GPU* tests (llvmpipe's internal threading would
     require an uninstrumented-driver suppression set that would also mask real engine races;
     the CPU-side probe covers every engine-owned synchronization edge instead).
6. **Attribution sweep:** full-body grep over `origin/main..HEAD` (39 commits) for
   claude/anthropic/co-authored/generated/🤖 → zero matches.
7. **Subagent evidence** (each independently spot-verified before adoption): ABI rule-by-rule
   audit (F3, F7 — `release()` body and `loadMaterial` path construction re-read directly);
   driver-branch sweep incl. Xvfb surface-mode queries on both ICDs (F8 matrix); test/CI audit
   incl. windows-cross `ctest -N` filter verification, 7/17 confirmed (F4, F9).

---

## Style appendix (low-priority, non-blocking)

- `docs/threading.md:49-52`: "flush() … always called from within an already-guarded
  uploadTo*() call" — `material_system.cpp:1813` calls `flush()` directly (still main-thread
  in every path; wording only).
- `executor.cpp` `lookupResolvedIndex` builds a `std::string` per resolver call — per-draw
  resolver use in future chunked passes would allocate per draw; consider `string_view` keys.
- Comment-to-code ratio in `executor.cpp`/`transient_pool.h` is extraordinary; consider
  extracting the frame-pacing contract into `executor.h` where consumers will actually see it
  (overlaps F10).
- `ci.yml:195-198` step name says "counters only" while the gate also pixel-probes (see F9) —
  update the comment when next touching CI.

---

# Closure confirmation (gate owner, main @ a744d23)

Verification depth: F1 re-verified by this auditor's own original TSAN reproduction recipe
(the mandated closure bar); F2/F3/F4/F5p/F6/F10 spot-verified against the diffs *and* the
live tree at `a744d23`; full suite re-run; attribution sweep extended over
`a5e7dbf..a744d23` (4 commits — zero matches).

| Finding | Disposition | Verdict | Evidence |
|---|---|---|---|
| **F1 (High)** | Fixed, `d444064` | **CONFIRMED CLOSED** | Two-phase delete verified in source: `markPublished()` (release-store) called on the submitter strictly after `AddPinnedTask()` returns (`scheduler.cpp:483`); reaper and both destructor drains gate every `delete` on `isPublished()` (acquire) — the acquire/release pairing gives a genuine synchronizes-with edge, not a timing heuristic. **Mandatory re-run of this audit's own probes: minimal legal-usage repro 10/10 runs = 0 TSAN warnings (pre-fix: 3-4/run, 3/3 runs); full executor-shaped harness = 0 (pre-fix: 1).** Rebuilt bit-identically to the original recipe (same stub headers, same pinned enkiTS v1.12 source, plus the now-required real `debug_checks.h|.cpp` with `RX_DEBUG_CHECKS` on). New in-tree concurrent multi-submitter test (`scheduler_test.cpp:458`) exercises the exact repro shape (per-item `runOnIoThread` from parallelFor chunks) — honest about what non-TSAN CI can and cannot observe. Disclosed residual (destructor-drain window leaks-not-races, loudly logged with its own tally kept separate from the documented "dropped" contract) is the correct trade: deleting an unpublished task there could reopen the UAF for the caller-races-destructor misuse case. Double-delete impossible (`outstandingIo` removal precedes `trash_` insertion, so the two drains are disjoint). Upstream-first rule satisfied: master checked, byte-identical, nothing to port. |
| **F2 (Medium)** | Fixed, `45002dc` | **CONFIRMED CLOSED** | `bindless->release()` now runs inside the same fence-gated retired closure as the texture teardown (`material_system.cpp`, `releaseTexture`), tagged `currentFrameNumber` — slot returns only after the releasing frame's fence, covering both the in-flight prior frames and same-frame-earlier-draws cases from the finding. Guard preserved (closure runs on main from `onFrameCompleted`). Regression test (`test_material_system.cpp:1272`) is genuinely discriminating: capacity-4 table filled, release, then `createTexture2D` must *throw* twice pre-drain and an early `onFrameCompleted(41)` must not free it — pre-fix this deterministically reuses the LIFO-freed exact index and the test fails; post-drain it asserts the *exact* released index returns. **Extension assessed on merits and ACCEPTED:** the prerequisite `~MaterialSystem` fix (explicit texture drain before `impl.allocator` teardown, `material_system.cpp:1114-1160`) closes a real pre-existing member-declaration-order UAF (`textures` declared before `allocator` ⇒ allocator destroyed first under reverse-order destruction while live `Texture2D`s still need it) — correctly scoped, contract-compliant (post-`vkDeviceWaitIdle` immediate release is exactly what bindless.h's contract permits), and precisely the kind of latent bug the new test existed to flush out. |
| **F3 (Medium)** | Fixed, `45002dc` | **CONFIRMED CLOSED** | `RxUnknownBase::release()` wraps the delete in catch-all (`api_impl.cpp:140-159`); leak-not-double-delete on a throwing destructor is the right posture. `loadMaterial`'s `path` construction moved inside the `try` (`api_impl.cpp:574`) and the catch logs the raw `const char*`, not `path.string()`. Honesty of the half-inspection-only test: acceptable — the `std::system_error` half is structurally unreachable on Linux (`path::value_type == char`), the test comment discloses exactly that, and the code change (construction inside try) is target-independent; nothing on any target can now reach the boundary uncaught through this path. |
| **F4 (Medium)** | Fixed, `9b027c2` | **CONFIRMED CLOSED** | Local key now hashes `CMAKE_BUILD_TYPE`, `DepCache.cmake` itself, the active toolchain file, and all sorted `zig-wrappers/*` into the SHA256 (`DepCache.cmake:14-54`); CI keys mirror the same file set per preset (`ci.yml`). Empirically: fresh `cmake --preset linux-native` at `a744d23` resolves the new-format keys cleanly (6/6 HITs, no configure errors; both presets set `toolchainFile`, so the `file(SHA256 ${CMAKE_TOOLCHAIN_FILE})` input is always defined). The hit→touch→miss→revert→hit cycle in audit-closure-c-report.md is consistent with the mechanism (file hash is a direct key input — invalidation is arithmetic, not policy). Residual noted, non-blocking: files a toolchain file itself `include()`s would still be invisible; none exist today. |
| **F5-partial (Low)** | Fixed, `d444064` | **CONFIRMED CLOSED** | `RX_ASSERT_MAIN_THREAD("Scheduler::pumpMain")` at `scheduler.cpp:493` — the highest-value guard from the finding — plus an `RX_DEBUG_CHECKS`-gated guard-fires test (`scheduler_test.cpp:532`). |
| **F6 (Low)** | Documented, `d444064` | **CONFIRMED CLOSED** | pass.h (:229-235) and docs/threading.md (:124-132) now state that an exception escaping a chunk ≥ 1 callback is process-fatal (`std::terminate`) while chunk 0's remains catchable — the finding's "or document" arm, satisfied at both the point of use and the contract doc. |
| **F10 (Low)** | Documented, `d444064` | **CONFIRMED CLOSED** | `executor.h:374-385` names the at-most-one-`execute()`-per-fence-bounded-frame bound in the public contract, including the concrete failure mode if violated — exactly what the finding required. |
| **F5-remainder (Low)** | RULED: Stage 1 acceptance | **RULING SOUND** | Deferring `execute()/realize()/beginFrame()/onFrameCompleted()` guards is proportionate for a Low once `pumpMain()` (the one *dispatch* hole that funnels arbitrary closures) is guarded; recorded with rationale in the ledger. |
| **F8 (Low)** | RULED: matrix stands as documentation | **RULING SOUND** | The inspection-only branches are honestly recorded; a forced-driver sweep remains this auditor's recommendation for each future stage gate, but is not a Stage 0 exit condition. |
| **F9 (Low)** | RULED: Stage 1 test tasks | **RULING SOUND** | None of the items invalidates the 17/17; carrying them as named Stage 1 tasks is the right vehicle. |
| **F11 (Low)** | RULED: accepted-as-documented | **RULING SOUND** | Installed-path serialization is the documented cost of the drain guarantee; revisit at SDK phase as flagged. |

Cross-cutting re-verification at `a744d23`: full suite **17/17 green** (includes the three new
audit-closure tests); attribution sweep over all four closure commits clean; no tree
modifications beyond the closure commits and ledger/report updates.

**GATE VERDICT: STAGE 0 CLEARED.** All Critical/High/Medium findings verified closed against
this audit's own reproduction bars; all Low findings verified closed or covered by sound,
recorded rulings.
