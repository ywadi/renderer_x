# Task 4 report: material texture sampling wiring (seed 10, carried from Phase 3)

## What this closes

Phase 3 Task 8's review (`.superpowers/sdd/2026-08-10-phase3-render-graph-materials/task-8-review.md`,
"The central question: texture path vs. texture sampling") found the public
`createTexture2D` → `setTexture` path already reached a real GPU-visible
bindless index, but no material shader could actually *sample* it: neither
`material.slang` nor `forward_entry.slang` declared a single bindless
resource. This task closes exactly that gap: a material's `evaluate()` can
now call `rx_sampleTexture(gParams.albedoIndex, uv)` and have it visibly
change the rendered pixel.

## Design decisions and where they landed

**Shader side (`shaders/material/material.slang`).** The bindless globals
and `rx_sampleTexture` live in `material.slang`, not `forward_entry.slang`:
every material module already `import material;` (see
`test_textured.slang`), while `forward_entry.slang` itself never samples a
texture — so the globals a material calls have to live in the module
materials actually import.

- `gTextures`/`gSamplers`: the same three-binding global bindless
  descriptor set every other shader in this codebase already declares
  against the real `rx::rhi::BindlessTable` (`[[vk::binding(0,0)]]
  Texture2D gTextures[]`, `[[vk::binding(1,0)]] SamplerState gSamplers[]`
  — identical shape to `shaders/multipass/lit.vert.slang`). No new
  descriptor set: `reflectMaterialLayout()` recognizes exactly this shape
  and folds it into the SAME `BindlessTable` every other set-0 binding
  uses, reusing Phase 3's existing `PipelineLayoutBuilder::build()`
  external-set-0 substitution unchanged.
- **Default sampler index, resolved via a push constant, not a fixed
  slot.** `rx_sampleTexture(uint textureIndex, float2 uv)`'s signature (per
  the brief) carries no sampler-index parameter, so *some* mechanism has to
  supply which bindless sampler slot to use. A compile-time-fixed slot
  (e.g. "sampler 0") was rejected outright: `BindlessTable` is this
  engine's single **process-wide** bindless set, so nothing guarantees
  `MaterialSystem`'s own default sampler is the first one ever registered
  against a table other subsystems (present/future: shadow maps, a KTX2
  sampler cache, ImGui) also register samplers into. Instead
  `MaterialSystem::create()` creates and registers exactly one default
  sampler once, and `bindInstance()` writes its real registered bindless
  index into a new `gMaterialGlobals` push-constant range on every
  draw-time bind — the same "push-constant-carried bindless index" idiom
  `lit.vert.slang`/`tonemap.vert.slang` already use elsewhere in this
  codebase, reused rather than inventing a new mechanism (e.g.
  specialization constants, which are also possible but would have been a
  genuinely new mechanism in this codebase; the push-constant path reuses
  an already-proven one).
- **Sampler recipe: LINEAR + CLAMP_TO_EDGE.** LINEAR matches this
  codebase's own "general-purpose color texture" convention
  (`samples/03_bindless_mesh`'s `linearInfo`). CLAMP_TO_EDGE, **not**
  sample 03's own REPEAT, was a real finding, not a stylistic pick: this
  is one process-wide default shared by every material, and a material
  sampling right up to UV 0/1 at a mesh seam gets wrong-neighbor bleed
  under REPEAT (verified directly — the first implementation used REPEAT
  and produced a visible 4-way-blended-corner artifact at this task's own
  quadrant probes; see "What went wrong once" below) versus a well-defined
  edge-duplicated result under CLAMP_TO_EDGE. Per-material sampler/wrap
  selection is real, supportable future work, not built speculatively here
  since nothing tests it yet.

**Reflection (`src/rx_material/material_system.cpp`,
`reflectMaterialLayout()`).** The pre-existing walk hard-rejected any
top-level global that wasn't the material's own `ParameterBlock<TParams>
gParams`. It now recognizes two additional shapes, both narrowly scoped to
exactly what `material.slang` declares (matching this function's own
established discipline of rejecting anything untested rather than
generalizing speculatively):

1. `DescriptorTableSlot`-category globals at set 0, binding
   `BindlessTable::kSampledImageBinding`/`kSamplerBinding`, classified by
   element kind (mirroring `rx_shader::reflect()`'s own `mapElementType()`)
   and confirmed genuinely unbounded via a NAME-correlated
   `getBindingRangeBindingCount()` lookup (not index-correlated, unlike
   `reflect()`'s own flat-globals-only walk — this program's top-level
   parameters also include a `ParameterBlock`, and whether that consumes a
   slot in `getGlobalParamsTypeLayout()`'s own binding-range list was not a
   documented fact worth assuming either way).
2. `PushConstantBuffer`-category globals whose reflected element type name
   is exactly `RxMaterialGlobals`, with a defensive size check
   (`bindInstance()` pushes exactly `sizeof(uint32_t)` bytes for it; a
   mismatch is rejected at load time rather than risking an out-of-bounds
   push).

**Empirical finding that reshaped the ledgered test expectations
(`test_material_system.cpp`).** The original plan assumed Slang would
dead-code-eliminate `gTextures`/`gSamplers`/`gMaterialGlobals` from a
material that never calls `rx_sampleTexture`. Verified directly against
this project's shipped Slang build (not assumed) that this is **false**:
`import material;` pulls every one of `material.slang`'s own top-level
globals into the importing module's linked program regardless of which of
them the reachable entry-point code actually touches. So **every**
material — `test_unlit.slang` included, which references none of the new
globals — now reflects 3 bindings (its own `gParams` plus the two bindless
arrays) and 1 push range, not just the original 1 binding.
`test_material_system.cpp`'s own `"MaterialSystem::loadMaterial reflects
the set-1 parameter block..."` test was updated to assert this real shape
(via a `findBinding(set, binding)` lookup rather than a hardcoded index,
since Slang's own top-level parameter ordering for this composite is not a
documented contract worth pinning down beyond "some order"). This is
harmless by construction: the same external `BindlessTable` substitution
applies whether a material's own SPIR-V touches 0, 1, 2, or all 3 of the
set-0 slots, and `bindInstance()`'s push-constant write is itself guarded
on `record->layoutInfo.pushRanges` being non-empty, not on an assumption
about which materials need it.

## Test fixture and GPU tests

`src/rx_material/tests/data/test_textured_sample.slang` (new data file, no
`tests/CMakeLists.txt` change needed — data files aren't individually
enumerated there): single `uint albedoIndex` field, `evaluate()` returns
`rx_sampleTexture(gParams.albedoIndex, uv)` directly — the real sampling
case, deliberately distinct from `test_textured.slang`'s own
numerically-inert-read fixture (kept as-is; it is the "path but not
sampling" case Task 8's review found and documented, still valid as that).

Two new `TEST_CASE`s added to the existing `src/rx_material/tests/test_api_factory.cpp`
(no new `.cpp` file, no `tests/CMakeLists.txt` touch):

1. **`"IRxMaterialInstance::setTexture's bound texture actually changes the
   rendered image via rx_sampleTexture..."`** — `createTexture2D` (public
   API) with a hand-built 2×2 four-color RGBA8 texture (row-major: TL, TR /
   BL, BR), `setTexture` (public API) onto a `test_textured_sample.slang`
   instance, then a full-screen quad rendered through the internal
   `MaterialSystem::bindInstance()` bridge (the same
   public-API/internal-bridge split sample 06_materials established — draw
   submission is deliberately outside `rx_api.h`'s surface this phase),
   read back via `vkCmdCopyImageToBuffer` + a host-visible buffer. Asserts
   all 4 quadrant-representative pixels (1/8 and 7/8 fractions of the
   64×64 render target, comfortably inside each LINEAR-filter exact-match
   band under CLAMP_TO_EDGE) match the 4 source colors **byte-exact**
   (`memcmp`, zero tolerance).
2. **`"hot-reload of a textured material keeps sampling correctly..."`** —
   loads a v1 module from a temp file (never mutates the committed
   fixture), renders and confirms the quadrant colors, edits the temp file
   to a textually-different-but-semantically-identical v2 (forcing a real
   content-hash change and recompile per D9), calls the public
   `reloadChanged()`, confirms the module hash changed, then re-renders and
   confirms the SAME quadrant colors survive the reload.

Both pass with byte-exact quadrant matches (verified directly, not merely
"no crash") once the CLAMP_TO_EDGE fix landed (see below).

## What went wrong once (and how it was caught, not guessed)

Two real defects surfaced only by actually running the GPU test, not by
re-reading the code:

1. **Winding.** The full-screen quad is fed directly as clip-space
   corners (`forward_entry.slang`'s own "PHASE 3 SCOPE NOTE": no
   camera/transform exists yet), so sample 06_materials' own `addQuad()`
   winding (correct for *its* Y-flipped-projection setup) does not
   transfer. A hand derivation using Vulkan's documented signed-area
   facing formula got this backwards on the first attempt — the quad
   rendered nothing at all (uniform clear color at every probe, confirmed
   via a temporary pixel dump before removing it). The working winding
   (`(A,C,B)/(A,D,C)`) is documented in `test_api_factory.cpp` as
   empirically verified against this exact pipeline, not re-asserted as a
   trusted formula result.
2. **Sampler addressing.** The first `defaultSamplerInfo` used REPEAT
   (copying sample 03's own "linear+repeat" sampler verbatim). This
   produced a real, measured 4-way-blended-corner artifact at the
   quadrant probes (dumped and inspected directly: e.g. the top-left probe
   read back `(163,60,46,255)` instead of the pure `(255,0,0,255)` red
   texel) — REPEAT's wraparound blends the wrong-edge neighbor into any
   sample near a UV boundary, which a 2×2 non-tiled test texture sits
   in by construction. Switched the engine's one default sampler to
   CLAMP_TO_EDGE (see "Design decisions" above); the same test then
   matched byte-exact.

Both are documented in-place (`material_system.cpp`'s `defaultSamplerInfo`
comment, `test_api_factory.cpp`'s `kVertices` comment) so a future reader
sees the verified reasoning, not a silently-corrected guess.

## GUID policy

`rx_api.h` was not touched — no ABI-visible shape change (confirmed:
`git diff` touches only `shaders/material/material.slang`,
`src/rx_material/material_system.cpp`, and `rx_material` test files). Per
the documented policy, **no GUID regeneration action is needed.**

## Verification

**Both presets build clean:** `cmake --build --preset linux-native` (full,
all targets including every sample) and `cmake --build --preset
windows-cross-zig --target rx_material_gpu_tests rx_material_tests` both
succeed with no new warnings from this task's own files (windows-cross-zig
emits one pre-existing, unrelated `_WIN32_WINNT` redefinition warning from
the toolchain itself).

**rx_material suite, both required runs:**

```
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a \
  ./build/linux-native/src/rx_material/tests/rx_material_gpu_tests --validate
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  VK_LAYER_PATH=/home/ywadi/sponza/vvl xvfb-run -a \
  ctest --preset linux-native -R rx_material --output-on-failure
```

Under the **default configuration** (`RX_TRACY=ON`, matching the dev
presets), both runs show 23/27 `rx_material_gpu_tests` cases passing with
zero validation errors and exactly 4 failing — **all 4, and only those
4, for one identical, pre-existing, out-of-scope reason** (see "Concern"
below), reproduced identically against both the system validation layer
and the "newer" layer at `/home/ywadi/sponza/vvl`. `rx_material_tests`
(device-free) passes 10/10 both times.

**Isolated verification of this task's own logic — zero validation
errors, unconditionally.** Rebuilt with `-DRX_TRACY=OFF` (isolating the
one pre-existing failure mode below, which is Tracy's own GPU-context
command pool, entirely orthogonal to material texture sampling) and reran:
**27/27 `rx_material_gpu_tests` pass, 0 failures, 0 validation errors**;
`rx_material_tests` 10/10. Also ran `sample_06_materials --validate`
(existing checker/rim materials that never call `rx_sampleTexture`) to
confirm no regression to non-texture materials from the reflection change:
all 4 objects' pixel-gate assertions still `matched=true` byte-for-byte
identical to their pre-Task-4 values.

## Concern: pre-existing, out-of-scope validation failure in Tracy's GPU context

Every one of the 4 failures above (2 pre-existing `bindInstance` GPU tests
that predate this task entirely, plus this task's 2 new render+readback
tests) fails for the exact same reason, confirmed by reproducing it against
the unmodified pre-Task-4 baseline (`git stash` of this task's own diff,
rebuild, rerun — identical failure, identical VUID):

```
VUID-vkBeginCommandBuffer-commandBuffer-00050: ... attempts to implicitly
reset cmdBuffer created from VkCommandPool ... that does NOT have the
VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT bit set.
```

Root cause, traced directly: `rx::graph::Executor::create()` calls
`rx::rhi::createGpuProfileContext(device)` [Phase 4 Stage 0 Task 3, landed
via this worktree's `git merge main`], and
`src/rx_rhi_vk/src/tracy_gpu.cpp:15-17` creates its own `VkCommandPool`
with only `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT` — missing
`VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`. Any test that
constructs a real `rx::graph::Executor` under `RX_TRACY=ON` with
validation enabled trips this, **regardless of what that test actually
exercises** — `MaterialSystem::bindInstance()` structurally requires a
real `PassContext&`, which only `Executor` can construct
(constructor is private, friend-gated), so no test of `bindInstance()`
can route around needing an `Executor` at all.

This is squarely outside Task 4's file scope
(`shaders/material/*.slang`, `src/rx_material/material_system.cpp`,
`rx_material` tests) and was not introduced by this task — it is not
touched or fixed here, per the dispatch's explicit scope boundary. Flagging
it here for the coordinator: the fix, for whoever owns
`src/rx_rhi_vk/src/tracy_gpu.cpp`, is a one-line addition of
`VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` to that pool's
`VkCommandPoolCreateInfo::flags`. Until fixed, every GPU test in this
codebase that builds a real `rx::graph::Executor` (not just
`rx_material`'s) will show this same validation error under the default
`RX_TRACY=ON` dev configuration.
