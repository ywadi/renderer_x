# Task 3 re-review: fix round 1 + supplemental (73f8c4a..5c4aa23)

Scoped re-review only: verifies the five `task-3-review.md` findings and the
supplemental consistency item, checks the two new-seam/scope items called out
by the coordinator, and looks for regressions the fix round itself might have
introduced. Does not re-review already-approved Task 3 aspects.

Reviewed: `task-3-review.md` (prior findings), `review-73f8c4a..5c4aa23.diff`
(both fix-round commits), `task-3-report.md`'s "Fix round 1" + "Fix round 1
supplemental" sections, and the current tree state (`git log` confirms `HEAD`
is `5c4aa23`, on top of `980031f`/`73f8c4a`; both new commits' author/
committer is `Yousef Wadi <ywadi85@gmail.com>`, grep-verified clean of any AI
attribution phrase in both commit messages and in the full `73f8c4a..5c4aa23`
diff content). Independently rebuilt (`cmake --build build/linux-native
--target rx_graph_gpu_tests rx_graph_tests`, forced via `touch` on every
changed file to rule out a stale-binary false pass, zero warnings) and re-ran
`ctest --test-dir build/linux-native --output-on-failure` (11/11 passed,
including all four sample gates) and `rx_graph_gpu_tests --validate` directly
(4 test cases, 57 assertions, matching the report's exact numbers, only the
two pre-existing documented false-positive validation messages,
`hasValidationErrors()` false throughout).

Beyond re-running the shipped suite, per this task's explicit instruction I
wrote two independent probes in scratchpad (never touching the repo — `git
status`/`git diff --stat` confirmed clean before and after) against the real,
built library:

1. **Misuse-compiles-away probe** (Important finding 3): a standalone .cpp
   holding a `const RenderGraph&` and attempting exactly the prior review's
   demonstrated misuse (`PassContext ctx; graph.passAt(0).invokeExecute(ctx);`).
   Compiled directly against the current headers — result: **hard compile
   failure on both lines**, `no matching constructor for initialization of
   'PassContext'` (all three candidates rejected: deleted copy, deleted move,
   the one-argument `explicit PassContext(const Executor&)`) and
   `'invokeExecute' is a private member of 'rx::graph::Pass'`. This confirms
   the fix closes the gap at the language level, not merely by convention.
2. **Independent stage-union probe** (Critical finding 1): a standalone
   program, written from scratch (not copied from the shipped regression
   test), reproducing the prior review's "shared resource, two independent
   final readers at different pipeline stages" shape but with the two
   readers' **declaration/topological order reversed** relative to the
   shipped test (compute reader declared and executed before the graphics
   reader) and different pass/resource names, to rule out any order-
   dependence in the fix. Compiled against the real headers and linked
   directly against the freshly rebuilt `librx_graph.a`/`librx_rhi_vk.a`/etc.
   (extracted compile/link commands from `compile_commands.json`/
   `ninja -t commands`), run under `xvfb-run` against lavapipe with real
   validation enabled. Result: `rx::graph::detail::debugLastFrameFinalStages()`
   after the first `execute()` call returns `0xc80`
   (`COLOR_ATTACHMENT_OUTPUT_BIT | FRAGMENT_SHADER_BIT | COMPUTE_SHADER_BIT`)
   — both readers' stages are present regardless of which one runs
   topologically last, and a second `execute()` call recomputes the identical
   value fresh. Zero validation errors either call. (My first run used an
   incomplete "expected" value of just the two readers' bits and mismatched
   at `0xc80` vs `0x880`; diagnosing that mismatch confirmed the accumulation
   loop unions *every* pass touching the resource across the whole
   `execute()` call, including the producer's own `ColorOutput` write stage
   — not a defect, exactly the fix's own documented "carrying a little extra
   history forward is conservative, never unsafe" rationale, corrected in the
   probe and reverified.)

## Finding-by-finding verification

**1. CRITICAL — `finalStageThisExecute` union, not overwrite.** Fixed.
`executor.cpp`'s end-of-pass loop now reads `finalStageThisExecute[physIdx] |=
combined.stages;` (was `=`). Verified in the current source (line 584), not
just the diff. The shipped regression test (`"Executor::execute unions every
final-touching pass's stage..."`) reproduces the prior review's exact probe
shape and passes; my own independently-written, order-reversed probe against
the rebuilt library confirms the same result and rules out order-dependence.
Dead.

**2. IMPORTANT — CI `windows-cross-zig` exclusion.** Fixed, minimally and
consistently. `ci.yml`'s `-E` regex changed from `'rx_rhi_vk|sample'` to
`'rx_rhi_vk|rx_graph_gpu|sample'` — a single alternative added to the same
`ctest -E` mechanism every other exclusion in this job already uses, with the
adjacent comment block extended to name `rx_graph_gpu_tests` alongside
`rx_rhi_vk_tests`. Independently confirmed against the real
`windows-cross-zig` ctest registry (not just reading the regex):
`ctest --test-dir build/windows-cross-zig -N -E 'rx_rhi_vk|rx_graph_gpu|sample'`
lists exactly 5 tests (`shader_spirv_test`, `rx_core_tests`,
`rx_platform_tests`, `rx_shader_tests`, `rx_graph_tests`) — `rx_graph_gpu_tests`
correctly excluded, while `rx_graph_tests` (the device-free suite, which must
keep running under Wine) is correctly **not** excluded (`rx_graph_gpu` is not
a substring of `rx_graph_tests`, so no collateral exclusion). A direct
`-R 'rx_graph_gpu'` query on the same build confirms the target being
excluded actually exists and matches by name. Dead.

**3. IMPORTANT — `PassContext`/`Pass::invokeExecute()` hardening.** Fixed at
the compiler level, verified by attempting the exact misuse and getting a
hard compile error (see probe 1 above) rather than merely reading the source
and trusting it compiles away. `PassContext` now has exactly one constructor
(`explicit PassContext(const Executor&)`), private, friended to `Executor`
alone; copy/move deleted. `Pass::invokeExecute()` moved to `private`, friended
to `Executor` alone, with a minimal forward-declared `class Executor;` in
`pass.h` used only to name it in the friend grant (`pass.h` still never
includes `executor.h`, preserving header hygiene). `Executor::execute()`'s own
construction site updated to `PassContext ctx(*this);`. A caller holding only
a `const RenderGraph&` (e.g. via the still-public `RenderGraph::passAt()`) can
no longer construct a `PassContext` or invoke a pass's callback at all — both
halves of the original gap are closed together, not just one. Dead.

**4. MINOR — eviction off-by-one.** Fixed exactly as specified.
`transient_pool.cpp`'s `sweepStale()` changed from `>` to `>=
kStaleAfterExecutes` for both the image and buffer loops, matching the
ruling's literal "unused for `kFramesInFlight+1` consecutive executes"
threshold precisely (verified by re-reading the arithmetic: an entry last
touched at frame `F` is now retired exactly at `currentFrame - lastUsedFrame
== kStaleAfterExecutes`, not one call later). No behavior change beyond
tightening the threshold by one call; still always safe (never destroys
early). Comments at both the header and implementation updated consistently.
Dead.

**5. MINOR — `imageFormat()` silently wrong on a buffer name.** Fixed.
`Executor::resolveImageFormat()` now throws `std::out_of_range` via the new
shared `requireKind()` helper when the resolved resource is a buffer,
replacing the prior silent `VK_FORMAT_UNDEFINED` return. Dead.

**Supplemental — `image()`/`imageView()`/`buffer()` wrong-kind-name
consistency.** Fixed, in the same style as finding 5, via the same
`requireKind()` helper (one function, four call sites: `imageView`, `image`,
`buffer`, `imageFormat`, each naming both the queried resource and its actual
kind in the thrown message, e.g. `"'data' is a buffer resource, not an
image"`). `executor.h`'s resolver-contract comment updated to state the rule
once for all four. Covered by three new `CHECK_THROWS_AS` assertions added to
the existing buffer-reuse GPU test's `"consume"` callback, exercised against
the real `"data"` buffer and real `"bb"` backbuffer image through the real
`Executor` — independently reproduced: `rx_graph_gpu_tests --validate` shows
4/4 cases, 57/57 assertions, matching the report exactly. Dead.

## Additional checks requested by the coordinator

**`detail::debugLastFrameFinalStages()` seam.** Appropriately scoped: lives in
`rx::graph::detail`, both in `executor.h`'s declaration and `executor.cpp`'s
definition, documented explicitly as "Test/debug-only seam -- NOT part of the
stable public contract," mirroring `barriers.h`'s own established `detail::`
convention for exactly this purpose (also independently justified: this
codebase enables no Vulkan synchronization validation anywhere, so no
`--validate` run could ever catch a wrong cross-frame stage value without a
direct-inspection seam). Grepped the entire repository (excluding review
materials) for the symbol: it is defined once, declared once, and called
exactly once, from `test_execute_gpu.cpp`'s regression test — **no production
caller**. `Executor` grants it a single named-function friend declaration
(not a broad `friend class`), consistent with the minimal-exposure style the
rest of this file already uses.

**Regression risk from the friend/private changes.** None found. Full
11/11-test suite passes after a forced, non-incremental rebuild of every
changed file (ruling out a stale-binary false pass); zero compiler warnings
from any changed file. `Executor::execute()`'s only call site for
`PassContext`'s constructor is updated correctly (`PassContext ctx(*this);`),
and it is the only caller in the codebase (grep-confirmed no other
construction site). `Pass::invokeExecute()`'s only caller is likewise
`Executor::execute()`; `RenderGraph`'s own friend status on `Pass` is
unaffected (still needed for `addPass()`/`compile()`'s own private-member
access, untouched by this fix).

**Regression risk from the union change on previously-asserted barrier
fields.** None found. The union is strictly additive (`|=` instead of `=`)
against a field (`lastFrameFinalStages`) that only ever feeds a *srcStage*
override on a resource's first barrier each `execute()` call — widening it
can only add extra (harmless, already-completed) wait scope, never remove a
wait an existing passing test depended on. All three pre-existing GPU test
cases ("invert", "resize-rerealize", "buffer cross-frame reuse") still pass
unchanged, and the buffer-barrier synthesis path
(`synthesizeFirstUseBufferBarrierIfNeeded`) that the Verified-correct section
of the prior review scrutinized in detail is untouched by this fix round.

## Verdict

All 6 items addressed (5 findings + 1 supplemental). No new defects found in
the fix-round diff; the two additional coordinator-requested checks
(`debugLastFrameFinalStages()` scoping, friend/private regression risk) both
came back clean.
