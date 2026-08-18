# Matrix — Issue #29: Executor per-frame allocation elimination

**Ticket:** #29 "Executor per-frame allocation elimination" (`phase-4`,
`stage-2`)
**Plan task:** Task 23, `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:424-450`.
**Origin:** 2026-08-18 claim validation, claim 7 (`render-graph hot-path
allocation`).
**Binding constraint:** unlike #5/#6/#7, this ticket has no D-number of its
own; it is bound by CLAUDE.md's performance-is-an-exit-criterion policy
("fast-path-as-default... per-object state churn and retrofit-later designs
are rejected at review") and by D26's zero-alloc precedent for the SAME
phase's DrawListBuilder (issue #6), which this matrix cross-references as
the sibling discipline.

**Sources consulted:**
- `gh issue view 29 --json body` (2026-08-18) — full body including the
  seven named sites and the sequencing note.
- Plan Task 23, `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:424-450`.
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/claim-validation-2026-08-18.md`
  claim 7 and its "Key evidence" section (lines 45-51).
- `src/rx_graph/executor.cpp` — read directly at current HEAD (commit
  `94991deb5e432d6f16a31560a4d63a8e227f6532`; confirmed **zero drift** from
  the claim-validation doc's citations, which were taken at `bf5b853` — `git
  diff --stat bf5b853..HEAD -- src/rx_graph/executor.cpp` is empty; the only
  intervening commit is a docs-only restructure).
- `src/rx_rhi_vk/src/deletion_queue.cpp` — read directly (not part of the
  claim-validation doc's citation set; checked per this gate's own brief
  instruction to look for missed sites).
- Tracy client, vendored: `.deps-cache/tracy-3583b279609598f7/include/tracy/client/TracyScoped.hpp`
  (`Name()`, backing `ZoneName`), `.../tracy/common/TracyAlloc.hpp`
  (`tracy_malloc`), `.../tracy/tracy/TracyVulkan.hpp` (`TracyVkZoneTransient`)
  — read directly this session.
- `src/rx_core/include/rx_core/profile.h` (RX_ZONE*/RX_GPU_ZONE_DYNAMIC
  macro definitions), `src/rx_rhi_vk/include/rx_rhi_vk/tracy_gpu.h`.
- `src/rx_graph/tests/{doctest_main.cpp,doctest_main_gpu.cpp,test_execute_gpu.cpp,test_compile.cpp,test_barriers.cpp}`
  — read/greped directly to establish the existing test-harness shape and
  confirm no counting-allocator/operator-new-interposition precedent exists
  anywhere in the codebase today.
- `src/rx_material/tests/test_material_system.cpp:854-1042` — the closest
  in-repo precedent for a rigorous concurrency-sensitive test (not a
  zero-alloc test itself, but the pattern this gate recommends reusing for
  the "who actually ran on a worker thread" proof, by analogy — see the
  cross-reference row).
- C++20 heterogeneous unordered-container lookup: `std::unordered_map::find`
  transparent overload, cppreference.com/w/cpp/container/unordered_map/find
  — standard-library API surface, general C++20 knowledge (see Verification
  health for the exact confidence level claimed).

---

## The matrix

| Feature | First-tier precedent (named, cited) | Phase-4 disposition | Library support (verified, cited) | Proposed acceptance criterion |
|---|---|---|---|---|
| Site 1-2 — per-execute tracking vectors (`firstBarrierSeen`, `attachmentEverWritten`) | Ticket text names these explicitly. | consume-now — binding. | **Verified, zero drift**: `executor.cpp:995-996` — `std::vector<bool> firstBarrierSeen(resources.size(), false); std::vector<bool> attachmentEverWritten(resources.size(), false);`, both freshly constructed every `execute()` call. | Both become `Impl`-persistent `std::vector<bool>` members, resized (not reconstructed) only when `resources.size()` changes, and cleared (`std::fill`, not reallocated) at the top of every `execute()`. Test: capacity is unchanged across N steady-state frames (see the methodology rows below). |
| Site 3-4 — per-execute `unordered_map`s (`finalStageThisExecute`, `finalAccessThisExecute`) | Ticket text names these explicitly, and specifies the fix shape: "the two `unordered_map`s become index-addressed vectors (physical indices are dense)." | consume-now — binding. | **Verified, zero drift**: `executor.cpp:997-998` — `std::unordered_map<uint32_t, VkPipelineStageFlags2> finalStageThisExecute; std::unordered_map<uint32_t, VkAccessFlags2> finalAccessThisExecute;`. The "physical indices are dense" premise is independently confirmed: `firstBarrierSeen` on the very next lines is already sized directly off `resources.size()`, the same dense range these two maps are keyed by. | Both become `std::vector<VkPipelineStageFlags2>`/`std::vector<VkAccessFlags2>` sized to `resources.size()`, indexed directly by `physicalIndex` — no hashing needed at all (this is a strictly simpler fix than site 15 below, which DOES need a hashing strategy because its key is a string, not a dense index). Test: same capacity-stability assertion, plus a correctness test that direct-index lookup returns identical values to what the map-based version would have for a representative barrier sequence. |
| Site 5-6 — per-pass scratch (`colorPhysIdx`, `colorAttachments`) | Ticket text names these explicitly. | consume-now — binding. | **Verified, zero drift**: `executor.cpp:1061` (`std::vector<uint32_t> colorPhysIdx;`), `:1077` (`std::vector<VkRenderingAttachmentInfo> colorAttachments;`) — both constructed fresh inside the per-pass loop body, i.e. once per PASS per frame (not once per `execute()` call — a higher call frequency than sites 1-4, since a graph typically has multiple passes). | Both become `Impl`-persistent scratch buffers, `.clear()`d (not reconstructed) at the top of each pass iteration; sized by the pass's own attachment count, which per D18/CLAUDE.md's "fast-path-as-default" should stabilize after the first pass of the first frame for an unchanged graph. |
| Site 7-8 — barrier vectors (`vkImageBarriers`, `vkBufferBarriers`) | Ticket text groups these as one item ("both barrier vectors"). | consume-now — binding. | **Verified, zero drift**: `executor.cpp:289` (`std::vector<VkImageMemoryBarrier2> vkImageBarriers;`), `:353` (`std::vector<VkBufferMemoryBarrier2> vkBufferBarriers;`) inside `applyBarriers()`, called once per pass position per frame (same frequency class as sites 5-6). | Both become `Impl`-persistent, `.clear()`d per call; capacity should stabilize to each pass's own peak barrier count. |
| Site 9 — debug-label `std::string` | Ticket text names this explicitly. | consume-now — binding. | **Verified, zero drift**: `executor.cpp:255` (`const std::string label(name);` inside `beginDebugLabel()`) — constructed once per pass per frame purely to null-terminate `name` (a `std::string_view`) for `VkDebugUtilsLabelEXT::pLabelName`, which the driver copies/consumes synchronously per that call site's own comment (`:251-254`). | Replaced with a reusable, `Impl`-owned fixed/growable char buffer that copies `name` plus a null terminator into it (amortized growth, same discipline as the vectors above) rather than constructing a new `std::string` — `pLabelName` only needs to be valid for the duration of the one `vkCmdBeginDebugUtilsLabelEXT` call, so the buffer can be safely reused/overwritten next pass. |
| Site 10-11 — chunk command-buffer vectors | Ticket text names these explicitly. | consume-now — binding. | **Verified, zero drift**: `executor.cpp:1346` (`std::vector<VkCommandBuffer> chunkBuffers(chunkCount, VK_NULL_HANDLE);`), `:1463` (`std::vector<VkCommandBuffer> validChunkBuffers;` + `.reserve()`) inside `recordChunkedPass()`, called once per CHUNKED pass per frame. | Both become `Impl`-persistent (per-frame-slot, since chunked recording is concurrent with other frame-in-flight state — see the existing `ChunkCommandPool` array's own `[frameSlot][threadIndex]` indexing as the precedent shape to follow), cleared and refilled per call rather than reconstructed. |
| Site 12 — temporary `std::string` per name resolve | Ticket text names this explicitly. | consume-now — binding. | **Verified, zero drift**: `executor.cpp:1486` (`auto it = impl.nameToIndex.find(std::string(name));` inside `lookupResolvedIndex()`). **Frequency correction to the ticket's own framing**: this is not merely "a temporary string per name resolve" in the singular — `lookupResolvedIndex` backs FIVE separate `PassContext` resolvers (`imageView`, `image`, `buffer`, `imageFormat`, `historyValid`, `:1515,1529,1539/1547/1558`), each of which a pass callback may call MULTIPLE times per pass (once per named resource it touches) — so the real per-frame call count is `Σ(named-resource resolves per pass)` across every pass, not one per pass. | See site 15 below (the heterogeneous-lookup fix eliminates this allocation entirely, rather than merely making it cheaper). |
| **New finding — Tracy `ZoneName`/`RX_ZONE_DYNAMIC_NAME` allocates per call (CPU zones)** | Not in the claim-validation doc's 7 sites (that enumeration covers only `executor.cpp`'s own `std::`-container constructions). Directly answers this gate's own brief prompt ("Tracy zone macros?"). | log-don't-drop → **new gap requiring a scoping decision** (see Conflicts). | **Verified by direct source trace, not inferred**: `RX_ZONE_DYNAMIC_NAME(text,size)` → `ZoneName(txt,size)` (`rx_core/profile.h:98`) → `___tracy_scoped_zone.Name(txt,size)` (Tracy's own macro expansion, `TracyScoped.hpp` line 186 area) → `Name()`'s body: `auto ptr = (char*)tracy_malloc(size); memcpy(ptr, txt, size);` (`TracyScoped.hpp:126-140`) → `tracy_malloc` resolves to `rpmalloc()` (Tracy's bundled allocator) or plain `malloc()` as a fallback (`TracyAlloc.hpp:28-39`) — a genuine heap allocation on every call, when `TRACY_ENABLE` is on. Call sites in `executor.cpp`: `:1024` (`RX_ZONE_DYNAMIC_NAME(pass.name().data(), pass.name().size())`, once per pass per frame) and `:1359` (same macro, inside `recordOneChunk`, once per CHUNK per pass per frame — the highest-frequency call site of any found this session). | Proposed criterion: the coordinator must decide whether the zero-alloc test builds with `TRACY_ENABLE` on or off (see Conflicts). If on (matching D3's "Tracy from Stage 0" default and the dev/CI builds every other gate/sample uses), this allocation is real, in-scope, and NOT eliminable without dropping per-pass dynamic zone naming — the honest acceptance criterion documents it as an accepted, third-party-attributed exception (analogous to how `compile()`/`realize()` are explicitly exempted), not silently excluded from the count. |
| **New finding — GPU zone dynamic naming likely shares the same pattern (lower confidence)** | Same Tracy client library, same dynamic-name-allocation design principle observed in the CPU-zone trace above. | log-don't-drop → flagged for implementer verification, not independently confirmed this session. | **UNVERIFIED — partially traced, not to completion**: `RX_GPU_ZONE_DYNAMIC(ctx,varname,cmd,nameText)` (`tracy_gpu.h:81-82`) expands to `TracyVkZoneTransient(...)` which constructs a `tracy::VkCtxScope` with the raw name pointer/length (`TracyVulkan.hpp:751-773`) — this session traced the macro expansion down to the `VkCtxScope` constructor call but did **not** read `VkCtxScope`'s constructor body itself to confirm it also calls `tracy_malloc` for the dynamic name (unlike the CPU-zone `Name()` method, which was read to completion). Call site: `executor.cpp:1046` (`RX_GPU_ZONE_DYNAMIC(impl.gpuProfileCtx, rxGpuPassZone, cmd, pass.name().data())`, once per pass per frame). | Whoever implements Task 23 should complete this trace (read `VkCtxScope`'s constructor in the vendored `TracyVulkan.hpp`) before deciding the same Tracy-scoping conflict resolution also covers the GPU-zone path — presented here as an open question, not a confirmed fact, per this gate's own verification-honesty rule. |
| **New finding — `DeletionQueue::onFrameFenceSignaled` allocates whenever anything is pending (different file, not one of the 7)** | Not in the claim-validation doc's 7 sites (`deletion_queue.cpp` is a separate translation unit `executor.cpp` calls into — `executor.cpp:978-981`, per the claim-validation doc's own "Key evidence" section, calls `impl.pool.sweepStale(...)` and `impl.deletionQueue.onFrameFenceSignaled(...)` back-to-back). Directly answers this gate's own brief prompt ("deletion-queue churn?"). | log-don't-drop → new gap, scoped narrowly (see acceptance criterion). | **Verified by direct read**, `src/rx_rhi_vk/src/deletion_queue.cpp:28-52`: the function has a genuine zero-alloc fast path (`if (items_.empty()) return;`, line 29-31) — so in the narrowest reading of "steady state" (a completely static graph with nothing ever queued for deferred destruction), this function allocates nothing. But whenever `items_` is non-empty for ANY reason, it unconditionally constructs a fresh `std::vector<Item> remaining; remaining.reserve(items_.size());` (line 40-41) EVERY call, even if every item survives (none are due yet) — a real, repeatable allocation whenever deferred destruction is in flight, which is a broader and more realistic "steady state" than a graph that never resizes/reallocates anything. | Proposed criterion: Task 23's own zero-alloc test should include a variant where the deletion queue has at least one long-lived pending item across the N measured frames (simulating an in-flight deferred resource retirement) and assert this path is EITHER also fixed (two-pass-without-fresh-vector, e.g. `std::erase_if`-style in-place compaction reusing `items_`'s own storage) OR explicitly excluded from Task 23's scope with a documented reason (`deletion_queue.cpp` is arguably a separate ticket's file, not `executor.cpp`) — silently ignoring it because it lives in a different file is not defensible per this gate's own retrofit-economics test, since the exact same "steady-state N-frame" test methodology this ticket is building would trivially have caught it if pointed at the right scenario. |
| **Negative finding (verified-clean) — `vkAllocateCommandBuffers` in the chunked path is NOT a hidden 8th site** | Directly answers this gate's own brief prompt ("vkAllocateCommandBuffers in the chunked path?"). Presented as positive counter-evidence, matching the claim-validation doc's own stated discipline of recording honest counter-evidence (its "compile()/realize() are setup/resize-only" note). | N/A — already correct; **this pattern is the recommended template for fixing the 7 real sites**, not itself a defect. | **Verified by direct read**, `executor.cpp:704-745` (`acquireChunkCommandBuffer`): `vkAllocateCommandBuffers` (`:734`) is called ONLY when `cp.usedThisFrame >= cp.buffers.size()` (`:723`, the reuse-check guard) — i.e. only the first time a given `(frameSlot, threadIndex)` pair needs MORE simultaneous secondaries than any past frame at that slot ever did; every subsequent frame reuses `cp.buffers[cp.usedThisFrame++]` after a bulk `vkResetCommandPool` (`:671-677`, called once per frame via `resetChunkPoolsForFrameSlot`). The function's own comment states the budget "stabilizes after the first `kFramesInFlight` execute() calls" for this project's samples. | No test needed for correctness (already correct); recommend the fix for sites 1-11 explicitly cite this exact function as the in-repo precedent for "amortized-growth reuse pattern," since it is a working, reviewed example of precisely the discipline Task 23 needs to apply elsewhere in the SAME file. |
| Zero-alloc test methodology — counting-allocator hook vs. operator-new interposition | Ticket text offers both as options ("a counting allocator hook or capacity-snapshot check"). No existing precedent for EITHER exists anywhere in this codebase (`grep`ed the whole `src/` tree for `operator new`/counting-allocator/leak-tracking patterns this session — zero hits outside third-party vendored code). | consume-now — a genuinely new pattern for this codebase, not a reuse case. | N/A — general C++ technique, not a library; global `operator new`/`operator delete` overrides are well-documented (e.g. in *Effective Modern C++*-class references and countless game-engine allocator-tracking implementations) but are UNVERIFIED here as a specific citation, since this is standard-language-feature knowledge rather than a third-party API surface. | Recommend **against** global operator-new interposition for this specific test binary: `rx_graph_gpu_tests` (`doctest_main_gpu.cpp`) links Vulkan (via volk), the Vulkan loader/validation layers, and Tracy's own `rpmalloc` — a process-wide `operator new`/`delete` override risks interacting with all three in ways this gate cannot verify are safe without a dedicated spike, and CLAUDE.md's "production quality only" bar argues against shipping an unverified global-override technique into the test harness. See the capacity-snapshot row below for the recommended alternative. |
| Zero-alloc test methodology — capacity-snapshot alternative (recommended) | Same ticket text offers this as the second option. | consume-now — recommended primary mechanism. | N/A — pure C++, no linkage risk; fits directly into the EXISTING test harness shape (`rx_graph`'s tests already construct `Executor`/`RenderGraph` objects and call `execute()` in a loop across GPU test cases — `test_execute_gpu.cpp`'s existing `TEST_CASE`s are the direct precedent for the harness shape, though none currently assert on allocator/capacity state). | Concrete design: after a short warm-up (enough frames for every `Impl`-persistent buffer identified above to reach its peak steady-state size — matching the SAME warm-up-then-measure discipline `doctest_main_gpu.cpp`'s own vk-bootstrap warm-up already establishes as this project's convention), snapshot `.capacity()` on every converted `std::vector`/buffer via a test-only accessor (mirroring the existing `detail::debugChunkStats()`/`debugCompileCount()` "test-only seam, not part of the stable public contract" convention already used elsewhere in this codebase — `rx_core/debug_checks.h:52-58`, `material_system.h`'s own equivalent), run N further frames, snapshot again, and assert every capacity is bit-for-bit unchanged. This is strictly a capacity check, not a true allocation count — it cannot detect an allocation that happens to return a same-sized block (astronomically unlikely for `std::vector`'s doubling growth policy, and not a realistic false-negative risk in practice), which should be stated as a known, accepted limitation of the chosen methodology rather than silently assumed away. |
| Tracy-based allocation tracking as a third option (worth naming, not adopted) | Tracy itself ships a documented memory-profiling integration (`TracyAlloc`/`TracyFree` macros wrapping global `operator new`/`delete`, verified to exist in the vendored client via `TracyAlloc.hpp`'s own `tracy_malloc`/`tracy_free` pair — the SAME allocator the profiler's own dynamic-zone-naming allocations above go through). | N/A-Phase-4 — mentioned for completeness since Tracy is already a Phase-4 dependency (D3), but adopting it as the TEST's own zero-alloc oracle would conflate the profiler's own allocations (see the ZoneName finding above) with the renderer's, making the test's pass/fail signal ambiguous rather than clean. | Verified to exist (`TracyAlloc.hpp`), not verified as fit-for-this-purpose. | Not recommended as the primary mechanism; the capacity-snapshot approach above avoids this ambiguity entirely. |
| Heterogeneous `string_view` map lookup — C++20 mechanics | C++20 (P0919R3/P2136R3) added a **transparent-lookup overload set** to `std::unordered_map`/`unordered_set`: `find`/`count`/`contains`/`equal_range` gain a template overload usable with ANY key type `K` when both the map's `Hash` and `KeyEqual` template parameters expose `is_transparent` as a valid nested type — cppreference.com/w/cpp/container/unordered_map/find documents this overload set precisely (general C++ standard-library knowledge, not independently re-fetched this session; see Verification health). `std::equal_to<>` (the transparent specialization, C++14) already satisfies the `KeyEqual` half; `std::hash<std::string>` does **not** satisfy the `Hash` half by default (it has no `is_transparent` member) — a custom transparent hash functor is required. | consume-now — this is the fix for site 12/the `nameToIndex` map specifically, distinct from sites 3-4 (which need no hashing at all, per that row above). | N/A — C++20 standard-library feature; no third-party library needed (contradicts nothing in CLAUDE.md's "prefer ready-made libraries" rule, since this is a language/stdlib feature, not a subsystem to write from scratch). | `nameToIndex`'s declared type (`executor.cpp:207,863`) changes from `std::unordered_map<std::string, uint32_t>` to `std::unordered_map<std::string, uint32_t, TransparentStringHash, std::equal_to<>>`, where `TransparentStringHash` is a small functor with `using is_transparent = void;` and `operator()(std::string_view) const { return std::hash<std::string_view>{}(s); }` (relying on the widely-relied-upon, though not word-for-word standard-mandated, fact that `std::hash<std::string>` and `std::hash<std::string_view>` produce identical hashes for equal character sequences on every toolchain this project targets — libstdc++, libc++, MSVC STL all guarantee this in practice; flagged as a documented assumption, not silently relied upon). `lookupResolvedIndex` then calls `impl.nameToIndex.find(name)` directly with the `std::string_view` parameter it already has — zero temporary `std::string` construction. Test: a unit test asserts `find(string_view)` returns the identical iterator/result as the old `find(std::string(view))` did, across both a hit and a miss case, proving the transparent overload is actually being selected (not silently falling back to an implicit `std::string` conversion, which would defeat the whole point while still compiling cleanly). |

---

## Conflicts

1. **Tracy's own dynamic-zone-naming allocations are real, per-call, and not
   addressed by the ticket's 7-site enumeration or its "byte-identical
   rendering... zero validation errors" constraint list.** The ticket's
   acceptance criteria (issue #29 body) do not mention Tracy at all; D3
   (Tracy from Stage 0) and CLAUDE.md's measured-claims policy make
   `TRACY_ENABLE` the default posture for every build this project's samples
   and CI actually run. If the zero-alloc test is compiled with
   `TRACY_ENABLE` on (the representative configuration), sites like
   `executor.cpp:1024`/`:1359` (traced to `tracy_malloc` in this session)
   will make a naive "assert zero allocations" test fail for reasons Task
   23's own 7-site fix cannot address, since they are third-party profiler
   internals, not renderer logic. If instead the test is compiled with
   `TRACY_ENABLE` off, it stops testing the configuration that actually
   ships/gets measured. Not resolved here; flagged for the coordinator to
   pick an explicit scoping rule (most likely: measure with Tracy on, but
   exempt Tracy's own allocations from the count by construction — e.g. a
   counting mechanism that only tracks allocations attributable to
   `rx_graph`'s own code, which argues AGAINST the global-operator-new-override
   option and FOR the capacity-snapshot option, since capacity-snapshotting
   only watches the specific `Impl` buffers this ticket controls and is
   naturally blind to Tracy's unrelated internal allocations).

## New gaps

- **`DeletionQueue::onFrameFenceSignaled`'s non-empty-path allocation** —
  see the matrix row. Proposed phase fit: still Phase 4 (either folded into
  Task 23's own scope with a one-line note explaining the cross-file reach,
  or spun out as its own small follow-up ticket in the same stage) — NOT
  deferred past Phase 4, since D18/CLAUDE.md's performance-gate discipline
  is standing up in this exact stage and a known, characterized allocation
  source left uncounted undermines the "measured claims only" policy for
  any scene with in-flight deferred resource retirement (which streaming/
  hot-reload workloads will exercise routinely, not as an edge case).
- **GPU-zone dynamic-naming allocation, unconfirmed.** See the matrix row —
  needs the `VkCtxScope` constructor trace completed before Task 23's
  implementer can apply the same Conflict-1 resolution to the GPU-zone path
  with confidence.

## Verification health

**Verified first-hand this session, with exact file:line citations,
including a direct byte-for-byte drift check against the claim-validation
doc's own citations (`bf5b853` → current HEAD, zero drift confirmed via
`git diff --stat`):** all 7 originally-claimed sites; the
`vkAllocateCommandBuffers` chunked-path counter-evidence; the
`DeletionQueue::onFrameFenceSignaled` finding; the CPU-side Tracy
`ZoneName`/`tracy_malloc` allocation chain (traced end-to-end through three
vendored headers to the actual `rpmalloc()`/`malloc()` call); the complete
absence of any existing counting-allocator/operator-new-interposition
precedent anywhere in `src/`; the existing `RX_ASSERT_MAIN_THREAD`/
`setViolationHookForTests`/`debugChunkStats`-style "test-only seam" naming
convention this matrix's proposed capacity-snapshot accessor should follow.

**Inferred / not independently verified to completion:** the GPU-zone
(`VkCtxScope`) allocation path — traced through the macro expansion but not
through the constructor body itself (explicitly flagged, not presented as
fact); the claim that `std::hash<std::string>`/`std::hash<std::string_view>`
produce identical hashes is standard C++-ecosystem knowledge relied upon by
the transparent-lookup idiom generally, not something this session verified
against this project's specific toolchains' standard-library source.

**Dead links / version ambiguity:** none — the Tracy client version read
was the one actually vendored in this repo's `.deps-cache`
(`tracy-3583b279609598f7`), not a floating upstream reference, so the
`tracy_malloc` finding is grounded in the EXACT version this project
builds against, not a generic claim about "Tracy" as a library.
