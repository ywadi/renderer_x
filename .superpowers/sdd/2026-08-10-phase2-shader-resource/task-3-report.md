# Task 3 Report: Device descriptor-indexing enablement + BindlessTable

## Summary

Implemented the full Task 3 scope: `Device::create()` now chains a
`VkPhysicalDeviceVulkan12Features` (via vk-bootstrap's
`set_required_features_12`) enabling exactly the ten descriptor-indexing
bits named in the brief, with a new diagnostic (`logDescriptorIndexingFeatureGaps`)
that turns a physical-device-selection failure into a loud, per-device,
per-feature-name error instead of vk-bootstrap's generic "no suitable
device" message. `rx::rhi::BindlessTable` (`bindless.h`/`bindless.cpp`) is
the engine's global bindless descriptor set: one update-after-bind
`VkDescriptorSetLayout`/pool/`VkDescriptorSet` with three runtime-array
bindings (sampled images, samplers, storage buffers), generational
`BindlessHandle`s built on `rx::core::HandlePool`, and a documented
release-safety contract. All four brief-mandated test scenarios pass in a
new `bindless_test.cpp` joining `rx_rhi_vk_tests`. Full `ctest` is green
(6/6) on linux-native; `windows-cross-zig` configures and builds clean
including the new files.

## Implementation

### 1. Device feature enablement (`device.cpp`)

Added a `VkPhysicalDeviceVulkan12Features` with exactly the ten bits from
the brief (`descriptorIndexing`, `runtimeDescriptorArray`,
`descriptorBindingPartiallyBound`, `descriptorBindingVariableDescriptorCount`,
`descriptorBindingSampledImageUpdateAfterBind`,
`descriptorBindingStorageImageUpdateAfterBind`,
`descriptorBindingStorageBufferUpdateAfterBind`,
`descriptorBindingUpdateUnusedWhilePending`,
`shaderSampledImageArrayNonUniformIndexing`,
`shaderStorageBufferArrayNonUniformIndexing`), chained via
`.set_required_features_12(features12)` alongside the existing
`features11`/`features13` chains — same pattern already established there.

For the "selection failure = loud startup error naming the missing
feature" requirement: vk-bootstrap's own selection error never names a
specific missing feature bit, so `logDescriptorIndexingFeatureGaps()`
(anonymous namespace, called only on `select()` failure) enumerates every
physical device the instance can see, queries each one's real
`VkPhysicalDeviceVulkan12Features` via `vkGetPhysicalDeviceFeatures2`, and
logs one `RX_LOG_ERROR` per device per missing feature, by name. A single
shared table (`kRequiredDescriptorIndexingFeatures`, name + accessor
function pointer) is used both to build this diagnostic and to keep it
from silently drifting out of sync with the ten enabled bits (adding an
eleventh feature to one list without the other would be a compile-time
array-size mismatch if someone forgets, though nothing currently enforces
that beyond code review — flagged as a minor sharp edge in Concerns
below). If no visible device is actually missing any of the ten bits, the
diagnostic says so explicitly rather than staying silent, so a selection
failure caused by something else (surface support, queue families, the
1.1/1.3 feature sets, API version) doesn't get misattributed.

This path could not be exercised by an automated test in this environment:
the two available real devices (this machine's NVIDIA RTX 2080 proprietary
driver, and llvmpipe/lavapipe under the same instance) both support every
required bit, and the brief's own Tests section only mandates the
BindlessTable-capacity failure path, not a device-selection failure.
Verified the formatting mechanics directly instead (a standalone throwaway
program confirmed `VkPhysicalDeviceProperties::deviceName`, a raw
`char[256]`, formats correctly through spdlog/fmt) and by code review; a
real missing-feature run has not been observed end to end.

### 2. `rx::rhi::BindlessTable` (`bindless.h` / `bindless.cpp`)

One descriptor set (set 0) built directly by `BindlessTable::create()`
(its own pool + layout, not routed through `PipelineLayoutBuilder`, per the
as-built context): binding 0 = `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE`, binding
1 = `VK_DESCRIPTOR_TYPE_SAMPLER`, binding 2 =
`VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`, all three
`PARTIALLY_BOUND_BIT | UPDATE_AFTER_BIND_BIT`, binding 2 (the last, the
only legal position) additionally `VARIABLE_DESCRIPTOR_COUNT_BIT`; set
layout carries `UPDATE_AFTER_BIND_POOL_BIT`; pool carries
`UPDATE_AFTER_BIND_BIT`; the descriptor set is allocated with
`VkDescriptorSetVariableDescriptorCountAllocateInfo` supplying the exact
`storageBuffers` capacity for binding 2.

`BindlessHandle` carries a `BindlessResourceKind` tag alongside
`(index, generation)` so one handle type can serve all three
`register*()`/`release()` calls while each resource class keeps its own
independent generation sequence internally — implemented as three private
`rx::core::HandlePool<Tag, EmptyPayload>` members (one tag struct per
resource kind), reusing `HandlePool` exactly as directed rather than
reinventing generational bookkeeping. `HandlePool` has no built-in capacity
cap (it always grows), so each `register*()` checks the returned index
against that resource class's fixed capacity, and — since this can only
happen if a caller genuinely over-registers beyond the table's configured
size — immediately releases the phantom slot back and returns an invalid
handle with a logged error, rather than writing an out-of-range
`dstArrayElement`.

Writes go straight to the GPU-visible set via `vkUpdateDescriptorSets` on
every successful `register*()` call. `release()` only returns the slot to
its pool's free list; it never touches the descriptor's GPU-visible
contents or the underlying resource. The header's "RELEASE-SAFETY
CONTRACT" section documents this explicitly, including that in-flight
safety for the underlying `VkImageView`/`VkSampler`/`VkBuffer` (as opposed
to this table's own index bookkeeping, which is safe immediately) is
Task 4's `DeletionQueue` responsibility, per the as-built context.

**Capacity pre-check against real device limits.** `create()` queries
`VkPhysicalDeviceVulkan12Properties` via `vkGetPhysicalDeviceProperties2`
and rejects (before touching any Vulkan object) any requested capacity
exceeding this device's real
`maxDescriptorSetUpdateAfterBind{SampledImages,Samplers,StorageBuffers}`,
logging exactly which capacity and by how much. Also rejects any
zero-valued capacity (a `VkDescriptorPoolSize`/binding with
`descriptorCount == 0` is not how this table's fixed three-binding layout
expresses "unused").

### 3. Tests (`bindless_test.cpp`, joins `rx_rhi_vk_tests`)

Local headless-device fixture (`makeHeadlessBindlessFixture`), matching
`pipeline_layout_test.cpp`'s established per-file-duplication pattern —
Vulkan 1.2 descriptor-indexing features *and* Vulkan 1.3
`dynamicRendering`/`synchronization2` (the latter needed because this
file's real-resource helper calls `rx::rhi::transitionImage()`, which
records a `vkCmdPipelineBarrier2` requiring the feature — discovered by a
real validation error on first run, fixed by adding the
`features13`/`set_required_features_13` chain `pipeline_layout_test.cpp`
didn't need).

Four `TEST_CASE`s, each building its own table/device (doctest cases don't
share state):
1. `create()` with capacities 1024/16/256 → non-null layout/set; register a
   real 1x1 sampled image + sampler (raw Vulkan, transitioned to
   `SHADER_READ_ONLY_OPTIMAL` via `CommandContext`) + a real storage
   buffer (via `Allocator::createHostVisibleBuffer`) → all three handles
   valid, index 0, generation 1; zero validation errors.
2. Register/release/re-register cycles: three sampled-image registrations
   → indices 0/1/2 gen 1; release index 1, re-register → reuses index 1 at
   generation 2 (`HandlePool`'s LIFO free list); double-release is a
   verified no-op; a subsequent fresh registration lands on index 3, not a
   stale slot.
3. A minimal `VkPipelineLayout` built directly from the table's own
   `descriptorSetLayout()` (structurally guaranteed compatible), then
   `vkCmdBindDescriptorSets` in a `CommandContext::runOnce` with no pipeline
   bound and no draw/dispatch recorded → validation clean.
4. Queries the real `maxDescriptorSetUpdateAfterBindSampledImages` at
   runtime and requests exactly `limit + 1` for `sampledImages` → `create()`
   returns `std::nullopt` with a logged error, zero validation errors
   (guards the RADV-sentinel edge case: if a future run reports the
   unbounded `UINT32_MAX` limit, the test explicitly notes the path is
   unexercisable there rather than asserting something false).

All existing Phase 1/2 tests in this binary (context/device/buffer/
clear_color/frame_sync/pipeline_layout) still pass unchanged — the Device
change is additive.

## Verification

- **linux-native:** full build clean; `ctest`: **6/6 pass**
  (`shader_spirv_test`, `rx_core_tests`, `rx_platform_tests`,
  `rx_shader_tests`, `rx_rhi_vk_tests`, `sample_01_triangle_headless`).
  `rx_rhi_vk_tests` run directly: **12 test cases / 195 assertions, all
  passing, 0 failed.** The only `[error]`-level log lines in a full run are
  the two deliberately-exercised clean-rejection paths (this task's
  absurd-capacity case, and Task 2's pre-existing oversized-push-constant
  case) — both expected, both `std::nullopt` returns, neither a validation
  error. `hasValidationErrors()` is `false` on every `TEST_CASE` in this
  file.
- **windows-cross-zig:** configure + full build clean, including
  `bindless.cpp`/`bindless_test.cpp` compiling and `rx_rhi_vk_tests.exe`
  linking successfully via zig/LLD.
- Commit hygiene: plain commit message, no AI attribution trailer —
  verified directly with `git log -1 --format='%B'` after committing.

## Deviations from brief / spec

1. **`BindlessTable::create()` takes `VkPhysicalDevice` in addition to
   `VkDevice`** — the brief's shorthand signature is
   `create(VkDevice, capacities) -> std::optional<BindlessTable>`. The
   brief's own required test ("absurd capacity ... → clean error, no
   crash") cannot be implemented as a *clean* error without a way to check
   the request against the device's real
   `maxDescriptorSetUpdateAfterBind*` limits before issuing any Vulkan
   call — letting `vkCreateDescriptorSetLayout`/`vkCreateDescriptorPool`
   fail on an over-limit request instead would depend on
   driver/validation-layer behavior this engine has no contract with (and
   would risk counting as a validation error rather than a clean
   application-level rejection). Adding the physical-device handle was the
   only way found to satisfy the literal test requirement; this is a
   signature elaboration, not a Fixed Decision change — the spec's Fixed
   decision #5 pins the architecture (one set, three runtime-array
   bindings, the flag combination, index-in-push-constants), not this
   static factory's exact parameter list. No coordinator sign-off sought
   given the constraint left no alternative and nothing in the Fixed
   Decisions relitigates.
2. **`registerStorageBuffer`'s parameter order is `(buffer, range, offset = 0)`**,
   not literally the brief's two-token shorthand `(VkBuffer, range)` —
   `offset` added as a defaulted trailing parameter so the common
   two-argument call form the brief describes still works, while allowing
   a non-zero offset when a caller needs one (unavoidable for a real
   sub-allocated buffer later in the phase).
3. Everything else matches the brief's interfaces exactly: capacities
   struct shape, `descriptorSetLayout()`/`descriptorSet()` accessors,
   `registerSampledImage`/`registerSampler` signatures,
   `BindlessHandle::index()`, generational semantics, exact feature list
   enabled via `set_required_features_12`.

## Concerns for the coordinator

1. **The device-selection-failure diagnostic path is implemented but not
   exercised by any test** (see Implementation §1) — no available device
   in this environment lacks any of the ten required features, and it
   isn't in the brief's Tests section. Confirmed the string-formatting
   mechanics work correctly in isolation; a real end-to-end
   missing-feature run has not been observed. Low risk (the logic is
   straightforward vkGetPhysicalDeviceFeatures2 + table walk) but worth
   knowing the exact scenario has never actually printed for real.
2. **`kRequiredDescriptorIndexingFeatures` (the diagnostic's name/accessor
   table) and the `features12.xxx = VK_TRUE` block are two hand-maintained
   lists of the same ten names** — nothing enforces they stay in sync
   beyond code review; a future feature addition/removal needs both
   updated together. Considered generating one from the other but found no
   clean way to do so without reflection-like machinery disproportionate
   to ten bools; flagging rather than over-engineering.
3. Per the as-built context, `BindlessTable`'s set-0 layout is intentionally
   separate from (and must stay binding-for-binding compatible with)
   whatever `PipelineLayoutBuilder`/`reflect()` produces for a real
   shader's set 0 — this task does not itself prove that compatibility
   end-to-end (no sample/shader exists yet); that proof is explicitly
   deferred to Task 6 per the as-built context, unchanged from the brief.
4. Zero-capacity rejection and the per-resource-class capacity-exhaustion
   guard in `register*()` are defensive additions beyond the brief's
   explicit test list — not spec deviations (they don't change the
   architecture), but worth a look since they're new failure surfaces this
   task introduced without a dedicated test-per-guard (the capacity
   pre-check *is* tested via the absurd-capacity case; the zero-capacity
   guard and the register-time exhaustion guard are exercised only by code
   review, since no test in the brief calls for either).

## Files created

- `src/rx_rhi_vk/include/rx_rhi_vk/bindless.h`
- `src/rx_rhi_vk/src/bindless.cpp`
- `src/rx_rhi_vk/tests/bindless_test.cpp`

## Files modified

- `src/rx_rhi_vk/src/device.cpp` (`VkPhysicalDeviceVulkan12Features` chain
  via `set_required_features_12`; `logDescriptorIndexingFeatureGaps`
  diagnostic on selection failure)
- `src/rx_rhi_vk/CMakeLists.txt` (+`src/bindless.cpp` in the `rx_rhi_vk`
  library; +`tests/bindless_test.cpp` in the existing `rx_rhi_vk_tests`
  binary, comment updated for the seventh test TU)

## Readiness for Task 4

`BindlessTable::release()`'s documented contract ("returns the index slot
immediately; never touches the underlying GPU resource; in-flight
resource-destruction safety is DeletionQueue's job") is exactly the seam
Task 4's `DeletionQueue` needs to close: a real eviction flow will call
`BindlessTable::release(handle)` for the bookkeeping and separately retire
the actual `VkImageView`/`VkSampler`/`VkBuffer` destruction through
`DeletionQueue`, keyed on the frame fence, per the spec's Fixed decision
#9. `Device::create()` now enables every descriptor-indexing feature
`BindlessTable` needs process-wide, so Task 4/5/6 code building a real
windowed `Device` gets a table-ready device with no further feature
plumbing.
