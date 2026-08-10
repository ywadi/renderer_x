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
   the actual SPIR-V" duplication risk (low today — same source bytes, same
   session config — but a real seam to close).
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
