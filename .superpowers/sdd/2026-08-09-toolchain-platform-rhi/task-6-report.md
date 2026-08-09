# Task 6 Report: rx_rhi_vk::Context (instance + validation layers)

Status: **DONE_WITH_CONCERNS** (implementation, both presets' configure+build, and the
validation-error-counting logic are all verified correct; the default `ctest` run on
*this specific dev machine* currently fails for a root-caused, pre-existing host-environment
reason unrelated to any code in this task — see "The validation-layer finding" below).

## What was implemented

- `src/rx_rhi_vk/include/rx_rhi_vk/context.h`, `src/rx_rhi_vk/src/context.cpp`: `rx::rhi::Context`
  exactly per the brief's design — `Context::create(requiredExtensions, enableValidation)` builds a
  `VkInstance` via `vkb::InstanceBuilder`, optionally with a validation-layer debug messenger whose
  callback increments a `std::shared_ptr<int>` error counter (shared with the returned `Context`) on
  any `WARNING`/`ERROR`-severity message. `.instance()`, `.debugMessenger()`, `.hasValidationErrors()`
  as specified. Move-only, RAII-correct (destructor and move-assignment both tear down the debug
  messenger before the instance, in the correct order).
- `src/rx_rhi_vk/tests/context_test.cpp`: the brief's test verbatim, plus
  `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` (see "Deviations" below).
- `src/rx_rhi_vk/CMakeLists.txt`: as specified in the brief.
- `third_party/CMakeLists.txt`: added volk (source-only fetch) and vk-bootstrap (dependency cache),
  and — required to make vk-bootstrap's own build actually work — converted Vulkan-Headers from a
  plain `FetchContent` (Task 5.6) to a dependency-cache install, and bumped its pin. Full rationale
  below.
- `CMakeLists.txt`: `add_subdirectory(src/rx_rhi_vk)`.
- `src/rx_platform/CMakeLists.txt`: updated a comment that described the old Vulkan-Headers
  mechanism (plain FetchContent, non-imported target, "already global") — now inaccurate since
  Vulkan-Headers is a dependency-cache/`find_package` install. No functional change; `rx_platform`
  still links `Vulkan::Headers` by the same name.

## The vk-bootstrap-on-Windows investigation (the flagged risk)

Checked vk-bootstrap's actual `CMakeLists.txt` at the pinned commit
(`556b79b165386f6c1a18362d30f2a076fdaa2778`) directly. Confirmed the risk is real, in a more
specific way than "just missing the loader":

vk-bootstrap's library target needs a `Vulkan::Headers` target and looks for one via, in order:
(1) `find_package(VulkanHeaders CONFIG QUIET)` — the standalone Vulkan-Headers CMake package,
distinct from the SDK-style one; (2) `find_package(Vulkan QUIET)` — the loader-import-library
one this project avoids everywhere else (broken here for `windows-cross-zig`, same root cause as
Task 5.6); (3) `FetchContent` of Vulkan-Headers as a last resort. **Critically, paths (2)-succeeding
and (3) both explicitly set `VK_BOOTSTRAP_INSTALL OFF`** (upstream's own comment: *"If we had to use
a direct path to get the headers, disable installing"*) — meaning if vk-bootstrap's isolated
dependency-cache sub-build (a fully separate `cmake -S -B` process; it cannot see this project's own
in-tree `Vulkan::Headers` target at all) ever fell into (2) or (3), `cmake --build --target install`
would install *nothing*, and this project's own `find_package(vk-bootstrap REQUIRED ...)` would fail
to find a `vk-bootstrap-config.cmake` — on Windows for certain (no loader there), and, I confirmed
empirically, **also on `linux-native` before the fix**, because path (1) alone (the standalone
`VulkanHeaders` package) wasn't available to that sub-build either.

**Fix applied**: converted this project's own Vulkan-Headers dependency from a plain `FetchContent`
(Task 5.6's approach) to a dependency-cache install (`rx_add_cached_dependency` +
`find_package(VulkanHeaders CONFIG ...)`), which produces a real `VulkanHeadersConfig.cmake` package.
Then passed `-DCMAKE_PREFIX_PATH=${Vulkan-Headers_CACHE_DIR}` into vk-bootstrap's own dependency-cache
`CMAKE_ARGS`, so its sub-build's *first* lookup attempt (path 1 above) succeeds directly on both
presets, never reaching the install-disabling fallback paths. Verified directly: after this fix,
`.deps-cache/vk-bootstrap-*/lib/cmake/vk-bootstrap/vk-bootstrap-config.cmake` exists and
`find_package(vk-bootstrap REQUIRED ...)` succeeds, on **both** `linux-native` and
`windows-cross-zig`, built from a clean `.deps-cache`.

## A second, unflagged blocker found and fixed: Vulkan-Headers/vk-bootstrap version skew

Fixing the above surfaced a real compile failure in vk-bootstrap's own isolated sub-build:
`unknown type name 'VkDepthClampModeEXT'`, `PFN_vkGetLatencyTimingsLegacyNV`, etc., in
`VkBootstrapDispatch.h`/`VkBootstrapFeatureChain.h`. Root cause: vk-bootstrap's pinned commit's
*generated* dispatch tables were generated against Vulkan-Headers `1.4.357`
(`gen/CurrentBuildVulkanVersion.cmake: VK_BOOTSTRAP_SOURCE_HEADER_VERSION_GIT_TAG "v1.4.357"`,
verified directly against the pinned commit's source) and reference extension types unconditionally
that don't exist in the older `vulkan-sdk-1.3.280.0` headers this project had pinned since Task 5.6.

**Fix**: bumped `RX_VULKAN_HEADERS_TAG` to `vulkan-sdk-1.4.357.0` — the exact same commit as vk-bootstrap's
own required tag, and also the same SDK version already pinned for volk in the brief (not a
coincidence; these three were clearly meant to be used together). This is safe: Vulkan headers are
additive/backward-compatible, and this project's own requested instance API version stays pinned at
`1.3.0` via `require_api_version(1, 3, 0)` in `context.cpp`, independent of which header version it's
compiled against — so this does not pull in any Vulkan 1.4 feature requirement, consistent with the
"Vulkan 1.3 baseline" constraint.

## A third issue found and fixed: `volk_SOURCE_DIR` cross-directory visibility

The brief's literal third_party snippet (`FetchContent_Declare(volk...)` +
`FetchContent_MakeAvailable(volk)`) has two real problems, both verified directly against the
installed CMake 3.22.1's `FetchContent.cmake` module source and by attempting the literal approach:

1. **Scoping**: `FetchContent_Populate`/`_MakeAvailable` set `<name>_SOURCE_DIR` via
   `set(... PARENT_SCOPE)` from inside a *function*, which only reaches the scope that directly
   called it — `third_party/CMakeLists.txt`'s own directory scope. `src/rx_rhi_vk` is a *sibling*
   directory (both `add_subdirectory()`'d from the root), not a descendant of `third_party`, so
   `${volk_SOURCE_DIR}` would be empty there — this project's other FetchContent'd deps
   (doctest, glm) never hit this because they're consumed only via their exported *targets*
   (which are automatically global for non-imported targets), never via a raw `_SOURCE_DIR` variable
   from a sibling directory.
2. **The `volk` target itself**: `FetchContent_MakeAvailable` implicitly `add_subdirectory()`s volk's
   own `CMakeLists.txt`, which defines a `volk` STATIC target that unconditionally compiles `volk.c`.
   Its only paths to find Vulkan headers for that target are `find_package(Vulkan)` (broken here),
   `$ENV{VULKAN_SDK}` (unset), or a target literally named `Vulkan-Headers` (not this project's
   `Vulkan::Headers`) — none resolve, so `VOLK_INCLUDES` stays empty and that target would fail to
   compile (`<vulkan/vulkan.h>` not found) the moment a default/"all" build runs it. Confirmed this
   project *does* run full "all" builds as its "builds cleanly" bar (matches Task 5.6's own
   verification, which used `cmake --build --preset X` with no `--target`).

**Fix**: fetch volk via `FetchContent_Populate` only (source, no `add_subdirectory`, so no stray
`volk` target ever enters the build graph), and explicitly `set(volk_SOURCE_DIR "${volk_SOURCE_DIR}"
PARENT_SCOPE)` to propagate it up to the root scope, which `src/rx_rhi_vk` (added later from the root
`CMakeLists.txt`) inherits. Verified: a full `cmake --build --preset linux-native` (no target, "all")
builds `volk.c` exactly once, only as part of `rx_rhi_vk`, with no stray `volk` target.

## The validation-layer finding (the reason for DONE_WITH_CONCERNS)

`ctest --preset linux-native -R rx_rhi_vk_tests` **fails on this machine as delivered**, but I
root-caused it precisely and it is not a defect in this task's code:

```
[error] [vulkan validation] Validation Error: [ VUID-VkInstanceCreateInfo-flags-zerobitmask ] ...
  vkCreateInstance: parameter pCreateInfo->flags must be 0.
[error] [vulkan validation] Validation Warning: [ VUID_Undefined ] ...
  Instance Extension VK_KHR_portability_enumeration is not supported by this layer.
```

vk-bootstrap's `InstanceBuilder::build()` unconditionally enables `VK_KHR_portability_enumeration`
(extension *and* the corresponding `VkInstanceCreateInfo::flags` bit) whenever the Vulkan **loader**
reports it available — with no `InstanceBuilder` API to opt out (checked: only
`PhysicalDeviceSelector::disable_portability_subset()` exists, a different, device-level knob).
This machine's Vulkan loader/ICD stack reports that extension as available (confirmed via
`vulkaninfo --summary`), but this machine's **system-packaged** `VK_LAYER_KHRONOS_validation` is
`1.3.204.1-2` (Pop!_OS/Ubuntu jammy's `apt` package — confirmed via `apt-cache policy`, and it's also
the newest candidate available in that distro's repos, so `apt upgrade` cannot fix it, and I have no
passwordless `sudo` to force anything anyway) — a validation layer build old enough to predate the
extension, which is exactly what its own warning says: *"Instance Extension
VK_KHR_portability_enumeration is not supported by this layer."* An unrecognized extension's flag bit
then trips the generic "flags must be 0" check as a false positive.

I verified this precisely, not by assumption: the exact same test binary, run with `VK_LAYER_PATH`
pointed at a validation layer already present elsewhere on this disk (`/home/ywadi/sponza/vvl`,
version `1.4.357` — coincidentally the exact same SDK version this task already pins for
Vulkan-Headers/volk/vk-bootstrap), **passes with zero validation errors**:

```
$ VK_LAYER_PATH=/home/ywadi/sponza/vvl DISPLAY=:1 ctest --preset linux-native -R rx_rhi_vk_tests --output-on-failure
Test #3: rx_rhi_vk_tests ..................   Passed    0.10 sec
100% tests passed, 0 tests failed out of 1
```

This conclusively demonstrates the `Context` implementation is correct and does produce zero
validation errors under a current validation layer — the failure is purely this dev box's outdated
`apt`-packaged component, not this task's code.

**Why I didn't bake a fix into the repo**: the only ways to make `ctest` pass unconditionally
regardless of the host's ambient validation layer would be (a) build
`KhronosGroup/Vulkan-ValidationLayers` from source through the dependency cache — a much heavier,
multi-repo dependency (SPIRV-Tools, SPIRV-Headers, Vulkan-Utility-Libraries) disproportionate to this
task's stated scope ("Context (instance+validation) only"), or (b) fetch a prebuilt LunarG SDK
tarball — not available as a clean pinned GitHub asset (checked: `Vulkan-ValidationLayers`' GitHub
releases only publish Android binaries), and inconsistent with this project's GitHub-only pinned-dependency
convention. Both felt like real scope creep for a task whose explicit file list is
`context.h`/`context.cpp`/`CMakeLists.txt`/`context_test.cpp`. I'm flagging this explicitly rather than
picking one unilaterally, matching how Task 5.6's own risk-for-Task-6 was flagged rather than silently
absorbed.

**Suggested follow-up** (mirroring how Task 5.5/5.6 were spun out of a risk noted in Task 5): Task 14
(CI matrix) should confirm whatever CI image it uses ships a validation layer new enough to know about
`VK_KHR_portability_enumeration` (GitHub Actions' `ubuntu-latest` + a current LunarG SDK setup step
almost certainly does), and/or a future task could decide to vendor a pinned validation layer if local
dev-machine reproducibility across arbitrary distros becomes a real requirement.

## Build/test commands and actual output

### `linux-native` — full build, clean from scratch

```
$ rm -rf build/linux-native && cmake --preset linux-native
...
-- [dep-cache] MISS for vk-bootstrap (key=vk-bootstrap-3208b0a00c2e4b82) - building once
...
[1/3] Building CXX object CMakeFiles/vk-bootstrap.dir/src/VkBootstrap.cpp.o
[2/3] Linking CXX static library libvk-bootstrap.a
[2/3] Install the project...
-- Installing: .../vk-bootstrap-3208b0a00c2e4b82/lib/cmake/vk-bootstrap/vk-bootstrap-config.cmake
-- Configuring done
-- Generating done

$ cmake --build --preset linux-native
[1/24] ... [24/24] Linking CXX executable src/rx_rhi_vk/rx_rhi_vk_tests
```
(Full 24-target "all" build succeeds, including `rx_rhi_vk`/`rx_rhi_vk_tests`, `rx_core`,
`rx_core_tests`, `rx_platform`, `rx_platform_tests`, `tools/*` — no regressions.)

### `linux-native` — ctest (all tests)

```
$ DISPLAY=:1 ctest --preset linux-native --output-on-failure
Test #1: rx_core_tests ....................   Passed    0.00 sec
Test #2: rx_platform_tests ................   Passed    0.06 sec
Test #3: rx_rhi_vk_tests ..................***Failed    0.09 sec
  [error] Validation Error: VUID-VkInstanceCreateInfo-flags-zerobitmask ...
  [error] Validation Warning: VK_KHR_portability_enumeration is not supported by this layer.
  CHECK_FALSE( ctx->hasValidationErrors() ) is NOT correct!
67% tests passed, 1 tests failed out of 3
```
(`rx_core_tests`/`rx_platform_tests` unaffected — no regression. `rx_rhi_vk_tests` fails for the
root-caused, pre-existing environment reason above; instance creation itself succeeds — the first two
`CHECK`s in the test pass, only the validation-error-count check fails.)

### `linux-native` — same binary, corrected validation layer (diagnostic only, not part of the repo)

```
$ VK_LAYER_PATH=/home/ywadi/sponza/vvl DISPLAY=:1 ctest --preset linux-native -R rx_rhi_vk_tests --output-on-failure
Test #3: rx_rhi_vk_tests ..................   Passed    0.10 sec
100% tests passed, 0 tests failed out of 1
```

### `windows-cross-zig` — full build, clean from scratch

```
$ rm -rf build/windows-cross-zig && cmake --preset windows-cross-zig
...
-- [dep-cache] MISS for vk-bootstrap (key=vk-bootstrap-4f2e05bb4436eacd) - building once
[1/3] Building CXX object CMakeFiles/vk-bootstrap.dir/src/VkBootstrap.cpp.obj
[2/3] Linking CXX static library libvk-bootstrap.a
[2/3] Install the project...
-- Installing: .../vk-bootstrap-4f2e05bb4436eacd/lib/cmake/vk-bootstrap/vk-bootstrap-config.cmake
-- Generating done

$ cmake --build --preset windows-cross-zig
[1/24] ... [24/24] Linking CXX executable src/rx_platform/rx_platform_tests.exe
```
(Full 24-target "all" cross-build succeeds, including `rx_rhi_vk.lib`/`rx_rhi_vk_tests.exe` — confirms
the vk-bootstrap-on-Windows fix works, not just linux-native.)

### `windows-cross-zig` — bonus ctest under Wine (not required by the task; noted for completeness)

```
$ DISPLAY=:1 ctest --preset windows-cross-zig -R rx_rhi_vk_tests --output-on-failure
Test #3: rx_rhi_vk_tests ..................   Passed    1.23 sec
```
Passes under Wine — plausibly because Wine's `vulkan-1.dll` shim/translation doesn't fully engage
`VK_EXT_debug_utils` the same way (so the debug messenger + its extension may not actually get set up
at all, silently), which per Task 5.6's own precedent isn't a signal this task's verification bar
relies on (only `linux-native`'s ctest is required to pass per that established convention). Not
investigated further since it wasn't required and a passing-by-omission result there doesn't need
root-causing to trust the `linux-native` result.

## Files changed

- `third_party/CMakeLists.txt` — added volk (`FetchContent_Populate`), vk-bootstrap
  (`rx_add_cached_dependency`), converted Vulkan-Headers from plain `FetchContent` to
  `rx_add_cached_dependency` + `find_package`, bumped its pin to `vulkan-sdk-1.4.357.0`.
- `src/rx_rhi_vk/CMakeLists.txt`, `src/rx_rhi_vk/include/rx_rhi_vk/context.h`,
  `src/rx_rhi_vk/src/context.cpp`, `src/rx_rhi_vk/tests/context_test.cpp` — new.
- `CMakeLists.txt` — `add_subdirectory(src/rx_rhi_vk)`.
- `src/rx_platform/CMakeLists.txt` — comment-only update (stale description of the Vulkan-Headers
  mechanism); no functional change.

Committed as `e908912` — "Add rx_rhi_vk::Context: Vulkan instance and validation layers". Verified
author/committer identity is the repo's own git config (`Yousef Wadi <ywadi85@gmail.com>`) and the
commit message contains no AI-attribution strings.

## Deviations from the brief, and why

1. **`context_test.cpp`**: added `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` before
   `#include <doctest/doctest.h>`, absent from the brief's literal snippet. `rx_rhi_vk_tests` is a
   single-translation-unit test executable (no shared `doctest_main.cpp`, matching
   `rx_platform_tests`'s established pattern from Task 5) — without this define, linking fails with
   undefined doctest runtime/`main()` symbols. Verified necessary by first trying the brief's literal
   version.
2. **`context.h`**: added `#include <memory>` (missing from the brief's snippet, which uses
   `std::shared_ptr` without including its header — works only by luck via transitive includes).
3. **`debugCallback`**: added `VKAPI_ATTR`/`VKAPI_CALL` to its signature (the brief's version omits
   them). Harmless no-op on x86_64 (this project's only target), but exactly matches
   `PFN_vkDebugUtilsMessengerCallbackEXT`'s and vk-bootstrap's own `default_debug_callback`'s actual
   declared signature.
4. **Vulkan-Headers**: switched from plain `FetchContent` (Task 5.6) to a dependency-cache install,
   and bumped its version pin — both required, detailed above, not optional polish.
5. **volk**: `FetchContent_Populate` instead of the brief's `FetchContent_MakeAvailable`, plus an
   explicit `PARENT_SCOPE` propagation — required, detailed above.
6. **vk-bootstrap**: added `-DCMAKE_PREFIX_PATH=${Vulkan-Headers_CACHE_DIR}` to its `CMAKE_ARGS`
   (absent from the brief's snippet) — required, detailed above.

None of these change the public `rx::rhi::Context` API from what the brief specified; all are
CMake-wiring/build-correctness fixes plus two small header-hygiene additions.

## Self-review

- **Completeness**: instance creation + validation-layer debug messenger both implemented;
  `linux-native` and `windows-cross-zig` both configure and fully build (all targets) from a clean
  `.deps-cache`, verified directly, not assumed.
- **Quality**: RAII reviewed — move constructor/move-assignment/destructor all null out the
  moved-from handles and tear down debug-messenger-before-instance; the `shared_ptr<int>` error
  counter's underlying `int` stays valid across moves since only the smart-pointer bookkeeping moves,
  not the pointee's address (the raw pointer handed to the debug-messenger's `pUserData` stays valid
  for the object's whole lifetime). Error-counting genuinely works — proven by both the failing
  (errors correctly detected and counted) and passing (zero when the environment is correct) runs
  above, not by code inspection alone.
- **Discipline**: no `VkDevice`/`VkPhysicalDevice`/swapchain code anywhere in `rx_rhi_vk` — `Context`
  only wraps instance + validation, per Task 6's stated boundary; Device is explicitly left to Task 7.
- **Testing**: "zero validation errors" was verified as a real, reproducible outcome (with a correct
  validation layer), and the one observed failure mode on this exact machine was root-caused to a
  specific, named, pre-existing package-version fact about this host, not left unexplained.

## Concerns for the reviewer

1. `ctest --preset linux-native -R rx_rhi_vk_tests` fails on this specific machine due to the
   validation-layer version-skew issue above — this is the main reason for **DONE_WITH_CONCERNS**
   rather than a clean DONE. The code and CMake wiring are correct and verified; the concern is
   entirely about this host's ambient package state and what (if anything) the project wants to do
   about it going forward (see "Suggested follow-up" above).
2. The Vulkan-Headers pin change (1.3.280.0 → 1.4.357.0) affects `rx_platform` too (same
   `Vulkan::Headers` target). Reviewed `rx_platform`'s actual usage (`VkInstance`, `VkSurfaceKHR` via
   SDL3's Vulkan interop) — nothing version-sensitive, and `rx_platform_tests` still passes on both
   presets, so this is verified safe, not just assumed compatible.

---

## Fix report: review finding addressed (commit `a5a99b6`)

Review came back "Needs fixes" with one Important finding: Task 6's own defining acceptance test
(`rx_rhi_vk_tests`) was red on the reference dev machine with nothing committed to flag, guard, or
work around it — only a note in this report. The reviewer independently reproduced and confirmed the
root cause described above (this machine's `apt`-packaged `VK_LAYER_KHRONOS_validation` 1.3.204
doesn't recognize `VK_KHR_portability_enumeration`, which vk-bootstrap's `InstanceBuilder` enables
unconditionally with no opt-out), and confirmed the `Context`/debug-callback code itself is correct.
The ask: add a narrowly-scoped guard for this specific known false positive in the debug callback
(not a broad suppression), still log it visibly (not hidden), and prove the guard can't mask a real
error — verified against this machine's actual, unmodified validation layer, not a swapped-in newer
one.

### What changed

`src/rx_rhi_vk/src/context.cpp`: added `isKnownPortabilityEnumerationLayerBug(const char* message)`,
called from `debugCallback` for any message at `WARNING` severity or above. It returns `true` only for
messages matching one of two precise, distinctive signatures:

1. The exact VUID string `VUID-VkInstanceCreateInfo-flags-zerobitmask` (a real, globally-unique
   Khronos-assigned VUID — in this project's current code, `VkInstanceCreateInfo::flags` is never set
   by anything except vk-bootstrap's automatic portability-enumeration bit, so this VUID cannot fire
   for any other reason today).
2. Both the substring `VK_KHR_portability_enumeration` **and** the substring
   `is not supported by this layer` present together (the warning's own VUID field is the generic
   Khronos placeholder `VUID_Undefined`, used by many unrelated informational messages, so it is
   deliberately *not* used alone as a match key — matching on the extension name plus its exact
   phrase is the precise, narrow signal instead).

Matched messages are logged via `RX_LOG_WARN` (prefixed `"(known false positive: validation layer
predates VK_KHR_portability_enumeration)"`) instead of `RX_LOG_ERROR`, and are **not** counted toward
`errorCount`/`hasValidationErrors()`. Everything else at `WARNING`+ severity is still logged via
`RX_LOG_ERROR` and still counted, unchanged.

### Covering test command and output — this machine's actual, unmodified validation layer

No `VK_LAYER_PATH` override; the system's own `apt`-packaged `1.3.204` layer, exactly as installed:

```
$ unset VK_LAYER_PATH
$ DISPLAY=:1 ctest --preset linux-native -R rx_rhi_vk_tests --output-on-failure
Test #3: rx_rhi_vk_tests ..................   Passed    0.09 sec
100% tests passed, 0 tests failed out of 1
```

Full suite, same unmodified layer, no regressions:

```
$ DISPLAY=:1 ctest --preset linux-native --output-on-failure
Test #1: rx_core_tests ....................   Passed    0.00 sec
Test #2: rx_platform_tests ................   Passed    0.06 sec
Test #3: rx_rhi_vk_tests ..................   Passed    0.08 sec
100% tests passed, 0 tests failed out of 3
```

Direct binary run (`--success`) confirms the two known-false-positive messages are still emitted,
just as visible `[warning]`s, and the test's own `CHECK_FALSE(ctx->hasValidationErrors())` now
correctly evaluates `false`:

```
[warning] [vulkan validation] (known false positive: validation layer predates VK_KHR_portability_enumeration) Validation Error: [ VUID-VkInstanceCreateInfo-flags-zerobitmask ] ... flags must be 0 ...
[warning] [vulkan validation] (known false positive: validation layer predates VK_KHR_portability_enumeration) Validation Warning: [ VUID_Undefined ] ... Instance Extension VK_KHR_portability_enumeration is not supported by this layer. ...
...
CHECK_FALSE( ctx->hasValidationErrors() ) is correct!
  values: CHECK_FALSE( false )
[doctest] Status: SUCCESS!
```

### Proof the guard doesn't mask a genuinely different real error

Temporarily added a second `TEST_CASE` to `context_test.cpp` (removed before the fix commit — `git
diff` against the committed file was empty afterward, confirmed below) that deliberately triggers a
real, distinct validation error unrelated to portability enumeration: calling
`vkEnumeratePhysicalDevices(ctx->instance(), nullptr, nullptr)`, which violates
`VUID-vkEnumeratePhysicalDevices-pPhysicalDeviceCount-parameter` (a required non-null output
parameter passed as null). Ran it against the same unmodified `1.3.204` system layer:

```
[error] [vulkan validation] vkEnumeratePhysicalDevices: Received NULL pointer for physical device count return value. [VUID-vkEnumeratePhysicalDevices-pPhysicalDeviceCount-parameter]
CHECK( ctx->hasValidationErrors() ) is correct!
  values: CHECK( true )
[doctest] test cases: 2 | 2 passed | 0 failed | 0 skipped
```

This message is logged via `RX_LOG_ERROR` (not `RX_LOG_WARN`) and correctly increments the error
counter — `hasValidationErrors()` reports `true` for this genuine error, while the known false
positive from the same test run's `Context::create` call is still filtered. This confirms the guard
is scoped to the one specific known issue and does not broadly suppress validation reporting.

After confirming this, removed the probe `TEST_CASE`; `git diff src/rx_rhi_vk/tests/context_test.cpp`
against the working tree was empty (file is byte-for-byte what's committed), then rebuilt and
reran the full suite (shown above) to confirm the final, committed state still passes.

Also re-verified `windows-cross-zig` still builds cleanly after this change
(`cmake --build --preset windows-cross-zig`: all targets, including the rebuilt `rx_rhi_vk`/
`rx_rhi_vk_tests.exe`, succeeded with no errors).

### Commit

`a5a99b6` — "Guard rx_rhi_vk::Context against a known validation-layer false positive" (new commit,
does not amend `e908912`). Verified author/committer identity is the repo's own git config
(`Yousef Wadi <ywadi85@gmail.com>`) and `git log -1 --format='%B' HEAD` contains no AI-attribution
strings.

### Updated status

**DONE** — the previously-flagged concern (Task 6's acceptance test red on the reference machine with
no committed mitigation) is now resolved: the test passes on this machine using its actual,
unmodified validation layer, the fix is narrowly scoped and proven not to mask real errors, and both
presets still build cleanly.
