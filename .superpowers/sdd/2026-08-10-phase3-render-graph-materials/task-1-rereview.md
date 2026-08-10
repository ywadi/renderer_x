# Task 1 Fix Round 1 Re-Review

Scoped re-review of commit `089af6858581e356f343a38c0a87304c1becf32e` ("fix:
detect render graph pass dependency cycles and split texture-input stage by
pass kind"), against the three findings in `task-1-review.md`. This is a
narrow check of the fix diff only -- the rest of Task 1 was already
approved and is not re-litigated here.

## Verdict: All findings addressed

## 1. Cycle detection (Important) -- ADDRESSED

`RenderGraph::compile()` (`src/rx_graph/render_graph.cpp`) now tracks
`reachableCount` alongside the existing reachability DFS (step 3a), runs
Kahn's algorithm restricted to the reachable subgraph (step 3b, unchanged),
and immediately after compares `executionOrder.size()` to `reachableCount`
(step 3c, new). On a mismatch it runs a second DFS with a three-state
recursion-stack marker (`0` unvisited / `1` on-path / `2` done) over the
same `dependsOn` producer edges, restricted to `reachable[p]`, finds a
concrete back-edge, and throws `std::runtime_error` whose message walks the
DFS path from the revisited pass to the point of detection and back,
`'A' -> 'B' -> 'A'`-style, naming every pass in the cycle by its declared
name -- not a generic "some pass never ran."

**Verified directly, not just read:**

- Rebuilt (`cmake --build build/linux-native --target rx_graph_tests` ->
  `ninja: no work to do`, confirming the committed source is what's already
  built) and ran `rx_graph_tests` directly: 13 test cases / 66 assertions,
  0 failed, including the new `"a circular pass dependency throws, naming
  passes in the cycle"` case, which observably prints
  `rx_graph: dependency cycle detected among passes: 'A' -> 'B' -> 'A'` and
  the test's own `CHECK`s that the message contains both `"A"` and `"B"`
  pass.
- Manual strict recompile of the modified `render_graph.cpp`
  (`g++ -Wall -Wextra -Wpedantic -Wshadow -O2 -std=gnu++20`): zero warnings.
- `git show 089af68`: author/committer both `Yousef Wadi <ywadi85@gmail.com>`;
  grep for `claude|anthropic|co-authored|copilot|gpt|openai` across the
  commit -> no matches.
- Full-suite regression (`ctest --test-dir build/linux-native
  --output-on-failure`, all 10 targets): 100% passed.

**DFS correctness, probed beyond the one required test case** (three
standalone probes compiled with the project's own `zig-cxx-linux` wrapper,
linked against the already-built `librx_graph.a`/`librx_core.a`/
`libspdlog.a`, run directly -- source kept in this session's scratchpad
only, never added to the repo):

| Scenario | Result |
|---|---|
| The brief's own 2-cycle rooted at the backbuffer writer (`present` outside the cycle, `A`/`B` inside it) | `'A' -> 'B' -> 'A'`, both named -- matches the committed test exactly |
| A 2-cycle (`M`/`N`) reachable *only* through a side-effect pass (`watchdog`), entirely disconnected from the backbuffer's own writer chain -- confirms the outer DFS loop's multi-root traversal, not just the backbuffer-rooted component | Threw, message `'M' -> 'N' -> 'M'`, both named |
| A 3-cycle that passes *through* the root itself (`present` both writes `bb`, the root, *and* is a member of the cycle: `present -> X -> Y -> present`) -- distinct from the simpler case where the root sits outside the cycle | Threw, message `'present' -> 'X' -> 'Y' -> 'present'`, all three named |
| A 2-cycle (`X`/`Y`) among passes that are neither reachable from the backbuffer's writer nor a side effect -- i.e., a cycle entirely inside what would be *culled* dead code | Did **not** throw; `X`/`Y` both report `isCulled() == true`; `executionOrder()` is `{2}` (just `present`) -- the dead cycle is harmlessly discarded exactly like any other unreachable subgraph |

**On culling order and dead-cycle handling specifically** (the scrutiny
this task asked for): reachability (culling) runs *before* both Kahn's
algorithm and cycle detection. `indegree`/`successors` are built only for
`reachable[p]` passes (`render_graph.cpp:270`, `if (!reachable[p])
continue;`), and the cycle-detection DFS's outer loop only starts from
`reachable[p]` passes (`render_graph.cpp:348`). Consequently a cycle that
lies entirely among passes that would be culled is never examined at all --
confirmed empirically above, not just by code inspection. This is the
correct and sensible choice: dead/unreachable code shouldn't block an
otherwise-valid compile just because it happens to be cyclic, and it's
consistent with the pre-existing (already-approved) culling semantics from
Task 1's first pass. It is *lightly* documented -- the comment at
`render_graph.cpp:315-317` states the invariant that makes the DFS correct
("dependsOn[p] for any reachable p contains only reachable producers") but
doesn't spell out in so many words that a cycle confined to unreachable
passes is deliberately left undiagnosed. This is a documentation nicety,
not a defect, and is not one of the three findings this round was scoped
to -- noting it only as a possible future polish item, not a blocker.

## 2. Texture-input stage mapping extension (coordinator ruling) -- ADDRESSED

`Pass::resolveAccess()`'s `TextureInput` case now reads:

```cpp
access.stages =
    computeClass ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
access.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
access.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
```

exactly matching the ruling: Compute-class -> `COMPUTE_SHADER`/
`SHADER_SAMPLED_READ`/`SHADER_READ_ONLY_OPTIMAL`; Graphics-class (raster)
keeps `FRAGMENT_SHADER` with the same access/layout. The new test
`"texture input stage resolves per pass kind, like storage buffers already
did"` exercises both classes against the *same* underlying resource
(`"sampler"`, no attachment output -> Compute-class; `"present"`, has a
color output -> Graphics-class) and asserts all three fields for both.
Confirmed passing directly in the 13/66 test run above; `computeClass` is
derived the same way as the already-approved storage-buffer split
(`!pass.hasAttachmentOutput()`), so no new classification logic was
introduced -- only the existing rule was applied to one more switch case.

## 3. `CompiledGraph` class-level doc comment (Minor) -- ADDRESSED

`render_graph.h`'s class comment for `CompiledGraph` now reads:

> Before the owning RenderGraph's first compile() call (or after reset()),
> an unpopulated CompiledGraph reports empty spans from
> executionOrder()/resources(), but isCulled()/passAccesses() throw
> std::out_of_range for any `passIndex` at all -- there is no pass count
> yet to be in range of. See those two methods' own comments for the exact
> per-method contract.

This precisely matches the actual per-method contracts already documented
on `isCulled()` ("Out-of-range throws std::out_of_range") and
`passAccesses()` ("Out-of-range `passIndex` throws std::out_of_range"),
which in turn match the implementation (`culled_.at(passIndex)` /
`passAccesses_.at(passIndex)`, both empty vectors pre-compile, so `.at()`
throws for every index, including 0). Doc-only change; no behavior
modified, none needed.

## New-defect check

- Diff matches the working tree exactly (`render_graph.cpp`,
  `render_graph.h`, `test_compile.cpp` read directly and cross-checked
  against `review-16ccad4..089af68.diff` line by line -- no drift).
- New includes (`<algorithm>`, `<functional>`) are used
  (`std::find`, `std::function` for the recursive cycle-search lambda) and
  nothing else changed unexpectedly.
- The cycle-detection block runs strictly before step 4 (resource
  resolution), so a thrown cycle exception never leaves `g.compiled` in a
  partially-overwritten state -- `g.compiled = std::move(compiled)` only
  happens at the very end of `compile()`, after the new check, matching the
  exception-safety pattern the pre-existing validation throws already use.
- Self-edges (a pass reading/writing the same resource it itself just
  wrote) are still excluded from `dependsOn` (unchanged guards from Task
  1's original pass), so the new DFS can't be confused by a spurious
  self-loop.
- No warnings under `-Wall -Wextra -Wpedantic -Wshadow`; full 10-target
  regression suite green; both presets' build state unaffected beyond the
  intended files (`ninja: no work to do` for everything else).
- No AI attribution in the fix commit (checked directly against `git show`,
  not just trusted from the report).

## Files reviewed

- `/media/ywadi/second/renderer_x/.superpowers/sdd/2026-08-10-phase3-render-graph-materials/task-1-review.md`
- `/media/ywadi/second/renderer_x/.superpowers/sdd/2026-08-10-phase3-render-graph-materials/review-16ccad4..089af68.diff`
- `/media/ywadi/second/renderer_x/.superpowers/sdd/2026-08-10-phase3-render-graph-materials/task-1-report.md`
- `/media/ywadi/second/renderer_x/src/rx_graph/render_graph.cpp`
- `/media/ywadi/second/renderer_x/src/rx_graph/include/rx_graph/render_graph.h`
- `/media/ywadi/second/renderer_x/src/rx_graph/tests/test_compile.cpp`
