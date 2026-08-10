# Phase 3 Final Whole-Branch Review

**Range:** `2d48444..022d8e7` (43 commits) **Reviewer:** final whole-branch gate (post Task 1-9, all task-reviewed through fix rounds)
**Scope:** cross-task seams, spec exit-criteria audit, deferred-minors triage, docs-vs-reality, repo hygiene, build/test truth. NOT a re-review of any single task (see ledger for those verdicts).

---

## 1. Cross-task seams

Investigated directly (own reading of `pass_signature.h`, `pass.h`, `executor.h/.cpp`, `pass_signature.h`, `material_system.h/.cpp`, `api_impl.cpp`) plus a dedicated background sub-review covering the same four seams independently. Both passes agree.

### 1.1 rx_graph -> rx_material via `PassSignature` — **1 real defect (Medium, dormant)**

`PassSignature::colorFormats` is a fixed `std::array<VkFormat, 8>` (`src/rx_graph/include/rx_graph/pass_signature.h:43`). `Executor::execute()` populates it with `signature.colorCount = min(colorPhysIdx.size(), 8)` (`src/rx_graph/executor.cpp:568-572`) — a **silent clamp**. But the real `vkCmdBeginRendering` scope a few lines earlier builds `colorAttachments`/`renderingInfo.colorAttachmentCount` from the **uncapped** `colorPhysIdx.size()` (`executor.cpp:501-521,544`). Neither `Pass::addColorOutput` nor `RenderGraph::compile()` enforces any maximum color-attachment count. If any pass ever declares more than 8 color outputs, `getPipeline()`'s cache key and the `VkPipelineRenderingCreateInfo` it builds (`material_system.cpp:1450-1453`) would silently describe fewer attachments than the actual dynamic-rendering scope has — a real pipeline/rendering-info mismatch.

This is exactly the kind of blind spot a single task review cannot see: Task 3's executor review had no reason to think about rx_material's fixed-size cache-key struct (which didn't exist yet); Task 5's review had no reason to check whether rx_graph enforces an attachment cap (it doesn't).

**Currently dormant, not release-blocking**: no sample or test in this phase declares more than 2 color outputs on any pass (verified by grep), and Vulkan's own driver-reported `maxColorAttachments` limits realistic usage to a similar range. No release user is affected. Recommend a cheap follow-up (throw in `compile()` if a pass declares >8 color outputs, or grow the array) — tracked as a new finding, not a ledger minor, and not blocking this release.

`PassContext` lifetime: sound. `bindInstance()` reads `passContext.passSignature()` once into a local value and never retains a reference — `PassSignature` is returned/stored by value throughout, so there is no dangling-reference hazard here.

### 1.2 rx_material internals vs public ABI (`api_impl.cpp`) — **checked, sound**

Every method in `api_impl.cpp` (`MaterialInstanceImpl::setFloat/setFloat4/setTexture`, `MaterialImpl::paramInfo`, `TextureImpl::bindlessIndex`/dtor, `MaterialSystemImpl::loadMaterial/reloadChanged/createTexture2D`) was checked against the **current** `material_system.h`/`instance.h` signatures. Task 7's collapse of the duplicate reflection session is fully realized: `paramInfo()` reads straight from `MaterialSystem::materialParams()` (the same already-linked program that produced the real SPIR-V), blob sizes match `paramBlockSize()` exactly, and every exception-boundary catch matches what the internal methods actually throw today. No stale Task-6-era assumption survives.

### 1.3 Samples 05 vs 06 consumption-style divergence — **checked, sound**

Sample 06's `rx::material::`/`rx_material/` usage is 100% confined to the explicitly marked `namespace materialBridge { ... }` block in `samples/06_materials/main.cpp`; no `IRx*` pointer is cast to an internal type outside it — the D5/D11 bridge-confinement contract holds. Both samples call `graph.compile()`/`executor->realize()` once at startup (and again only on resize) and drive `Executor::execute()` once per frame keyed off the same `FrameSync` frame-in-flight index; no divergence in call frequency or teardown ordering between the raw-rx_graph consumer (05) and the rx_material-mediated one (06).

### 1.4 Hot-reload cross-subsystem lifetime (pipeline cache + DeletionQueue + bindless) — **checked, sound**

Traced one full reload cycle: `reloadChanged()` tags stale-pipeline retirement with the frame number of the most recently completed `beginFrame()` call — always ≥ any frame that could still be referencing the stale pipeline, since the reload happens before the *new* frame's own `beginFrame()`. `DeletionQueue`'s actual destruction only fires once a later iteration's fence wait confirms that tagged frame is done; `DescriptorArena::beginFrame()` is only ever called after that same slot's fence wait. No premature-destruction or stale-descriptor window found in what sample 06 actually exercises. Immediate (non-deferred) destruction of old shader modules/layouts in `reloadChanged()` was already checked against Vulkan-Docs VUID text in the Task 7 review; not re-litigated here.

---

## 2. Spec exit-criteria audit (D1-D12)

| Decision | Status | Evidence |
|---|---|---|
| D1 (from-scratch graph, not vendored) | **Met** | `render_graph.cpp`'s own header comment states re-implementation, not port; no Granite source present in the tree outside `.superpowers`/scratch research notes. |
| D2 (dynamic-rendering-native, no subpass layer) | **Met** | `grep -rn "VkRenderPass\b"` across `src/rx_graph`, `src/rx_material`, `samples/05_multipass`, `samples/06_materials` → zero hits. `vkCmdBeginRendering`/`vkCmdEndRendering` per pass confirmed in `executor.cpp`. |
| D3 (sync2 invalidate/flush barriers) | **Met** | `grep -rn "vkCmdPipelineBarrier\b"` (legacy) → zero hits in new dirs; `barriers.cpp` implements the invalidate/flush state machine; Task 2's Critical (aggregate invalidate-tracking gap) was fixed and re-verified per ledger. |
| D4 (feature set + explicit deferrals) | **Met** | `grep -n "AsyncCompute\|QueueClass::" src/rx_graph/executor.cpp` → **zero matches**: the executor never branches on `QueueClass`, confirming every pass maps to the graphics queue as specified. Transient pooling (not placed-aliasing) confirmed by `transient_pool.h/.cpp` design. |
| D5 (COM-lite ABI, DLL deferred) | **Met (code); doc gap found** | `rx_api.h` read in full: pure-virtual single inheritance from `IRxUnknown` only, `RxResult` codes, GUID-per-interface, static_assert-pinned PODs — all present. `rx_material` builds as `add_library(rx_material STATIC ...)` (`src/rx_material/CMakeLists.txt:24`) — no `rx.dll` exists, matching D5's explicit deferral. **But** `docs/abi.md:1` opens with "The RendererX DLL's public interface follows a COM-lite pattern..." — stale/inaccurate framing (see §4). |
| D6 (Slang-interface material model) | **Met** | `shaders/material/material.slang` (`IMaterialShader`), link-time specialization via `createCompositeComponentType`+`link` per Task 5 review (spec PASS, probe-verified). |
| D7 (lazy content-hash-keyed cache) | **Met** | Verified directly: `PipelineKey key{record->contentHash, req.pass.hash(), req.specializationBits}` (`material_system.cpp:1371`) — exactly the three-part key D7 specifies, no more, no less. |
| D8 (parameter/specialization split) | **Met** | `IRxMaterialInstance` exposes only `setFloat`/`setFloat4`/`setTexture` (no recompile trigger); `specializationBits` fixed at 0 in Phase 3 core, already part of the cache key for future use. |
| D9 (hot reload by content hash) | **Met** | `reloadChanged()` re-hashes, uses a fresh `slang::ISession`, keep-last-good on failure, retires via `DeletionQueue` — confirmed by code read + seam 1.4 trace above. |
| D10 (sample 05, zero hand-written barriers) | **Met** | `grep -rn "vkCmdPipelineBarrier2" samples/05_multipass` → zero hits — the D10 acceptance grep is clean. Headless gate passes (`sample_05_multipass_headless`, ctest confirmed). |
| D11 (sample 06, public-surface-only) | **Met** | Bridge confinement confirmed (§1.3). Interface-contract tests present: QI identity, refcount round-trip, `RX_E_NOINTERFACE`/`RX_E_INVALIDARG` on bad input — all present in `test_api_contract.cpp`/`test_api_factory.cpp` (17 relevant `TEST_CASE`s enumerated). |
| D12 (testing bar) | **Met** | See §6: 15/15 green on linux-native at HEAD (fresh, genuine incremental rebuild — not a stale cache), zero validation errors (gates fail closed on any `[error]`-level validation line), both presets build, packaging verified self-contained (§4). |

**All 12 decisions are substantively met.** The one gap found (docs/abi.md's DLL framing) is cosmetic, not a code or test gap.

---

## 3. Deferred-minors triage (ledger's final list)

| # | Minor | Ruling | Rationale |
|---|---|---|---|
| 1 | [T1] Dead cyclic subgraphs silently culled, lightly documented | **CARRY** | Verified in `render_graph.cpp`: cycles among passes *unreachable* from the backbuffer/side-effects are dropped before the cycle-detecting Kahn's-algorithm/DFS step ever runs on them; cycles among *reachable* passes correctly throw with a named diagnostic (Task 1 fix round 1). Silently dropping genuinely dead code is sound behavior, not a correctness bug — zero observable difference for any real graph. Doc-only polish, no release user affected. |
| 2 | [T5] No corrupt-pipeline-cache-content regression test | **CARRY** | Code path is correct today (`material_system.cpp:988-1019`: unreadable/corrupt cache file logs a warning and falls back to a fresh cache — Vulkan itself guarantees `vkCreatePipelineCache` never rejects malformed data outright) and was manually probe-verified per the Task 5 review. This is test-coverage debt for an already-correct, already-verified path, not a shipped defect. |
| 3 | [T5] `MaterialSystem::layoutInfo()` reference invalidated by `HandlePool` reallocation, undocumented on the public accessor | **CARRY** | Confirmed the hazard is real (`HandlePool`'s backing `slots_` vector can reallocate on growth, per `rx_core/include/rx_core/handle.h`). But `grep -rn "\.layoutInfo(\|->layoutInfo(" src/ samples/` finds **exactly one call site in the whole tree** — a single test (`test_material_system.cpp:318`) that never holds the reference across a later `loadMaterial()` call. `api_impl.cpp` uses `materialParams()` instead, never `layoutInfo()`. Zero blast radius in shipped code or the public ABI today; a one-line doc-comment fix is appropriate opportunistically, not release-blocking. |
| 4 | [T4-carried] `rx_shader::reflect()` lacks storage-buffer stride exposure | **CARRY** | Confirmed absent (`grep -n "stride" src/rx_shader/include/rx_shader/*.h` → no hits). This is a future drift-guard *enhancement* (Task 4 used a manual `sizeof`/static_assert mechanism instead, which is what actually shipped and is sound) — not a defect in anything delivered this phase. Appropriate as a Phase 4 backlog item, as already ledgered. |
| 5 | [T9-informational] windows-cross has no ctest runner for GPU/present paths | **CARRY** | Not a defect — a documented, deliberate environment limitation (no Vulkan device under Wine in CI), with manual-verification rows already present in `MANUAL_VERIFICATION.md`. No action possible or needed. |

**All 5 ledger minors: CARRY.** None affect a release user or the public repo's credibility; all are either already-correct-and-just-undertested, or genuinely inert given today's actual usage.

---

## 4. Docs vs reality

Dedicated audit (background sub-review) covering README.md, `docs/abi.md`, `samples/README.md`, `MANUAL_VERIFICATION.md` against the actual shipped code.

**Confirmed accurate** (spot-checked, not exhaustively re-transcribed):
- Sample 05's "zero hand-written barriers" claim (`samples/README.md`) — grep-confirmed.
- Sample 06's "public-API-only" claim, bridge confinement — confirmed (§1.3 above).
- `tools/package_samples.sh`'s per-sample manifests match `samples/README.md`'s stated redistribution lists exactly for both new samples (05's six `.slang` files including `scene_types.slang`; 06's `materials/` + `material_shaders/` split).
- `--present`/`--validate` flags are genuinely parsed and do what both docs claim.
- `MANUAL_VERIFICATION.md`'s 05/06 rows are honestly hedged (unchecked boxes, "not yet performed on real hardware," Xvfb-only functional verification explicitly disclosed) — if anything, it undersells rather than overclaims.
- `docs/abi.md`'s substantive rules (GUID-per-interface, refcount=1-on-creation, `RxResult` code list, static_assert convention, pre/post-v1.0 GUID policy) all match `rx_api.h` exactly — already checked line-by-line by the Task 9 reviewer; re-confirmed here.

**Finding — OVERCLAIM/STALE (Low, fix recommended):** `docs/abi.md:1` — *"The RendererX DLL's public interface follows a COM-lite pattern..."* There is no DLL. `src/rx_material/CMakeLists.txt` builds `rx_material` as `STATIC`, with its own comment stating explicitly: *"there is no separate rx.dll yet — D5 defers the standalone-DLL artifact."* The spec's own D5 title is *"...the standalone DLL artifact is deferred."* This is the one document a future contributor will read as the authoritative description of the ABI boundary; its opening sentence describes an artifact that doesn't exist yet. One-sentence reword (e.g., "RendererX's public interface — designed for eventual DLL packaging — follows a COM-lite pattern...") fully resolves it.

**Not a finding (sequencing, not a defect):** README.md declares "Phase 3 (complete)" while no `v0.3.0-phase3` tag exists yet (`git tag -l` → only `v0.1.0-phase1`, `v0.2.0-phase2`). This is expected: the plan's own execution notes place tagging *after* this final review passes and the branch is pushed — "complete" correctly describes the engineering work (all 9 tasks done, tests green), not the release-tag step, which is the very next action after this review closes.

---

## 5. Repo hygiene for a public push

- **AI-attribution sweep: clean.** `git log --format='%an|%ae|%cn|%ce|%s' 2d48444..022d8e7 | grep -iE 'claude|anthropic|co-authored|opus|sonnet|haiku|fable|ai assistant'` → zero matches. Full commit-body sweep → zero matches. All 43 commits: author = committer = `Yousef Wadi <ywadi85@gmail.com>`. Content-level sweep (every file touched in the range, at its final `022d8e7` content, grepped case-insensitively for `claude|anthropic|co-authored|generated by|written by`) found only benign, expected hits: the policy statement itself in `CLAUDE.md`/the plan (stating the *rule*, not violating it), a worktree path literally named `.claude/worktrees/...` in two task reports, and `task-7-report.md` quoting the grep command it used to verify no attribution exists. **No real attribution anywhere.**

- **Git status: one real finding (Medium-High, fix before push).** `git status` shows the entire `.superpowers/sdd/2026-08-10-phase3-render-graph-materials/` ledger is almost entirely **untracked** at HEAD: `progress.md` (the ledger itself), every `task-{1,2,3,6,9}-report.md` and `task-{1,2,3,4,6,7,8,9}-review.md`/`rereview.md`, every `task-N-brief.md`, and all 16 `review-*.diff` snapshots. Only `research-*.md` and `task-{4,5,6,7,8}-report.md` are actually committed (piggybacked onto specific feature commits). This is inconsistent with this exact repo's own precedent: **every** prior phase's SDD directory (`2026-08-09-toolchain-platform-rhi/`, `2026-08-10-phase1-completion/`) has its ledger, every brief/report, and every review `.diff` fully committed (9 and 11 diffs respectively). Exit criterion 3 says "Ledger complete" — today that's only true on local disk, not in the history that will actually get pushed. **Recommend:** commit the outstanding SDD files before tagging `v0.3.0-phase3`, matching prior-phase practice, so the audit trail this whole SDD process produced is actually part of the public record it's meant to document.

- **`.superpowers/sdd` content professionalism: clean.** Spot-checked `progress.md` (full read) and `task-9-report.md`/`task-9-review.md` (full read, including two fix-round re-reviews) — factual, evidence-cited, professional tone throughout; no inappropriate content; independently re-confirms zero AI attribution within the ledger's own text.

- **CI workflow: coherent.** Read `.github/workflows/ci.yml` in full. Windows-cross-zig's test-exclusion regex (`rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample`) was simulated against the real, current 15-test ctest registry (`ctest --test-dir build/linux-native -N`): excludes exactly `rx_rhi_vk_tests`, `rx_graph_gpu_tests`, `rx_material_gpu_tests`, and all six `sample_*_headless` tests; runs `shader_spirv_test`, `rx_core_tests`, `rx_platform_tests`, `rx_shader_tests`, `rx_graph_tests`, `rx_material_tests` — matching the workflow's own inline comment exactly. Both jobs' Slang/deps caches use per-job keys (no cross-job cache starvation, per the header comments' own documented history of that bug class).

- **Minor, non-blocking, local-only:** two stale git worktrees remain registered under `.claude/worktrees/` (`git worktree list` shows `agent-a381281ec33251156`, `agent-ae1008d3da87e4133`) from earlier parallel-dispatch work; `.claude/` is gitignored, so these never reach the public remote — disk hygiene only, no action needed for this push.

---

## 6. Build/test truth (re-run at HEAD)

- `cmake --build --preset linux-native` → `ninja: no work to do` (already current), then forced a genuine incremental rebuild via `tools/check_build_budget.sh linux-native 60` (touches `src/rx_core/src/log.cpp`, a real leaf dependency of every downstream target) → **3s**, well within the 60s budget, with visible real relinking of every sample/test binary (16 link steps observed) — this is genuine evidence the binaries reflect HEAD, not a stale cache.
- `cmake --build --preset windows-cross-zig` → `ninja: no work to do` (already current and clean).
- `ctest --preset linux-native --output-on-failure` (run twice: once before, once immediately after the forced rebuild above) → **15/15 passed both times**, ~26s total. No `[error]`/`VUID` text anywhere in the output. Full test list: `shader_spirv_test`, `rx_core_tests`, `rx_platform_tests`, `rx_shader_tests`, `rx_rhi_vk_tests`, `rx_graph_tests`, `rx_graph_gpu_tests`, `rx_material_gpu_tests`, `rx_material_tests`, and all six `sample_*_headless` gates including `sample_05_multipass_headless`/`sample_06_materials_headless`.
- `git status` confirmed clean (no stray diffs) after the rebuild.

**Confirmed: 15/15, both presets green, zero validation errors, matching D12 and the ledger's own final gate.**

---

## Overall assessment

No code-correctness defect found in this whole-branch pass blocks release: the one real cross-task gap (§1.1, `PassSignature`'s 8-color-attachment silent clamp) is dormant and unreachable by anything shipped this phase. All 12 spec decisions are met. All 5 ledger minors are sound to carry. The two real, actionable items are non-code: commit the outstanding SDD ledger/review files (repo-hygiene/audit-trail completeness), and reword one stale sentence in `docs/abi.md`. Neither requires dispatching an implementer fix round or re-review — both are mechanical, pre-tag housekeeping.
