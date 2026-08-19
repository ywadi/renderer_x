# Closure-sweep report — no-deferral conversions (Stage 1)

Base commit `55410f0`. All five items closed, one commit each, in this
order:

| Item | Commit | Subject |
|---|---|---|
| 1 | `b1a304f` | fix(rx_rhi_vk): make GPU test binaries fail on teardown-time validation errors |
| 2 | `27926ec` | fix(third_party): patch vendored MikkTSpace shift-by-32 UB (mikktspace.c:1667) |
| 3 | `25bf348` | fix(samples): wire FrameSync::advanceFrame(Allocator\*) into sample 04's frame loop |
| 4 | `1d07330` | test(rx_asset): combined glTF-import -> TextureCache -> pixel test (Task 14 minor 4.6) |
| 5 | `5a10901` | ci: add restore-keys cache fallback and per-ref concurrency serialization |
| docs | `ffff2f5` | closure-sweep report (original five-item evidence) |
| fix-round Critical | `b6f3e2f` | fix(third_party): make mikktspace PATCH_COMMAND idempotent across reconfigures |
| fix-round minor | `6468294` | ci: prune stale .deps-cache entries before save |
| fix-round docs | `3dfaf31` | closure-sweep fix-round evidence |
| micro-item | `d2aab5d` | ci: fail-safe both prune steps against an empty deps-cache manifest |

Final CI-equivalent gate, run after all five original commits (see the
"Final re-verification after the fix round" section below for the gate
re-run after the two fix-round commits above):

- linux-native: full `cmake --build --preset linux-native` clean, then
  `ctest --preset linux-native --output-on-failure` — **20/20 passed**,
  106.36s total.
- windows-cross-zig: full `cmake --build --preset windows-cross-zig`
  clean (147/147 targets), then the exact CI ctest invocation
  (`ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample' --output-on-failure`)
  under `xvfb-run -a` + Wine — **10/10 passed**, 108.00s total.

All five commits verified author `Yousef Wadi <ywadi85@gmail.com>` (local
git config), zero AI-attribution strings in any commit message.

---

## Item 1 — Harness gap: destructor-time validation errors escape the exit code

**Root cause.** `rx::rhi::Context::errorCount_` (`context.h`/`context.cpp`)
is a per-`Context` `shared_ptr<int>`, fresh per `Context::create()` call.
Every GPU test fixture's `CHECK_FALSE(context.hasValidationErrors())`
reads that per-instance counter, but the check line runs *before* the
fixture's own destructor chain — any validation error raised while
tearing down the fixture's other members (after the `CHECK_FALSE` line,
still inside the same `TEST_CASE` scope) was never re-checked, and once
the fixture (and its `Context`) is destroyed the counter is gone too. The
binary still exited 0.

**Fix.** Added a process-lifetime tally,
`Context::processValidationErrorCount()`, backed by a single
`std::atomic<std::size_t>` in `context.cpp`'s anonymous namespace, fed by
the *same* real-error branch of `debugCallback()` that already increments
a `Context`'s own `errorCount_` (the known-false-positive filter list —
`isKnownPortabilityEnumerationLayerBug()` and its three siblings — is
untouched and stays authoritative). Every GPU test binary's `main()`
(`doctest_main.cpp`/`doctest_main_gpu.cpp`/`doctest_main_gltf_gpu.cpp`
across `rx_rhi_vk`, `rx_graph`, `rx_material`, `rx_asset`'s two GPU
binaries — 5 files total) now reads that tally *after*
`doctest::Context::run()` returns, i.e. after every fixture including the
last one has been fully destroyed, and forces the process exit nonzero
if it is ever nonzero, printing a named `FAILED` line.

**Happy-path verification** (all 5 binaries, `--validate`, real Vulkan
device):

```
rx_rhi_vk_tests:        64 test cases, 1738 assertions, SUCCESS, exit 0
rx_graph_gpu_tests:      8 test cases,  614 assertions, SUCCESS, exit 0
rx_material_gpu_tests:  32 test cases,  978 assertions, SUCCESS, exit 0
rx_asset_tests:         30 test cases,  446 assertions, SUCCESS, exit 0
rx_asset_gltf_gpu_tests:57 test cases, 8644419 assertions, SUCCESS, exit 0
```

**Deliberate regression proof (scratch worktree, not shipped).** Built a
`git worktree` at commit `b1a304f` (this item's own fix already applied),
reverted `TextureCache::~TextureCache()`'s `[G6]` sampler-cache cleanup
loop (`src/rx_asset/texture_cache.cpp`) to an empty body — the exact
teardown-time leak class this item's evidence cites
(`VUID-vkDestroyDevice-device-00378`) — rebuilt `rx_asset_tests` only,
and ran it:

```
[doctest] test cases:  30 |  30 passed | 0 failed | 0 skipped
[doctest] assertions: 446 | 446 passed | 0 failed |
[doctest] Status: SUCCESS!
[rx_asset_tests] FAILED: 6 unfiltered Vulkan validation error(s) observed during this run (possibly raised during a fixture's own
teardown, after that TEST_CASE's CHECK_FALSE(hasValidationErrors()) already ran) -- see the "[vulkan validation]" ERROR line(s) above.
```

Exit code: **1** (confirmed via a separate un-piped run: `echo $? -> 1`).
`grep -c VUID-vkDestroyDevice-device-00378` on the captured log: **6**,
matching the reported tally exactly — every individual `TEST_CASE`'s own
`CHECK_FALSE` still passed (doctest itself says `SUCCESS!`), and the
binary *still* exits nonzero because of the new post-run check. This is
the exact defect the brief's evidence describes ("reviewer reproduced
`VUID-vkDestroyDevice-device-00378` 3x with a green exit") — now closed.
Worktree removed afterward (`git worktree remove --force`); no trace left
in the main tree (only the build-generated `volk.c` object was touched
there, not a tracked file).

---

## Item 2 — MikkTSpace vendored UB patch

**Reproduction (before).** Configured a throwaway build
(`build/ubsan-mikkt`, not committed) with
`CMAKE_C_FLAGS="-fsanitize=undefined -O1 -g -fno-omit-frame-pointer"`
(matching Task 15's own UBSan recipe) and ran the full
`rx_asset_gltf_gpu_tests` suite (`--test-case-exclude="*WALL-CLOCK*"`):

```
thread 751619 panic: shift exponent 32 is too large for 32-bit type 'unsigned int'
_deps/mikktspace-src/mikktspace.c:1667:21: 0x13c6773 in QuickSortEdges (.../mikktspace.c)
	t=(uSeed<<t)|(uSeed>>(32-t));
                    ^
_deps/mikktspace-src/mikktspace.c:1697:3: ... (7 recursive QuickSortEdges frames)
_deps/mikktspace-src/mikktspace.c:1517:2: 0x13bad2c in BuildNeighborsFast (...)
_deps/mikktspace-src/mikktspace.c:307:2: 0x13b5b9b in genTangSpace (...)
src/rx_asset/mikktspace_bridge.cpp:101:5: 0x139e7e6 in generateTangentsDeindexed (...)
```

Under zig's trap-based sanitizer this panic is **fatal** (not merely a
printed diagnostic) — it aborted the whole process mid-suite
(`test cases: 5 | 4 passed | 1 failed ... FATAL ERROR: test case CRASHED: SIGABRT`,
`timeout: the monitored command dumped core`), confirming the class of
real-world consumer risk, not a cosmetic warning.

**Root cause.** `t = uSeed & 31` (so `t ∈ [0,31]`) followed by
`t = (uSeed<<t) | (uSeed>>(32-t))` — a naive rotate-left. When `t==0`,
`32-t == 32`, and a 32-bit shift by 32 is undefined behavior in C (out of
range for the promoted operand's width). On every target this project
actually builds for (x86-64, native and windows-cross-zig), the
underlying shift instruction already masks its count to 5 bits in
hardware, so the *original* expression already evaluated to
`uSeed>>0 == uSeed` at runtime whenever `t==0` — i.e. the correct
rotate-left-by-0 identity was already the observed behavior; only its
formal legality was wrong.

**Fix.** Upstream (`github.com/mmikk/MikkTSpace`, pinned
`3e895b49d05ea07e4c2133156cfa94369e19e409`) has had no commits since
2020-03-25 and no tags/releases, so patched locally. Since MikkTSpace is
`FetchContent_Populate`'d (not physically vendored into the repo tree,
matching this project's existing volk/VMA/stb convention), the patch had
to be durable across a fresh fetch, not a one-off edit of the populated
build-tree copy. Added a `PATCH_COMMAND patch -p1 -i
<repo>/third_party/patches/mikktspace-ubsan-shift-fix.patch` to the
existing `FetchContent_Declare(mikktspace ...)` call
(`third_party/CMakeLists.txt`) — CMake/ExternalProject's own standard
patch mechanism, applied once per population, no hand-rolled patching
logic. The patch file carries the `// [RendererX local patch, 2026-08-19]`
comment block inline (UBSan citation, root-cause explanation, upstream
status) plus the one-line fix:
`t = (t==0) ? uSeed : ((uSeed<<t)|(uSeed>>(32-t)));`

Verified end-to-end: wiped the populated `_deps/mikktspace-src` in both
`build/linux-native` and the throwaway UBSan build dir, reconfigured
(triggering a fresh clone + patch), and confirmed
`grep "RendererX local patch" .../mikktspace.c` finds the patched line in
the freshly-fetched copy in both build trees, and again after a full
`windows-cross-zig` reconfigure (three independent re-populations, all
patched correctly).

**Behavior-preservation (byte-identical).** Added a temporary bit-exact
dump (`std::memcpy` a `float` into a `uint32_t`, printed as hex) to two
of the existing tangent tests in `gltf_pipeline_test.cpp`
("tangent-less-but-UV'd primitive generates tangents via MikkTSpace" —
the 6-corner shared-edge quad — and "a simple unit-square triangle
produces a sane tangent aligned with +U" — a single triangle), captured
BEFORE the patch (unpatched vendored source) and AFTER (patched), and
diffed:

```
$ diff /tmp/mikkt_before_dumps.txt /tmp/mikkt_after_dumps.txt && echo "TANGENT OUTPUT IS BYTE-IDENTICAL BEFORE/AFTER PATCH"
TANGENT OUTPUT IS BYTE-IDENTICAL BEFORE/AFTER PATCH
```

(16 tangent components across both fixtures, every one bit-for-bit
identical: `0x3f800000`/`0x00000000` pairs, i.e. exact `1.0F`/`0.0F`.)
The device-free `rx_asset_gltf_tests` suite: 48/48 test cases, 292/292
assertions, both before and after. The debug dump was reverted
(`git checkout`) before committing — not part of the shipped diff.

**Re-run UBSan after the patch (full `rx_asset_gltf_gpu_tests` suite,
same recipe, same exclusion):**

```
[doctest] test cases:     56 |     56 passed | 0 failed | 1 skipped
[doctest] assertions: 590906 | 590906 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Zero UBSan findings (`grep -c "runtime error\|panic: shift\|mikktspace"` on
the captured log: 0), exit 0 — and the suite now *completes* (56/56)
where it previously aborted mid-run. Also re-confirmed on the ordinary
(non-sanitizer) `linux-native` preset: `rx_asset_gltf_gpu_tests` 899,855
assertions, 0 failed.

---

## Item 3 — FrameSync::advanceFrame(Allocator\*) live wiring

**Fix.** Sample 04 (`samples/04_streaming/main.cpp`) is the
streaming/eviction sample — the natural real consumer of a live memory
report. Both its frame loops (`runHeadless()` and `runPresent()`, the two
existing `frameSync->advanceFrame();` call sites) now pass `&*allocator`,
matching `memory_budget_test.cpp`'s own established
`frameSync->advanceFrame(&*allocator)` idiom for the mechanism test this
closes the gap for.

**Headless-gate verification.** Added the same discriminating check that
test already established — `Allocator::setCurrentFrameIndexCallCount()`
must equal exactly `kHeadlessTotalFrames` (60), never a comparison of
`report()`'s own budget/usage numbers, which a quiet driver can report
identically whether or not the refresh call ever fired. Real run:

```
[info] sample_04_streaming: FrameSync-driven budget refresh live -- 60 setCurrentFrameIndex() call(s), max heap budget 50494949376 bytes (RealExtension)
[info] streaming headless gate PASSED
```
Exit 0.

**Discrimination proof (temporary local revert, restored after).**
Reverted just the headless call site back to the bare `frameSync->advanceFrame();`
(no argument), rebuilt, ran:

```
[error] sample_04_streaming: FrameSync::advanceFrame(Allocator*) budget-refresh wiring fired 0 time(s), expected exactly 60 (one per rendered frame)
[info] sample_04_streaming: FrameSync-driven budget refresh live -- 0 setCurrentFrameIndex() call(s), max heap budget 50494949376 bytes (RealExtension)
[error] streaming headless gate FAILED
```
Exit code (un-piped): **1**. Note the max-heap-budget number stayed
identical (50494949376) with 0 calls — direct, empirical confirmation
that report()'s own budget number is NOT a discriminating signal on this
runner (a quiet driver reports the same number either way), and that
`setCurrentFrameIndexCallCount()` — the assertion actually used — is what
catches the regression. File restored from backup, rebuilt, re-confirmed
PASSED/exit 0 before committing.

---

## Item 4 — Combined glTF→TextureCache→pixel test (Task 14 minor 4.6)

**Fix.** Added one `TEST_CASE` to `texture_cache_test.cpp` (joins the
`rx_asset_tests` binary, which already has the rendering/readback
machinery this reuses verbatim — `buildQuadPipeline()`/
`renderAndReadbackQuadrants()`/`approxEqual()`) combining, unmodified:

- `import_gltf_basisu_test.cpp`'s own `Registry::importGltf(pool,
  scheduler, textures)` call shape (`GeometryPool::create()` +
  `rx::task::Scheduler::create()` + a real `TextureCache`), against the
  already-committed `assets/test/cube_basisu.gltf` fixture
  (`KHR_texture_basisu`, references `cube_basisu_misleading_normal.ktx2`
  as its material's `baseColorTexture`).
- This file's own `renderAndReadbackQuadrants()` rendering path, sampling
  the resolved `TextureRecord::bindlessIndex` through a real bindless
  descriptor set.

Zero new rendering/readback/import machinery — only two new `#include`s
(`rx_asset/geometry_pool.h`, `rx_asset/registry.h`, `rx_task/scheduler.h`)
and a small `gltfFixturePath()` helper (duplicating
`import_gltf_basisu_test.cpp`'s own `testAssetDir()`, this codebase's
established per-file fixture convention). No CMakeLists.txt changes were
needed: `rx_asset_tests` already transitively links `rx_task`/`fastgltf`
through `rx_asset`'s own `PUBLIC` dependency on both.

**Fixture content (previously undocumented — no generator script records
it).** A scratch probe run of the exact combined call chain (kept only
long enough to read the values, then replaced with the real assertions)
found `cube_basisu_misleading_normal.ktx2` decodes to the SAME
`TL=red/TR=green/BL=blue/BR=yellow` quadrant layout the other KTX2-only
quadrant fixture already uses: `253,0,0` / `0,253,0` / `0,0,253` /
`255,255,0` (within tolerance 2 of each primary color).

**Result:**

```
record.width=4 record.height=4 bindlessIndex=4   (non-fallback, real texture)
CHECK(approxEqual(readback->topLeft,     {255,   0,   0}, 2))  -- pass
CHECK(approxEqual(readback->topRight,    {  0, 255,   0}, 2))  -- pass
CHECK(approxEqual(readback->bottomLeft,  {  0,   0, 255}, 2))  -- pass
CHECK(approxEqual(readback->bottomRight, {255, 255,   0}, 2))  -- pass
```

Full `rx_asset_tests` binary: **31/31 test cases, 469/469 assertions,
SUCCESS**, exit 0 (up from 30/30 pre-existing).

**Discrimination proof (temporary edit, restored after).** Swapped the
rendered bindless index for `checkerboardHandle()`'s own (the D11
fallback texture) instead of the resolved real texture's index, rebuilt,
ran:

```
ERROR: CHECK( approxEqual(readback->topLeft, {255, 0, 0}, 2) ) is NOT correct!
ERROR: CHECK( approxEqual(readback->topRight, {0, 255, 0}, 2) ) is NOT correct!
ERROR: CHECK( approxEqual(readback->bottomLeft, {0, 0, 255}, 2) ) is NOT correct!
ERROR: CHECK( approxEqual(readback->bottomRight, {255, 255, 0}, 2) ) is NOT correct!
[doctest] test cases:  1 |  0 passed | 1 failed | 30 skipped
```

All four quadrant assertions fail loud — confirms the test is
discriminating, not vacuous. Reverted from backup before committing.

Also confirmed passing under the `windows-cross-zig` Wine ctest run
(`rx_asset_tests` is one of the 10 tests exercised there, not in the
GPU-exclusion list) as part of the final CI-equivalent gate.

---

## Item 5 — CI cache hardening (ci.yml)

**(a) restore-keys fallback.** Added a same-prefix `restore-keys:` entry
to all four `.deps-cache`/`assets/fetched` cache steps (both jobs) — the
two steps the brief calls out by name; the `slang-prebuilt` cache was
deliberately left alone (its key only rotates on `fetch_slang.cmake`
edits, which are rare, unlike `third_party/CMakeLists.txt`/
`fetch_assets.sh` edits, which are common).

Verified the fallback is safe (a stale seed cannot serve an incorrect
build), not merely assumed:
- `.deps-cache`: read `cmake/DepCache.cmake`'s
  `rx_add_cached_dependency()` directly — each dependency's own
  subdirectory is keyed independently on `SHA256(name|tag|triple|
  zig-version|build-type|CMAKE_ARGS|config-file-hashes)`, and the build
  only ever consults the exact subdirectory matching the *current* hash
  (`if(NOT EXISTS "${_marker}")` — a `[dep-cache] MISS`, i.e. a real
  rebuild, exactly like a cold cache, for any dependency whose hash
  changed). A restored stale cache's unrelated/outdated subdirectories
  are simply never looked up. This mechanism can only ever skip
  rebuilding dependencies that provably did not change; it cannot serve
  a stale build for one that did.
- `assets/fetched`: read `tools/fetch_assets.sh` directly — each asset's
  marker file is VERSION-named (`.rx-fetched-damagedhelmet-<VERSION>`),
  so a stale restore either has the current version's marker (fast
  no-op, correct) or doesn't (re-fetches and re-verifies checksums from
  scratch, since the version-specific filename simply isn't found).

**(b) concurrency serialization.** Added a workflow-level `concurrency`
block:

```yaml
concurrency:
  group: ci-${{ github.ref }}
  cancel-in-progress: true
```

with an inline comment stating the `cancel-in-progress: true` choice
explicitly: an outdated commit's run has no value once a newer push to
the same ref exists, so cancelling frees the runner instead of finishing
a run nobody needs (per the brief's own sanctioned rationale).

**Syntax validation.** `actionlint` was not preinstalled; downloaded the
prebuilt release binary (v1.7.12, `github.com/rhysd/actionlint`) and ran
it directly against `ci.yml`:

```
$ actionlint .github/workflows/ci.yml
$ echo $?
0
```

Zero findings, both before this change (baseline `HEAD:ci.yml`, sanity
check) and after (the edited file) — confirms this is real schema-aware
validation, not a vacuous pass. Also parsed with PyYAML as a basic
structural sanity check (parses cleanly, `concurrency` block present with
the intended keys).

**Not pushed** (per global constraints) — coordinator to confirm via
`gh workflow view`/a real run after push, per the brief's own
instruction.

---

# Fix round — review findings

Independent review approved items 1/3/4/5 with re-verification (harness-
gate revert reproduced exit-1 on the reviewer's own hardware; sample-04
numbers matched exactly; the combined test confirmed genuinely executing
with per-quadrant asserts; DepCache/fetch_assets staleness-safety proven
structurally). One Critical (item 2's `PATCH_COMMAND` not idempotent
across reconfigures) and one minor (`.deps-cache` unbounded accumulation
under the new `restore-keys` fallback) were sent back. Both closed below.

Commits: `b6f3e2f` (Critical), `6468294` (minor).

## Critical — item 2's PATCH_COMMAND is not idempotent

**Reproduction** (exactly as the reviewer described — no manual state
tampering): on an already-configured `linux-native` tree with item 2's
original commit (`27926ec`) in place, a second, completely ordinary
`cmake --preset linux-native` failed:

```
[0/7] Performing update step for 'mikktspace-populate'
[2/7] Performing patch step for 'mikktspace-populate'
FAILED: .../mikktspace-populate-stamp/mikktspace-populate-patch
cd .../mikktspace-src && patch -p1 -i .../mikktspace-ubsan-shift-fix.patch && ...
patching file mikktspace.c
Reversed (or previously applied) patch detected!  Assume -R? [n]
Apply anyway? [n]
Skipping patch.
1 out of 1 hunk ignored -- saving rejects to file mikktspace.c.rej
ninja: build stopped: subcommand failed.

CMake Error at /usr/share/cmake-3.22/Modules/FetchContent.cmake:1087 (message):
  Build step for mikktspace failed: 1
```

**Root cause.** FetchContent's underlying ExternalProject-style UPDATE
step reruns on every fresh `cmake` process (the `mikktspace_POPULATED`
guard in `third_party/CMakeLists.txt` is an in-memory global property
that resets on every new `cmake` invocation — only the on-disk
ExternalProject stamp files persist), and a rerun UPDATE step
unconditionally reruns PATCH too. Confirmed this is NOT unique to
mikktspace: reconfiguring with `-DFETCHCONTENT_QUIET=OFF` (which
suppresses normal populate-step output by default — confirmed via
`grep -n FETCHCONTENT_QUIET /usr/share/cmake-3.22/Modules/FetchContent.cmake`)
shows every OTHER `FetchContent_Populate`'d dependency in this file
(`doctest`, `glm`, `volk`, `vma`, `stb`) silently repeating
`"Performing update step"` / `"No patch step"` on every single
reconfigure too — they were never actually broken by this only because
none of them carries a `PATCH_COMMAND`, so there is nothing to reapply;
`git checkout` of an already-checked-out pinned commit is a genuine
no-op. mikktspace is the ONLY dependency anywhere in this repository with
a `PATCH_COMMAND` (confirmed: `grep -rn PATCH_COMMAND third_party/
CMakeLists.txt` — one match, the mikktspace declaration itself).

**Fix.** Added `UPDATE_DISCONNECTED TRUE` to the mikktspace
`FetchContent_Declare` — CMake's own documented mechanism for a
commit-hash-pinned dependency that is never expected to move: it makes
the update step run only as part of the initial population (nothing to
update yet, so download+patch proceed normally), then skips it entirely
on every later configure. Not `patch -N` (tested: `patch -p1 -N -i
<file>` on the already-patched tree still exits nonzero on this GNU
patch version, `patch: **** malformed patch...` / reject, not a clean
skip) and not a `|| true` swallow (would silently hide a genuinely
broken patch on a real content change).

**Verification matrix, both presets, all sequential foreground runs (no
backgrounding, per the coordinator's process correction after an
earlier attempt raced two concurrent background `cmake` invocations and
produced one unrelated, environment-specific "compiler path changed"
failure unrelated to this fix — resolved by wiping and re-running
sequentially; see "Process note" below):**

| Step | linux-native | windows-cross-zig |
|---|---|---|
| (a) fresh configure, wiped build dir | pass — patch applied once (`[1/8] download → [3/8] patch`, step 2/update genuinely absent from the DAG under UPDATE_DISCONNECTED) | pass — identical sequence |
| (b) second ordinary configure, no changes | pass — `ninja: no work to do.` for mikktspace, patch still applied, no `.rej` | pass — reproduced 3 consecutive times after the process correction (`ninja: no work to do.` every time) |
| (c) third configure after a comment-only edit to `third_party/CMakeLists.txt` | pass — patch still applied, no `.rej` | pass — patch still applied, no `.rej` |
| (d) full build | 149/149 targets clean | 147/147 targets clean |
| (e) other FetchContent deps | see below | see below |

(e) **Other FetchContent deps' exposure.** `doctest`/`glm`/`volk`/`vma`/
`stb` share the identical structural exposure (silent update-step rerun
on every configure, confirmed above) but are NOT broken by it and did
NOT need fixing: none carries a `PATCH_COMMAND` or any other
non-idempotent step (confirmed both by `grep -rn PATCH_COMMAND` across
the whole repo — one match, mikktspace's own, now fixed — and by dozens
of successful reconfigures across this whole task with zero failures for
any of them). `rx_add_cached_dependency`-based deps (spdlog,
Vulkan-Headers, SDL3, vk-bootstrap, enkiTS, tracy, fastgltf,
meshoptimizer, draco, ktx) are structurally outside this exposure class
entirely — `cmake/DepCache.cmake`'s own `.rx-built` marker check gates
the ENTIRE clone/build/install sequence, so an already-built dependency
does zero work of any kind on reconfigure (confirmed directly: these
never appear in the FETCHCONTENT_QUIET=OFF trace at all, only
`[dep-cache] HIT` messages).

**Process note (non-blocking, for the record).** One reconfigure attempt
right after the coordinator's "stop backgrounding" correction reproduced
the reversed-patch failure on windows-cross-zig's SECOND configure, even
with the fix in place. Investigated rather than dismissed: this
coincided with a stray earlier background `cmake --preset
windows-cross-zig` run that had itself failed with an unrelated
"CMAKE_C_COMPILER changed, cache must be deleted" error (traced to this
sandbox exposing the same physical repository at two bind-mounted paths,
`/media/ywadi/second/renderer_x` and `/home/ywadi/d2/renderer_x` --
confirmed same device+inode via `stat`) after having been launched
CONCURRENTLY alongside the linux-native background configure — a race
the coordinator's process correction was specifically meant to
eliminate. A full wipe (`rm -rf build/windows-cross-zig`) plus a
carefully sequential, single-process reproduction of the exact same
(a)/(b)/(c)/(d) sequence immediately afterward succeeded cleanly and
repeatably (3 consecutive clean second-configures, shown in the table
above) — the fix itself is sound; the one anomalous failure is
attributed to leftover state from concurrent background execution, not
a flaw in UPDATE_DISCONNECTED.

## Minor — .deps-cache unbounded accumulation under restore-keys

**Problem.** The `restore-keys` fallback (item 5(a), original sweep)
means a cache restore can carry forward hash-keyed subdirectories from
an older configure that the current one no longer references (a removed
dependency, a re-pinned tag, a changed `CMAKE_ARGS`/toolchain hash).
Nothing previously deleted them, so the saved `.deps-cache` cache entry
would grow monotonically across many `third_party/CMakeLists.txt` edits,
eventually approaching GitHub Actions' 10GB total cache limit.

**Fix (pruning, not merely documenting — closed per the no-deferral
policy).** `cmake/DepCache.cmake`'s `rx_add_cached_dependency()` now
writes every key it resolves this configure to
`${CMAKE_BINARY_DIR}/deps-cache-manifest.txt` (build-dir-local, never
itself cached/restored — always freshly and completely regenerated by a
real configure). Safety argument: this function only reaches the point
of recording a key AFTER computing it; if that dependency's own
clone/build/install subsequently fails, `FATAL_ERROR` aborts the whole
configure (and the job) before the manifest is ever read downstream — so
by construction, every key present in a manifest that survives to the
prune step names a dependency that is genuinely built and installed.
`ci.yml` adds a "Prune stale dependency cache entries" step (both jobs,
immediately after Configure) that deletes any `.deps-cache/` subdirectory
NOT listed in that job's own manifest, before the cache-save (an
implicit post-job step of the existing "Cache third-party dependency
builds" step) uploads whatever remains.

**Verification.** Dry-run against this session's own real,
locally-accumulated `.deps-cache` (114 subdirectories, 1.8GB, built up
by many manual reconfigures across this whole closure-sweep task):

```
=== .deps-cache dirs BEFORE prune ===
114
1.8G	.deps-cache
=== manifest (authoritative current set) ===
spdlog-e2c63655474c23af
Vulkan-Headers-e7a4bc77264ec07b
SDL3-4b365ff3c972099c
vk-bootstrap-7a3bfdb6b0730102
enkiTS-cb6775641402d1e2
tracy-d3259c11d7efada2
fastgltf-555a6d6653d57030
meshoptimizer-fe36010e0c110beb
draco-a43087e6ee3db89f
ktx-6e7e780595aa2b62
would remove: 104, would keep: 10
```

The 10 "keep" entries matched the manifest exactly, byte for byte; the
104 "remove" entries were every subdirectory NOT in it. This was a
**dry run only** — the destructive `rm -rf` was deliberately not executed
against the real local `.deps-cache`, since this checkout's cache
directory is shared between BOTH presets locally (unlike CI, where each
job restores its own isolated `deps-cache-<preset>-*` cache entry into a
fresh checkout) — pruning by only the linux-native manifest here would
have incorrectly deleted windows-cross-zig's own currently-needed
entries, a local-environment artifact that does not apply to CI's actual
per-job isolation. `actionlint` (1.7.12) reports zero findings against
the final `ci.yml` with both prune steps added.

## Final re-verification after the fix round

- linux-native: full `cmake --build --preset linux-native` clean
  (149/149 targets — 2 more than the original sweep's 147, since the
  DepCache.cmake edit invalidated every dependency's own cache key one
  time, forcing a full one-time dependency rebuild cascade, expected and
  documented behavior per that file's own header comment), then
  `ctest --preset linux-native --output-on-failure` — **20/20 passed**,
  101.40s total.
- windows-cross-zig: full `cmake --build --preset windows-cross-zig`
  clean (147/147 targets), then the exact CI ctest invocation under
  `xvfb-run -a` + Wine — **10/10 passed**, 108.68s total.

## Micro-item — empty-manifest fail-safe for both prune steps

Final re-review flagged one latent edge, closed per the no-deferral
policy (commit `d2aab5d`): both prune steps' `test -f "${manifest}"`
guard caught a MISSING manifest but not an EMPTY-but-existing one, which
would have passed that check and fallen through to the loop, where every
`grep -qxF` lookup misses against an empty file — deleting every
`.deps-cache/` subdirectory instead of none. Unreachable under the
current call graph (`rx_add_cached_dependency()` always appends at least
one key before Configure can succeed), but made structurally impossible
rather than left implicit: changed to `test -s "${manifest}"` (exists
AND non-empty) in both jobs, one line each, with a comment explaining the
distinction. Verified directly under `bash --noprofile --norc -eo
pipefail` (GitHub Actions' own `run:` shell) for all three cases against
a real `.deps-cache` with two subdirectories: missing manifest → exit 1,
both dirs untouched; empty manifest → exit 1, both dirs untouched;
populated manifest listing one of the two keys → exit 0, the unlisted
dir removed, the listed one kept. `actionlint` 1.7.12: zero findings on
the resulting `ci.yml`.

---

## Concerns / notes for the coordinator

- None blocking. All five original items plus both fix-round findings
  closed, verified, and green on both CI presets locally (build clean +
  full ctest pass, matching CI's exact invocations, including under Wine
  for windows-cross-zig).
- Item 5's `actionlint` binary was fetched ad hoc into `/tmp` for this
  session (not installed system-wide, not added to the repo or CI) —
  purely a local verification aid; CI itself does not run actionlint.
- The fix round's minor (deps-cache pruning) is verified by dry run, not
  a real local execution, for the environment-specific reason explained
  above (shared local `.deps-cache` across presets) — CI's own per-job
  isolation means this is not a caveat on the mechanism itself, only on
  how thoroughly it could be exercised locally.
- Two untracked SDD files were present in the working tree at start
  (`closure-sweep-brief.md`, `task-16-brief.md`), one other in-flight
  agent's own worktree modifications to `progress.md`/the toolchain spec
  doc, and a new untracked review diff file appeared during this fix
  round (`review-55410f0..ffff2f5.diff`, not authored by this sweep) —
  none of these were touched, per the "only your own files" constraint.
