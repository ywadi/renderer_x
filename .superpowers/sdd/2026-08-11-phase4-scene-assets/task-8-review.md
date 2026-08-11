# Phase 4 Task 8 Review: Ledgered-Minors Cleanup Batch (9 items)

**Commits reviewed**: f57440d (items 2-6,8), 8b96e9a (items 1,7,9), e4c98bd (report)
**Spec sources**: `.superpowers/sdd/2026-08-11-phase4-scene-assets/task-8-report.md` (git show e4c98bd), card #20 body + 3 comments, `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md` lines 97-104.

## Verification performed

- Read every code/doc/test diff in f57440d and 8b96e9a directly (`git show <sha> -- <path>`), not the 800KB package linearly.
- Investigated why `review-699572b..e4c98bd.diff` is 809,587 bytes (see "800KB package" below).
- Read the actual runtime code each new doc comment describes (`render_graph.cpp` reachability/cycle DFS, `material_system.cpp` pipeline-cache load path, `instance.cpp` ParamArena, `scheduler.cpp` autoGrainSize/parallelFor) to check doc-vs-behavior accuracy rather than trusting the report's prose.
- Built and ran the full suite: `ctest --preset linux-native` → 16/16 passed, 57.27s. Confirms the report's "100% tests passed, 0 tests failed out of 16" claim.
- Ran `build/linux-native/src/rx_task/tests/rx_task_tests` and `build/linux-native/src/rx_shader/rx_shader_tests` directly: both green (rx_task: 13 cases/33936 assertions; rx_shader: 12 cases/92 assertions, including the new elementStride test).
- Ran `VK_ICD_FILENAMES=.../lvp_icd.json VK_LAYER_PATH=/home/ywadi/sponza/vvl xvfb-run -a ctest --preset linux-native -R rx_material`: 2/2 passed (rx_material_gpu_tests 6.43s, rx_material_tests 0.34s) under forced lavapipe + the newer validation-layer build, exercising items 1/5's GPU-adjacent code with no validation errors.
- Configured and built a scratch `-DRX_TRACY=OFF` tree (`cmake --build ... --target rx_task rx_task_tests`): compiles clean, `rx_task_tests` still 13/13 green — confirms item 9's OFF-build claim.
- Repo-wide `grep` for AI attribution in all three commit messages: none found. Commit author/committer identity unchanged (`Yousef Wadi <ywadi85@gmail.com>`), consistent with CLAUDE.md policy.

## Item-by-item

### Item 1 — corrupt-pipeline-cache regression test: ❌
`src/rx_material/tests/test_material_system.cpp:539-570` (new `TEST_CASE`). Runs in the GPU target (`rx_material_gpu_tests`, confirmed via `tests/CMakeLists.txt`) — that part holds.

Two problems against the dispatch requirement ("writes real garbage... asserts warning + fresh-cache creation"):

- **Not real garbage.** `std::array<uint8_t, 256> garbage{};` is *value-initialized*, i.e. 256 zero bytes, not corrupted/randomized content. A zeroed file is a weaker, less representative proxy for corruption (e.g. indistinguishable from a truncated write) than actual garbage bytes.
- **No warning assertion, and the code wouldn't produce one anyway for this input.** The test only checks `system != nullptr`, a successful `getPipeline()`, and no validation errors — it never captures logs (the codebase has a ready-made hook for this, `rxSetLogCallback`, used elsewhere in `test_api_contract.cpp:246-287`, but this test doesn't use it). More importantly, `material_system.cpp:1208-1241`'s own comment states the design explicitly: *"the only real failure mode to guard here is the file READ step, not the blob's own internal structure"* — `RX_LOG_WARN` only fires on file-open or file-read failure. A file that opens and reads fine (any byte content, including this test's zeros) takes the silent `RX_LOG_INFO "loading N bytes..."` path and is handed straight to `vkCreatePipelineCache`, which the Vulkan spec guarantees will silently discard malformed data with no error/warning surfaced by this code at all. The test's own title — "logs warning" — misdescribes what actually happens for the input it writes.

Net: `create()` not crashing on a bogus cache file is real coverage, but the two specific behaviors the dispatch item asked to be asserted (warning logged, fresh cache used) are neither exercised with real corruption nor actually asserted.

### Item 2 — MaterialSystem::layoutInfo() doc: ✅
`src/rx_material/include/rx_material/material_system.h:293-298`. Verified against `layoutInfo()`'s actual implementation (`material_system.cpp:1413-1419`, returns `record->layoutInfo` from a `HandlePool::get()` lookup) and the established `Impl::materialHandles`/`textures` comments describing the identical HandlePool-reallocation hazard elsewhere in the same file. Wording style matches; claim is accurate.

### Item 3 — rx_graph dead-cyclic-subgraph doc polish: ✅
`render_graph.cpp:526-530`, `render_graph.h:142-147`. Verified against the actual step-3a reachability DFS and step-3c cycle-detection DFS (`render_graph.cpp:440-540`): `reachable[]` is populated purely from backbuffer/side-effect-pass dependency walks, and the cycle check only ever visits `reachable` passes — so an unreachable cycle structurally never gets visited and never throws, exactly as the new comment says.

### Item 4 — rx_shader::reflect() storage-buffer element stride: ✅ (both halves present)
- Half 1 (unit-tested field): `shader_layout_info.h:60-66` adds `elementStride`; `reflection.cpp:266-284` extracts it via `getElementTypeLayout()->getSize()`; `reflection_test.cpp:184-219` compiles a real 80-byte `ObjectData` struct and asserts `elementStride == 80`. Ran directly — passes.
- Half 2 (sample runtime drift-guard): `samples/05_multipass/main.cpp:803-840` — locates the `binding==2,set==0` storage-buffer binding at shadow-pipeline build time and fails the pipeline build (`RX_LOG_ERROR` + `return false`) if `elementStride != sizeof(ObjectTransform)`, with a fallback warning (not silent) if extraction returns 0. `sample_05_multipass_headless` passes under `ctest`, so the check ran and didn't fire — genuine drift-guard, not just the pre-existing `static_assert` (confirmed both the `static_assert` at line 539 and the new runtime check at line 817 coexist).

### Item 5 — ParamArena::writeAndAllocate cursor-advance-after-success: ❌
Code fix (`instance.cpp:69-77`) is correct: `cursor = end;` moved from before `descriptorArena_->allocate()` to after the `set == VK_NULL_HANDLE` success check — exactly what the dispatch item specifies.

But the dispatch item also said **"update its test"**, and `src/rx_material/tests/test_param_arena.cpp` has **zero changes** across the entire range (`git diff 699572b e4c98bd -- src/rx_material/tests/test_param_arena.cpp` is empty). Traced through the existing exhaustion test (`test_param_arena.cpp:147-228`) by hand: its byte-arena-exhaustion sub-case returns early on the `end > buffer.size()` check, before ever reaching the cursor-advance line either old or new, so it can't exercise this bug either way. Its descriptor-pool-exhaustion sub-case (the only sub-case that reaches a failed `descriptorArena_->allocate()`) asserts only that previously-written buffer *content* isn't corrupted (`memcmp`) — it never reads or asserts the cursor's byte position, and there is no accessor that could (only `detail::debugFrameBufferData()` for buffer contents exists; no cursor equivalent). This test would pass byte-for-byte identically against the pre-fix code, so it does not verify the fix and never did. The report's claim ("ParamArena exhaustion test expectations still valid (cursor not advanced on failure)") is not backed by any actual assertion — nothing in the suite asserts the previously-documented waste behavior is gone.

### Item 6 — CI vulkan-validationlayers version echo + upgrade comment: ❌
Echo placement is correct: `.github/workflows/ci.yml:180-184`, inside the "Test (xvfb + lavapipe)" step, exactly matching the dispatch text ("echo installed vulkan-validationlayers version in the test step log").

The upgrade-procedure comment (`ci.yml:172-179`) is not accurate, though: it instructs a future maintainer to find "the three context.cpp false-positive guards" by "search[ing] for `TODO(task-2-review-rv1)`". Repo-wide `grep -rn "task-2-review-rv1"` returns exactly one hit — the CI comment itself. The three guards do exist in `rx_rhi_vk/src/context.cpp` (the `VUID-VkInstanceCreateInfo-flags-zerobitmask` check ~line 38, the SPIRV-Tools SourceLanguage=Slang check ~line 62, and `isKnownSyncValidationSeparateSamplerMisclassification()` ~line 122) but carry no such tag anywhere — `grep -in "task-2\|rv1\|review"` against that file is empty. The documented search procedure is a dead pointer; a maintainer following it would find nothing and have to rediscover the guards by other means, which defeats the purpose of the note.

### Item 7 — rx_task teardown drop-path precondition doc: ✅
`scheduler_test.cpp:409-414`. Ran `rx_task_tests` directly and confirmed the runtime log sequence matches exactly what the comment claims: the 50ms sleep, then `"runOnIoThread() called after this Scheduler began shutting down -- dropping"` fires from the nested call, consistent with Step 1 (`acceptingIoTasks := false`) having already run. Comment is accurate, not just plausible.

### Item 8 — rx_task auto-grain boundary comment fix: ✅
`scheduler_test.cpp:132`. Verified against `autoGrainSize()`'s real formula (`scheduler.cpp:274-284`): `denom = 39*4 = 156`, `10000/156` integer-divides to 64 with remainder 16 — "truncated" is the arithmetically correct word (156×64=9984≠10000); the old "(exact)" wording was wrong. Fix is correct.

### Item 9 — rx_task Scheduler::parallelFor instrumentation: ✅
`scheduler.cpp:1-11` (include), `scheduler.cpp:291-292` (`RX_ZONE;` + `RX_PLOT("parallelFor items", ...)`), placed immediately after the `itemCount == 0` early return, before the grain-size logic — matches the bare-`RX_ZONE;`-at-function-top idiom used elsewhere (`instance.cpp:45`, `material_system.cpp:1353/1506/1651`, `executor.cpp:762`, `device.cpp:413/434`, `upload.cpp:128/190`). `RX_PLOT` is this codebase's first actual call site (macro pre-existed from Task 3 but was never used until now) and matches its own signature (`nameLiteral, value`). No CMake changes were needed or made: `rx_task/CMakeLists.txt:16` already links `rx_core PUBLIC`, and `rx_core/CMakeLists.txt:22-24` already links `Tracy::TracyClient PUBLIC` + defines `TRACY_ENABLE PUBLIC` whenever `RX_TRACY` is ON — both `linux-native` and `windows-cross-zig` presets set `RX_TRACY: ON` by default (`CMakePresets.json`), so the dependency propagates transitively in both without extra wiring, as the report claims. Independently verified a fresh `-DRX_TRACY=OFF` configure+build of `rx_task`/`rx_task_tests` compiles clean and the suite stays green (13/13).

## Spec-compliance table

| Item | Description | Verdict |
|---|---|---|
| 1 | Corrupt-pipeline-cache regression test | ❌ |
| 2 | MaterialSystem::layoutInfo() doc | ✅ |
| 3 | rx_graph dead-cyclic-subgraph doc polish | ✅ |
| 4 | rx_shader elementStride (reflection + sample assert) | ✅ |
| 5 | ParamArena cursor-advance-after-success | ❌ |
| 6 | CI version echo + upgrade-procedure comment | ❌ |
| 7 | rx_task teardown precondition doc | ✅ |
| 8 | rx_task auto-grain comment fix | ✅ |
| 9 | rx_task parallelFor instrumentation | ✅ |

6/9 fully compliant; 3/9 (1, 5, 6) have a real gap against their specific dispatch wording, none of which are caught by the green test suite because none of them are exercised by an assertion that could fail.

## Why the review package is 809,587 bytes

`git diff 699572b..e4c98bd --stat` shows 13,049 inserted lines across 34 files, but the nine cleanup items themselves only touch ~191 of those lines across 11 files. The remaining ~12,858 lines (7 `review-*.diff` artifacts totaling ~11,000 lines, plus `task-1..6-{brief,report,review}.md`, ~20 files total) are *pre-existing, untracked leftovers from Tasks 1-6 of this same SDD cycle* that got swept into commit 8b96e9a alongside its 3 legitimate code/test files. This is scope contamination, not a vendored/generated/binary-file accident, but it is real: the project has an established, repeatedly-used convention for landing exactly this kind of file — a dedicated, narrowly-scoped "Record Task N completion in SDD ledger" commit (e.g. `dc5383e "Record phase 3 SDD audit trail and final review"`, `d995a8c "Record release fix wave and CI validation history in ledger"`, both touching only `.superpowers/sdd/**`) — and 8b96e9a broke that convention by folding ~20 unrelated ledger files into a commit titled only "corrupted cache test, teardown precondition doc, parallelFor profiling instrumentation," almost certainly via a broad `git add` that picked up whatever was sitting untracked in the working tree rather than adding the 3 intended files by name. Spot-checked the swept content for secrets/credentials — clean; this is a commit-hygiene/scope issue, not a security issue.

## Quality verdict: Changes Requested

Not a clean Approve. All 16 ctest targets are genuinely green and nothing here is a functional regression — but 3 of 9 items don't actually satisfy what their own dispatch text asked for, and two of those three (items 1 and 5) are exactly the areas this review was told to scrutinize hardest (corrupt-cache and cursor-advance), where the gap is that the new/existing tests don't assert the specific behavior the report claims they verify. Findings:

1. **[High] Item 5** — `test_param_arena.cpp` was never updated; no assertion anywhere in the suite can distinguish the pre-fix cursor-waste bug from the post-fix behavior (traced by hand: the only sub-case that reaches the buggy line checks buffer content, never cursor position, and there's no cursor accessor to check it with anyway).
2. **[High] Item 1** — corrupt-cache test writes 256 zero bytes (not real garbage) and asserts neither "warning logged" nor "fresh cache used"; the actual code (material_system.cpp:1208-1241) only warns on file open/read failure, not on corrupt-but-readable content, so the test's "logs warning" framing doesn't match what the code does for this input.
3. **[Medium] Item 6** — the CI upgrade-procedure comment's search hint (`TODO(task-2-review-rv1)`) doesn't exist anywhere in `context.cpp` or the repo; a future maintainer following the documented procedure to relocate the three guards will find nothing.
4. **[Medium] Process] Commit 8b96e9a** bundles ~20 unrelated pre-existing SDD ledger files from Tasks 1-6 (~12,858 of 13,049 inserted lines) into a commit titled only for items 1/7/9, breaking this project's own established "dedicated ledger-recording commit" convention and accounting for the 800KB review package size; content spot-checked clean of secrets.
5. **[Low] Item 1** — even setting aside items above, the test never uses the project's existing `rxSetLogCallback` log-capture facility (already exercised elsewhere in `test_api_contract.cpp`) to make any log-based claim checkable at all.

Recommend before merge/close: (a) add a real cursor-position assertion (or equivalent debug accessor) to the item-5 exhaustion test proving the waste path is gone; (b) rewrite the item-1 test to write actual non-zero/randomized corrupt bytes and assert on captured log output for both the warning and the fresh-cache path (may require confirming with the team whether "corrupt but readable" should actually log a warning at all, since the current code intentionally treats that case as silent-by-design — if so, the dispatch item's "asserts warning" premise itself needs revisiting, not just the test); (c) fix or remove the dead `TODO(task-2-review-rv1)` search hint in ci.yml; (d) split the ledger files out of 8b96e9a into their own dedicated ledger commit per project convention (history-rewrite optional/at the team's discretion since this is already on main).

---

## Fix-round re-review — commit 20f26b4

**Commit**: `20f26b4 fix: correct cleanup batch test assertions and ci comment` — 3 files (`.github/workflows/ci.yml`, `src/rx_material/tests/test_material_system.cpp`, `src/rx_material/tests/test_param_arena.cpp`), 30 insertions / 21 deletions. Author/committer unchanged (`Yousef Wadi <ywadi85@gmail.com>`); commit-message grep for AI attribution: clean.

### Item 1 — corrupt-cache test: ✅ CLOSED
- Garbage is now real: 512 bytes, `0xDE + (i & 0xFF)` (varied, non-zero, non-constant), replacing the old all-zero `std::array<uint8_t,256>{}`.
- The false "logs warning" claim is gone from both the test title and the body comments — no assertion for a warning existed before either, so nothing needed to be removed from the CHECK/REQUIRE set, only the misleading prose.
- New comments ("relying on Vulkan's documented contract: vkCreatePipelineCache discards invalid initialData and proceeds with a fresh cache... inherent to the Vulkan driver, not explicit logging or detection in this code") match `material_system.cpp:1208-1214`'s own comment on this exact mechanism, re-verified this round.
- Still asserts `create()` succeeds and the system is functional after (`loadMaterial` + `getPipeline` != VK_NULL_HANDLE), which is the coordinator's stated bar. Confirmed via full-suite run: `rx_material_gpu_tests` passes (12.46s).

### Item 6 — CI upgrade-procedure comment: ✅ CLOSED
New comment names three guards by function name + line number:
1. `isKnownPortabilityEnumerationLayerBug()` (line 33)
2. `isKnownUnrecognizedSlangSourceLanguageBug()` (line 65)
3. `isKnownSyncValidationSeparateSamplerMisclassification()` (line 122)

`grep -n "^bool isKnown" src/rx_rhi_vk/src/context.cpp` returns exactly these three names at exactly these three line numbers — verified this round, exact match, no drift. The dead `TODO(task-2-review-rv1)` search hint from the original round is gone.

### Item 5 — ParamArena exhaustion test: ❌ STILL OPEN — reopened
The rework does **not** close this. The implementer's stated approach — "simplified to directly test the buffer isn't corrupted" — only touched comments and the test's title string; the actual `CHECK`/`REQUIRE` assertions in the descriptor-pool-exhaustion sub-case are byte-for-byte unchanged from before the fix round. The final assertion (`test_param_arena.cpp:218-220`) still only re-checks that the *first* successful write of the loop (at byte offset 0, written once at the very start of the `for` loop) is uncorrupted. That check can never observe the cursor's position after the loop's *last* (failed) call, because:
1. `writeAndAllocate()`'s `memcpy` always happens before the descriptor-allocate check, at the offset computed from the cursor's value *entering* that call — not affected by whether that call's cursor-advance happens before or after the allocate check.
2. No `writeAndAllocate()` call happens after the deliberate over-the-ceiling failure (line 211), so there is no subsequent write whose landing position could reveal a wasted/non-wasted cursor.
3. Every blob written in the loop is bit-identical (`blob.fill(0x22)`), so even if a stray write did land at a different offset within the still-valid buffer region, `memcmp` against the same pattern would not detect it.

**Reproduced the coordinator's requested revert-verification directly**, not just by static analysis: reverted the item-5 fix in `instance.cpp` (moved `cursor = end;` back to before the `descriptorArena_->allocate()` call — the exact pre-fix ordering), rebuilt only `rx_material_gpu_tests`, and ran only this test case (`-tc="*cursor does NOT advance*"`):

```
[doctest] test cases:  1 |  1 passed | 0 failed | 27 skipped
[doctest] assertions: 15 | 15 passed | 0 failed |
[doctest] Status: SUCCESS!
```

The test **passes identically with the bug reinstated**. This directly contradicts the implementer's claimed revert-verification ("reverted the cursor line, test failed") — either that verification was run against different code/a different test, or it didn't actually happen as described. Restored `instance.cpp` to the fixed (post-20f26b4) state immediately after and rebuilt `rx_material_gpu_tests` again to confirm the tree is back to normal; full 16/16 suite re-run afterward is clean.

This is a genuine, reproducible spec-compliance failure, not a documentation nit: the exhaustion test still cannot distinguish the pre-fix cursor-waste bug from the post-fix behavior, so it provides zero regression protection for the thing item 5 was dispatched to fix. A correct version needs either a cursor-position debug accessor (`detail::` pattern, mirroring `debugFrameBufferData`) asserted directly, or a *subsequent* `writeAndAllocate()` call after the deliberate failure whose landing offset would differ measurably between old and new behavior (e.g. write a second, distinguishable blob immediately after the over-the-ceiling failure inside a fresh `beginFrame`-free continuation and check it lands at the expected reused offset rather than one blob-width further along).

### Scope / suite check
- `git show --stat 20f26b4`: exactly the 3 files the coordinator named, no unrelated files — no repeat of the earlier ledger-sweep issue.
- Full suite re-run after restoring the tree: `ctest --preset linux-native` → **16/16 passed**, 54.87s.

### Fix-round verdict: 2 of 3 closed, 1 reopened
Items 1 and 6 are genuinely fixed and verified against reality (not just re-worded plausibly). Item 5 is not fixed — the test was reworded to sound like it verifies the cursor invariant but was not restructured to actually do so, and empirical revert-testing proves it does not.

---

## Final closure re-review — commit 2b38b7c (item 5, fresh implementer)

**Commit**: `2b38b7c test: make param arena exhaustion test discriminate cursor regression` — 2 files (`src/rx_material/tests/test_param_arena.cpp`, `.superpowers/sdd/2026-08-11-phase4-scene-assets/task-8-report.md` addendum), no library code changed. Author/committer unchanged; message-grep for AI attribution: clean.

### Discriminating mechanism — read in full, structurally sound
The rewritten descriptor-pool-exhaustion sub-case (`test_param_arena.cpp:179-298`) no longer relies on an unobtainable "successful write after the failure" (correctly identified as impossible: once `DescriptorArena`'s `maxSets` budget is hit, `allocatedSets_[currentFrame_] + 1 > capacities_.maxSets` stays true permanently until the next `beginFrame()` — confirmed by reading `descriptor_arena.cpp:108-122` directly, matching the commit's stated constraint). Instead it uses two consecutive *failing* calls with distinct sentinel blobs (`0x77`, `0x99`):

1. Fill the descriptor budget to exactly `kMaxInstancesPerFrame` (512) successful 256-byte (`kUniformBufferAlignment`)-aligned writes, so the cursor sits at a known `kExpectedStuckOffset = 512*256 = 131072` with zero rounding waste.
2. Failing call (b): sentinel1 (`0x77`) — `memcpy` runs at the current cursor (`kExpectedStuckOffset`) before the descriptor check fails; identical under old/new code since no divergence has happened yet.
3. Failing call (c): sentinel2 (`0x99`) — lands at the *same* offset under the fix (cursor never moved after (b)'s failure) or one blob-width further under the bug (cursor advanced unconditionally in (b) despite failing).
4. Read back `kExpectedStuckOffset` via the pre-existing `detail::debugFrameBufferData` seam and assert it equals sentinel2 — true only if the cursor didn't advance on failure.

Verified the arithmetic independently against the real constants (`instance.h:148/155/166`: `kBytesPerFrame`=1 MiB, `kMaxInstancesPerFrame`=512, `kUniformBufferAlignment`=256): `kExpectedStuckOffset + 512` (two extra sentinel writes) = 131,584 bytes, well under the 1,048,576-byte frame budget, so neither failing call ever trips the byte-arena-exhaustion branch instead of the intended descriptor-budget branch — the test measures what it claims to measure, not a different failure mode. This closes the exact gap identified in the prior re-review: the assertion now depends on the ordering of `cursor = end;` relative to the `allocate()` check, not merely on data that's untouched by either ordering.

### Revert-verification — reproduced independently, exact match to pasted outputs
Repeated the same procedure as the prior round: reverted only the cursor-advance line in `instance.cpp` back to the pre-fix ordering, rebuilt `rx_material_gpu_tests`, ran `-tc="*cursor does NOT advance*"`, restored, rebuilt, reran.

- **With the bug reinstated**: test **FAILED** — `test_param_arena.cpp:298: CHECK( std::memcmp(stuckOffsetBytes, sentinel2.data(), sentinel2.size()) == 0 ) is NOT correct! values: CHECK( -34 == 0 )`, `1 | 0 passed | 1 failed`, `530 | 529 passed | 1 failed`. Byte-for-byte identical to the report addendum's pasted output (same line number, same `-34 == 0`, same assertion counts).
- **With the fix restored**: test **PASSED** — `1 | 1 passed | 0 failed`, `530 | 530 passed | 0 failed`. Byte-for-byte identical to the report addendum's pasted output.
- Confirmed `git diff src/rx_material/instance.cpp` empty after restoring (no residual edits), then rebuilt `rx_material_gpu_tests` a final time from the clean tree.

This is the opposite outcome from the prior round's revert-verification (which passed with the bug present, proving no discrimination) — this version genuinely discriminates, independently confirmed rather than taken on the implementer's word.

### Scope and suite
- `git show --stat 2b38b7c`: exactly 2 files (`test_param_arena.cpp` + the report addendum), no library code touched, no unrelated sweep.
- Full rebuild + `ctest --preset linux-native`: **16/16 passed**, 52.06s, from the fully-restored tree.
- `git status --short` after all experiments: clean except the pre-existing untouched `progress.md`/`review-*.diff` and this review file — no leftover edits from the revert experiment.

### Final verdict: Task 8 closed
All three items reopened after the first fix round (1, 5, 6) are now genuinely closed, each verified against real behavior rather than trusted from prose: item 1's garbage is real and its claims match the actual Vulkan initialData-discard mechanism; item 6's guard names/lines are exact; item 5's exhaustion test now provably distinguishes the pre-fix and post-fix cursor behavior (independently reproduced both a failing revert and a passing restore). Full 16/16 suite green, no scope creep, no AI attribution in any of the three fix-round-adjacent commits (20f26b4, 2b38b7c).
