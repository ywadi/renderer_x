# Task 3 review: rx_graph Executor (77f42cf..73f8c4a)

Reviewed: brief (`task-3-brief.md`), implementer report (`task-3-report.md`),
full diff (`review-77f42cf..73f8c4a.diff`), and the current tree state
(`git log` confirms `HEAD` is `73f8c4a`, author/committer
`Yousef Wadi <ywadi85@gmail.com>`, no AI attribution in subject/body or any
changed file — grep-verified). Also re-read the unchanged parts of
`resources.h`, `pass.h`, `render_graph.cpp` (the `resolveAccess` stage/access
table), `bindless.h`'s RELEASE-SAFETY CONTRACT, `deletion_queue.h`, and
`rx_rhi_vk/context.cpp`'s validation-message filter, since several scrutiny
points hinge on exactly what those already-reviewed files guarantee.

Re-ran the implementer's own gate independently rather than trusting the
pasted output: `cmake --build build/linux-native --target rx_graph_gpu_tests
rx_graph_tests` (already up to date after a clean rebuild) then `ctest -R
rx_graph --output-on-failure` — both targets pass, and `rx_graph_gpu_tests
--validate` run directly reproduces the report's exact numbers (3 test
cases, 39 assertions, only the two documented known-false-positive
validation messages, `hasValidationErrors()` false throughout).

Beyond re-running the shipped suite, I wrote two temporary probes directly
against the real, built `Executor`/`RenderGraph` public API (via a
temporarily-appended `TEST_CASE` in `test_execute_gpu.cpp`, and — for the
second probe only — temporary `RX_LOG_ERROR` instrumentation inside
`executor.cpp` to observe otherwise-private internal state). Both
modifications were fully reverted via `git checkout` immediately after
capturing their output; `git diff --stat HEAD -- src/rx_graph/` is empty as
of this review. No probe code was left in the repo.

## Spec compliance

| # | Requirement | Verdict |
|---|---|---|
| 1 | `PassContext`/`Executor` interfaces exactly as specified (`create`/`realize`/`execute`, name-based resolvers) | ✅ |
| 2 | Transient pool keyed by (format,extent,usage,samples)/(size,usage), via existing `rx::rhi::Texture2D`/`Allocator` (no raw `vmaCreate*` calls), DEVICE_LOCAL | ✅ |
| 3 | Reused across frames and across compiles when descriptors match | ✅ |
| 4 | Unused entries destroyed via `DeletionQueue` after `kFramesInFlight+1` executes | ✅ functionally (see Minor finding 4 for a one-call-later-than-literal off-by-one, safe direction) |
| 5 | First-use-of-frame image barriers: `oldLayout=UNDEFINED`, `srcStage` = tracked last-frame final-use stage, `ALL_COMMANDS` on very first use | ⚠️ mechanism exists and is documented at the override site, but the *tracking* it reads from is wrong for a resource with more than one final-touching pass per `execute()` call — see Critical finding 1 |
| 6 | Depth attachments: `LOAD_OP_CLEAR`(1.0)/`STORE_OP_STORE` | ✅ (implemented correctly; untested by the shipped suite, but I independently probed a real depth-stencil-format pass through the real Executor and found no validation error — see "Verified correct") |
| 7 | Per-pass debug labels via `vkCmdBeginDebugUtilsLabelEXT`, extension queried once at `create()` | ✅ |
| 8 | Dynamic rendering + sync2 only; one begin/end scope per graphics pass; `vkCmdPipelineBarrier2` for all barriers | ✅ |
| 9 | Zero validation errors under `--validate` | ✅ for every scenario the shipped suite exercises — but this gate cannot catch Critical finding 1 (Vulkan synchronization validation is not enabled anywhere in this codebase — confirmed by grep), so "zero validation errors" is not evidence against that finding |
| 10 | `compile()`/`CompiledGraph` stay device-free | ✅ (grep-verified: zero real `volk`/`rx_rhi_vk` usage in `render_graph.cpp`/`barriers.cpp`/any `rx_graph` public header — only descriptive comments mention the terms) |
| 11 | No volk in public headers | ✅ |
| 12 | Reuse existing rx_rhi_vk infra (Device, DeletionQueue, BindlessTable contract) instead of parallel implementations | ✅ |
| 13 | No AI attribution | ✅ (grep-verified clean across commit message/author and every changed file) |
| 14 | TDD: failing GPU test first, both required cases (`invert`, `resize-rerealize`) | ✅ (plus an additive third case for the self-found buffer-barrier gap) |
| 15 | ctest gate `rx_graph_gpu_tests --validate`, following the existing GPU-gate pattern | ✅ |
| 16 | **CI excludes GPU tests on windows-cross the same way existing ones are excluded** (brief's own step 2 text) | ❌ — see Important finding 2 |
| 17 | Green: `ctest --preset linux-native -R rx_graph`, both presets build, zero validation errors | ✅ (independently reproduced) |
| 18 | Coordinator ruling: `CompileInfo::backbufferFinalLayout`, `present-final` covers default + `TRANSFER_SRC_OPTIMAL` | ✅ |
| 19 | Coordinator ruling: backbuffer external/never pooled, keeps compile-time first-use semantics | ✅ |
| 20 | Coordinator ruling: debug labels guarded by extension query | ✅ |
| 21 | Global constraint: production grade, no half-measures | ⚠️ see Important findings 2-3 |

**Overall spec verdict: ❌** — requirement 5 (the correctness of the
cross-frame `srcStage` override this task's own central mechanism depends
on) and requirement 16 (CI exclusion, explicitly named in the brief's own
step 2) both fail. Everything else is compliant.

## Critical finding: `lastFrameFinalStages` is overwritten, not unioned, across passes in one `execute()` call — silently drops an outstanding reader's stage whenever a resource has more than one final-touching pass

This is the task's own first scrutiny point ("is `lastFrameFinalStages`
tracked correctly through EVERY path a resource can end a frame in") and it
is not.

`executor.cpp`'s end-of-`execute()` bookkeeping:

```cpp
for (const auto& [physIdx, combined] : combineAccessesByResource(accesses)) {
    if (!impl.resources.at(physIdx).isBackbuffer) {
        finalStageThisExecute[physIdx] = combined.stages;   // assignment, not |=
    }
}
```

runs once per pass, inside the topological-order loop. `PhysicalResource`'s
own doc comment (`resources.h`) defines `lastUsePass` as "positions into
`executionOrder()` ... of this resource's first and last touching pass" —
i.e. the resource's *last* touch is whichever pass sits at the highest
topological position. Because the assignment above runs in that same
position order and **overwrites** rather than **unions**, the final value
left in `finalStageThisExecute[physIdx]` (and therefore copied into the
pooled entry's `lastFrameFinalStages` a few lines later) is *only* the
stage of the single pass at the highest position that touches this
resource — every earlier pass that also touches the same resource in this
same `execute()` call, with no intervening write, has its stage silently
discarded.

This is exactly the bug class Task 2's own review already found and fixed
once, one layer down (`task-2-review.md`'s Critical finding: "a WAR hazard's
execution dependency must cover the pipeline stage of every outstanding
reader, not just the most recent one"). Task 2's fix
(`invalidatedStagesUnion`) only protects a *write* within one compile walk;
it has no way to protect the *executor's cross-frame carry-forward* of
"what stage was this resource left at", because that state does not exist
until Task 3 built it.

**Verified empirically, not just by reading the code.** I built a graph
where a resource ("shared") is written once, then read by two independent
final passes with no write after either — the exact "shadow map read by a
lighting pass and a debug pass" shape that is completely ordinary in real
engine graphs, not a contrived corner case:

```cpp
graph.addPass("write").addColorOutput("shared", ...);
graph.addPass("readGraphics").addTextureInput("shared").addColorOutput("bb", ...);
graph.addPass("readCompute", QueueClass::AsyncCompute)
    .addTextureInput("shared").addStorageBufferOutput("outB", ...).setSideEffect();
```

Temporarily instrumenting the two relevant lines in `executor.cpp` (fully
reverted afterward — `git diff HEAD -- src/rx_graph/` is empty) and running
two consecutive `execute()` calls produced:

```
PROBE: pass at pos 0 ('write') sets finalStageThisExecute['shared'] = 1024 (was 0)
PROBE: pass at pos 1 ('readGraphics') sets finalStageThisExecute['shared'] = 128 (was 1024)
PROBE: pass at pos 2 ('readCompute') sets finalStageThisExecute['shared'] = 2048 (was 128)
   -- readGraphics's FRAGMENT_SHADER_BIT (128) is discarded here, not unioned
---- execute() call 1 ----
PROBE: first-use image barrier for 'shared' this call: srcStage override = 2048
   -- COMPUTE_SHADER_BIT only. FRAGMENT_SHADER_BIT (128) never makes it into
      the srcStage that frame 2's write-after-read barrier waits on.
```

Frame 2's "write" pass re-transitions "shared" from `UNDEFINED` with
`srcStage` overridden to whatever `lastFrameFinalStages` holds — `2048`
(`COMPUTE_SHADER_BIT`) only. `readGraphics`'s fragment-shader sampled read
of "shared" from frame 1 is never named in that wait. Vulkan gives no
completion-order guarantee between two independent, unsynchronized reads in
different pipeline stages issued in the same command buffer — nothing
requires `readGraphics`'s fragment shader invocations to have actually
finished by the time frame 2's write begins clobbering the same memory. This
is a genuine, reproducible write-after-read hazard, and it is **silent**:
this codebase does not enable Vulkan synchronization validation anywhere
(`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` — grepped,
absent from `context.cpp`), so no `--validate` run, on any test, will ever
catch this class of bug. It also would not likely manifest visibly on
lavapipe (a largely serial software rasterizer), meaning it could ship
straight through to real hardware where GPUs genuinely do complete
independent work out of program order.

None of the three shipped GPU tests exercise a resource with more than one
final-touching pass — "color" (invert test), "aux" (resize test), and
"data" (buffer test) are each touched by exactly one reader/writer pair, so
the overwrite-vs-union distinction never has an opportunity to matter in the
delivered suite.

**Fix direction** (not prescribing the exact shape): the minimal, always-safe
fix is `finalStageThisExecute[physIdx] |= combined.stages;` (accumulate
across every pass touching a resource in one `execute()` call, resetting
only makes sense to skip since a subsequent WRITE's own barrier already
waits on prior readers via Task 2's `invalidatedStagesUnion` fix — carrying
a little extra history forward is conservative, never unsafe). A more
precise fix would mirror what `barriers.cpp`'s internal per-resource state
already computes (`invalidatedStagesUnion`, reset on write) and expose it
per-resource instead of re-deriving an approximation in the executor.

## Important finding: CI's `windows-cross-zig` exclusion for `rx_graph_gpu_tests` was left undone, and the report's stated justification does not match the brief

The brief's own step 2 ("Verify failure") reads: "ctest gate registered as
`rx_graph_gpu_tests` with `--validate` (follow the existing GPU-gate
registration pattern in `src/rx_rhi_vk/tests/CMakeLists.txt`; **CI excludes
GPU tests on windows-cross the same way existing ones are excluded**)."
That parenthetical is part of the specified deliverable, not a side note.

The report's "Known, deliberate scope gaps" section says this was skipped
because it is "explicitly out of scope ('do not edit CI in this task')" and
then goes on to correctly predict the consequence: `windows-cross-zig`'s
`ctest -E 'rx_rhi_vk|sample'` regex does not match `rx_graph_gpu` (confirmed
independently: `ctest --preset windows-cross-zig -N -E 'rx_rhi_vk|sample'`
still lists `rx_graph_gpu_tests`), so that job would hit a hard
`REQUIRE(device.has_value())` failure under Wine (no real Vulkan device),
rather than the graceful skip guard, the moment this branch's CI actually
runs there.

I searched every file in this task's materials folder
(`task-3-brief.md`, `task-1/2-*.md`, `progress.md`, `research-*.md`) for the
quoted phrase "do not edit CI in this task" — it does not appear anywhere.
The brief's own text says the opposite: CI exclusion for the new GPU target
is part of this task's own step 2. I cannot rule out an out-of-band verbal
instruction from the coordinator that simply isn't captured in the written
materials I have access to, but as far as the provided record shows, the
implementer left a one-line, in-scope, brief-specified CI fix undone and
cited a scope restriction that contradicts the brief's own literal text.
The disclosed consequence (this branch's CI will fail on `windows-cross-zig`
once triggered) is real and independently confirmed; only the stated
justification for accepting it doesn't check out.

## Important finding: the two additive API extensions leave `Pass::invokeExecute()` callable, with an uninitialized `PassContext`, by anyone holding a `const RenderGraph&`

The report itself flags `RenderGraph::passAt()` and `rx::rhi::Device::
instance()` for scrutiny. `Device::instance()` is fine: a trivial,
documented, `const`-qualified accessor exposing exactly the same class of
already-stored handle every other accessor on `Device` already exposes
(`physicalDevice()`, `device()`, ...) — no new exposure class, no finding.

`passAt()` is a different story in combination with what it returns.
`Pass::invokeExecute(PassContext&) const` (`pass.h`) is declared in `Pass`'s
**public** section, with no `friend class Executor;` restricting who may
call it — unlike every other place this same codebase uses exactly that
pattern to fence off a "only this one specific consumer may do this"
capability (`Executor`/`PassContext` already friend each other for their
own private resolvers). `PassContext` (`executor.h`) has no user-declared
constructor at all, so its implicit default constructor is public and
leaves `executor_` at its default, `nullptr`.

The combination: any code holding a `const RenderGraph&` (not even a
mutable reference) can do

```cpp
PassContext ctx;                 // executor_ == nullptr, publicly constructible
graph.passAt(3).invokeExecute(ctx);   // fully public, no friend guard
```

and if that pass's stored callback calls `ctx.imageView(name)` (or `image`/
`buffer`/`imageFormat`), it dereferences a null `Executor*`
(`executor_->resolveImageView(name)`) — undefined behavior / crash, not a
clean, catchable error. Nothing in the delivered path actually does this
(`Executor::execute()` always sets `ctx.executor_ = this` correctly before
invoking), so this is not a functional bug in the shipped code today. It is
a real robustness gap in the *design* of the two beyond-the-brief additions
the implementer already asked to have scrutinized: the rest of this same
file's design goes out of its way to make misuse of `Executor`'s internals
impossible (private constructor, private `Impl`, friend-gated
`PassContext`), and these two additions don't carry that same discipline
forward, in a codebase whose own standing rule is "no half-assed
solutions... ever."

**Fix direction**: add `friend class rx::graph::Executor;` to `Pass` and
move `invokeExecute()` to `private`, or otherwise ensure a `PassContext`
cannot be constructed with a null `Executor*` from outside this library
(e.g. a private constructor taking the `Executor*`, with `Executor` as the
only friend able to call it).

## Minor finding: pooled entries survive one `execute()` call longer than the ruling's literal wording

The coordinator ruling: "entries idle for `kFramesInFlight+1` executes
retired via DeletionQueue" (`kStaleAfterExecutes = 3`). `sweepStale()`'s
condition is `currentFrame - lastUsedFrame > kStaleAfterExecutes` (strictly
greater than 3). An entry last touched at frame `F` is idle during calls
`F+1, F+2, F+3` — three idle calls, matching the ruling's threshold — but
`currentFrame - lastUsedFrame == 3` at that point, and `3 > 3` is false, so
it survives to `F+4` before eviction. This errs in the safe direction (holds
memory one call longer than strictly specified, never destroys anything
early) and does not affect correctness — a trivial `>=` vs `>` mismatch
against the literal ruling text, not worth more than a note.

## Minor finding: `PassContext::imageFormat()` on a valid buffer-named resource silently returns `VK_FORMAT_UNDEFINED` instead of erroring

`executor.h`'s documented contract for the four resolvers is: `name` must be
one of `CompiledGraph::resources()`'s own names, or it throws
`std::out_of_range` — no carve-out for "the name exists but is a buffer, not
an image." `ResolvedResource::format` is only ever set for image resources
(`Executor::realize()`); for a buffer resource it stays defaulted to
`VK_FORMAT_UNDEFINED`, so calling `imageFormat()` on a legitimately
registered buffer name returns a silently-meaningless value instead of
either a real format or a thrown error. Low severity (a caller passing a
buffer's name to `imageFormat()` is already misusing the API by name-typing
alone), but inconsistent with the documented "throws on anything not
resolvable" contract's own spirit.

## Verified correct (no finding)

- **Buffer-barrier synthesis fix** (`synthesizeFirstUseBufferBarrierIfNeeded`):
  the gap analysis is right — re-read `barriers.cpp`'s `applyAccess()`
  directly: a buffer's true first access has `needBarrier` false
  unconditionally (`layoutDiffers` is always false for buffers, and
  `write && everAccessed` is false on first write), so `buildBarriers()`
  genuinely never emits anything for it, matching Task 2's own
  `"compute's first write to a fresh buffer needs no barrier at all"` test.
  The synthesized barrier's fields are correct: `srcAccess=NONE`,
  `srcStage` from the pool entry's `lastFrameFinalStages`, `dst` from the
  same `combineAccessesByResource()` this pass's own accesses would
  produce (mirroring what `buildBarriers()` itself would have used had it
  handled this case) — and the shared `firstBarrierSeen` vector correctly
  prevents ever double-barriering a resource `applyBarriers()` already
  handled. The third GPU test (two `execute()` calls against a pooled
  storage buffer) is a real, non-vacuous regression proof for this specific
  fix, though (per the Critical finding above) it does not exercise the
  separate multi-consumer stage-union gap, since "data" has exactly one
  reader ("consume").
- **Dynamic rendering attachment construction**: color/depth classification
  is exhaustive and unambiguous (every non-attachment `AccessKind` resolves
  to a layout that is neither `COLOR_ATTACHMENT_OPTIMAL` nor
  `DEPTH_ATTACHMENT_OPTIMAL` — confirmed against `resolveAccess()`'s actual
  table, not just the report's paraphrase). `renderArea` correctly falls
  back to the depth attachment's own extent only when there are zero color
  attachments (true depth-only case); `isGraphicsPass` correctly triggers
  for color-only, depth-only, and color+depth. `DepthStencilOutput` resolves
  to `VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL` (confirmed in
  `render_graph.cpp`, unchanged from Task 1) — I was concerned this might
  conflict with `aspectMaskForFormat()`'s `DEPTH_BIT|STENCIL_BIT` for a
  combined depth-stencil format (a real Vulkan VUID restricts combining a
  depth-only layout with a stencil-inclusive aspect mask in general), so I
  built and ran a real graph through the real `Executor` with a
  `VK_FORMAT_D24_UNORM_S8_UINT` depth-stencil attachment
  (`setDepthStencilOutput`) end to end with `--validate` — zero validation
  errors, confirming this specific combination is fine on the actual
  installed validation layer version and driver in this environment (probe
  reverted afterward; not left in the repo).
- **Cross-realize/execute pool safety**: acquisition (`acquireImage`/
  `acquireBuffer`) only ever happens inside `realize()`; staleness
  eviction (`sweepStale`) only ever happens inside `execute()`; the two are
  never interleaved within a single call, so "entry retired then
  re-acquired in the same execute()" as literally stated cannot occur.
  `realize()` can reclaim any idle, not-yet-retired entry regardless of its
  staleness clock (by design — a resize that bounces back to a previous
  size should reclaim, not reallocate), which is correct and intentional.
  `lastUsedFrame` is bumped at both acquire time and at the end of every
  `execute()` that actually touches the bound resource, so a resource
  realize() bound and every subsequent execute() keeps using never looks
  stale to `sweepStale()`.
- **Key-collision reuse across realize() calls** (a pool entry handed to a
  logically different resource after a topology change): the mechanism
  itself is sound — the barrier's `oldLayout=UNDEFINED`/`srcStage`-override
  approach does not care who the entry's *previous* logical owner was, only
  what stage its memory was last left "busy" in, which is exactly what
  `lastFrameFinalStages` is supposed to encode (modulo the Critical finding
  above about that value itself sometimes being incomplete).
- **Warm-up `doctest_main_gpu.cpp` copy**: diffed directly against
  `rx_rhi_vk/tests/doctest_main.cpp` — the copy is faithful (same
  `enableValidation=true`, same fallback-to-headless-extensions logic, same
  `main()` structure), not a landmine. This matters because an incorrect
  copy would poison every later GPU test binary process-wide, per that
  file's own documented vk-bootstrap hazard.
- **Bindless deferred-release contract**: the "invert" test's callback
  registers `colorHandle` inside the pass execute callback and defers its
  `release()` through a `DeletionQueue`, confirmed complete only after
  `runOnce()`'s own `vkQueueWaitIdle` — matches `bindless.h`'s
  RELEASE-SAFETY CONTRACT, not an eager bare `release()`.
- **Validation evidence**: independently reproduced — rebuilt
  `rx_graph_gpu_tests` from a clean state, ran `ctest -R rx_graph
  --output-on-failure` (both targets pass) and `rx_graph_gpu_tests
  --validate` directly (3 test cases, 39 assertions, only the two
  pre-existing documented false positives, `hasValidationErrors()` false
  throughout) — matches the report's pasted output exactly.
- **Header hygiene / device-freedom / no AI attribution**: grep-verified
  clean across every changed and unchanged core file, and the commit
  itself.

## Quality verdict

**5 findings**: 1 Critical, 2 Important, 2 Minor. Not approved as-is — the
Critical finding is a real, empirically-reproduced (via a temporary,
reverted probe against the actual built library) cross-frame GPU
synchronization hazard for an ordinary graph shape (a shared resource with
more than one final consumer) that none of the three shipped tests happen
to exercise, and that this codebase's validation setup cannot detect even
if it were exercised. Recommend a fix round targeting the
`finalStageThisExecute` accumulation in `Executor::execute()` before this
Executor is trusted for anything beyond single-consumer-per-resource graphs,
plus resolving the CI-exclusion gap (Important finding 2) and hardening
`Pass::invokeExecute()`/`PassContext` against direct misuse (Important
finding 3).
