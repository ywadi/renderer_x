# Closure-sweep brief — no-deferral conversions (Stage 1)

You are the implementer for a five-item closure sweep in RendererX
(Vulkan 1.3 renderer middleware, C++20, repo `/media/ywadi/second/renderer_x`,
main checkout — base commit `55410f0`, tree clean except SDD workspace
files which are not yours). These are previously-identified items the
project's standing NO-DEFERRAL policy converts to immediate closure.
CI is currently GREEN — it must stay green. Each item is independent;
commit them separately (one commit per item, or logically grouped).

## Item 1 — Harness gap: destructor-time validation errors escape the test exit code (project-wide)

Evidence (Task 14 review): every GPU test fixture checks
`CHECK_FALSE(fixture->context.hasValidationErrors())` BEFORE the
fixture's destructor chain runs; a validation error raised during
teardown (the sampler-leak / unflushed-upload class) prints but the
binary exits 0 — reviewer reproduced `VUID-vkDestroyDevice-device-00378`
3x with a green exit. CI has no stderr gate either.
FIX: give every GPU test binary a post-teardown validation re-check
that fails the run. Recommended mechanism (adapt to what the harness
actually supports — read the existing doctest main files first, e.g.
`src/rx_graph/tests/doctest_main_gpu.cpp` and each module's
equivalent): after `context.run()` returns and all fixtures are
destroyed, if the process observed any validation error (the existing
error-capturing sink — find how hasValidationErrors() is fed and make
the counter process-lifetime, not fixture-lifetime), print a named
error and force a nonzero exit code. Apply across ALL GPU test mains
(rx_rhi_vk, rx_graph, rx_material, rx_asset). Add ONE deliberate
regression proof: a scratch-worktree experiment (not a shipped test)
reintroducing a teardown-time leak (e.g. the Task 14 sampler-destructor
revert) showing the binary NOW exits nonzero — paste the evidence.
Constraint: do not change what counts as a known-false-positive; the
existing filter logic stays authoritative.

## Item 2 — MikkTSpace vendored UB patch

Evidence (Task 15 ASan/UBSan run): UB at `mikktspace.c:1667` under
UBSan. Upstream is unmaintained (no commits since 2020; pinned by
commit hash). FIX: reproduce the UBSan report first (paste it), apply
the MINIMAL local patch to the vendored file with a clearly-marked
comment block (`// [RendererX local patch] <reason, UBSan citation,
date>`), record the patch in the vendoring notes location used at
adoption (find where Task 13's vendoring commit documented MikkTSpace
— keep the record style consistent). Re-run UBSan on the covering
tests to show the report is gone and behavior is unchanged (tangent
outputs byte-identical on the existing fixtures — assert via the
existing tangent tests).

## Item 3 — FrameSync::advanceFrame(Allocator*) live wiring

Evidence (Task 10 deferred minor): the per-frame budget-refresh wiring
exists and is mechanism-tested but no running sample exercises it.
FIX: wire `FrameSync::advanceFrame(&allocator)` into sample 04's frame
loop (the streaming/eviction sample — the natural consumer; read its
main.cpp first) replacing its no-arg call; verify with the sample's
existing headless gate that budget values in the memory report are
live (add a minimal assertion or logged-once line to the headless path
showing a nonzero, refreshed budget — keep it consistent with the
sample's existing output conventions). Both presets.

## Item 4 — Combined glTF→TextureCache→pixel test (Task 14 minor 4.6)

Evidence: role inference via glTF import and pixel-correct rendering
were proven in two separate tests; no single test proves the full path
glTF-file → importGltf(with TextureCache) → bindless texture →
rendered pixels. FIX: one GPU test importing a committed glTF that
references a KTX2 texture (fixtures exist — cube_basisu.gltf), drawing
with it through the existing test rendering path, quadrant/pixel
readback asserting the texture's actual content appeared. Reuse
existing fixtures + readback helpers; no new machinery.

## Item 5 — CI cache hardening (ci.yml)

Evidence (build-time incident, ledgered): (a) deps-cache keys rotate on
any third_party edit and there is no `restore-keys` prefix fallback, so
a key rotation forces a 100% cold ~20m rebuild instead of a warm
partial; (b) close-together pushes race the cache save ("Unable to
reserve cache..."), and a second overlapping run rebuilds cold.
FIX: (a) add `restore-keys` prefix fallbacks to the deps-cache (and
fetched-assets) cache steps so an older same-prefix cache seeds the
build (verify the deps mechanism tolerates a stale seed — it must
rebuild only changed deps; read how the cache dir is consumed);
(b) add a workflow-level `concurrency` group serializing runs per ref
(`cancel-in-progress: true` for pushes to main is acceptable — an
outdated commit's run has no value once a newer push exists; state the
choice in a comment). Validate ci.yml syntax (actionlint if available,
else careful YAML review + `gh workflow view` after push — but you do
NOT push; note it for the coordinator).

## Global constraints (binding)

- **NO AI attribution of any kind** in commits; author stays local git
  config; conventional factual messages; commit locally; do NOT push;
  do NOT touch board/issues/plan/spec/ledger; only your own files.
- Production grade; keep CI green (linux-native serial ctest 20/20 +
  windows-cross build clean before you finish); zero unfiltered
  validation errors; per-directory style; abandon/teardown paths get
  real-resource tests where applicable (standing lesson).

## Report contract

Full report →
`.superpowers/sdd/2026-08-11-phase4-scene-assets/closure-sweep-report.md`
(per-item proof, command output tails, the item-1 nonzero-exit
evidence, UBSan before/after). FINAL MESSAGE: ONLY status, commit
SHAs, one-line test summary, concerns.
