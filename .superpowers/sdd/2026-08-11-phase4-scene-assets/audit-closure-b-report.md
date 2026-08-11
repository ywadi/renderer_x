# Stage 0 audit closure -- track B (src/rx_material)

Scope: `src/rx_material` only (+ its tests). Findings closed: F2, F3, F7 (the
items the audit named against `src/rx_material`). Base: `a5e7dbf` (merged
`main --ff-only`, matches the audit's own HEAD).

## F2 (MEDIUM) -- bindless slot released before the frames-in-flight fence

**Fix.** `MaterialSystem::releaseTexture()` (`material_system.cpp`) used to
call `impl.bindless->release(record->bindlessHandle)` immediately, while the
real `Texture2D` teardown correctly deferred through `DeletionQueue` until
`onFrameCompleted()`. Per the audit's own suggested fix, the bindless
release now runs INSIDE the same `DeletionQueue`-retired closure as the
texture teardown -- both wait for the identical fence-confirmed point. The
closure still runs on the main thread from `onFrameCompleted()`, so
`BindlessTable::release()`'s `RX_ASSERT_MAIN_THREAD` guard stays satisfied.

**Regression test.** Added to `tests/test_material_system.cpp`:
"MaterialSystem::releaseTexture defers returning the bindless slot itself to
the free list until onFrameCompleted, not just the real Texture2D teardown".
Fills the fixture's 4-slot sampled-image bindless capacity, releases one
texture, then asserts `createTexture2D()` throws capacity-exhausted
(`std::runtime_error`) on every attempt until the exact frame number is
confirmed via `onFrameCompleted()` -- at which point the next
`createTexture2D()` call succeeds and is handed back the EXACT index that
was released (bindless's free list is LIFO, per `HandlePool`'s own
`acquire()`/`release()` in `rx_core/handle.h`), positively distinguishing
"deferred" from "coincidentally still failing for some other reason." This
test genuinely discriminates the bug: run against the pre-fix code, its
first `CHECK_THROWS_AS` fails (the slot IS handed back immediately) instead
of throwing.

**Follow-on defect found while writing this test (fixed, same file).** The
test above is the first in this codebase to leave `TextureRecord`s alive in
`impl.textures` when a `MaterialSystem` is destroyed (every prior texture
test released and fence-confirmed every texture it created before letting
the `MaterialSystem` go out of scope). Doing so crashed
(`rx_material_gpu_tests` SIGSEGV in `RelWithDebInfo`; a VMA debug build
reproduced it cleanly as `Assertion 'm_pMetadata->IsEmpty() ...' failed` at
allocator teardown) via a genuine, pre-existing bug: `MaterialSystem::Impl`
declares `textures` (a `HandlePool<TextureTag, TextureRecord>`) BEFORE
`allocator`, so ordinary reverse-declaration-order member destruction tears
`allocator` down FIRST -- while any still-live `Texture2D` inside `textures`
needs that exact `VmaAllocator` to run its own `vmaDestroyImage` teardown
afterward. `~MaterialSystem()` now explicitly walks `impl.textureHandles`,
releases each surviving texture's bindless slot (safe unconditionally here,
mirroring `defaultSamplerHandle`'s own release just below, because
`vkDeviceWaitIdle()` already ran earlier in the same destructor), and moves
each texture's real `Texture2D` into a loop-scoped local so its destructor
runs immediately -- while `allocator`/`device` are still unquestionably
alive -- rather than leaving it to `impl.textures`' own, wrongly-ordered
implicit RAII. This also fixes a second, quieter defect the same bug
masked: a texture never explicitly released before shutdown previously never
returned its bindless slot to the table's free list at all.

This follow-on was not named in the audit; it was found empirically (root-
caused via a scratch `-O0`/VMA-debug build, confirmed by reading
`MaterialSystem::Impl`'s member order) while making the required F2
regression test pass safely, and is squarely inside this track's assigned
file. Fixing it in place was the only option consistent with "no half-assed
solutions" -- rewriting the test to avoid the scenario would have shipped a
known, reproducible crash path unfixed.

Verified: `sample_04_streaming` (the sample the `bindless.h` release-safety
contract itself points to as the worked release-then-recreate example)
still passes; zero validation errors under forced lavapipe + the newer
(`sponza/vvl`) validation layer for the full `rx_material` suite.

## F3 (MEDIUM) -- ABI-boundary exception leaks

**Fix 1 -- `RxUnknownBase::release()`** (`api_impl.cpp`): the `delete
static_cast<Derived*>(this)` call now runs inside try/catch. Both
`std::exception` and a catch-all are logged (`RX_LOG_ERROR`) and swallowed,
never rethrown -- the refcount already hit zero, so the object's contract
with every caller is already "gone," and leaking it outright (rather than
risking a second `delete` against a possibly-partially-destroyed object) is
the safe choice, per the audit's own suggested fix.

**Fix 2 -- `MaterialSystemImpl::loadMaterial()`** (`api_impl.cpp`): the
`std::filesystem::path path(slangModulePath)` construction moved from
outside `loadMaterial`'s try block to inside it (previously a
narrow->wide `path::value_type == wchar_t` conversion on the
windows-cross-zig target could throw `std::system_error` on malformed
input, unguarded). The catch clause now logs the raw `slangModulePath`
`const char*` instead of `path.string()`, since `path` may not exist by the
time the catch runs.

**Regression test -- malformed-path case, added in two halves (both
needed, honestly scoped):**

- `test_api_contract.cpp` (device-free half): passes a byte sequence
  invalid as UTF-8 (`0xFF`/`0xFE`/`0x80` bytes, no embedded NUL) to
  `loadMaterial()` on a device-free (`internal_ == nullptr`) instance.
  Confirms `RX_E_FAIL`, no crash. **Honesty note, stated in the test's own
  comment too:** a device-free instance's `internal_ == nullptr` guard
  returns before ever reaching the (now-fixed) path-construction line at
  all -- this case cannot, and does not claim to, exercise the fix itself.
  It proves malformed byte content flowing into `loadMaterial()`'s argument
  never crashes a device-free instance, which is the honest ceiling of what
  a device-free test can prove here.
- `test_api_factory.cpp` (GPU-backed half, the one that genuinely exercises
  the fixed line): the same byte sequence, passed to `loadMaterial()`
  against a REAL internal `MaterialSystem` (real `VkDevice`). Confirms
  `RX_E_COMPILE` (the file genuinely doesn't exist at that path; the
  existing `compileMaterial()`/`readFileBytes()` machinery reports it as a
  normal, caught `std::runtime_error`, not a crash), and that no API object
  is leaked (`debugLiveApiObjectCount()` unchanged).
- **Stated honestly, per the task's own instruction:** on Linux/libstdc++
  (and every POSIX libc++), `std::filesystem::path::value_type` is `char`,
  so constructing a path from a `const char*` is a straight byte copy with
  no narrow->wide conversion at all -- this exact input can never actually
  throw `std::system_error` on this platform, on either side of the fix.
  Both new tests verify the REACHABLE half of F3 on Linux (malformed bytes
  flow safely through to a documented error code, never a crash); the
  `std::system_error`-specific half is windows-cross-only and verified here
  only by inspection of the fix itself -- this repo's CI has no
  windows-cross GPU job that could run a device-backed test like the
  GPU-backed one above against that target (this matches the Stage 0
  audit's own F8 observation about windows-cross's GPU-test coverage).

## F7 (LOW, ABI misc) -- items applying to this track

- `rx_api.h:138-141` (`RxTextureDesc`) and `:266-268` (`RxMaterialSystemDesc`)
  had `sizeof` pins but no `alignof` static_asserts (docs/abi.md requires
  both). Added both: `alignof(RxTextureDesc) == 8` (a fixed literal,
  matching the struct's own already-64-bit-only `sizeof == 32` pin) and
  `alignof(RxMaterialSystemDesc) == alignof(void*)` (relative, matching that
  struct's own `sizeof == sizeof(void*)` pin). Re-pinned in
  `tests/test_api_header_self_contained.cpp` too, alongside that file's
  existing sizeof re-pins (same "successful compilation of this file IS the
  test" discipline that file already documents for itself), plus
  `alignof(RxGuid)`, which that file was missing.
- "No GUID-uniqueness test exists across the five `kIID_*` constants
  (manually verified unique)." Replaced with an enforced compile-time proof:
  10 `static_assert(!guidEquals(...))` pairwise checks (all
  <sub>5</sub>C<sub>2</sub> combinations) added to
  `test_api_header_self_contained.cpp`, using a local `constexpr
  guidEquals()` comparing `RxGuid`'s four members directly (no padding,
  per that type's own pinned static_asserts). A future GUID collision now
  fails the build, not a future manual re-check.
- "Exception-boundary tests are structural only (`isDocumentedResult`) --
  genuine fault injection would have caught F3." The new F3 malformed-path
  test in `test_api_factory.cpp` is genuine fault injection through the
  real, fixed `loadMaterial()` code path (a real file-not-found failure
  triggering a real, caught `std::runtime_error`) -- not merely a
  structural "returned a documented `RxResult`" check. No further action
  taken on this item beyond that: forcing an actual `bad_alloc` for
  `release()`'s own try/catch specifically was not attempted, matching this
  codebase's own established, deliberate precedent
  (`test_api_contract.cpp`'s `isDocumentedResult()` comment: "Forcing a
  genuine bad_alloc ... was considered and rejected as not worth the
  fault-injection scaffolding it would need") -- and `RxUnknownBase` is
  anonymous-namespace-private to `api_impl.cpp`, so no external test
  translation unit could instantiate a throwing `Derived` against it
  regardless.
- MSVC-consumability / never-compiled-by-any-toolchain-in-repo item: purely
  informational in the audit (a truthfully-scoped claim, not a verified
  one); no code or test action implied or taken.

## Verification

- `rx_material_tests` + `rx_material_gpu_tests`, default (this machine's
  selected Vulkan device): both suites, 100% pass (14/14 + 32/32 test
  cases; 978 assertions in the GPU suite).
- Same two suites, forced lavapipe (`VK_ICD_FILENAMES=lvp_icd.json`) +
  the newer validation layer (`VK_LAYER_PATH=<sponza/vvl>`, confirmed via
  `VK_LOADER_DEBUG=info` that the loader actually inserted that manifest):
  100% pass, zero validation-layer messages (only the pre-existing,
  documented "known false positive" lines, unrelated to this track).
- Full `ctest`, `linux-native` preset (RelWithDebInfo, `RX_DEBUG_CHECKS=ON`,
  `RX_TRACY=ON`): 17/17 passed.
- Full `ctest`, `windows-cross-zig` preset (same build type/flags, run
  under Wine): 17/17 passed.
- Both presets build clean (`cmake --build`) end to end, including every
  sample.
- Root-caused the F2 follow-on crash with a disposable scratch `-O0` +
  `RX_TRACY=OFF` CMake configuration under `build/debug-diag` (not a
  preset, not committed, deleted after use) plus `gdb`; no production files
  outside `src/rx_material` were touched at any point (a brief diagnostic
  `fprintf` pair was added to and then fully reverted from
  `src/rx_rhi_vk/src/texture.cpp` during root-causing -- confirmed via `git
  diff --stat` showing no changes to that file in the final state).
- `git diff` grepped clean of any AI-attribution pattern.

## Files touched

- `src/rx_material/material_system.cpp` -- F2 fix (`releaseTexture()`) +
  the follow-on destructor-ordering fix (`~MaterialSystem()`).
- `src/rx_material/api_impl.cpp` -- F3 fixes (`release()` try/catch;
  `loadMaterial()` path-construction reorder).
- `src/rx_material/include/rx_material/rx_api.h` -- F7 `alignof` pins.
- `src/rx_material/tests/test_material_system.cpp` -- F2 regression test.
- `src/rx_material/tests/test_api_contract.cpp` -- F3 device-free
  regression test.
- `src/rx_material/tests/test_api_factory.cpp` -- F3 GPU-backed regression
  test.
- `src/rx_material/tests/test_api_header_self_contained.cpp` -- F7
  `alignof` re-pins + GUID-uniqueness compile-time proof.

## Concerns / handoff notes

- The F2 follow-on fix (destructor member-ordering / bindless-leak-on-
  shutdown) is a real defect fix beyond the audit's literal F2/F3/F7 text,
  discovered as a direct consequence of writing the F2 regression test the
  task required. Flagging explicitly per the task's own spirit (verify
  subagent work; no half-assed solutions) -- this is not scope creep for
  its own sake, it was a blocking prerequisite for landing F2's own test
  safely.
- Did not touch `cmake/`, `.github/`, `src/rx_task`, or `rx_graph` docs --
  those are the sibling tracks' scope per the coordinator's own split.
- F1 (HIGH, `rx_task::Scheduler::runOnIoThread()` TSAN race) and F4
  (MEDIUM, dep-cache key under-capture) are explicitly out of this track's
  scope (`src/rx_task`/`cmake` respectively) -- not addressed here,
  presumably the other track(s)' responsibility per the coordinator's
  split.
