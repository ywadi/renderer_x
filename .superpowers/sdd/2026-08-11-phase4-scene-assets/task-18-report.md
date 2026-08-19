# Task 18 report: `rx_scene` (Scene proxies, D19) + reversed-Z Camera (D13)

**Ticket:** #5 "Scene submission layer". **Plan task:** Task 18. **Gate:**
`gate/matrix-issue05-scene-proxies.md` as amended by
`gate/rulings-2026-08-18.md` §#5 + RC5. BASE=71ffe3e.

Order of authority followed: rulings > spec (matrix citations of D12/D13/
D19/D24/D26/D27) > matrix > ticket (`gh issue view 5`).

## Files created

- `src/rx_scene/CMakeLists.txt`
- `src/rx_scene/include/rx_scene/scene.h`, `src/rx_scene/include/rx_scene/camera.h`
- `src/rx_scene/scene.cpp`, `src/rx_scene/camera.cpp`
- `src/rx_scene/tests/CMakeLists.txt`, `doctest_main.cpp`, `scene_test.cpp`,
  `camera_test.cpp`, `thread_guard_test.cpp`
- `CMakeLists.txt` (one line: `add_subdirectory(src/rx_scene)`, placed
  right after `add_subdirectory(src/rx_asset)`)

Entirely device-free: no `VkDevice`, no `rx::platform::Window`. 1,902 lines
total (headers+impl+tests+CMake).

## Design decisions and their justification

### Handles: `rx::core::Handle<Tag>` type, hand-rolled SoA slot bookkeeping

`RenderableHandle`/`LightHandle` are `rx::core::Handle<struct RenderableTag>`/
`<struct LightTag>` (the generational index+generation TYPE, `rx_core/handle.h:1-24`)
per the gate ruling. `rx::core::HandlePool<Tag,T>` (the AoS container,
`handle.h:26-97`) is deliberately NOT reused — it stores one whole `T` per
slot in a single `std::vector<Slot>`, which would force an AoS
`RenderableDesc`-shaped row per slot, directly contradicting D19. `Scene`
instead hand-rolls the identical generational index/freelist algorithm
(`isLiveRenderableIndex`/`requireLiveRenderable`, `scene.cpp:29-40`) once
per SLOT, applied uniformly across every SoA column.

### MeshBoundsFn: the one necessary interface deviation

**This is the one place this task's design diverges from a literal reading
of the plan/gate text, and it needs its own explanation up front.**

The gate's "per-instance bounding-box override" row requires
`createRenderable()`/`setTransform()` to populate/refresh a renderable's
world-space AABB from "the mesh's stored bounds" — i.e.
`asset::MeshAsset::bounds`, resolvable only via `asset::Registry::mesh()`.
A hard `Scene` → `asset::Registry&` dependency was considered and rejected:
`Registry::registerMesh()` (the only way to put NON-fallback `MeshAsset`
content into a Registry) is `private`, friend-scoped to `import_gltf.cpp`'s
two orchestration functions, reachable only through a GPU-backed
`importGltf()` call. Task 18 is a device-free task end to end (brief's own
"Steps: device-free tests... → implement → commit") — a hard Registry
dependency would leave this library's own tests only ever able to exercise
the Registry's always-empty/invalid FALLBACK mesh, never real nonzero-volume
AABB transform math, device-free.

`MeshBoundsFn = std::function<asset::AABB(asset::MeshHandle)>` (scene.h) is
the minimal seam that avoids that, mirroring this codebase's own existing
`std::function`-as-injected-callback precedent (`rx::asset::
ImportCompletionFn`, registry.h) rather than inventing a new abstraction
style. `meshBoundsFromRegistry(const asset::Registry&)` (scene.h/scene.cpp)
is the one-line production adapter — it forwards VERBATIM to
`registry.mesh(handle).bounds`, inheriting that method's own D24
residency-tolerant contract with zero duplicated logic. This is the same
class of "necessary, not incidental" deviation this codebase already
documents elsewhere (GeometryPool::create()'s `Allocator&` addition,
Registry::importGltf()'s `Scheduler&` addition) — flagged here explicitly
per that convention.

Consequence: `Scene::setTransform()` re-invokes the SAME `MeshBoundsFn`
every call (never caches an `AABB`/`MeshAsset*` across calls) — this is
also exactly the mechanism the brief's "D24 residency-tolerant resolve test
at the proxy level" needs: a mesh that becomes nonresident between two
`setTransform()` calls is reflected automatically the next call, with zero
extra Scene-side logic (see `scene_test.cpp`'s eviction-proxy test, and the
revert-discrimination evidence below).

### Aspect ratio / culling far plane on Camera

The plan's illustrative `Camera` text ("pos/orientation, vfov, near") names
no aspect-ratio field, but a perspective matrix cannot be built without one
— `Camera::aspectRatio` is added, documented as a necessary deviation
(camera.h). `cullingFarPlane` is the gate-mandated finite far distance for
`cullingProj()`.

### Light storage: plain `std::vector<LightRecord>`, not column-split

D19's SoA emphasis is specifically about the 30k+-renderable culling hot
path (DrawListBuilder, Task 19); Phase 4 has no light-count-at-scale hot
loop (a scene has a handful of lights). Documented explicitly in scene.h so
this isn't mistaken for an inconsistency with the renderable columns.

## Per-criterion proof (gate matrix + rulings)

| Criterion | Where | Proof |
|---|---|---|
| Reuse `Handle<Tag>` type, not AoS `HandlePool`; SoA columns, span-accessor test | scene.h/.cpp | `transformsSpan()`/`worldBoundsSpan()`/`layersSpan()`/`channelsSpan()`/`castsShadowsSpan()`/`prioritySpan()`/`meshSpan()`; `scene_test.cpp` "SoA columns are real, contiguous..." test asserts per-field byte stride == `sizeof(T)` |
| Handle lifecycle / generational failure | scene.cpp | `scene_test.cpp` "handle lifecycle" test: create/destroy/reuse with bumped generation, stale handle throws `std::out_of_range` on every accessor/mutator; double-destroy is a safe no-op |
| Per-instance bounding-box override (mesh-derived default) | scene.cpp `recomputeWorldBounds` | `scene_test.cpp` "populates world-space AABB..." + "setTransform updates worldBounds..." tests |
| D24 residency-tolerant resolve at the proxy level | scene.cpp (MeshBoundsFn re-invoked every call) | `scene_test.cpp` "Scene re-resolves... D24 at the proxy level" test (simulated eviction, no crash, reflects fallback-shaped bounds) |
| No-dirty-tracking (O(1) `setTransform`) | scene.cpp | No dirty-bit/two-pass API exists anywhere on `Scene` (absence verified by inspection of scene.h's public surface); 30k-call benchmark below |
| `castsShadows: bool = true` (RC5) | scene.h `RenderableDesc` | `scene_test.cpp` "castsShadows defaults true" test |
| `priority: uint8_t` 0-7 default 4 (Filament precedent) | scene.h/.cpp (clamped in `createRenderable`) | `scene_test.cpp` "priority is clamped" test |
| Per-submesh material override, concrete typed field | scene.h `RenderableDesc::submeshMaterialOverrides` | `scene_test.cpp` "concrete typed field" test |
| Reserved skinning/morph slots (inert, tested) | scene.h `RenderableDesc` | `scene_test.cpp` "round-trip exactly with no truncation" test |
| LightManager forward-compat sizing (FG2) | scene.h `LightRecord`/`detail::` seam | `scene_test.cpp` "sized/typed for punctual fields" test (Point-type row round-trips exactly via `detail::createLightRecordForTesting`/`lightRecordForTesting`) |
| Transform-pool prev-frame-slot layout note | scene.h/`transformsSpan()` | `scene_test.cpp` "accepts a same-shaped 'previous transforms' copy" test |
| `getLayers`/`getChannels`/`setChannels` API parity (matrix-issue07) | scene.h/.cpp | `scene_test.cpp` "API parity" test; `setLayers(h,0)` hides without touching destroy, own test |
| Camera reversed-Z infinite-far `proj()`/`viewProj()` | camera.h/.cpp | `camera_test.cpp` near→1.0, far→~0.0 monotonic tests |
| Camera `cullingProj()` (finite, separate) | camera.h/.cpp | `camera_test.cpp` finite-far exact-0.0-at-far test; `cullingFrustumPlanes()` non-degenerate-6-planes test |
| Frustum plane extraction (Gribb-Hartmann) | camera.cpp `extractFrustumPlanes` | `camera_test.cpp` classification test (inside/outside on all 6 sides) + degenerate-Far-under-infinite-far documentation test |
| Camera jitter (inert, preserve-later) | camera.h/.cpp | `camera_test.cpp` jitter-threading test (exact delta on the two documented matrix terms; `cullingProj()` unaffected) |
| Exposure stays on tonemap (D22) | camera.h (no field) | Verified by absence + documented rationale in camera.h's top comment |

## Bug the tests caught during TDD (organic revert-discrimination evidence)

The first `cullingFrustumPlanes()` implementation assigned `Top = row3 -
row1` / `Bottom = row3 + row1` (the naive, non-Y-flip-aware pairing). The
classification test (`camera_test.cpp`) failed immediately:

```
CHECK( planeDot(planes[topIdx], glm::vec3(0.0F, 6.0F, -5.0F)) < 0.0F ) is NOT correct!
  values: CHECK( 7.77817 <  0 )
CHECK( planeDot(planes[bottomIdx], glm::vec3(0.0F, -6.0F, -5.0F)) < 0.0F ) is NOT correct!
  values: CHECK( 7.77817 <  0 )
```

Root cause: `proj()`/`cullingProj()` bake in Vulkan's Y-flip (`m[1][1] =
-fY`), so row1's sign is inverted relative to row0's — `row3 + row1` is
actually the boundary that rejects a point too far ABOVE (+Y), and `row3 -
row1` rejects one too far BELOW. Fixed in `camera.cpp` (`Top = row3 +
row1`, `Bottom = row3 - row1`), with the derivation recorded as a code
comment so this doesn't get "fixed" back to the naive pairing later. This
is real, not staged, evidence the plane-classification test discriminates
correct from incorrect frustum math.

## Deliberate revert-and-restore evidence (in-tree)

Two additional probes, run and then reverted, on top of the organic one above:

**Probe 1 — drop the AABB recompute in `setTransform()`:**
```diff
-    transform_[idx] = transform;
-    recomputeWorldBounds(idx);
+    transform_[idx] = transform;
+    // (recomputeWorldBounds(idx) call removed)
```
Result: exactly 2 test cases / 3 assertions failed (`setTransform updates
worldBounds...`, `D24 at the proxy level`) — every other test (including
all handle-lifecycle/priority/skinning/light tests) stayed green. Restored;
re-verified 26/26 test cases, 228/228 assertions green on `linux-native`
AND under Wine (`windows-cross-zig`).

**Probe 2 — drop the generation check in `isLiveRenderableIndex()`:**
```diff
-    return handle.index() < generation_.size() && alive_[handle.index()] && generation_[handle.index()] == handle.generation();
+    return handle.index() < generation_.size() && alive_[handle.index()];
```
Result: exactly 1 test case / 5 assertions failed (the "handle lifecycle...
loud failure for a stale handle" test — stale-handle accessors stopped
throwing). Every other test stayed green. Restored; re-verified 26/26 /
228/228 green on both platforms again.

No probe artifacts remain (`grep -rn "REVERT-DISCRIMINATION PROBE" src/rx_scene` → no matches).

## Benchmark (published, MEASURED — Tracy-zoned via `RX_ZONE` in
`createRenderable`/`setTransform`)

30,000 `Scene::setTransform()` calls, wall-clock (`std::chrono::steady_clock`),
against a trivial O(1) mesh-bounds provider (isolates Scene's own
mechanism from whatever a real `asset::Registry` lookup would cost):

| Platform | Total (30k calls) | Per-call average | Throughput |
|---|---|---|---|
| linux-native (native x86_64, this session's runs) | 1,074-1,141 us | ~0.036-0.038 us | ~26.3-27.9M calls/sec |
| windows-cross-zig under Wine | 1,074-1,106 us | ~0.036 us | ~27.6-27.9M calls/sec |

No dirty-bit or two-pass update API exists anywhere on `Scene`'s public
surface (verified by inspection of `scene.h`) — `setTransform()` is a
single O(1) SoA write plus one `AABB::transformed()` call, consistent with
D12's flat (no-hierarchy) transform model. These numbers are a **trend
metric per D18** (not a CI-blocking gate) — no dedicated Steam Deck
hardware was available in this environment; both desktop-class numbers
above are two to three orders of magnitude under the informal 2-second
stall-detector bound asserted in the benchmark test itself
(`scene_test.cpp`).

## Test suite results

**linux-native**, full serial `ctest --output-on-failure -j1` (23 tests,
run BEFORE the revert probes, then `rx_scene_tests` re-verified green
after each probe/restore):
```
100% tests passed, 0 tests failed out of 23
Total Test time (real) = 153.61 sec
```
`rx_scene_tests` alone: `26 | 26 passed | 0 failed`, `228 | 228 passed | 0 failed`.

**windows-cross-zig under Wine** (`xvfb-run -a ctest -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample' --output-on-failure`,
11 tests — the project's own documented exclusion set for GPU-backed
binaries Wine cannot run):
```
100% tests passed, 0 tests failed out of 11
Total Test time (real) = 145.55 sec
```
`rx_scene_tests` alone under `wine ... rx_scene_tests.exe`:
`26 | 26 passed | 0 failed`, `228 | 228 passed | 0 failed`.

Zero compiler warnings on either preset after the two `[[nodiscard]]`
warnings caught in the first build (fixed with `static_cast<void>(...)` at
the two test-only discard sites, `scene_test.cpp`).

Zero validation-layer/Vulkan involvement (this library touches no GPU
object at all) — the "zero unfiltered validation errors" gate is satisfied
vacuously and by construction, not tested directly (nothing here could
produce one).

## Deviations from the plan/gate text (summary; each detailed above)

1. `MeshBoundsFn` seam instead of a hard `asset::Registry&` dependency on
   `Scene` — necessary for device-free testability of real AABB math; the
   production path (`meshBoundsFromRegistry`) is a byte-for-byte forward to
   `Registry::mesh()`, inheriting its D24 contract.
2. `Camera::aspectRatio` field added (perspective matrix requires it; the
   plan text didn't name it).
3. `detail::createLightRecordForTesting`/`lightRecordForTesting` test-only
   seam added (mirrors `rx_material`'s `detail::debugCompileCount()`
   convention) so the FG2 forward-compat-sizing criterion can be proven
   against a Point-shaped row without exposing Point/Spot creation on the
   Phase-4 public surface.

None of these touch any file outside `src/rx_scene/` or the single
`add_subdirectory` line in the top-level `CMakeLists.txt`.

## Self-review

- Every public `Scene`/light method carries `RX_ASSERT_MAIN_THREAD` (D5) —
  verified enforced, not just documented, by `thread_guard_test.cpp`
  (worker-thread call trips the hook; main-thread call does not).
- Every per-handle mutator/accessor throws `std::out_of_range` for a
  dead/unknown/stale handle (Registry/MaterialSystem's own established
  convention); `destroyRenderable`/`destroyLight` are the one deliberate
  exception (idempotent no-op, matching `HandlePool::release()`'s own
  convention) — documented at each call site.
- No AI attribution anywhere in code, comments, or this report.
- No file outside `src/rx_scene/` was touched except the single
  `add_subdirectory(src/rx_scene)` line in the top-level `CMakeLists.txt`;
  `.superpowers/sdd/.../progress.md`'s own concurrent modification (from
  another active session in this shared tree) was left untouched and is
  excluded from this task's commit via explicit pathspecs.
- No board/issue/plan/spec/ledger file was touched.
- Commits are local only (not pushed).

## Known limitations / what Task 19 (DrawListBuilder) still owns

- The "five-case CI mask matrix" (camera `cullMask` AND renderable
  `layers`) is a culling-behavior test — Scene only provides the storage
  and accessors; the AND-test itself has no consumer until DrawListBuilder
  exists.
- `extractFrustumPlanes()` applied to `Camera::viewProj()` (infinite far)
  yields a documented-degenerate `Far` plane (near-zero normal, always
  "inside") — `cullingFrustumPlanes()` is the sanctioned non-degenerate
  entry point; DrawListBuilder must use it, not `viewProj()`'s planes,
  for real culling.
