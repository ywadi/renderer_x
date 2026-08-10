# Task 6 report: COM-lite public surface (rx_api.h)

**Branch:** `main` (worked directly, no worktree)
**Commit:** see final commit hash reported alongside this report.

## What was built

- `src/rx_material/include/rx_material/rx_api.h` (new): the ABI boundary header.
  Self-contained (`<cstdint>` only, no rx_*/STL/Vulkan/Slang include). Exactly the
  verbatim shape from the task brief: `RxResult`/error codes, `RxGuid`,
  `IRxUnknown`/`IRxTexture`/`IRxMaterialInstance`/`IRxMaterial`/`IRxMaterialSystem`,
  `RxMaterialSystemDesc`, `extern "C" rxCreateMaterialSystem`. Top-of-file doc
  comment cites design doc D5 for the ABI rules. GUIDs generated with `uuidgen`
  (five real UUIDs, converted to `RxGuid` literals with a small one-off Python
  script — not hand-transcribed hex). `static_assert(sizeof(RxGuid) == 16)`,
  `static_assert(alignof(RxGuid) == 4)`, and
  `static_assert(sizeof(RxMaterialSystemDesc) == sizeof(void*))` are pinned in the
  header itself, not just in tests.
- `src/rx_material/include/rx_material/rx_api_detail.h` (new, not in the brief's
  literal file list): declares `rx::material::detail::debugLiveApiObjectCount()`
  — a test-only seam (mirrors `material_system.h`'s own
  `detail::debugCompileCount()` carve-out) so the refcount-round-trip contract
  test can observe "destroyed exactly once" from outside `api_impl.cpp`'s
  translation unit. Never included by `rx_api.h`; not part of the ABI.
- `src/rx_material/api_impl.cpp` (new): the implementation. `MaterialSystemImpl`,
  `MaterialImpl`, `MaterialInstanceImpl` each single-inherit (via a CRTP
  `RxUnknownBase<Derived, Interface>` helper that supplies `addRef()`/`release()`
  once) their one public interface, marked `final`. `release()` always
  `delete static_cast<Derived*>(this)` — no virtual destructor anywhere in the
  hierarchy. `queryInterface` does a `memcmp`-based GUID compare (RxGuid has no
  padding, matching `IsEqualGUID`'s own technique) and returns the identical
  object pointer for `IRxUnknown` and the object's own interface (COM identity,
  automatic from single inheritance). Every public entry point (factory,
  `loadMaterial`, `createInstance`) wraps internal work in `try/catch
  (const std::exception&)` and maps to an `RxResult`, logging diagnostics via
  `RX_LOG_ERROR` — nothing ever throws across the boundary.
- `src/rx_material/CMakeLists.txt` (modified): added `api_impl.cpp` to
  `add_library(rx_material STATIC ...)`.
- `src/rx_material/tests/CMakeLists.txt` (modified): added a new device-free
  target `rx_material_tests` (contract tests) alongside the existing
  `rx_material_gpu_tests`, and added `test_api_factory.cpp` to the latter.
- Tests: `test_api_contract.cpp`, `test_api_header_self_contained.cpp`,
  `doctest_main_plain.cpp` (device-free target `rx_material_tests`);
  `test_api_factory.cpp` (added to `rx_material_gpu_tests`); a new fixture
  `tests/data/test_textured.slang` (see below).

## Interface layout decisions

- **No namespace wrapper.** `rx_api.h`'s types live at global scope, matching
  Slang's own `slang.h`: `SlangUUID`/`ISlangUnknown`/`SLANG_COM_INTERFACE` are all
  declared *before* `namespace slang { ... }` opens — confirmed by reading the
  file directly rather than assuming. The `Rx`/`IRx` prefix is the de facto
  namespacing, exactly like D3D's global `ID3D11Device`/`HRESULT`.
- **GUID constants as free `static constexpr RxGuid kIID_...`**, not a nested
  static function (Slang's `getTypeGuid()` pattern) or a data member — the brief's
  literal text asked for "one static constexpr RxGuid kIID_IRx... per interface,"
  and a namespace-scope constant has zero object-layout impact (no vtable slot,
  no per-instance storage), so it can't violate the "no data members" rule even
  though the name (`kIID_...`) reads like a member.
- **`RxUnknownBase<Derived, Interface>` CRTP helper** in `api_impl.cpp` (never
  exposed to consumers) shares `addRef()`/`release()`'s identical bodies and the
  live-object counter across all three concrete classes. This adds intermediate
  C++ classes to the *implementation*-side inheritance chain, but never changes
  the ABI-facing vtable slot order/count a consumer sees through an
  `IRx*`-typed pointer — the rule under research is "single inheritance, no
  data members" for the *interface* declarations in `rx_api.h`, which this
  doesn't touch. All three concrete classes are marked `final`, which also
  resolved a real `-Wdelete-non-abstract-non-virtual-dtor` warning caught during
  a manual `-Wall -Wextra -Wpedantic -Wshadow` pass (this repo's CMake sets no
  warning flags anywhere, so `ninja` alone would not have caught this — I
  extracted the real compile command from `compile_commands.json` and reran it
  with warnings enabled for every new file before considering the task done).
- **`RxMaterialSystemDesc.internalMaterialSystem` is allowed to be null.** The
  factory only rejects null `desc`/`outSystem` with `RX_E_INVALIDARG`; a null
  internal pointer is a documented, legal "device-free" instance on which
  `queryInterface`/`addRef`/`release`/`reloadChanged` all work normally (none
  touch the internal pointer) but `loadMaterial()` returns `RX_E_FAIL` instead of
  crashing. This is what makes the device-free contract tests possible without
  fabricating a fake non-null pointer that nothing would ever validate — a real,
  useful `IRxMaterialSystem` object backs every one of those tests, not a mock.
- **IRxMaterialInstance's name/type validation uses a SECOND, independent,
  reflection-only Slang session** (`reflectMaterialParams()` in `api_impl.cpp`),
  not an extension of `material_system.h`. Task 6's Modify list for this task
  named only `CMakeLists.txt`; `MaterialSystem`'s own reflection
  (`reflectMaterialLayout()`, Task 5) only produces one whole-`ParameterBlock`
  binding descriptor — no per-field name/type breakdown — so there is no way to
  answer "does parameter X exist, and what type is it" from what
  `material_system.h` exposes today. Rather than fabricate a name/type table or
  hand-parse Slang source text (this project's standing "prefer a ready-made
  library" rule explicitly calls out parsers as something to reuse, not
  reinvent), `api_impl.cpp` composes just the material module (no
  `forward_entry.slang`, no entry points) and calls
  `IComponentType::getLayout()` on the **unlinked** composite — verified
  directly against this project's shipped Slang v2026.14.1 build (see Test
  results below) rather than assumed from documentation. Field kinds are
  classified using this engine's own already-established D8 convention: a
  `float` field is `Float`, a 4-wide `float` vector is `Float4`, and a plain
  `uint` field is `TextureIndex` (bindless-table index — D8 says a material never
  puts a resource-typed field inside `TParams`, only plain data, and a bindless
  index is exactly that). Anything else reflects as `Unsupported` — a real,
  named field with no matching setter, so calling any setter on it correctly
  returns `RX_E_INVALIDARG`, not `RX_E_NOTFOUND`.
  - **Cost, named explicitly:** this is a second Slang session per
    `loadMaterial()` call, on top of `MaterialSystem`'s own — real, measurable
    duplicate work. The `slang::IGlobalSession` (the expensive part — stdlib
    load) is shared across every `loadMaterial()` call on one
    `IRxMaterialSystem` instance (created lazily, once), mirroring
    `MaterialSystem::Impl`'s own reasoning for the identical tradeoff, but the
    per-call `ISession`/composite/layout work is genuinely duplicated. **Task 7
    should collapse this** by having `MaterialSystem` itself expose a per-field
    parameter table computed from the program it already links (it already
    retains `MaterialRecord::linkedProgram`), once that task is touching
    `material_system.h` anyway for GPU-binding.
- **`test_textured.slang`** (new fixture, not in the brief's literal Create
  list): `test_unlit.slang`/`test_solid.slang` only declare a `float4` field, so
  neither can exercise `setTexture()`'s success path or its `IRxTexture`
  addRef/release lifecycle at all. Added one more tiny fixture (`uint
  albedoIndex`, same D8 convention) specifically so that code path has a real,
  passing, non-mocked-material test rather than being provably-untested dead
  code. `test_api_factory.cpp` also defines a minimal test-only `FakeTexture :
  IRxTexture` double (there is no `rxCreateTexture` factory yet in this task's
  surface) to drive it.

## What Task 7 must pick up

1. **`reloadChanged()`** is a documented no-op returning `RX_OK` (declared now
   purely for GUID/vtable stability). Task 7 wires real behavior: watch every
   module path `loadMaterial()` was called with, re-load/re-link changed ones
   (against a fresh `MaterialSystem`/session per `material_system.cpp`'s own
   documented same-module-name reload caveat), invalidate the affected
   pipeline-cache entries.
2. **Collapse the duplicate Slang reflection pass** described above:
   `MaterialSystem` should expose a per-field parameter name/type table (set,
   scalar/vector kind) computed once from its own already-linked program,
   removing `api_impl.cpp`'s second session entirely. This also removes the
   current "params computed from a SEPARATE session than the one that produced
   the actual SPIR-V" duplication risk (low today — same source bytes, and as
   of fix round 1 below, genuinely identical session/target configuration —
   but a real seam to close). **Correction:** this item originally claimed
   "same session config" as already true; it was not (see Fix round 1, F3)
   until the fix below.
3. **Connect `IRxMaterialInstance`'s CPU-side blob to actual GPU binding.** Task
   6 validates and stores `setFloat`/`setFloat4`/`setTexture` values in a plain
   `std::unordered_map<std::string, StoredParam>` inside `MaterialInstanceImpl` —
   nothing currently reads this blob to populate a real descriptor set/uniform
   buffer for `gParams`. Task 7 needs: (a) a way to get the reflected field
   *offsets* (not just names/kinds — `reflectMaterialParams()` doesn't compute
   byte offsets today, since Task 6 never needed them), (b) per-instance
   descriptor-set/buffer allocation, (c) a write path that walks the stored
   blob and pushes it to the GPU-visible buffer before draw.
4. **`IRxTexture`'s own creation path.** Nothing in Task 6 creates a real
   `IRxTexture` (no `rxCreateTexture` factory) — it only exists as a parameter
   type for `setTexture()`. The `setTexture` success-path test in
   `test_api_factory.cpp` uses a hand-rolled test-only double
   (`FakeTexture`), not a real renderer-backed texture. A real texture-loading
   public surface is future work.
5. Consider whether `RxMaterialSystemDesc.internalMaterialSystem == nullptr`
   being *legal* (vs. rejected with `RX_E_INVALIDARG`) is still the right call
   once a real standalone consumer exists outside this repo's own tests — it
   was a deliberate, documented choice made specifically to make device-free
   ABI contract testing possible without a fake non-null pointer; a future task
   could tighten this if the device-free testing need goes away (e.g. a
   dedicated test-only factory overload).

## Test results

- `ctest --preset linux-native -R rx_material --output-on-failure`: **2/2
  passed** (`rx_material_gpu_tests`, `rx_material_tests`).
  - `rx_material_tests` (new, device-free): 9 test cases / 40 assertions, all
    passing — `rxCreateMaterialSystem` null-arg rejection, null-internal
    "device-free" construction, `queryInterface` null-out-param rejection,
    `queryInterface` unknown-IID → `RX_E_NOINTERFACE` + null out-param,
    `queryInterface` identity (`IRxUnknown` and the object's own interface
    resolve to the same pointer), refcount round-trip (`addRef`→2,
    `release`→1 still alive, `release`→0 destroyed exactly once, verified via
    `debugLiveApiObjectCount()`), `loadMaterial` on a null-internal instance →
    `RX_E_FAIL` never crashes, `loadMaterial` null-arg rejection,
    `reloadChanged` no-op.
  - `rx_material_gpu_tests`: 9 test cases, including this task's 3 new ones —
    factory + `loadMaterial` happy path against a real `VkDevice`-backed
    internal `MaterialSystem` with `test_unlit.slang` (validates the real
    reflected `tint : float4` field: `setFloat4("tint", ...)` → `RX_OK`,
    `setFloat("tint", ...)` → `RX_E_INVALIDARG`, `setTexture("tint", ...)` →
    `RX_E_INVALIDARG`, unknown name → `RX_E_NOTFOUND` on all three setters, null
    args → `RX_E_INVALIDARG`, QI identity on the instance); `setTexture`
    validation + `IRxTexture` refcount lifecycle against `test_textured.slang`'s
    real `albedoIndex : uint` field (addRef on store, release on overwrite,
    release on instance destruction — all verified via the `FakeTexture`
    double's own refcount, not just "it didn't crash"); a real Slang syntax
    error (`test_bad_syntax.slang`) mapped to `RX_E_COMPILE` with the
    diagnostic logged, not thrown.
  - Full repo regression: `ctest --preset linux-native --output-on-failure` —
    **14/14 passed**, nothing else regressed.
- Both presets build clean: `cmake --build build/linux-native` and
  `cmake --build build/windows-cross-zig`, targets `rx_material`,
  `rx_material_tests`, `rx_material_gpu_tests` — zero errors on either.
- Manual warning check (this repo's CMake sets no `-Wall`/`-Wextra` anywhere, on
  any target, so `ninja` alone would not surface this): extracted each new
  file's real compile command from `compile_commands.json` and reran it with
  `-Wall -Wextra -Wpedantic -Wshadow` appended. Caught and fixed one real issue
  (`-Wdelete-non-abstract-non-virtual-dtor` on all three impl classes plus the
  test-only `FakeTexture` — fixed by marking all four `final`). Zero warnings
  after the fix.
- Empirically verified (not assumed from Slang's docs) before relying on it:
  `slang::IComponentType::getLayout()` returns a complete, correct
  `ProgramLayout` — with the `gParams` field walk finding real names/types —
  when called on a **composite that was never linked** (no
  `forward_entry.slang`, no entry points in the composite at all). Confirmed by
  the `test_api_factory.cpp` happy-path test actually validating `tint`/
  `albedoIndex` by NAME against real reflected data, not a hand-maintained
  table.

## Files

- `src/rx_material/include/rx_material/rx_api.h` (new)
- `src/rx_material/include/rx_material/rx_api_detail.h` (new)
- `src/rx_material/api_impl.cpp` (new)
- `src/rx_material/CMakeLists.txt` (modified)
- `src/rx_material/tests/CMakeLists.txt` (modified)
- `src/rx_material/tests/test_api_contract.cpp` (new)
- `src/rx_material/tests/test_api_header_self_contained.cpp` (new)
- `src/rx_material/tests/doctest_main_plain.cpp` (new)
- `src/rx_material/tests/test_api_factory.cpp` (new)
- `src/rx_material/tests/data/test_textured.slang` (new)

## Concerns

- The second Slang reflection session (item 2 under "What Task 7 must pick
  up") is real, measurable duplicate work per `loadMaterial()` call. It is
  bounded (reflection only, no codegen/linking) and the expensive
  `IGlobalSession` is shared, but it is still a genuine seam a reviewer should
  weigh against extending `material_system.h` directly in a follow-up rather
  than living with the duplication long-term.
- `RxMaterialSystemDesc.internalMaterialSystem == nullptr` being accepted by
  the factory (rather than rejected) is a real API design choice, not a
  neutral default — flagged above (item 5) for explicit reconsideration once
  there's a real external consumer.

---

## Fix round 1 (review: `task-6-review.md`, commit `6c49910`)

Review verdict: spec ❌ on "error codes never exceptions" (1 High, 2 Medium, 1
Low). All four addressed below.

### F1 (High) — `IRxMaterialInstance`'s three setters had no `try/catch`

`setFloat`/`setFloat4`/`setTexture` called `material_->paramKind(name)` (an
implicit `const char*` → `std::string` conversion) and
`params_[name] = param` (`std::unordered_map::operator[]`, which can
rehash/allocate) with no boundary `try/catch` — a real, if narrow, path for
`std::bad_alloc` to cross the ABI uncaught, contradicting the file's own
header comment claiming universal coverage. Fixed: each of the three setters
now wraps its body (after the null-argument checks, which need no exception
handling at all) in `try/catch (const std::exception&)` mapping to
`RX_E_FAIL`, matching the factory/`loadMaterial`/`createInstance` pattern
exactly.

While fixing `setTexture` specifically, also corrected a related ordering
issue the try/catch alone would not have caught: the original code called
`texture->addRef()` *before* `params_[name] = param`, so a hypothetical
`bad_alloc` from the map write (now caught, mapped to `RX_E_FAIL`) would
still have leaked one reference on `texture` (addRef'd, never stored, never
released). The fixed version captures whatever the name previously held,
performs the (possibly-throwing) map write first, and only THEN calls
`texture->addRef()`/`previousTexture->release()` — so a caught exception on
that path now leaves every refcount exactly as it was on entry, not just
"doesn't crash."

**Full audit of every remaining entry point (per the coordinator's
instruction to re-audit, not just fix the flagged three):**

| Entry point | Can throw? | Verdict |
|---|---|---|
| `rxCreateMaterialSystem` (factory) | `new MaterialSystemImpl` (`bad_alloc`) | Already wrapped in `try/catch` — unchanged. |
| `MaterialSystemImpl::queryInterface` | `memcmp`, pointer assignment, `addRef()` (atomic) | None of these throw; no catch needed. |
| `MaterialSystemImpl::loadMaterial` | `internal_->loadMaterial()`, `reflectMaterialParams()`, `new MaterialImpl`, string/map ops | Already wrapped — unchanged (see F2 for the reorder inside the same try). |
| `MaterialSystemImpl::reloadChanged` | Nothing — unconditional `return RX_OK` | No catch needed. |
| `MaterialImpl::queryInterface` | Same as above | No catch needed. |
| `MaterialImpl::createInstance` | `new MaterialInstanceImpl` (`bad_alloc`) | Already wrapped — unchanged. |
| `MaterialImpl::name()` | `name_.c_str()` (`noexcept`) | No catch needed. |
| `MaterialInstanceImpl::queryInterface` | Same as above | No catch needed. |
| `MaterialInstanceImpl::setFloat/setFloat4/setTexture` | `std::string` construction, `unordered_map::operator[]` | **Fixed this round** (see above). |
| `RxUnknownBase::addRef`/`release` | Atomic ops only; `delete` on a destructor that itself only calls atomic ops / other `release()`s / STL destructors, none of which throw | No catch needed — and cannot meaningfully catch inside a destructor-driven `delete` anyway. |

Result: every `RxResult`-returning virtual method reachable from an
`IRx*`-typed pointer now either provably cannot throw (verified above, not
assumed) or is wrapped in the same catch-and-map pattern. No further gap
found.

### F2 (Medium) — orphaned internal state on a specific partial-failure path

`MaterialSystemImpl::loadMaterial()` called the expensive, stateful
`internal_->loadMaterial()` (creates a real `VkDescriptorSetLayout`/
`VkPipelineLayout`, registers a `MaterialRecord`) *before* the cheap,
side-effect-free reflection-only pass. A reflection failure after a
successful internal load permanently orphaned the just-created internal
record (Task 5's `MaterialSystem` has no unload path).

**Fix applied: reordered, did not add unwind logic.** The reflection-only
pass now runs first; `internal_->loadMaterial()` is only called once
reflection has already succeeded. This is the reorder the review itself
suggested as preferable ("reordering... would eliminate nearly all of this
risk without requiring any change to Task 5's surface"), and it was possible
because `reflectMaterialParams()` never depended on `internal_` at all — it
independently re-reads and composes the same file. No unwind/release logic
was needed or added, since after the reorder there is no longer a window
where the internal, stateful call has already succeeded but the method can
still fail for an unrelated reason. (The narrow residual case — reflection
succeeds, then `internal_->loadMaterial()` itself throws — is not a new
orphan: Task 5's own `MaterialSystem::loadMaterial()` never registers a
`MaterialRecord` before it successfully finishes, per `material_system.cpp`'s
own exception-safety, so a throw there still orphans nothing.)

### F3 (Medium) — session-configuration divergence, and an inaccurate report claim

Confirmed and fixed. The authoritative session (`MaterialSystem::create()`)
sets an explicit SPIR-V capability floor (`spirv_1_3`) via
`targetDesc.compilerOptionEntries`; the reflection-only session
(`reflectMaterialParams()`) omitted it. Fixed by copying the identical
`slang::CompilerOptionEntry`/`findCapability("spirv_1_3")` setup into
`reflectMaterialParams()`, field for field.

**Report claim corrected:** the original report stated the two sessions
share "same session config" — false at the time it was written (this exact
gap existed). Corrected in the "What Task 7 must pick up" section above.
**Remaining intentional divergences, listed explicitly (there are none at
the `TargetDesc`/`SessionDesc` field level after this fix):** the two
sessions are necessarily separate `slang::ISession`/`slang::IGlobalSession`
*instances* — `MaterialSystemImpl::reflectionGlobalSession_` is a distinct
object from whatever global/session `rx::material::MaterialSystem::Impl`
privately owns, because `api_impl.cpp` has no access to that private state
(see this file's own top comment). That is a structural consequence of the
two-session design already disclosed in the original report, not a
configuration difference — every field on both `TargetDesc`s
(`format`, `profile`, `compilerOptionEntries`/`compilerOptionEntryCount`) and
both `SessionDesc`s (`targets`/`targetCount`, `searchPaths`/
`searchPathCount`) is now identical.

### F4 (Low) — TDD evidence was narrative-only

For Task 6's *original* submission: confirmed the review's characterization
is accurate — no red-run transcript was captured. The actual red state at
that time was a *build* failure, not a runtime test failure: `rx_api.h`/
`api_impl.cpp` did not exist yet when `test_api_contract.cpp`/
`test_api_factory.cpp` were first written, so the test binaries simply
failed to compile/link until the implementation existed. That transcript was
not saved.

For *this* fix round, real transcripts were captured both before and after
the F1/F2/F3 code changes (both new/extended tests were added first, then
run against the pre-fix `api_impl.cpp`, then run again after). Honest
finding: **both runs pass identically** — the two new tests are explicitly
"cheap structural guard" tests (per the coordinator's own instruction that
forcing `bad_alloc` injection to get a true red run "is not worth
scaffolding"), and none of the inputs exercised (real names, unknown names,
empty string, a 4096-byte name, null arguments) actually trigger an
allocation failure under normal conditions — so there is no runtime-observable
difference between the try/catch-absent and try/catch-present versions for
any input short of genuine OOM. This is disclosed plainly rather than
implied as a red→green fix: these two tests are regression guards for
future changes and for the ordering fix inside `setTexture`, not proof that
F1 was previously reachable through them.

Before (pre-fix, `rx_material_tests`, `--` device-free entry-point audit
included):
```
[doctest] test cases: 10 | 10 passed | 0 failed | 0 skipped
[doctest] assertions: 50 | 50 passed | 0 failed |
[doctest] Status: SUCCESS!
```
After (post-fix, identical test binary, `api_impl.cpp` now has the F1/F2/F3
changes):
```
[doctest] test cases: 10 | 10 passed | 0 failed | 0 skipped
[doctest] assertions: 50 | 50 passed | 0 failed |
[doctest] Status: SUCCESS!
```
Before (pre-fix, `rx_material_gpu_tests --validate`):
```
[doctest] test cases:  10 |  10 passed | 0 failed | 0 skipped
[doctest] assertions: 143 | 143 passed | 0 failed |
[doctest] Status: SUCCESS!
```
After (post-fix, identical test binary):
```
[doctest] test cases:  10 |  10 passed | 0 failed | 0 skipped
[doctest] assertions: 143 | 143 passed | 0 failed |
[doctest] Status: SUCCESS!
```

### Tests added/extended this round

- `test_api_contract.cpp`: new `isDocumentedResult()` helper +
  "entry-point audit" `TEST_CASE` — every `RxResult`-returning method
  reachable from a null-internal (device-free) `IRxMaterialSystem`
  (`queryInterface`, `loadMaterial`, `reloadChanged`) across normal/edge/
  malformed inputs, asserting the return value is always a documented
  `RxResult` (i.e. the call returned normally at all — doctest's own
  exception trapping would fail the test if it didn't).
- `test_api_factory.cpp`: new "entry-point audit" `TEST_CASE` — the same
  guard for `setFloat`/`setFloat4`/`setTexture` against a **real**
  `IRxMaterialInstance` (a null-internal system can never produce one — see
  below), sweeping a real name, an unknown name, an empty-string name, a
  4096-byte name, and null name/value/texture arguments. Extended the
  existing bad-syntax `loadMaterial` test with a
  `rx::material::detail::debugLiveApiObjectCount()` before/after assertion
  proving a failed load leaves no orphaned `MaterialImpl` API object behind
  (see F2's own writeup above for exactly what this does and does not
  prove about the *internal* `MaterialRecord`).
- **Deviation from the literal ask, stated plainly:** the coordinator asked
  for the setter audit to run "on a null-internal system object." That is
  architecturally impossible: an `IRxMaterialInstance` can only ever be
  produced by `IRxMaterial::createInstance()`, and an `IRxMaterial` can only
  ever be produced by `IRxMaterialSystem::loadMaterial()` succeeding, which
  requires `internal_ != nullptr` and a real `internal_->loadMaterial()`
  success against a real `VkDevice` — a null-internal system's
  `loadMaterial()` always returns `RX_E_FAIL` before constructing anything.
  The setter audit therefore runs against a real, device-backed instance in
  `test_api_factory.cpp` instead; the device-free file audits every entry
  point that genuinely is reachable without a device.

### Re-verification after all four fixes

- `ctest --preset linux-native -R rx_material --output-on-failure`: 2/2
  passed (`rx_material_tests`: 10 cases/50 assertions;
  `rx_material_gpu_tests`: 10 cases/143 assertions).
- Full regression: `ctest --preset linux-native --output-on-failure` — 14/14
  passed.
- Both presets rebuilt clean: `cmake --build build/linux-native` and
  `cmake --build build/windows-cross-zig`, targets `rx_material`,
  `rx_material_tests`, `rx_material_gpu_tests` — zero errors.
- Manual warning re-check (same method as the original submission —
  extracted each changed file's real `compile_commands.json` invocation, ran
  it with `-Wall -Wextra -Wpedantic -Wshadow` appended): zero warnings on
  `api_impl.cpp`, `test_api_contract.cpp`, `test_api_factory.cpp`,
  `test_api_header_self_contained.cpp`.
- AI-attribution grep on every changed file: zero matches.

### Files touched this round

- `src/rx_material/api_impl.cpp` (F1: try/catch on the three setters + the
  `setTexture` refcount-ordering fix; F2: reorder in `loadMaterial`; F3:
  capability-floor alignment in `reflectMaterialParams`).
- `src/rx_material/tests/test_api_contract.cpp` (new entry-point audit test
  + `isDocumentedResult()` helper).
- `src/rx_material/tests/test_api_factory.cpp` (new entry-point audit test
  for the setters, `isDocumentedResult()` helper, extended the bad-syntax
  test with the live-object-count assertion, added the
  `rx_api_detail.h` include).
- This report (`task-6-report.md`) — this Fix round 1 section, plus the
  correction to the "same session config" claim under "What Task 7 must
  pick up," item 2.
