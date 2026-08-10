# Task 2 Report: Reflection → descriptor set layouts + pipeline layouts

## Summary

Implemented the full Task 2 scope: `rx::shader::reflect()` (`rx_shader`)
walking a linked Slang program's reflection data into the plain-struct
`ShaderLayoutInfo`, and `rx::rhi::PipelineLayoutBuilder` (`rx_rhi_vk`)
turning that into real `VkDescriptorSetLayout`/`VkPipelineLayout` objects,
with the >128-byte push-constant budget enforced there and only there.
Before writing any reflection code, I built a throwaway probe program
against the real shipped Slang v2026.14.1 library (compiled directly with
`g++`, no CMake) to determine the actual reflection API shapes empirically,
cross-checked against `spirv-dis` on the real compiled SPIR-V — this
surfaced a genuine discrepancy against the research file (see "Findings"
below) that changed the implementation's walk strategy. Both presets
configure, build, and test clean; `rx_shader_tests` (10 cases / 73
assertions) and `rx_rhi_vk_tests` (8 cases / 107 assertions) both pass with
zero validation errors.

## Findings / discrepancies vs. the research doc

**[R:A3]'s assumed descriptor-set-range walk is unreliable for set
attribution on this shipped Slang build; the top-level parameter walk plus
the binding-range API are used instead.** [R:A3] describes deriving
set/binding/type/count by walking
`TypeLayoutReflection::getDescriptorSetCount()` /
`getDescriptorSetDescriptorRange*()` on `getGlobalParamsTypeLayout()`. I
built a probe shader with two explicit descriptor sets
(`[[vk::binding(0,0)]] Texture2D gTextures[]`, `[[vk::binding(0,1)]]
SamplerState gSampler`, `[[vk::binding(1,1)]] ConstantBuffer<FrameData>
gFrame`) and walked it both ways:

- The top-level `ProgramLayout::getParameterByIndex()` walk reported
  `gFrame`'s `getBindingSpace() == 1` — correct, confirmed byte-for-byte
  against `spirv-dis`'s `OpDecorate %gFrame DescriptorSet 1`.
- The `getDescriptorSetDescriptorRange*()` walk on
  `getGlobalParamsTypeLayout()` instead grouped `gFrame`'s range under
  *descriptor-set-local-index 0* (`spaceOffset == 0`), not 1 — wrong, and
  would have produced a `VkDescriptorSetLayoutBinding` in the wrong set
  entirely had I trusted it.

I also found `TypeReflection::getElementCount()` does not reliably report
Slang's own documented `SLANG_UNBOUNDED_SIZE` sentinel for a genuinely
unsized `Texture2D gTextures[]` global: it returned `0` with no reflection
context and `2147483647` with one, neither of which is the sentinel.
`TypeLayoutReflection::getBindingRangeBindingCount()` (a different API,
walked on the same `getGlobalParamsTypeLayout()`) *did* report
`SLANG_UNBOUNDED_SIZE` correctly for the same array.

**Resolution implemented:** `reflect()` (`src/rx_shader/src/reflection.cpp`)
uses `ProgramLayout::getParameterByIndex()` for set/binding/category/name
(verified correct); descriptor *type* comes from
`param->getType()->unwrapArray()->getKind()` (`TypeReflection::Kind`) plus
`getResourceShape()`/`getResourceAccess()` for the `Kind::Resource` case
(`mapElementType()` in reflection.cpp); array *count*/unbounded-ness comes
from `TypeLayoutReflection::getBindingRangeBindingCount()` on
`getGlobalParamsTypeLayout()` (the only API observed to report the
unbounded sentinel correctly) — correlated to the same parameter by index
*and* a leaf-variable-name cross-check, so a future mismatch fails loudly
(logged, binding skipped) instead of silently attributing one binding's
count to another. (`TypeLayoutReflection::getBindingRangeType()`, which
returns a *different* enum — `slang::BindingType`, not
`TypeReflection::Kind` — was probed during investigation but is not called
by the actual walk; an earlier draft of this report and of `reflection.h`'s
comment incorrectly attributed type derivation to it, corrected after
review.) The count/unbounded-ness correlation held across two
independently-shaped probe shaders (the one above, and a second exercising
`StructuredBuffer`/`RWStructuredBuffer`/`RWTexture2D`/`Sampler2D`). Fully
documented in `reflection.h`'s comment on `reflect()` and inline in
`reflection.cpp`.

Everything else needed from [R:A3] (`BindingType`/`ParameterCategory` enum
values, `PushConstantBuffer`/`PushConstant` identification, `getSize()`/
`getOffset()` for push-constant ranges) matched the shipped `slang.h`
exactly as documented, including the `[[vk::push_constant]]` bare-struct
spelling (`SmallData pc;`, no `ConstantBuffer<T>` wrapper) producing the
same `getElementTypeLayout()`-bearing wrapper shape as the explicit
`ConstantBuffer<T>` spelling — verified both compile to an identical
reflection shape.

## Implementation

### 1. The Slang-free boundary (`ShaderLayoutInfo`)

Per the as-built context ("no Slang types may appear in rx_rhi_vk headers
or link deps") and the fact that `rx_shader/compiler.h` (needed for
`CompileResult`, `reflection.h`'s other dependency) unavoidably includes
`slang.h`, `ShaderLayoutInfo` itself needed its own header with zero Slang
dependency: `src/rx_shader/include/rx_shader/shader_layout_info.h` (plain
struct, only `<vulkan/vulkan.h>`/`<cstdint>`/`<vector>`). `reflection.h`
includes both `shader_layout_info.h` and `compiler.h`; `pipeline_layout.h`
includes only `shader_layout_info.h`. To make that a build-graph guarantee
rather than a convention, `src/rx_shader/CMakeLists.txt` gained a new
header-only `rx_shader_layout_types` INTERFACE target (this directory's
`include/`, `Vulkan::Headers` only) — `rx_shader` links it (transparent to
existing consumers) and `rx_rhi_vk` links *it*, never `rx_shader` itself.
Root `CMakeLists.txt` reordered `add_subdirectory(src/rx_shader)` before
`add_subdirectory(src/rx_rhi_vk)` so the target exists before it's
consumed (not strictly required by modern CMake's deferred target
resolution, but removes any doubt).

### 2. Mutex sharing across `compiler.cpp`/`reflection.cpp`

The as-built context flagged that reflection touches Slang objects under
the same mutex `Compiler` uses for front-end calls. `compiler.cpp`'s mutex
accessor was a function-local static inside an anonymous namespace
(internal linkage, unreachable from a second TU) — moved to a named
`rx::shader::detail` namespace with a declaration in a new, non-public
header (`src/rx_shader/src/detail/global_session_mutex.h`, not under
`include/`) so `reflection.cpp` can lock the *same* mutex object before
calling `getLayout()` and walking the result. Rationale documented in that
header: `getLayout()` is not in slang.h's short list of explicitly
"experimental concurrent-safe" operations (backend codegen on an
already-linked component type), so it stays under the same lock as
everything else per the "front-end operations... externally synchronized
unless documented otherwise" rule [R:A4/A6].

### 3. `rx::shader::reflect()` (`src/rx_shader/src/reflection.cpp`)

Walks flat global-scope parameters only (matches the spec's "no
intermediate metadata format" design and Task 2's actual test shapes;
`ParameterBlock<T>`/nested layouts are out of scope, logged-and-skipped if
encountered). For each global parameter: `PushConstantBuffer` category →
one `PushRange` (size from `getElementTypeLayout()->getSize()`, offset
from `getOffset(category)`); `DescriptorTableSlot` category → one
`Binding`, type from a `TypeReflection::Kind`/`SlangResourceShape`/
`SlangResourceAccess`-based mapping (verified against `spirv-dis` for
Sampler/Texture2D/CombinedSampler/ConstantBuffer/StructuredBuffer/
RWStructuredBuffer/RWTexture2D; other resource shapes mapped per
documented enum semantics but not separately smoke-tested, flagged in
code); every other category logged and skipped. Stage flags are merged
across every entry point in the linked program and applied to every
binding/push-range — a deliberately conservative default (over-including
stage visibility is always Vulkan-spec-legal), since global-parameter
reflection doesn't surface *which* entry point touches a given global.

### 4. `rx::rhi::PipelineLayoutBuilder` (`src/rx_rhi_vk/src/pipeline_layout.cpp`)

Rejects (before touching Vulkan at all) if the push-constant footprint
(`max(offset+size)` across ranges) exceeds 128 bytes [spec Fixed decision
#5, R:B2]. Otherwise groups bindings by set index, gap-filling any skipped
index with an empty (zero-binding) `VkDescriptorSetLayout` so
`vkCmdBindDescriptorSets`' positional addressing stays correct even for a
shader using e.g. set 0 and set 2 but not 1. A set containing any
`unboundedArray` binding gets `VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT`;
that specific binding gets `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` (via
`VkDescriptorSetLayoutBindingFlagsCreateInfo`) and a capacity of
`kUnboundedArrayDescriptorCapacity` (4096, a documented generic default —
Task 3's `BindlessTable` defines its own explicit capacities and is
expected to build its dedicated set directly rather than through this
constant). On any Vulkan failure, every set layout already created in that
call is destroyed before returning `std::nullopt`. `PipelineLayoutBundle`
is move-only RAII (public `setLayouts`/`layout` per the brief's exact
shape; a private `device_` for teardown, matching this library's existing
`friend`-constructed RAII types).

### 5. Tests

- `src/rx_shader/tests/reflection_test.cpp` (joins the existing
  `rx_shader_tests` binary): the mandated exact-value test (unbounded
  `Texture2D[]` + `SamplerState` + `ConstantBuffer<T>` + push constants) —
  every set/binding/type/count/stage asserted against a hand-computed
  table, plus the push-constant range's exact offset (0) and size (80,
  std430-packed `{uint,uint,float4x4}`); a >128-byte push-constant
  (`3×float4x4` = 192 bytes) shader asserting `reflect()` still succeeds
  and reports the real size (128-byte enforcement is `PipelineLayoutBuilder`'s
  job, verified separately); `reflect()` returns `nullopt` on a failed
  `CompileResult`.
- `src/rx_rhi_vk/tests/pipeline_layout_test.cpp` (joins the existing
  `rx_rhi_vk_tests` binary — the as-built context's explicit guidance for
  staying protected by that binary's vk-bootstrap warm-up rather than
  building a new one): a headless device built locally in this file with
  exactly the `VkPhysicalDeviceVulkan12Features` bits Task 3's brief will
  make process-wide (not touching `rx::rhi::Device::create()`, which
  doesn't enable them yet); `PipelineLayoutBuilder::build` on a
  hand-crafted `ShaderLayoutInfo` mirroring `reflection_test.cpp`'s exact
  shape → non-null `layout` + 2 non-null `setLayouts`, zero validation
  errors; the same >128-byte push-constant shape (192 bytes, matching
  `reflection_test.cpp`'s case) → `build()` returns `nullopt` with a
  logged error.

**Design note on the reflect()/PipelineLayoutBuilder test split:** since
`rx_rhi_vk` never links `rx_shader`, `pipeline_layout_test.cpp`
hand-constructs its `ShaderLayoutInfo` values rather than calling
`reflect()` — it deliberately mirrors `reflection_test.cpp`'s shapes
(including the identical 192-byte oversized case) so the two tests
together prove the brief's full contract ("reflection succeeds ... but
PipelineLayoutBuilder rejects") without either test needing the other
component.

## Verification

- **linux-native, full build:** all targets build clean.
  `ctest --preset linux-native`: **6/6 pass** (`shader_spirv_test`,
  `rx_core_tests`, `rx_platform_tests`, `rx_shader_tests` [10 cases / 73
  assertions], `rx_rhi_vk_tests` [8 cases / 107 assertions],
  `sample_01_triangle_headless`). `rx_rhi_vk_tests` run directly (not just
  via ctest) shows zero real validation errors — only the pre-existing,
  already-documented `VK_KHR_portability_enumeration` false positives
  `Context` already filters.
- **windows-cross-zig, full configure+build:** clean, all targets
  including the two touched executables. `ctest --preset
  windows-cross-zig`: **6/6 pass**, including `rx_shader_tests.exe` and
  `rx_rhi_vk_tests.exe` both actually executed under Wine on this machine
  (this preset's ctest is not GPU-excluded in this repo's current
  configuration) — both the new reflection and pipeline-layout tests pass
  identically to the Linux run.
- Manually confirmed (per the vk-bootstrap landmine constraint) that no
  new test binary was introduced: `pipeline_layout_test.cpp` joined the
  existing `rx_rhi_vk_tests` executable, protected by its existing
  `doctest_main.cpp` warm-up with no changes needed there.

## Deviations from brief / spec

1. **Added `src/rx_shader/include/rx_shader/shader_layout_info.h` and the
   `rx_shader_layout_types` CMake INTERFACE target — not in the brief's
   file list.** Required to satisfy the as-built context's explicit,
   non-negotiable constraint ("no Slang types may appear in rx_rhi_vk
   headers or link deps"): `reflection.h` needs `compiler.h` (for
   `CompileResult`), which unavoidably includes `slang.h`; without a
   separate Slang-free header for `ShaderLayoutInfo` alone,
   `pipeline_layout.h` would have had to include `reflection.h` and
   transitively drag `slang.h` into `rx_rhi_vk`. This is an adaptation of
   the brief's file list, not a spec-level deviation — no Fixed Decision
   changed, and no coordinator sign-off was sought since the brief's own
   constraint text left no other way to satisfy both requirements
   simultaneously (one shared `ShaderLayoutInfo` type, and zero Slang in
   rx_rhi_vk).
2. **`reflect()` does not use the descriptor-set-range walk [R:A3]
   describes.** Covered in "Findings" above — the shipped `slang.h`'s
   actual behavior (verified against real SPIR-V) wins per this task's own
   instructions.
3. Everything else matches the brief's interfaces exactly:
   `ShaderLayoutInfo`'s field shapes, `reflect()`'s signature,
   `PipelineLayoutBuilder::build`'s signature and `PipelineLayoutBundle`'s
   public shape.

## Concerns for the coordinator

1. **`kUnboundedArrayDescriptorCapacity` (4096) is a generic default with
   no empirical stress-test behind the specific number** — chosen to be
   comfortably within RADV/NVIDIA's effectively-unbounded
   `maxDescriptorSetUpdateAfterBindSampledImages` limits [R:B2], but Task 3
   should confirm whether `BindlessTable` truly bypasses this constant
   entirely (as this task assumes/documents) or whether some future caller
   of `PipelineLayoutBuilder::build` on an unbounded-array shader still
   needs it to be configurable.
2. **Resource-kind → `VkDescriptorType` mapping coverage beyond what was
   tested**: `Kind::ShaderStorageBuffer`/`Kind::TextureBuffer` (distinct
   `TypeReflection::Kind` values per `slang.h`) are mapped defensively but
   were never observed from any construct this task actually compiled
   (`StructuredBuffer`/`RWStructuredBuffer` both surfaced as
   `Kind::Resource` + `SLANG_STRUCTURED_BUFFER` instead) — flagged in code;
   a future task exercising an HLSL `tbuffer`/raw `ShaderStorageBuffer`
   construct should verify before trusting it.
3. Per-binding stage-flag merging is "every entry point in the program,"
   not "every entry point that actually references this specific global"
   — always spec-legal (over-inclusive, never under) but slightly loses
   precision; acceptable for Task 2's scope, worth knowing about if a
   later task cares about minimizing declared stage visibility for some
   other reason (e.g. driver-side pipeline compilation heuristics).

## Files created

- `src/rx_shader/include/rx_shader/shader_layout_info.h`
- `src/rx_shader/include/rx_shader/reflection.h`
- `src/rx_shader/src/detail/global_session_mutex.h`
- `src/rx_shader/src/reflection.cpp`
- `src/rx_shader/tests/reflection_test.cpp`
- `src/rx_rhi_vk/include/rx_rhi_vk/pipeline_layout.h`
- `src/rx_rhi_vk/src/pipeline_layout.cpp`
- `src/rx_rhi_vk/tests/pipeline_layout_test.cpp`

## Files modified

- `src/rx_shader/src/compiler.cpp` (mutex accessor moved to
  `rx::shader::detail`, given external linkage — behavior unchanged)
- `src/rx_shader/CMakeLists.txt` (+`rx_shader_layout_types` INTERFACE
  target, +`reflection.cpp`, +`reflection_test.cpp`)
- `src/rx_rhi_vk/CMakeLists.txt` (+`pipeline_layout.cpp`, links
  `rx_shader_layout_types`, +`pipeline_layout_test.cpp` in the existing
  `rx_rhi_vk_tests` binary)
- `CMakeLists.txt` (reordered `src/rx_shader` before `src/rx_rhi_vk`)

## Readiness for Task 3

`ShaderLayoutInfo`/`PipelineLayoutBuilder` are ready to consume real
`reflect()` output end to end (proven conceptually by the two tests
mirroring each other's shapes, though no single test currently pipes
`reflect()`'s actual output into `PipelineLayoutBuilder::build` in one
call — a sample in Task 5/6 will be the first place that happens for
real). `PipelineLayoutBuilder::build`'s unbounded-array handling
(`UPDATE_AFTER_BIND_BIT | PARTIALLY_BOUND_BIT` + set-level
`UPDATE_AFTER_BIND_POOL_BIT`) is exactly the binding-flag shape Task 3's
`BindlessTable` needs to be layout-compatible with, though `BindlessTable`
is expected to build its own dedicated global set directly (with its own
explicit per-resource-class capacities and the additional
`VARIABLE_DESCRIPTOR_COUNT` flag on its last binding, per its own brief)
rather than route through this generic builder. Task 3's device-feature
enablement (`VkPhysicalDeviceVulkan12Features` via
`set_required_features_12`) should use the exact same feature list
`pipeline_layout_test.cpp` already exercises locally — copying it verbatim
into `Device::create()` is a safe, already-proven starting point.

## Fix note (post-review)

Review came back Approved with the functional code verified correct (live
runs, hand-rechecked binding tables, clean link graph); two documentation-
accuracy findings were fixed after the fact, doc/comment/report text only,
zero functional changes:

1. **Misattributed API for descriptor-type derivation.** The Findings
   section above and `reflection.h`'s comment on `reflect()` originally
   stated `TypeLayoutReflection::getBindingRangeType()` was used "for
   descriptor type and array count/unbounded-ness" — that function is
   never called anywhere in `reflection.cpp`. The real mechanism (already
   correctly described in this report's Implementation item 3) is
   `param->getType()->unwrapArray()->getKind()` (`TypeReflection::Kind`)
   plus `getResourceShape()`/`getResourceAccess()` via `mapElementType()`;
   `getBindingRangeBindingCount()` (a different function on the same type)
   is the one actually used, and only for count/unbounded-ness, not type.
   Both the Findings section above and `reflection.h`'s comment block on
   `reflect()` are corrected to name the right function for each purpose
   and to distinguish `TypeReflection::Kind` from the unrelated
   `slang::BindingType` enum family `getBindingRangeType()` returns.
2. **Wrong assertion count.** This report's Verification section (and
   Summary) claimed `rx_shader_tests` runs "10 cases / 83 assertions"; the
   built binary actually reports 73 assertions (confirmed by re-running it
   directly). Corrected both occurrences to 73.
3. **Imprecise flag-bit prose in `reflection.cpp`.** A comment said
   `SlangResourceShape` modifier flags are "OR'd in starting at 0x10" —
   true for feedback/shadow/array/multisample (0x10/0x20/0x40/0x80) but
   `SLANG_TEXTURE_COMBINED_FLAG` is actually `0x100`. The masking code
   itself (`shape & 0x0F`) was already correct and untouched; only the
   prose was reworded to list each flag's actual value.

Verified after the fix: `rx_shader_tests` rebuilt and re-run directly
(linux-native) — still 10 cases / 73 assertions, all passing, output
unchanged apart from timestamps. No other file touched.
