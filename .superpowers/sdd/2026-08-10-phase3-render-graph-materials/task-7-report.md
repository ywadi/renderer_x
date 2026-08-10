# Task 7 report: material instances, parameter arena, hot reload

**Branch:** `main` (worked directly, no worktree)
**Commit:** see final commit hash reported alongside this report.

## What was built

### 1. `rx::rhi::DescriptorArena` (new, reusable RHI piece)

`src/rx_rhi_vk/include/rx_rhi_vk/descriptor_arena.h` + `.cpp`: one
`VkDescriptorPool` per frame-in-flight slot (`create(device, framesInFlight,
capacities)` — `framesInFlight` is a runtime parameter, not a hardcoded
constant, so this stays genuinely reusable rather than hardwired to Phase
3's `kFramesInFlight == 2`). `beginFrame(frameIndex)` calls
`vkResetDescriptorPool` on slot `frameIndex % framesInFlight()`;
`allocate(layout)` hands out one `VkDescriptorSet` from the current slot,
returning `VK_NULL_HANDLE` (logged) on exhaustion. Device-free-where-possible:
`create()` needs only a `VkDevice` — no `VkPhysicalDevice`/`VkInstance`/
window, unlike `BindlessTable`, since this class's pools are plain,
non-update-after-bind pools with no per-device capacity ceiling to
pre-check. Default capacities (`DescriptorArenaCapacities`, hoisted to
namespace scope — see the header comment for why a nested version breaks
the default-argument-in-a-member-function pattern): `maxSets = 512`,
`uniformBuffers = 512` per frame-in-flight slot.

Own GPU test: `src/rx_rhi_vk/tests/descriptor_arena_test.cpp` — allocates
across 3 simulated frames (0, 1, 0), proves slot 1 is independent capacity
from slot 0, proves `beginFrame()`'s reset genuinely reclaims slot 0's
capacity (not just "a fresh arena would also work"), writes real UBO
descriptors into allocated sets, and a rejection test for
`framesInFlight == 0`/zero capacities. Added to `rx_rhi_vk_tests`
(`src/rx_rhi_vk/CMakeLists.txt`).

### 2. Reflection collapse (coordinator addition 2)

Task 6's second, throwaway reflection-only Slang session
(`api_impl.cpp`'s `reflectMaterialParams()`, a private
`slang::IGlobalSession` member on `MaterialSystemImpl`) is **gone**.
`material_system.cpp`'s `reflectMaterialLayout()` (renamed return type to
`MaterialReflection`) now also walks `gParams`'s own element type's fields
— name, kind (`classifyFieldKind()`, ported verbatim from the deleted
`classifyFieldType()`), and **byte offset/size** within the block's Uniform
parameter category (`VariableLayoutReflection::getOffset()`/
`TypeLayoutReflection::getSize()`, both defaulting to
`slang::ParameterCategory::Uniform` — verified against the real
`slang.h` API signatures before use, not assumed) — computed once, from the
SAME already-linked `slang::IComponentType` that produces the material's
real SPIR-V, during `loadMaterial()`/`reloadChanged()`. New public types
(`src/rx_material/include/rx_material/instance.h`): `MaterialParamKind`
(`Float`/`Float4`/`TextureIndex`/`Unsupported`, identical classification
rules to the deleted code) and `MaterialParamInfo{name, kind, offset,
size}`. `MaterialSystem::materialParams(handle)`/`paramBlockSize(handle)`
expose them.

`api_impl.cpp`'s `MaterialImpl::paramInfo()` now does a linear scan over
`internalSystem_->materialParams(handle_)` — no Slang, no second session,
no `<slang.h>`/`<slang-com-ptr.h>` include in `api_impl.cpp` at all anymore
(a real simplification, not just a refactor: the file no longer touches
Slang directly in any way). `MaterialSystemImpl::loadMaterial()` collapsed
from "reflect first (side-effect-free), then call `internal_->
loadMaterial()`" (Task 6's F2 fix) down to a single `internal_->
loadMaterial()` call — the separate pre-flight reflection pass is gone
because there is no longer a second reflection to run; `internal_->
loadMaterial()`'s own already-correct exception safety (never registers a
`MaterialRecord` on throw) means there is nothing to orphan by calling it
directly.

### 3. Instance parameter arena → real GPU binding (coordinator addition 3)

`src/rx_material/include/rx_material/instance.h` + `instance.cpp`:
`ParamArena` — per-frames-in-flight, host-visible, persistently-mapped,
bump-allocated uniform-buffer arena (`kBytesPerFrame = 1 MiB`,
`kMaxInstancesPerFrame = 512`, matching `DescriptorArena`'s own default
capacities) paired with one `rx::rhi::DescriptorArena`. Each bump
allocation is rounded up to `kUniformBufferAlignment = 256` bytes before
use — the Vulkan spec's own guaranteed *upper bound* on
`minUniformBufferOffsetAlignment`, so this is always sufficient regardless
of the real device's (smaller-or-equal) requirement, without needing to
query it. `writeAndAllocate(setLayout, data, size)`: memcpy's into the
current frame's buffer at the aligned offset, flushes
(`Buffer::flush()` — correct and cheap even when already coherent),
allocates a set from the paired `DescriptorArena`, writes binding 0 as a
`VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` pointing at exactly those bytes.

`MaterialSystem` (material_system.h/.cpp) gained:
- `beginFrame(frameInFlightIndex, frameNumber)` — resets the arena/pool
  slot for `frameInFlightIndex` and stashes `frameNumber` (a monotonic
  counter, mirroring `FrameSync::frameNumber()`, distinct from the cycling
  `frameInFlightIndex`) for tagging internal `DeletionQueue` retirements.
- `onFrameCompleted(completedFrameNumber)` — forwards to the internal
  `DeletionQueue::onFrameFenceSignaled()`.
- `bindInstance(cmd, PassContext&, InstanceBinding{material, paramData,
  paramSize})` — **internal API, not on the ABI surface this phase**
  (D5/D11: draw submission is not public in Phase 3). Resolves/binds the
  material's `VkPipeline` for `passContext.passSignature()` (via the
  existing `getPipeline()`), validates `paramSize == paramBlockSize
  (material)` (throws `std::invalid_argument` otherwise — every real
  caller already has that size from `paramBlockSize()` itself), then
  `writeAndAllocate()`s the blob and binds the resulting set at **set
  index 1** (`kMaterialParamBlockSet`). Never touches set 0 — sampling
  stays bindless, matching the established pattern (samples/03's own
  `vkCmdBindDescriptorSets(..., 0, ...)` for the bindless table, done
  once per frame by the caller, not by this method).

`api_impl.cpp`'s `MaterialInstanceImpl` now owns a real
`std::vector<uint8_t> blob_` (sized to `paramBlockSize()` at construction,
zero-initialized) instead of Task 6's `unordered_map<string, StoredParam>`
of tagged unions. `setFloat`/`setFloat4` `memcpy` the value straight into
`blob_.data() + info->offset`. `setTexture` recovers a **real** bindless
index from the passed `IRxTexture*` through a private, ABI-invisible
`queryInterface`-based bridge (`kIID_InternalTextureBridge` — see "GUID
notes" below) rather than RTTI (D5 forbids `dynamic_cast`/`typeid`
anywhere on this boundary's object model); a texture that doesn't answer
to that GUID (Task 6's `FakeTexture` test double, or any non-engine-created
`IRxTexture`) falls back to writing index `0` — this is exactly what keeps
Task 6's existing `FakeTexture`-based refcount/lifecycle tests passing
unchanged. A `std::unordered_map<std::string, IRxTexture*> textures_`
alongside `blob_` still exists purely for COM refcount bookkeeping (the
GPU-bound bytes are the plain `u32` index, not the pointer).

`rx_api_detail.h` gained the draw-time bridge a future consumer (sample
06, Task 8) needs: `materialHandle(IRxMaterial*)`,
`materialInstanceBlobData/Size(IRxMaterialInstance*)` — safe, documented
unchecked downcasts, valid specifically because every `IRxMaterial*`/
`IRxMaterialInstance*` reachable at all was created by this same
`api_impl.cpp` (there is no other factory for either). This is a genuine
production seam, not test-only, despite living in the same header as
`debugLiveApiObjectCount()` — the header comment now says so explicitly.

### 4. `IRxTexture` creation path (coordinator addition 4)

`rx_api.h`: new `RxFormat` (`RX_FORMAT_RGBA8_UNORM`/`RX_FORMAT_RGBA8_SRGB`,
same `typedef int32_t` + unnamed-`enum` shape as `RxResult`) and
`RxTextureDesc` (`pixels`, `pixelBytes`, `width`, `height`, `format`,
`generateMips` — 32 bytes, no padding, `static_assert`-pinned) and
`IRxMaterialSystem::createTexture2D(const RxTextureDesc*, IRxTexture**)`.

`rx::material::MaterialSystem` (material_system.h/.cpp) gained
`createTexture2D(TextureCreateInfo)` → `TextureHandle` (a `HandlePool`-
backed registry, same handle-not-pointer discipline as `MaterialHandle`),
`textureBindlessIndex(handle)`, `releaseTexture(handle)`. Implementation:
`rx::rhi::Texture2D::create()` (device-local, `VK_IMAGE_USAGE_SAMPLED_BIT`)
+ this `MaterialSystem`'s own internal `rx::rhi::Uploader` (built once in
`create()`, alongside a new internal `rx::rhi::Allocator` — both sourced
from `Device`'s already-stored handles, no new `create()` parameter
needed) + `bindless->registerSampledImage()`. `releaseTexture()` follows
bindless.h's own RELEASE-SAFETY CONTRACT exactly: the `BindlessTable` slot
is freed immediately (safe — it never rewrites/destroys anything a
pending command buffer might read), and the real `Texture2D` teardown is
wrapped in a `shared_ptr` (for `DeletionQueue`'s own
copy-constructibility requirement) and `retire()`d, tagged with the frame
number `beginFrame()` most recently stashed — actually destroyed only
once `onFrameCompleted()` confirms that frame is done. Invalid/
already-released handles are a logged no-op, never a throw (this is the
path a COM-lite `IRxTexture::release()` calls unconditionally at refcount
zero, and D5 forbids anything reachable from that boundary from throwing).

`api_impl.cpp`'s new `TextureImpl` : `RxUnknownBase<TextureImpl,
IRxTexture>`, `IInternalTextureBridge` (ordinary, disjoint-vtable multiple
inheritance — no diamond/refcount-sharing complexity, since
`IInternalTextureBridge` is deliberately NOT `IRxUnknown`-rooted).
`~TextureImpl()` calls `internalSystem_->releaseTexture(handle_)`.

### 5. Hot reload (coordinator addition 1, D9)

`MaterialSystem::reloadChanged()` (material_system.cpp): walks
`materialHandles`, stats each `MaterialRecord::path` (best-effort — a
momentarily-unreadable file is skipped, retried next call, never fatal,
matching `samples/02_hotreload`'s own "02 pattern"), tracks
`lastKnownMtime` per record (a fresh record's first `reloadChanged()` call
just establishes the baseline, never treats "never observed" as
"changed"). On a real mtime change: builds a **fresh** `slang::ISession`
(`createMaterialSession()` — factored out of `create()` itself, so the two
paths cannot silently drift out of the exact configuration Task 6's F3
fix was about) reusing the already-expensive `impl.globalSession`, loads a
fresh `forward_entry.slang` + its two entry points into it
(`loadForwardEntry()`, also factored), and recompiles the changed material
against that fresh session+entry-module through the SAME core
`compileMaterial()` free function `loadMaterial()` itself uses (parameterized
on which session/entry-module/entry-points to use) — this is
`MaterialSystem`'s own analogue of `compiler.h`'s documented "fresh
Compiler per reload" same-module-name caveat, since reusing `impl.session`
for the SAME module path/name a second time would hit Slang's own
"already loaded with different source" diagnostic.

On success: erases every `getPipeline()`-cached `VkPipeline` keyed by the
module's OLD content hash and `retire()`s them through the internal
`DeletionQueue` (tagged with `beginFrame()`'s stashed frame number) —
subsequent `getPipeline()` calls re-link lazily against the NEW hash,
exactly like a first-time miss. The OLD `VkShaderModule`s and
`PipelineLayoutBundle` (pipeline layout + descriptor set layouts) are
destroyed **immediately**, not deferred — this is deliberate, not an
oversight: per the Vulkan spec's own documented behavior (the same one
that lets a `VkShaderModule` be destroyed immediately after the pipelines
built from it exist), a pipeline layout/descriptor-set-layout only needs
to remain valid through `vkCreate*Pipelines`/command-buffer *recording*
that references it, never through GPU *execution* — unlike `VkPipeline`
itself, which command execution genuinely does reference for its
duration. D9's own text ("pipeline destruction goes through the existing
fence-gated DeletionQueue") only calls out pipelines for exactly this
reason. On failure: logs (`RX_LOG_WARN`, Slang's diagnostic text
included) and leaves `record` completely untouched past the
`lastKnownMtime` update — keep-last-good, never a caller-visible error.

`IRxMaterialSystem::reloadChanged()` (`rx_api.h`) is wired to the real
`MaterialSystem::reloadChanged()`; its own contract ("always returns
RX_OK") is preserved — a per-material reload failure is logged inside
`reloadChanged()` itself, never surfaced as a non-RX_OK return.

**Mandatory 3-version regression test** (`test_material_system.cpp`):
v1 (load) → `getPipeline()` → P1; overwrite with v2 (different tint math,
identical reflected shape, mtime forced strictly later) →
`reloadChanged()` → `getPipeline()` (same request) → P2 ≠ P1, new content
hash; overwrite with v3 (syntactically broken, same missing-semicolon
shape as `test_bad_syntax.slang`) → `reloadChanged()` → `getPipeline()`
(same request) → still P2, same hash as v2. Also exercised through the
public ABI (`test_api_factory.cpp`'s `IRxMaterialSystem::reloadChanged`
test), confirming the wiring reaches the real internal path, not just a
forwarding stub.

## GUID regeneration note

`IRxMaterialSystem` gained a new pure-virtual method
(`createTexture2D`) — a real vtable-shape change. Per COM discipline,
this **requires** a new IID: an existing binary compiled against the old
three-method vtable must never be handed an object whose real vtable has
four slots under the SAME IID. `kIID_IRxMaterialSystem` was regenerated
in place (via `uuidgen`, converted through the same one-off Python script
Task 6 used, not hand-transcribed) rather than versioned into a new
`IRxMaterialSystem2` — this is the correct call specifically because
**nothing has shipped yet**: there is no existing binary anywhere holding
the OLD GUID's contract to preserve compatibility with. `rx_api.h` now
documents this explicitly at the GUID site: every future PRE-RELEASE
interface-shape change in this repo should regenerate its GUID the same
way; POST-release, the correct move becomes a new versioned interface
instead. `IRxTexture`/`IRxMaterial`/`IRxMaterialInstance` were not
touched (their own shapes didn't change) and keep their existing GUIDs.

A second, unrelated GUID was generated for `kIID_InternalTextureBridge`
(api_impl.cpp, private/anonymous-namespace, never in `rx_api.h`, never
crossing the ABI as a documented capability) — see "What was built,
section 3" above.

## Arena sizing constants + limits (all documented at their declaration site)

- `rx::rhi::DescriptorArenaCapacities`: `maxSets = 512`, `uniformBuffers =
  512` per frame-in-flight slot (default).
- `rx::material::ParamArena::kBytesPerFrame` = 1 MiB per frame-in-flight
  slot; `kMaxInstancesPerFrame` = 512 (matches DescriptorArena's default);
  `kUniformBufferAlignment` = 256 bytes (Vulkan spec's guaranteed upper
  bound on `minUniformBufferOffsetAlignment` — always sufficient
  regardless of the real device's own, possibly smaller, requirement).
- `MaterialSystem::kFramesInFlight` = 2, documented as required to match
  `rx::rhi::FrameSync::kFramesInFlight` (not literally shared — this
  header does not include `frame_sync.h` purely for one constant).
- Exhaustion of either arena is a clean, logged `VK_NULL_HANDLE`/thrown
  `std::runtime_error` (from `bindInstance()`), never a driver-dependent
  failure or silent corruption.

## Test results

- `ctest --preset linux-native --output-on-failure`: **14/14 passed**
  (full repo regression — nothing outside rx_material/rx_rhi_vk touched
  or regressed).
  - `rx_rhi_vk_tests`: DescriptorArena's own 2 new test cases included
    (28 assertions), zero validation errors.
  - `rx_material_gpu_tests`: **22 test cases / 296 assertions**, all
    passing, zero validation errors under `--validate`. New Task 7 cases:
    `materialParams`/`paramBlockSize` real-field reflection (both
    fixtures), invalid-handle throws, the mandatory 3-version hot-reload
    regression, `bindInstance()` end-to-end against a REAL
    `rx::graph::RenderGraph`+`Executor` (the only legitimate source of a
    real `PassContext&` — its constructor is private/friend-gated to
    `Executor` alone, so this required building an actual single-pass
    graph rather than a lighter substitute), `bindInstance()`'s
    `paramSize` mismatch rejection, `createTexture2D`/
    `textureBindlessIndex`/`releaseTexture` + `onFrameCompleted` lifecycle,
    `createTexture2D` input validation. Plus ABI-layer additions in
    `test_api_factory.cpp`: byte-exact blob write→readback via
    `rx_api_detail.h`'s bridge (the brief's mandatory "instance param
    write→readback of arena blob at reflected offsets (exact bytes)"
    case, exercised at the ABI layer), `createTexture2D` through the
    public ABI + `setTexture()`'s REAL (not `FakeTexture`) bindless-index
    write path, `createTexture2D` malformed-input rejection,
    `reloadChanged()` forwarding to a real internal reload (hash changes
    observably), `reloadChanged()` device-free no-op.
  - `rx_material_tests` (device-free): **10 test cases / 50 assertions**,
    unchanged pass count — confirms zero regression on the device-free ABI
    contract surface. `test_api_header_self_contained.cpp` extended to
    name `RxTextureDesc`/`RxFormat`/`createTexture2D` too (same
    "cannot be quietly defeated by an unreferenced declaration" rationale
    the file already documents for every other symbol).
- Both presets build clean: `cmake --build build/linux-native` and
  `cmake --build build/windows-cross-zig` — zero errors on either, for
  every touched/new target (`rx_rhi_vk`, `rx_rhi_vk_tests`, `rx_material`,
  `rx_material_gpu_tests`, `rx_material_tests`).
- Manual warning check (this repo's CMake sets no `-Wall`/`-Wextra`
  anywhere): extracted every new/changed file's real compile command from
  `compile_commands.json` and reran each with `-Wall -Wextra -Wpedantic
  -Wshadow` appended. Zero warnings/errors originating in any of this
  task's own files — the only warnings observed at all are pre-existing
  third-party `-Wnullability-extension` noise from vendored VMA headers
  (`vk_mem_alloc.h`'s own `_Nonnull`/`_Nullable` macros, included via
  `-I` not `-isystem`), unrelated to this task and present for any file
  that transitively includes `buffer.h`.
- AI-attribution grep (`claude|anthropic|co-authored|generated by|ai
  assistant|chatgpt|openai|copilot`, case-insensitive) across every
  changed/new file: zero matches.

## Files

- `src/rx_rhi_vk/include/rx_rhi_vk/descriptor_arena.h` (new)
- `src/rx_rhi_vk/src/descriptor_arena.cpp` (new)
- `src/rx_rhi_vk/tests/descriptor_arena_test.cpp` (new)
- `src/rx_rhi_vk/CMakeLists.txt` (modified — registers the two new files)
- `src/rx_material/include/rx_material/instance.h` (new)
- `src/rx_material/instance.cpp` (new)
- `src/rx_material/include/rx_material/material_system.h` (modified —
  `MaterialParamInfo`/`TextureCreateInfo`/`TextureHandle`,
  `materialParams`/`paramBlockSize`/`beginFrame`/`onFrameCompleted`/
  `bindInstance`/`reloadChanged`/`createTexture2D`/`textureBindlessIndex`/
  `releaseTexture`)
- `src/rx_material/material_system.cpp` (modified — field-level reflection,
  shared `compileMaterial()`/`createMaterialSession()`/`loadForwardEntry()`
  cores, `Impl` gains `physicalDevice`/`allocator`/`uploader`/`paramArena`/
  `deletionQueue`/`currentFrameNumber`/texture registry, real
  `reloadChanged()`/`bindInstance()`/`createTexture2D()`/
  `releaseTexture()`)
- `src/rx_material/api_impl.cpp` (modified — second Slang session deleted;
  blob-based `MaterialInstanceImpl`; new `TextureImpl` +
  `IInternalTextureBridge`; `loadMaterial`/`reloadChanged`/
  `createTexture2D` wired to the real internal system)
- `src/rx_material/include/rx_material/rx_api.h` (modified — `RxFormat`/
  `RxTextureDesc`, `IRxMaterialSystem::createTexture2D`, regenerated
  `kIID_IRxMaterialSystem` with GUID-regen-discipline comment, updated
  `reloadChanged`/`IRxMaterialInstance` doc comments)
- `src/rx_material/include/rx_material/rx_api_detail.h` (modified —
  `materialHandle`/`materialInstanceBlobData`/`materialInstanceBlobSize`
  draw-time bridge, header comment split into "test-only" vs "production
  bridge" seams)
- `src/rx_material/CMakeLists.txt` (modified — adds `instance.cpp`)
- `src/rx_material/tests/test_material_system.cpp` (modified — 7 new
  `TEST_CASE`s)
- `src/rx_material/tests/test_api_factory.cpp` (modified — 5 new
  `TEST_CASE`s)
- `src/rx_material/tests/test_api_contract.cpp` (modified — updated one
  test's title/comment to reflect real Task 7 behavior; the device-free
  assertion itself is unchanged)
- `src/rx_material/tests/test_api_header_self_contained.cpp` (modified —
  names the three new symbols)

## Concerns

- **`bindInstance()`'s test requires a real `RenderGraph`+`Executor` rig.**
  `PassContext`'s constructor is private and friend-gated to `Executor`
  alone (a deliberate Task 3 fix), so there is no lighter-weight way to
  obtain a real one for a unit test — `test_material_system.cpp`'s
  `bindInstance()` cases build an actual single-pass graph with an
  offscreen color attachment. This is heavier test infrastructure than
  the rest of this file's cases, but it is the only way to exercise the
  method as specified (`PassContext&`, not a bare `PassSignature`) rather
  than weakening the method's own signature for testability.
- **`setTexture()`'s bindless-index recovery has a silent fallback.** A
  third-party (non-engine-created) `IRxTexture` — including Task 6's own
  `FakeTexture` test double — has no real bindless registration to
  report, so `setTexture()` writes index `0` into the blob for it rather
  than failing the call. This is the only way to keep Task 6's existing
  `FakeTexture`-based tests passing without RTTI (D5 forbids
  `dynamic_cast`/`typeid` on this boundary), and is explicitly documented
  at `kIID_InternalTextureBridge`'s own declaration, but a caller who
  passes a non-engine texture and expects a *specific* wrong index to be
  rejected (rather than silently defaulting to 0) would not get that from
  this ABI today — sample 06 (Task 8) will only ever pass real
  `createTexture2D()`-backed textures, so this has no real-usage impact
  in Phase 3, but it is a real, if narrow, seam a reviewer should weigh.
- **`RxMaterialSystemDesc.internalMaterialSystem == nullptr` being legal**
  (flagged by Task 6's own report, item 5) was deliberately NOT revisited
  this task — it remains in scope for whenever a real external consumer
  exists, per Task 6's own framing, and nothing in Task 7's coordinator
  charter asked for it.
- **Pipeline-layout/descriptor-set-layout immediate destruction during
  reload** (not deferred through `DeletionQueue`, unlike `VkPipeline`
  itself) relies on the general Vulkan spec behavior that such objects
  need only remain valid through pipeline creation/command-buffer
  recording, never through GPU execution — the same behavior this
  codebase's own `PipelineLayoutBundle`/shader-module teardown already
  relies on elsewhere (and D9's own text scopes the DeletionQueue
  requirement to "pipeline destruction" specifically). Recorded here
  explicitly as a reviewed, deliberate design choice, not an oversight.

---

## Fix round 1 (review: `task-7-review.md`, commit `067fdde`)

Review verdict: spec ❌ on "production grade" (1 High: F1) plus 1 Medium
(F2). Both addressed below.

### F1 (High) — `setTexture()` silently bound bindless index 0 for a non-engine `IRxTexture`

**Root cause.** `MaterialInstanceImpl::setTexture()` already used the
correct, RTTI-free detection mechanism (a private
`kIID_InternalTextureBridge` `queryInterface` check to distinguish a real,
`createTexture2D()`-backed `TextureImpl` from anything else), but applied
the wrong *policy* on a QI failure: `bindlessIndex` stayed `0` (a real,
arbitrary bindless slot) and the call still returned `RX_OK`. A caller
passing any non-engine `IRxTexture` got silently wrong rendering with zero
diagnostic signal — the review correctly identified this as inconsistent
with `rx_api.h`'s own documented `IRxTexture` invariant ("always wraps a
renderer-owned... texture, created via `IRxMaterialSystem::
createTexture2D()`"), and with the reason originally given for keeping
the fallback (preserving Task 6's `FakeTexture` refcount tests) as not
holding up: the bridge GUID is anonymous-namespace-private, so no test
double could ever legitimately answer it regardless of policy.

**Fix applied** (`api_impl.cpp`): the QI-failure branch now returns
`RX_E_INVALIDARG` immediately (logged) instead of defaulting
`bindlessIndex` to `0` and proceeding. The real bindless index is only
ever read once the QI has already succeeded. `kIID_InternalTextureBridge`'s
own header comment updated to describe the reject-not-fallback behavior.
`rx_api.h`'s `IRxMaterialInstance`/`setTexture` doc comment now states the
invariant explicitly at the call site itself, not only on `IRxTexture`'s
own comment: `texture` must be engine-created (via `createTexture2D()`,
directly or through a `queryInterface()` chain on one); any other
implementation is rejected with `RX_E_INVALIDARG`.

**Test changes** (`test_api_factory.cpp`) — adapted, not deleted, per the
coordinator's instruction:
- The old "`IRxMaterialInstance::setTexture` validates a real
  bindless-index (uint) parameter and manages the `IRxTexture` reference
  it stores" test (which asserted `RX_OK` for a `FakeTexture`) is now
  "`IRxMaterialInstance::setTexture` rejects a non-engine-created
  `IRxTexture`... with `RX_E_INVALIDARG`" — same `FakeTexture` double,
  same fixture, now asserting the rejection, plus a new assertion that the
  rejection happens *before* any refcount mutation (`fake->refCount()`
  unchanged across the rejected call — proving the QI check runs ahead of
  any addRef()).
- A NEW test, "`IRxMaterialInstance::setTexture` manages the `IRxTexture`
  reference it stores across overwrite and instance destruction (real
  engine-created textures)", preserves the exact addRef/release lifecycle
  coverage the old `FakeTexture`-based assertions provided — same
  overwrite/destroy sequence, same three checkpoints (store, overwrite,
  instance destruction) — but against two REAL `IRxTexture`s obtained via
  `IRxMaterialSystem::createTexture2D()`, since a rejected texture never
  reaches the refcount-mutating code path at all. `TextureImpl` exposes no
  `refCount()` accessor of its own (unlike the test-only `FakeTexture`), so
  this test reads a live texture's current refcount via a new
  side-effect-free `probeRefCount()` helper (one `addRef()` immediately
  undone by one `release()`, net zero) rather than adding a debug-only
  accessor to the ABI-facing `IRxTexture`/`TextureImpl` itself.
- Every OTHER pre-existing `FakeTexture`-based `setTexture` call in this
  file (the "happy path" test's `wrongTypeTexture`/`notFoundTexture`, the
  "entry-point audit" test's `texture`) is unaffected by this fix: each of
  those hits a name-not-found or field-kind mismatch before the
  engine-texture check would ever run, so their expected results are
  unchanged — verified by re-running, not just reasoned about.

### F2 (Medium) — no standalone `ParamArena` test; every delivered exercise used `frameInFlightIndex == 0` only

**Root cause.** `instance.h`'s own header comment claimed `ParamArena`
"[l]ives at namespace scope... so it can be unit-tested on its own," but
no test actually did so — every exercise went through
`MaterialSystem::bindInstance()`, and both of `test_material_system.cpp`'s
`bindInstance()` cases called `beginFrame(0, ...)` exclusively. Nothing
confirmed slot isolation at the `ParamArena`-composed layer (as opposed to
`DescriptorArena` alone, which already had this coverage) or confirmed the
documented exhaustion/failure path is reachable in practice at a non-zero
frame-in-flight index.

**Fix applied:** new `src/rx_material/tests/test_param_arena.cpp` (added
to `rx_material_gpu_tests`), exercising `ParamArena` directly against a
bare `VkDevice` + `rx::rhi::Allocator` — no `MaterialSystem`,
`BindlessTable`, or `RenderGraph`/`Executor` involved, mirroring
`rx_rhi_vk/tests/descriptor_arena_test.cpp`'s own minimal headless fixture
(`Context::create({}, true)` + a bare `vkb::PhysicalDeviceSelector`/
`DeviceBuilder`, no descriptor-indexing or dynamic-rendering features
needed). A new test-only seam, `rx::material::detail::
debugFrameBufferData(const ParamArena&, uint32_t frameIndex)`
(`instance.h`/`.cpp`, `friend`-gated into `ParamArena`, mirroring this
codebase's own `detail::debugCompileCount()`/`detail::
debugLastFrameFinalStages()` carve-out convention), reads back the raw
bytes a given frame-in-flight slot's own host-visible buffer actually
holds — `writeAndAllocate()` itself returns only an opaque
`VkDescriptorSet` by design, so there was no other way to observe this
from outside `ParamArena`'s own implementation.

Two new test cases, both exercising `frameInFlightIndex` values other
than (and in addition to) `0`, per the coordinator's explicit instruction:
- **Frame isolation + reset-not-fresh-arena**: `beginFrame(0)` writes blob
  A; `beginFrame(1)` writes blob B; asserts slot 0 still holds exactly
  blob A's bytes (untouched by slot 1's own bump allocation) and slot 1
  holds exactly blob B's. Then `beginFrame(2)` (wraps back to slot 0) —
  writes blob C and asserts it landed at slot 0's buffer offset 0 again
  (proving the reset, not merely a coincidentally-still-empty arena, is
  what reclaimed the capacity) while slot 1 still holds blob B untouched.
- **Exhaustion, at `frameInFlightIndex == 1`**: (a) a single
  `writeAndAllocate()` request larger than `kBytesPerFrame` fails cleanly
  (`VK_NULL_HANDLE`, logged), and a normal-sized write immediately after
  it still succeeds with byte-exact content — proving the failed oversized
  request did not corrupt the slot; (b) a fresh `beginFrame(1)`, then
  `kMaxInstancesPerFrame` (512) successful `writeAndAllocate()` calls
  followed by one more that must fail cleanly — proving the descriptor-
  pool ceiling is reached at exactly the documented number and the
  overflow attempt fails without corrupting the 512 already-written
  blobs (re-checked byte-exact after the failed 513th call).

Both new cases pass, zero validation errors — confirmed the real Vulkan
error paths fire as designed (`rx::material::ParamArena::writeAndAllocate:
frame slot 1 exhausted...` and `rx::rhi::DescriptorArena::allocate:
vkAllocateDescriptorSets failed for frame slot 1...` both observed in the
log during this run, not merely asserted blind).

**Not fixed, noted for the record:** `writeAndAllocate()`'s byte-arena
cursor advances (memcpy + `cursor = end`) *before* attempting the
`DescriptorArena::allocate()` call; if that later allocation fails (pool
exhausted), the cursor has still moved, wasting some byte-arena space on
the failed call. This does not corrupt any already-written data (the
bounds check for the *next* write is still against the real, moved-forward
cursor, so no overlap can occur) and was not something the review's F2
finding asked to be fixed (it asked for a *test*, not a behavior change,
and no test assertion here depends on the cursor staying put after a
failed allocation) — flagged here explicitly rather than silently
patched, since a coordinator revisiting this later may want the cursor
rolled back on that specific failure path for byte-arena efficiency.

### Re-verification after both fixes

- `ctest --preset linux-native --output-on-failure`: **14/14 passed**
  (full repo regression).
- `rx_material_gpu_tests --validate`: **25 test cases / 344 assertions**
  (up from 22/296 — net +1 test case from splitting the F1 test into two,
  +2 new `ParamArena` cases), all passing, zero validation errors.
- `rx_material_tests`: **10 test cases / 50 assertions**, unchanged —
  confirms zero regression on the device-free ABI contract surface.
- `rx_rhi_vk_tests`: unaffected by this round's changes, re-run for
  completeness — unchanged pass count.
- Both presets rebuilt clean: `cmake --build build/linux-native` and
  `cmake --build build/windows-cross-zig` — zero errors, every
  touched/new target (`rx_rhi_vk`, `rx_material`, `rx_material_gpu_tests`,
  `rx_material_tests`) relinked successfully.
- Manual warning re-check (same method as the original submission —
  extracted each changed/new file's real `compile_commands.json`
  invocation, ran it with `-Wall -Wextra -Wpedantic -Wshadow` appended):
  zero warnings/errors originating in `api_impl.cpp`, `instance.cpp`,
  `test_api_factory.cpp`, `test_param_arena.cpp`.
- AI-attribution grep on every file touched this round: zero matches.

### Files touched this round

- `src/rx_material/api_impl.cpp` (F1: `setTexture()` now rejects a
  QI-bridge failure with `RX_E_INVALIDARG`; updated
  `kIID_InternalTextureBridge`'s own header comment).
- `src/rx_material/include/rx_material/rx_api.h` (F1: documented the
  engine-created-texture-only invariant directly at `IRxMaterialInstance`/
  `setTexture`'s own doc comment).
- `src/rx_material/include/rx_material/instance.h` (F2: new
  `detail::debugFrameBufferData()` test-only seam, `friend`-declared into
  `ParamArena`).
- `src/rx_material/instance.cpp` (F2: `debugFrameBufferData()`
  implementation).
- `src/rx_material/tests/test_api_factory.cpp` (F1: split/adapted the
  `FakeTexture`-based `setTexture` test into a rejection test + a new
  real-texture refcount-lifecycle test with a `probeRefCount()` helper).
- `src/rx_material/tests/test_param_arena.cpp` (F2: new standalone
  `ParamArena` test file — frame isolation/reset, exhaustion).
- `src/rx_material/tests/CMakeLists.txt` (F2: registers
  `test_param_arena.cpp` on `rx_material_gpu_tests`).
- This report (`task-7-report.md`) — this Fix round 1 section.
