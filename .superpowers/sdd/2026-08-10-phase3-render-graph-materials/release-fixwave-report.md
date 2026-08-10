# Release fix wave: CI run acfce89 (lavapipe 2/15 failures)

## Scope

Two independent fixes against the CI failures on lavapipe (software Vulkan):

1. `rx::rhi::DescriptorArena` relying on driver-optional `VkDescriptorPool`
   exhaustion detection for its documented `allocate() -> VK_NULL_HANDLE`
   contract.
2. Vulkan synchronization validation silently inactive in CI
   (`VK_EXT_validation_features unavailable` warning).

## Fix 1 — DescriptorArena enforces its own budgets

**Root cause (confirmed, matches the brief):** the Vulkan spec's own
`vkAllocateDescriptorSets` description text says the allocation "may fail
due to lack of space in the descriptor pool" if a call would push the
pool's total sets past `maxSets`, or any one descriptor type's count past
that type's pool size — "may", not "must". Only the resulting *error code*
(`VK_ERROR_OUT_OF_POOL_MEMORY`) is mandatory *if* an implementation chooses
to detect exhaustion at all. lavapipe/Mesa is a real, spec-conformant
implementation that does not choose to — it let CI's `descriptor_arena_test.cpp`/
`test_param_arena.cpp` allocate past the documented `maxSets` ceiling and
return a real, non-null `VkDescriptorSet`. This project's own dev-machine
driver happened to enforce the limit, which is why local GPU runs were
green while CI wasn't.

**Change:** `DescriptorArena` now tracks, per frame-in-flight slot, its own
allocated-set count and allocated-uniform-buffer-descriptor count against
the `Capacities` it was `create()`d with, and checks both *before* ever
calling `vkAllocateDescriptorSets`. `allocate()` gained a
`uniformBufferDescriptorCount` parameter (default `1`, matching every real
caller's single-UBO-binding shape — `rx_material`'s `ParamArena` — so no
existing call site needed to change) so the per-type budget accounts for
however many UBO descriptors one call's `VkDescriptorSetLayout` actually
consumes, since Vulkan gives no API to introspect an opaque layout handle.
Genuine driver-level failure (`VK_ERROR_OUT_OF_POOL_MEMORY` /
`VK_ERROR_FRAGMENTED_POOL` from `vkAllocateDescriptorSets` itself, e.g. real
fragmentation this class's own accounting can't see) is kept as a distinct,
logged fallback path — never removed, just no longer the only line of
defense.

Files:
- `src/rx_rhi_vk/include/rx_rhi_vk/descriptor_arena.h` — class-level
  "BUDGETS ARE ARENA-ENFORCED" comment (cites the spec text above),
  `allocate()` signature/doc update, new per-slot counter members.
- `src/rx_rhi_vk/src/descriptor_arena.cpp` — counter storage in
  `create()`/move-assign, reset in `beginFrame()`, the two budget checks
  (maxSets, then uniformBuffers) at the top of `allocate()`, distinct log
  lines for arena-enforced vs. genuine-driver-failure rejections.
- `src/rx_rhi_vk/tests/descriptor_arena_test.cpp` — updated comment on the
  existing exhaustion assertions (now arena-enforced, deterministic on
  every driver); added a new `TEST_CASE` with three sub-cases that size
  `maxSets`/`uniformBuffers` *differently* so each ceiling's exhaustion can
  only be explained by the budget that's actually smaller: (A)
  `uniformBuffers` smaller than `maxSets`, (B) `maxSets` smaller than
  `uniformBuffers`, (C) a real two-UBO-binding layout allocated with
  `uniformBufferDescriptorCount=2`, proving the budget sums descriptors per
  call rather than just counting calls (2+2=4 of 5 fits, a third call
  needing 2 more (6) fails even though 1 descriptor of naive headroom
  "remains").
- `src/rx_material/tests/test_param_arena.cpp` — comment-only update on the
  descriptor-pool-exhaustion sub-case clarifying it now exercises
  `DescriptorArena`'s arena-enforced budget (both `maxSets` and
  `uniformBuffers` sized to `kMaxInstancesPerFrame` by
  `ParamArena::create()`), not driver behavior. No assertion changes needed
  — the existing shape already tests the right thing once the underlying
  class enforces it itself.

## Fix 2 — sync validation warning: root cause was NOT in context.cpp

The brief's working hypothesis was that `context.cpp`'s
`VK_EXT_validation_features` availability probe checks instance-level
extension enumeration *without* layer scoping. **That hypothesis did not
hold up under direct verification** and the actual fix is elsewhere:

- Read the pinned vk-bootstrap commit's actual source
  (`third_party/CMakeLists.txt`'s `RX_VK_BOOTSTRAP_COMMIT` =
  `556b79b165386f6c1a18362d30f2a076fdaa2778`, checked out at
  `build/linux-native/_deps-src/vk-bootstrap/src/VkBootstrap.cpp`).
  `SystemInfo`'s own constructor already loops over every layer
  `vkEnumerateInstanceLayerProperties` returns and merges each one's own
  extensions via a **per-layer**
  `vkEnumerateInstanceExtensionProperties(layer.layerName, ...)` call into
  `available_extensions` — i.e. `context.cpp`'s
  `systemInfo->is_extension_available(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)`
  call is already layer-scoped.
- Reproduced this directly: pointing `VK_LAYER_PATH` at an empty directory
  (making `VK_LAYER_KHRONOS_validation` genuinely undiscoverable) against
  the *unmodified* `rx_rhi_vk_tests` binary reliably reproduces the exact
  CI warning text. Restoring the normal environment (layer discoverable)
  makes it disappear — proving the probe correctly activates sync
  validation whenever the layer is actually present, with no code change.
- Root-caused the CI gap instead: `.github/workflows/ci.yml`'s
  `linux-native` job's "Install system packages" step never installed
  `vulkan-validationlayers` — a separate Ubuntu package from
  `libvulkan-dev`/`vulkan-tools`/`mesa-vulkan-drivers`, confirmed via
  `apt-cache depends` that none of those three pull it in. This local dev
  machine has `vulkan-validationlayers 1.3.204.1-2` installed as its own
  explicit package (`dpkg -l`), which is *why* local runs already had sync
  validation active and never surfaced this gap.

**Change:** added `vulkan-validationlayers` to the `linux-native` job's
apt-get install line in `.github/workflows/ci.yml`, with a comment
documenting the verification above. `windows-cross-zig` was checked too:
every real `Context::create(..., /*enableValidation=*/true)` call site
lives in `rx_rhi_vk_tests`, `rx_graph_gpu_tests`, `rx_material_gpu_tests`,
or a `sample_*_headless` gate — all already excluded from that job's own
`ctest -E` filter (no real Vulkan under Wine) — so it needs no equivalent
package.

Also added a short verification comment directly in `context.cpp` at the
probe site itself, so a future reader doesn't re-open this same
(disproven) lead.

Files:
- `.github/workflows/ci.yml` — added `vulkan-validationlayers` to
  `linux-native`'s package list, with the verification comment above.
- `src/rx_rhi_vk/src/context.cpp` — comment only, no logic change: records
  that the probe was verified layer-scoped-correct and the real fix is in
  CI's package list.

## Verification

### 1. Full suite, default (real) GPU driver

```
$ ctest --preset linux-native --output-on-failure
...
100% tests passed, 0 tests failed out of 15
Total Test time (real) =  24.17 sec
```

### 2. Full suite, CI-mirrored (xvfb + lavapipe forced)

Lavapipe forced via `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`
(confirmed via `vulkaninfo --summary` under that env: `deviceName =
llvmpipe (LLVM 15.0.7, 256 bits)`, `driverName = llvmpipe`), run the same
way CI's own `Test (xvfb + lavapipe)` step does:

```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a ctest --preset linux-native -V
...
100% tests passed, 0 tests failed out of 15
Total Test time (real) =   8.80 sec
```

Evidence the arena-enforced budget is what's firing now (deterministic on
lavapipe, where it previously was NOT firing):

```
[error] rx::rhi::DescriptorArena::allocate: arena-enforced maxSets budget exhausted for frame slot 0 (4 of 4 sets already allocated this reset cycle)
[error] rx::rhi::DescriptorArena::allocate: arena-enforced uniformBuffers budget exhausted for frame slot 0 (3 of 3 UBO descriptors already allocated this reset cycle, 1 more requested)
[error] rx::rhi::DescriptorArena::allocate: arena-enforced maxSets budget exhausted for frame slot 0 (3 of 3 sets already allocated this reset cycle)
[error] rx::rhi::DescriptorArena::allocate: arena-enforced uniformBuffers budget exhausted for frame slot 0 (4 of 5 UBO descriptors already allocated this reset cycle, 2 more requested)
[error] rx::rhi::DescriptorArena::allocate: arena-enforced maxSets budget exhausted for frame slot 1 (512 of 512 sets already allocated this reset cycle)  # ParamArena's own exhaustion sub-case
```

Evidence the sync-validation warning is gone (layer now installed on this
run's simulated-CI environment) and no new, unguarded validation messages
surfaced now that sync validation is genuinely active against lavapipe for
the first time:

```
$ grep -c "VK_EXT_validation_features unavailable" ctest-lavapipe-final.log
0
$ grep "\[vulkan validation\]" ctest-lavapipe-final.log | grep -v "known false positive" | wc -l
0
```

Every `[vulkan validation]` line present in the run is one of the three
already-documented, evidence-backed false positives in `context.cpp`
(`isKnownPortabilityEnumerationLayerBug`,
`isKnownUnrecognizedSlangSourceLanguageBug`,
`isKnownSyncValidationSeparateSamplerMisclassification`) — no new guard was
needed.

Per-binary doctest tallies from the same lavapipe run, all zero failures:

```
[doctest] assertions: 6 | 6 passed | 0 failed |     (rx_core_tests)
[doctest] assertions: 12 | 12 passed | 0 failed |   (rx_platform_tests)
[doctest] assertions: 3 | 3 passed | 0 failed |     (shader_spirv_test)
[doctest] assertions: 86 | 86 passed | 0 failed |   (rx_shader_tests)
[doctest] assertions: 787 | 787 passed | 0 failed | (rx_rhi_vk_tests)
[doctest] assertions: 212 | 212 passed | 0 failed | (rx_graph_tests)
[doctest] assertions: 57 | 57 passed | 0 failed |   (rx_graph_gpu_tests)
[doctest] assertions: 344 | 344 passed | 0 failed | (rx_material_gpu_tests)
[doctest] assertions: 50 | 50 passed | 0 failed |   (rx_material_tests)
```

### 3. Both presets build clean

```
$ cmake --build --preset linux-native      # exit 0, no warnings emitted as errors
$ cmake --build --preset windows-cross-zig # exit 0
```

`windows-cross-zig`'s own `ctest -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample'`
selection (6 tests, matching CI) also re-run clean under `xvfb-run` as a
bonus consistency check (not part of the mandatory verification, since CI
was only failing on `linux-native`):

```
100% tests passed, 0 tests failed out of 6
```

## Deviation from the brief (and why)

Fix 2's brief assumed the root cause lived in `context.cpp`'s extension
probe. Direct verification (reading the pinned vk-bootstrap commit's actual
source, then reproducing/un-reproducing the exact CI warning locally by
toggling `VK_LAYER_PATH`) showed that code was already correct — the probe
is already layer-scoped and activates sync validation correctly whenever
the layer is discoverable. The real defect was `.github/workflows/ci.yml`
never installing the `vulkan-validationlayers` package on the
`linux-native` runner. Fixed there instead, with `context.cpp` receiving
only a documentation comment recording the (negative) finding. This is a
narrower change than the brief anticipated, not a larger one — no
`context.cpp` logic was touched.

## Commit

Single commit, both fixes (they share verification and are small enough
not to warrant splitting): descriptor arena enforcement + tests, CI
package fix, context.cpp documentation note. `.superpowers/sdd/.../progress.md`'s
pre-existing uncommitted edit (present before this fix wave started) was
left untouched — out of scope for "touch only what these two fixes
require."
