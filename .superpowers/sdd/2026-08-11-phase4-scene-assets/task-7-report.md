# Task 7 report: Parallel command recording as the engine default + sample 07_stress

## Commits (main, local only, not pushed)

1. `8afeb1a` — feat(rx_graph): parallel command recording as the engine default
2. `f8d5220` — feat(samples): migrate 05_multipass and 06_materials forward passes to setExecuteChunked
3. `1d146c8` — feat(samples): add 07_stress, the parallel-recording exit sample
4. `01558ab` — ci(packaging): wire sample 07's counter gate, wall-clock artifact, and packaging
5. `8f8e173` — test(rx_graph): cover the chunked bare/compute-pass path end to end
6. `a03e0be` — docs(threading): finalize the Task 7 chunk-recording contract in docs/threading.md

No AI attribution in any commit message (verified: `git log --format=%B` on all
six, grepped for "Claude"/"Co-Authored"/"generated" — none found). Author/committer
identity is the local git config's own (`Yousef Wadi <ywadi85@gmail.com>`),
untouched.

## Scope

- `src/rx_graph/include/rx_graph/pass.h`, `executor.h`, `executor.cpp`,
  `render_graph.cpp`, `CMakeLists.txt` — the core `setExecuteChunked()`
  API and its executor implementation.
- `src/rx_graph/tests/test_compile.cpp`, `test_execute_gpu.cpp` — new
  TDD/regression coverage.
- `src/rx_rhi_vk/src/context.cpp` — one new validation-layer false-positive
  guard (discovered running the chunked path under `--validate`).
- `src/rx_material/tests/test_api_factory.cpp`, `test_material_system.cpp` —
  mechanical propagation of `Executor::create()`'s new required
  `Scheduler&` parameter.
- `samples/05_multipass/main.cpp`, `samples/06_materials/main.cpp` —
  forward-pass migration to `setExecuteChunked()`.
- `samples/07_stress/` (new), `shaders/stress/*.slang` (new) — the exit
  sample.
- `tools/package_samples.sh`, `.github/workflows/ci.yml` — packaging +
  CI wiring for sample 07 (counter gate + wall-clock artifact per D18).
- `docs/threading.md` — finalized the Task 7 forward-reference into a real
  contract.

## The design, as implemented

### `Pass::setExecuteChunked()` (pass.h)

```cpp
Pass& setExecuteChunked(std::function<void(PassContext&, uint32_t chunkIndex, uint32_t chunkCount)> fn);
```

Mutually exclusive with `setExecute()` — calling either when the other is
already set throws `std::logic_error` naming the pass (no silent
"last-call-wins"). No opt-in flag, no caller-chosen chunk count, exactly
per D4's amendment: providing a chunked callback **is** the parallel path.

`PassContext::chunkCommandBuffer()` resolves the current chunk's real
secondary `VkCommandBuffer` (thread_local-backed in executor.cpp); throws
`std::out_of_range` (the established resolver error type/style) if called
from a whole-pass callback. `ctx.cmd` stays at its default `VK_NULL_HANDLE`
for a chunked pass's invocation — never assigned in that path — so
accidental misuse (recording onto the wrong buffer) fails loudly rather
than silently.

### Executor mechanics (executor.cpp)

- **`Executor::create(Device&, rx::task::Scheduler&)`** — the scheduler is
  now a required construction parameter; there is no null/disabled
  parallelism path. Every existing call site (rx_material's two GPU test
  binaries, rx_graph's own GPU test fixture, samples 05/06) was updated —
  mechanical propagation, not a design choice.
- **Per-(worker-thread-index × frame-in-flight-slot) command pools**,
  sized `workerCount() + 1` per slot at construction (the `+1` covers the
  participating main thread's own index 0 — `enkiTS`'s own convention,
  verified directly: "Thread 0 is main thread" — alongside every
  background worker's index 1..`workerCount()`). Each pool is created
  lazily (`VK_NULL_HANDLE` until first actually needed); its own arena of
  secondary command buffers grows on demand and is otherwise fully reused
  frame-to-frame via one whole-pool `vkResetCommandPool()` per frame-slot
  reuse (never per-buffer reset — matches `rx::rhi::FrameSync`'s own
  primary-pool convention).
- **Chunk count** (`rx::graph::detail::chunkCountForWorkerCount()`,
  `executor.h`): `min(scheduler.workerCount(), kMaxChunksPerPass=16)` — a
  pure function of the scheduler's own worker count, deliberately
  independent of any pass's own workload size (the API carries no such
  hint at all, by design). This is what makes `--threads 1`
  (`workerCount()==1`) collapse the forward pass to exactly **one** chunk
  — genuinely serial recording, the sample 07 A/B baseline — while the
  default worker count gets real fan-out.
- **Graphics passes**: every chunk's secondary begins with
  `VkCommandBufferInheritanceRenderingInfo` matching the pass's own
  attachment formats/sample count (derived from the same
  `PassSignature` the primary's own `vkCmdBeginRendering` call uses);
  `viewMask` stays 0 (no multiview anywhere in this engine, so primary and
  every secondary trivially agree). After every chunk finishes recording,
  the primary opens `vkCmdBeginRendering(...
  VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT...)`, replays every
  chunk's secondary via one `vkCmdExecuteCommands()` call in chunk-index
  order, then `vkCmdEndRendering()`. Recording every chunk *before*
  touching the primary at all (rather than interleaved with it) is
  equivalent, not merely close: secondaries are independent objects, so
  the wall-clock order they're *filled in* has no bearing on GPU-visible
  execution order, which the primary's own recorded command sequence alone
  governs.
- **Bare/compute-class passes**: no rendering scope at all, plain
  `VkCommandBufferInheritanceInfo` (no `...RenderingInfo` chained in) — a
  real, tested code path (see "New tests" below), not merely one that
  compiles.
- **Chunk 0 is guaranteed to run synchronously, on the calling thread**,
  before any other chunk begins — the one design element beyond the
  brief's literal text, discovered necessary rather than chosen for
  convenience. See "A real design extension beyond the brief" below.

### Tracy (spec D3)

`RX_ZONE_NAMED("graph_pass_chunk_fanout")` spans the whole fan-out (chunk 0
plus the blocking `parallelFor()` for the rest); `RX_ZONE_NAMED(
"graph_pass_chunk_record")` spans each individual chunk's own recording,
on whichever thread runs it. `RX_GPU_ZONE_DYNAMIC` (unchanged) still spans
the whole pass on the primary.

## A real design extension beyond the brief

The brief's binding design says chunks "fan out via `rx_task` `parallelFor`"
with no special-casing of any one chunk. Implementing samples/06_materials'
own migration surfaced a hard constraint that design text didn't
anticipate: `rx::material::MaterialSystem::bindInstance()` — which a
per-object material draw must call to resolve its pipeline and stream its
per-frame parameter UBO — is main-thread-only, **no exception**
(`docs/threading.md`'s own "every other method on this type" wording), and
rx_material exposes no split "resolve on main, record the bind commands
anywhere" API for this task's scope to build on. A generic `parallelFor()`
chunk has **no thread-affinity guarantee at all** (`Scheduler::parallelFor()`'s
own doc comment: the calling thread merely "participates", racing every
background worker for *every* chunk, including chunk 0) — so without some
additional guarantee, a pass with this exact shape of unavoidably-sequential,
main-thread-only per-frame setup would have had no safe way to use
`setExecuteChunked()` at all.

The fix: **chunk 0 of every chunked pass now always runs synchronously, on
the thread that called `Executor::execute()`** (contractually this
Executor's Scheduler's own main thread), to completion, before chunks
`[1, chunkCount)` are ever hand off to `parallelFor()`. This is documented
in three places kept consistent with each other: `pass.h`'s
`setExecuteChunked()` doc comment (the primary contract), `executor.cpp`'s
`recordChunkedPass()` (the implementation + full rationale), and
`docs/threading.md`'s "Worker-allowed" section (the project-wide threading
contract every other header points back to).

Consequences, all verified:
- `samples/06_materials`' forward pass keeps **all** of its real,
  necessarily-sequential work in chunk 0; chunks `1..chunkCount-1` are
  legitimate, intentional no-ops (still get a real, if empty, secondary
  recorded and stitched in — the parallel-recording *infrastructure* is
  still fully exercised on every commit).
- `samples/05_multipass`'s forward pass has no such constraint (its
  per-object work never touches `MaterialSystem`/`BindlessTable` mutation
  except once, for the shadow-map view, which now runs in chunk 0 too) and
  fans real per-object draw work out across every chunk.
- `samples/07_stress`'s forward pass has no main-thread-only dependency at
  all, so every chunk (including 0) does real, parallel draw-recording
  work — this is the sample the report's headline numbers come from.
- Cost: for `chunkCount > 1`, one chunk's worth of otherwise-parallel work
  runs serially first — negligible next to real per-frame draw volumes,
  and for `chunkCount == 1` (the `--threads 1` baseline) this **is** the
  fully-serial path, with zero `parallelFor()`/enkiTS task-submission
  overhead.

This is flagged here explicitly because it is a genuine addition to the
binding design surface (`docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md`'s
D4 amendment text does not mention it) — worth a coordinator look for
whether the spec doc itself should be amended to record it, since it will
matter to any future chunked pass with a similar main-thread-only
dependency.

## Samples 05/06 migration — byte-identical proof

Both samples' headless pixel gates were verified byte-identical pre/post
migration using a **direct A/B binary comparison**, not merely "should be
equivalent" reasoning: for each sample, the migrated `main.cpp` was copied
aside, the pre-migration whole-pass draw function was reconstructed
verbatim (from the exact text read before editing) in a scratch copy,
built, and run — capturing the exact probe-pixel readback log lines — then
the migrated version was restored, rebuilt, and run again for the same
capture.

**samples/05_multipass** (`recordLitDraws` → `recordLitDrawsChunked`,
ceil-division slicing over `kObjectCount==3`, shadow-map bindless
(re-)registration moved into chunk 0):

| | shadow probe | lit probe | cube probe |
|---|---|---|---|
| before (whole-pass) | `(60,58,58,255)` sum=176 | `(153,149,149,255)` sum=451 | `(103,113,164,255)` |
| after (chunked) | `(60,58,58,255)` sum=176 | `(153,149,149,255)` sum=451 | `(103,113,164,255)` |

Identical, channel for channel.

**samples/06_materials** (`recordDraws` → `recordDrawsChunked`, all real
work kept in chunk 0 per the constraint above):

| object | before | after |
|---|---|---|
| 0 (checker) | `(108,196,255,255)` matched=true | `(108,196,255,255)` matched=true |
| 1 (checker) | `(243,237,108,255)` matched=true | `(243,237,108,255)` matched=true |
| 2 (rim) | `(195,122,203,255)` matched=true | `(195,122,203,255)` matched=true |
| 3 (rim) | `(115,195,203,255)` matched=true | `(115,195,203,255)` matched=true |

Identical.

## samples/07_stress

Procedural instanced field: `--draws N` (default 30000) separate objects
across **4 mesh/pipeline-state variants** (cube/sphere × cull-back/cull-none
— a real fixed-function state axis, so `vkCmdBindPipeline` state changes
are non-trivial, not decorative), each its **own** `vkCmdDrawIndexed` call
(deliberately never real GPU-side instancing — a single `instanceCount`
draw would leave almost nothing for parallel recording to speed up, which
would defeat this sample's entire purpose: measuring CPU-side per-draw
recording cost at scale). Per-instance transform (world position + uniform
scale, no rotation — same convention samples/05_multipass already
established, for the same reason) and color live in one bindless storage
buffer, uploaded **once** at scene setup and never re-touched per frame —
deliberate, so recomputing/re-uploading instance data never conflates with
the forward pass's own recording cost, the one thing this sample measures.
`--threads N` overrides this sample's own `Scheduler`'s worker count — the
A/B measurement instrument.

Forward (chunked) + tonemap (whole-pass, byte-for-byte the same Reinhard
shape as samples/05_multipass's own tonemap pass) through the graph.

**Headless gate** (ctest, `--draws 16` — a 4×4 grid; the small drawCount
does NOT reduce coverage, since chunk count derives only from worker
count, never drawCount — verified directly, chunk count is the same `7`
whether `--draws` is `16` or `30000` on this dev machine): 3 frames, then
asserts **exact counters** (draws submitted == `--draws`, chunk count ==
`chunkCountForWorkerCount(scheduler->workerCount())`, pool allocations
within the documented budget `(workerCount()+1) * kFramesInFlight`) plus
**four analytic pixel probes**, one per variant, at each probed instance's
own known top-point world position projected through a fixed top-down
orthographic camera. Probes are **dominance-style** (a channel is clearly
higher than the others by a margin), not exact-value — this survives both
the forward pass's own lighting scale and the tonemap pass's Reinhard
curve without the test needing to reproduce either formula bit-for-bit
(both are monotonic per-channel operations, so channel dominance ordering
is preserved through both). Channel *position* (which byte is R/G/B) is
resolved from the real backbuffer format, not guessed — "channel-order-exact"
per the brief.

**One real bug found and fixed while building this sample**: GLM stores
matrices column-major; Slang's `float4x4` + `mul(M, v)` convention expects
row-major buffer-sourced data. This is not a novel discovery —
samples/05_multipass's own `updateFrame()` already transposes before
uploading to its storage buffer — but this sample's `viewProj` push
constant was the first time this codebase needed the same transpose for a
*push constant* rather than a storage-buffer row. Symptom before the fix:
a rendered image with clear perspective-style foreshortening under what
should have been a pure orthographic top-down view (spheres projected as
ellipses instead of circles — the geometric tell). Fixed by transposing
once per chunk (not once per draw, since `viewProj` is identical across
all of a chunk's draws — the fix costs nothing inside the loop this sample
exists to measure).

## New tests (TDD where the scale of the task allowed it up front;
regression coverage added for gaps found during implementation otherwise)

- `test_compile.cpp`: `setExecute()`/`setExecuteChunked()` mutual
  exclusion, device-free.
- `test_execute_gpu.cpp`, four new GPU `TEST_CASE`s:
  1. A 4-cell chunked graphics pass, proving every chunk draws its own
     cell exactly once, chunk-count/pool-allocation counters match the
     documented derivation, and — the strongest form of the
     byte-identical claim — a full-image `memcmp()` between the chunked
     recording and an equivalent whole-pass recording of the identical
     draw sequence.
  2. `PassContext::chunkCommandBuffer()` throws from a whole-pass
     callback; `ctx.cmd` stays `VK_NULL_HANDLE` inside a chunked one.
  3. A bare/compute-class chunked pass (no attachment output at all) —
     the brief's own "the API must not arbitrarily restrict compute"
     requirement had no direct coverage until this test: each chunk
     `vkCmdFillBuffer()`s its own disjoint slice of a storage buffer with
     a chunk-index-derived pattern (real GPU writes), read back and
     checked byte-exact per chunk.
  4. (Existing, unmodified) coverage that the new `Executor::create()`
     signature and every call site still compile/link/pass.

146 → 614 total assertions across `rx_graph_gpu_tests` (614 with the
compute-chunk test added; 146 before it, 39 of which are new from this
task).

## Verification

- **Full suite, linux-native, default validation layer**: `xvfb-run -a
  ctest --preset linux-native` — **17/17 pass** (16 pre-existing + the new
  `sample_07_stress_headless`).
- **Full suite, linux-native, forced lavapipe + the newer
  `VK_LAYER_KHRONOS_validation` build** (`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
  VK_LAYER_PATH=/home/ywadi/sponza/vvl`): **17/17 pass**, including sync
  validation — this is the layer combination that actually exercises
  `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` cleanly (the
  apt-packaged 1.3.204.1 layer has the known separate-sampler
  misclassification false positives context.cpp already guards).
- `rx_graph_gpu_tests` directly (not just via ctest), both layer
  configurations: **8/8 test cases, 614/614 assertions**, zero real
  validation errors.
- **A fourth `context.cpp` validation-layer guard** added: the same
  separate-sampler-misclassification root cause the third guard already
  suppresses (documented there in full, with its own newer-layer
  verification) also surfaces one level up — as a `vkCmdExecuteCommands:
  Hazard READ_AFTER_WRITE` message, not `vkCmdDraw:` — for a chunked
  pass's secondary command buffer specifically (samples/05_multipass's
  shadow-map sample, now recorded inside a secondary). Verified the SAME
  way the third guard was: reproduces under the apt-packaged layer,
  reports zero hazards under the newer build.
- **windows-cross-zig**: `cmake --build --preset windows-cross-zig`
  builds everything, **including `sample_07_stress.exe`**, clean (only the
  pre-existing, unrelated `_WIN32_WINNT` redefinition warning every file
  already has). `wine .../toolchain_check.exe` passes.
  `xvfb-run -a ctest --preset windows-cross-zig -E
  'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample'` — **7/7 pass** under
  Wine (GPU/sample tests excluded, same as every prior task — verified the
  regex's `sample` substring already covers `sample_07_stress_headless`
  with no change needed).
- **Build budget**: `tools/check_build_budget.sh` — linux-native 5s,
  windows-cross-zig 6s, both well within the 60s budget.
- **Packaging**: `tools/package_samples.sh` run end to end for both
  presets — `07_stress/` lands with its binary, four on-disk shader
  sources, Slang runtime libs, and LICENSE, matching the established
  per-sample layout.
- **CI YAML**: validated with `python3 -c "import yaml; ...load..."` (parses
  clean) and `actionlint` (zero findings).

## Numbers (dev machine, Intel Core i7-9700F @ 3.00GHz, 8 cores/8 threads,
hardware_concurrency()==8 → default Scheduler worker count == 7)

**Measurement**: `samples/07_stress --present --vsync off`, 30000 draws,
`cpu_record_ms` timed directly around `executor->execute()` (excluding
GPU submit/present), printed once per second, steady state (first 2-3
samples after startup discarded as warmup):

| | single-thread (`--threads 1`) | default worker count (7 threads) |
|---|---|---|
| samples | 12 | 13 |
| mean | **9.14 ms** | **3.38 ms** |
| min | 8.92 ms | 2.77 ms |
| max | 9.37 ms | 4.46 ms |

**Speedup: ~2.70×** CPU record-time reduction for the forward pass at
30,000 draws, going from fully-serial (`chunkCount==1`) to the default
worker count (`chunkCount==7`) on this 8-core machine.

Raw `stress:` stdout lines (both runs, the same shape CI's
`stress-numbers.txt` artifact publishes):

```
=== --threads 1 ===
stress: fps=35.7 cpu_record_ms=9.120 draws=30000
stress: fps=36.0 cpu_record_ms=8.920 draws=30000
stress: fps=34.9 cpu_record_ms=9.287 draws=30000
stress: fps=31.8 cpu_record_ms=9.369 draws=30000
stress: fps=30.7 cpu_record_ms=9.009 draws=30000
stress: fps=30.6 cpu_record_ms=9.264 draws=30000
stress: fps=30.3 cpu_record_ms=9.108 draws=30000
stress: fps=30.5 cpu_record_ms=9.159 draws=30000
stress: fps=30.6 cpu_record_ms=9.082 draws=30000
stress: fps=31.0 cpu_record_ms=9.186 draws=30000
stress: fps=30.3 cpu_record_ms=9.206 draws=30000
stress: fps=30.2 cpu_record_ms=8.951 draws=30000

=== default worker count (7 threads) ===
stress: fps=43.4 cpu_record_ms=4.457 draws=30000
stress: fps=44.5 cpu_record_ms=4.109 draws=30000
stress: fps=44.3 cpu_record_ms=3.989 draws=30000
stress: fps=41.6 cpu_record_ms=2.776 draws=30000
stress: fps=41.3 cpu_record_ms=3.817 draws=30000
stress: fps=41.8 cpu_record_ms=2.773 draws=30000
stress: fps=41.4 cpu_record_ms=2.829 draws=30000
stress: fps=41.5 cpu_record_ms=2.768 draws=30000
stress: fps=39.9 cpu_record_ms=2.790 draws=30000
stress: fps=41.1 cpu_record_ms=2.836 draws=30000
stress: fps=41.6 cpu_record_ms=2.922 draws=30000
stress: fps=39.5 cpu_record_ms=4.021 draws=30000
stress: fps=41.3 cpu_record_ms=3.840 draws=30000
```

A second, independent confirmation via headless mode (3 frames only, so
lower statistical confidence, but a completely different code path/timing
mechanism than `--present`): `--threads 1` → 11.68/11.50/9.22 ms;
default → 4.99/5.48/3.51 ms. Same order of magnitude and direction.

### Tracy capture evidence — disclosed limitation, not fabricated

I attempted to capture Tracy zone-stats evidence for the
`graph_pass_chunk_fanout`/`graph_pass_chunk_record` zones specifically,
using an ad-hoc `tracy-capture`/`tracy-csvexport` build (this project's own
`third_party/CMakeLists.txt` deliberately never builds the profiler/capture
tools, only `TracyClient` — verified in that file's own comment — so no
project-supported capture binary exists to reuse). I did successfully
capture real `.tracy` files (both `--threads 1` and default-worker-count
runs, ~8 MB each) with correct **zone structure** (names, nesting, per-frame
call counts consistent with the sample's own logged frame count). However,
the exported **absolute timing values** were inconsistent with the
directly-measured wall-clock numbers above by roughly two orders of
magnitude (e.g. the whole-`execute()` zone reporting ~52 µs mean when the
sample's own `cpu_record_ms` for the identical call, at the identical
moment, was 3-9 ms — a ~60-170× discrepancy), and the captured frame COUNT
over a fixed 6-second capture window implied a frame rate roughly 3× higher
than what the running sample's own per-second `fps` log simultaneously
reported. This points to a clock-calibration or protocol-version mismatch
in the ad-hoc capture toolchain, not a defect in the `RX_ZONE`/`RX_GPU_ZONE`
instrumentation itself (unchanged in shape from Task 3's own established,
reviewed usage). Rather than present numbers I have direct evidence
contradicts, I am reporting the directly-measured `cpu_record_ms`
figures above as authoritative, per this task's own "no prose estimates —
measurements only" standing — and disclosing this limitation rather than
omitting it.

## Concerns

1. **The chunk-0-synchronous executor extension** (above) is a real
   addition to the binding design, discovered necessary while implementing
   a downstream requirement (samples/06_materials' migration) that the
   original design text didn't anticipate. It is documented in three
   places (`pass.h`, `executor.cpp`, `docs/threading.md`) but the spec doc
   itself (`docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md`,
   D4) was not amended — flagging for a coordinator decision on whether it
   should be.
2. **Tracy capture tooling limitation** (above) — the numbers reported are
   solid (direct wall-clock measurement, cross-checked via two independent
   code paths), but the task brief's specific ask for a Tracy zone-stats
   text export as corroborating evidence could not be satisfied with
   numbers I trust, using tooling available in this environment. If exact
   zone-level Tracy evidence is required, it would need either the
   project's own pinned Tracy profiler GUI (not built by this repo's
   CMake, by design) or further investigation into the ad-hoc capture
   build's calibration handshake.
3. **`kMaxChunksPerPass = 16`** is a judgment call (documented in
   `executor.h` with its own rationale — the vkguide.dev "2-10 secondaries
   per frame optimal" citation from the threading research doc, with
   headroom above it), not a value derived from measurement on a
   very-high-core-count machine (this dev machine has 7 workers, well
   under the cap, so the cap itself is unexercised by any test or
   measurement in this task).
4. Samples 01-04 untouched, as directed — pre-graph tier references.
