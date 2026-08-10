# Task 6 review: COM-lite public surface (rx_api.h + api_impl.cpp)

**Commit reviewed:** `6c49910` (`feat: add COM-lite public material API surface`), diff range `2ead2d0..6c49910`.
**Reviewer method:** read brief/report/diff/research doc/spec D5 in full; re-derived every scrutiny point from the actual committed code (not the report's paraphrase); independently rebuilt both presets, re-ran `ctest`, recompiled every new file from scratch with `-Wall -Wextra -Wpedantic -Wshadow`, force-recompiled the header-self-containment TU, ran a standalone TSAN stress test of the exact refcount-release ordering pattern, checked GUID byte patterns for real-uuidgen shape, and grepped the actual commit diff for AI attribution.

---

## 1. Spec compliance (binding constraints, verified individually)

| # | Requirement | Verdict | Evidence |
|---|---|---|---|
| 1 | `rx_api.h` self-contained, `<cstdint>` only | ✅ | Only `#include <cstdint>` in the header; independently force-recompiled `test_api_header_self_contained.cpp` (includes nothing else) from scratch on linux-native — succeeds. |
| 2 | Pure-virtual, single inheritance rooted at `IRxUnknown` | ✅ | `IRxTexture/IRxMaterialInstance/IRxMaterial/IRxMaterialSystem : IRxUnknown`, every method `= 0`, no other base. |
| 3 | No overloaded method names | ✅ | Every interface's methods have distinct names; verified by inspection of all 5 interfaces. |
| 4 | No data members on interfaces | ✅ | All 5 interface `struct`s contain only virtual method declarations. |
| 5 | No virtual destructor on interfaces | ✅ | None of the 5 interfaces declare a destructor at all (implicit, non-virtual); `release()` plays that role per the header's own top comment. |
| 6 | GUID per interface, real `uuidgen` values (not placeholders) | ✅ | All 5 `kIID_*` constants decode as valid RFC4122 v4 UUIDs (version nibble `4`, variant nibble in `{8,9,a,b}` on every one), all 5 distinct — consistent with genuine `uuidgen` output, not hand-typed placeholders. |
| 7 | `extern "C"` factory | ✅ | `extern "C" RxResult rxCreateMaterialSystem(...)`. |
| 8 | POD boundary structs static_assert-pinned | ✅ | `RxGuid`: `sizeof==16`, `alignof==4` pinned in the header itself (not just tests); field layout (`u32+u16+u16+u8[8]`) mathematically forces zero padding at that exact size, so the memcmp path (below) is sound. `RxMaterialSystemDesc`: `sizeof==sizeof(void*)` pinned in the header. |
| 9 | Error codes never exceptions | ❌ | See Finding F1. `IRxMaterialInstance::setFloat/setFloat4/setTexture` have no `try/catch`, yet the implicit `const char*`→`std::string` conversion at the `material_->paramKind(name)` call site and `params_[name] = param` (`std::unordered_map::operator[]`) can throw `std::bad_alloc`, which would cross the ABI boundary uncaught. |
| 10 | Boundary entry points catch all internal exceptions, map to `RxResult` | ❌ | Same gap as #9 — the factory, `loadMaterial`, and `createInstance` all correctly wrap in `try/catch(const std::exception&)`; the three setters do not, despite `api_impl.cpp`'s own header comment claiming universal coverage ("every virtual method reachable from a caller holding only an IRx*-typed pointer"). |
| 11 | No STL/Vulkan/`rx_*` types in any signature | ✅ | Checked every declared method in `rx_api.h`: only `RxResult`, `uint32_t`, `const char*`, `float`, `const float[4]`, `IRx*` pointers, `const RxGuid&`, `void**`, `const RxMaterialSystemDesc*`. `internalMaterialSystem` is `void*`, never `rx::material::MaterialSystem*`. |
| 12 | COM identity: QI for `IRxUnknown` returns the same pointer as the object's own interface | ✅ | Every `queryInterface` impl returns `static_cast<Interface*>(this)` for both `kIID_IRxUnknown` and its own IID; single, non-virtual inheritance from one base guarantees identical addresses (no thunk adjustment possible). Exercised at both the `IRxMaterialSystem` and `IRxMaterialInstance` levels in the test suite; both pass. |
| 13 | Refcounts atomic | ✅ | `std::atomic<uint32_t> refCount_`; live-object debug counter is `std::atomic<uint64_t>`. |
| 14 | `reloadChanged()` declared, documented no-op → `RX_OK`, references Task 7 | ✅ | Declared in `rx_api.h` with a comment pointing at Task 7; `api_impl.cpp`'s implementation is an unconditional `return RX_OK;` with a comment detailing exactly what Task 7 must wire, and explicitly never touches `internal_` (so it stays valid even on a device-free instance). |
| 15 | Setters validate name→`RX_E_NOTFOUND`, type→`RX_E_INVALIDARG`, store into an instance-owned CPU blob | ✅ | `MaterialInstanceImpl::params_` (a `std::unordered_map<std::string, StoredParam>` member) is instance-owned; all three setters check existence then kind, in that order, matching the contract exactly. Exercised against two real materials (`float4`, `uint`) in `test_api_factory.cpp`. |
| 16 | No AI attribution | ✅ | `git show 6c49910` — commit message and full diff grepped for `anthropic\|claude\|co-authored\|chatgpt\|gpt-4\|openai\|ai-generated` etc.: zero matches. Author is `Yousef Wadi <ywadi85@gmail.com>`. |
| 17 | Production grade | ⚠️ | Mostly yes (see quality section) — F1/F2 are real production-quality gaps, not merely style. |
| 18 | Both presets green | ✅ | Independently rebuilt: `linux-native` — `rx_material_tests`/`rx_material_gpu_tests` both **pass** via `ctest`; `windows-cross-zig` — both targets build and link to `.exe` cleanly (no Wine device available to execute, consistent with this repo's existing CI exclusion pattern for GPU-backed Windows tests). |
| 19 | TDD evidence | ⚠️ | See Finding F4 — final-state evidence (test counts, assertions) is thorough and independently reproduced; the report doesn't transcribe an actual red run. |

**Overall spec verdict: ❌** — one explicitly-listed binding rule ("error codes never exceptions" / "boundary entry points catch all internal exceptions") is violated by three of the ABI's own public virtual methods. Every other itemized requirement is met, most with strong, independently-reproduced evidence.

---

## 2. Scrutiny points — detailed findings

### 2.1 Refcount release ordering (highest-scrutiny item)

```cpp
uint32_t RX_CALL addRef() override { return refCount_.fetch_add(1, std::memory_order_relaxed) + 1; }
uint32_t RX_CALL release() override {
    uint32_t remaining = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) { delete static_cast<Derived*>(this); }
    return remaining;
}
```

**Correct.** This is not the "relaxed fetch_sub" failure mode the brief warned about — `release()`'s decrement uses `memory_order_acq_rel`, which is *stronger* than the canonical `release` + conditional `acquire` fence pattern (it synchronizes on every call, not just the terminal one). Since RMW operations on one atomic always read the immediately-preceding value in that atomic's modification order, the last `release()` (the one that observes `remaining == 0`) necessarily synchronizes-with every prior `release()` on the same object, making all of thread A's writes before its `release()` visible to thread B's `delete` after B's `release()`. `addRef()`'s `relaxed` add is fine (doesn't publish new data, only needs atomicity).

Verified two ways:
1. Formal reasoning above (per the C++ memory model's synchronizes-with rule for RMW chains on one atomic).
2. An independent standalone repro of the exact pattern (`/tmp/.../scratchpad/refcount_hammer2.cpp`), run under ThreadSanitizer with ASLR disabled (`setarch -R`): 16 threads × 50,000 iterations × 200 rounds, each thread owning a disjoint memory slot it writes immediately before `release()`, with the object's destructor reading every thread's slot. **0 TSAN races reported, 200/200 objects destroyed exactly once, 0 leaks.** (An earlier version of this repro that had multiple threads write one *shared* field concurrently did trip TSAN — correctly, since that is a genuine data race in the harness itself, unrelated to the refcount ordering; the corrected version isolates the property actually under test.)

No finding here — this is the one scrutiny item that comes back clean.

### 2.2 Second, independent reflection-only Slang session for setter validation

**Ordering dependency, verified safe today:** `MaterialSystemImpl::loadMaterial()` calls the *authoritative* `internal_->loadMaterial(path)` (Task 5's `MaterialSystem`, which composes+links+builds the real pipeline layout against `forward_entry.slang`) **before** ever touching the reflection-only session. Since `internal_->loadMaterial()` throws on any shape violation (missing/duplicate `ParameterBlock`, wrong descriptor set) and that exception is caught by the enclosing `try`, the second session's blind "first `ParameterBlock`, take its fields, stop" logic only ever runs once the authoritative pass has already proven the material has exactly one valid `gParams` at set 1 — so the "does this material even have the right shape" question is never answered twice, only once, correctly.

**Genuine divergence found, contradicting the report:** the report claims the two sessions share "same session config." They do not. `MaterialSystem::create()` sets an explicit SPIR-V capability floor:
```cpp
capabilityEntry.name = slang::CompilerOptionName::Capability;
capabilityEntry.value.intValue0 = static_cast<int32_t>(spirvFloor);  // "spirv_1_3"
targetDesc.compilerOptionEntries = &capabilityEntry;
```
`reflectMaterialParams()`'s `targetDesc` in `api_impl.cpp` has no `compilerOptionEntries` at all. For the currently-supported field shapes (scalar `float`/`uint`, 4-wide `float` vector) this is very unlikely to change name/type classification — but it is a real, unacknowledged configuration gap between the session that will eventually drive GPU binding (Task 7) and the one answering "does parameter X exist, what type is it" today. See Finding F3.

**Field-shape agreement:** no evidence of disagreement was found for the two fixtures this task added/reused (`test_unlit.slang`'s `float4 tint`, `test_textured.slang`'s `uint albedoIndex`) — both are validated by NAME against real reflected data in `test_api_factory.cpp`, not a hand-maintained table, and both pass. Since `TParams` is a concrete (non-generic) struct never referenced by `forward_entry.slang`, its own field type layout should not depend on what else gets linked around it — this is a reasonable inference, not something either session cross-checks against the other today.

**Task 7 handoff note — adequately specific:** the report names the exact mechanism to add (`MaterialSystem` should expose a per-field name/type/set table computed once from `MaterialRecord::linkedProgram`, which it already retains) and why (removes the duplicate session, removes the divergence risk). It stops short of proposing a concrete method signature or return shape, but names the source field precisely enough that Task 7 doesn't have to rediscover it. Acceptable as a handoff note; not a blocking gap.

### 2.3 Null-internal factory policy

- **Documented in `rx_api.h` itself** (not just the report): the header's own comment on `RxMaterialSystemDesc` states plainly that a null `internalMaterialSystem` is legal, that `queryInterface`/`addRef`/`release`/`reloadChanged` all work normally, and that `loadMaterial()` alone returns `RX_E_FAIL`. ✅
- **Audited every entry point for a null-internal dereference:** `queryInterface` (all 3 impls) never touches `internal_`; `addRef`/`release` (shared base) never touch it; `reloadChanged()` never touches it (explicit no-op, comment confirms this is deliberate); `loadMaterial()` checks `internal_ == nullptr` and returns before any dereference; the factory itself only stores the pointer, never calls through it. **No unguarded dereference found.**
- **`RX_E_FAIL` vs. a more specific code:** defensible. The fixed error-code set (`RxResult`'s enum, specified verbatim in the task brief) has no "invalid object state" / "not configured" code distinct from `RX_E_FAIL`; `RX_E_INVALIDARG` would be a worse fit (the null pointer was supplied at construction, not to this call), so `RX_E_FAIL` is the best available choice given the enum as specified. Not a finding.

### 2.4 `queryInterface` GUID compare correctness

`guidEquals` does `std::memcmp(&a, &b, sizeof(RxGuid))`. Sound: `RxGuid`'s fields (`u32, u16, u16, u8[8]`) sum to exactly 16 bytes with every offset already satisfying its own natural alignment in declared order — the pinned `sizeof==16` static_assert is only satisfiable if there is zero padding anywhere in the struct, so `sizeof==16` is not just a size check here, it's a structural padding-freedom proof for this exact field list. Out-param nulling on the `RX_E_NOINTERFACE` path and `addRef()`-before-return on the success path are both present and correct in all three `queryInterface` implementations, and independently exercised by `test_api_contract.cpp`'s "poisoned pointer" test.

### 2.5 `setTexture` lifecycle

`MaterialInstanceImpl::setTexture` calls `releaseStoredTextureIfAny(name)` (releases the prior stored texture only if the prior stored value was itself `TextureIndex`-kind — correctly guarded, since `setFloat`/`setFloat4` cannot ever have been used to a store texture at that name given the kind check) before `texture->addRef()` and storing. The destructor releases whatever remains. `test_api_factory.cpp`'s `FakeTexture` double exercises **all three transitions** with explicit refcount assertions, not just "it didn't crash": addRef on store (`refCount 1→2`), release-of-previous + addRef-of-new on overwrite (`first: 2→1`, `second: 1→2`), and release on instance destruction (`second: 2→1`). Confirmed correct.

### 2.6 Header self-containment test

Real, not just claimed. `test_api_header_self_contained.cpp` is wired into `add_executable(rx_material_tests ...)` in `tests/CMakeLists.txt`, contains `#include <rx_material/rx_api.h>` and nothing else, and independently:
- Extracted its exact `compile_commands.json` command (only `-I` propagation from CMake target linkage, no `-include`/force-include flags) and force-recompiled it from scratch on `linux-native` — succeeds.
- Ran the full `rx_material` test suite via `ctest` — both binaries pass (2/2).
- Rebuilt the same targets on `windows-cross-zig` — both link cleanly to `.exe`.

### 2.7 `final`-marking / non-virtual-destructor safety

`MaterialInstanceImpl`, `MaterialImpl`, `MaterialSystemImpl` are all `final`, so nothing can derive further and reach the non-virtual-destructor hazard through them. `RxUnknownBase<Derived, Interface>` (the one non-final class in the chain) declares its destructor `protected`, not public — so even that intermediate base cannot be `delete`d through a base-typed pointer from outside the hierarchy; it isn't exposed to any consumer anyway (anonymous-namespace-private to `api_impl.cpp`). Every `release()` destroys via `delete static_cast<Derived*>(this)` (the exact static/concrete type), never a base-typed `delete` — so correctness never actually depends on virtual dispatch. The residual risk (a third-party ABI consumer calling `delete` directly on an `IRx*` pointer instead of `release()`) is the documented, expected COM-lite contract violation every precedent in the research doc (COM, Slang, chadaustin.me) accepts identically — not something Task 6 introduced. No finding.

---

## 3. Findings by severity

**F1 — High. Exception-safety gap on `IRxMaterialInstance`'s three setters.**
`setFloat`/`setFloat4`/`setTexture` in `api_impl.cpp` have no `try/catch`. The implicit `const char*` → `std::string` conversion at `material_->paramKind(name)` and `params_[name] = param` (`std::unordered_map::operator[]`, which can rehash/allocate) can both throw `std::bad_alloc`, which would then propagate directly across the ABI boundary — the exact cross-toolchain-termination failure mode D5/the research doc calls out, and a direct violation of "error codes never exceptions." Contradicts `api_impl.cpp`'s own header comment claiming *every* virtual method reachable from an `IRx*`-typed pointer follows the catch-and-map contract; the factory, `loadMaterial`, and `createInstance` all correctly do this, the three setters do not. Fix: wrap each setter body (after the null-argument checks) in `try/catch (const std::exception&)` mapping to `RX_E_FAIL`, matching the existing pattern.

**F2 — Medium. Orphaned internal state on a specific partial-failure path.**
`MaterialSystemImpl::loadMaterial()` calls the expensive, stateful `internal_->loadMaterial()` (which creates a real `VkDescriptorSetLayout`/`VkPipelineLayout` and registers a `MaterialRecord`) **before** running the reflection-only second session. If `ensureReflectionGlobalSession()`/`reflectMaterialParams()` subsequently fails, the method returns `RX_E_COMPILE` but the just-created internal record and its GPU resources are permanently orphaned (unreachable — no `MaterialImpl` is ever created to wrap it), freed only when the whole `internal_` `MaterialSystem` is eventually torn down. Task 5's `MaterialSystem` exposes no unload/release path, so there's no way to fully undo this today — but reordering the two passes (cheap, side-effect-free reflection first; the authoritative, stateful call only once that succeeds) would eliminate nearly all of this risk without requiring any change to Task 5's surface. Narrow trigger (requires an infra-level failure on the *second* session specifically, e.g. `slang::createGlobalSession()` failing under real OOM), bounded, non-crashing — hence Medium, not Critical.

**F3 — Medium. Session-configuration divergence, and an inaccurate report claim.**
The report states the two Slang sessions backing setter validation share "same session config." They don't: the authoritative session (`MaterialSystem::create()`) sets an explicit SPIR-V capability floor (`spirv_1_3`) via `targetDesc.compilerOptionEntries`; the reflection-only session (`reflectMaterialParams()`) omits it. Unlikely to change classification for the currently-supported field shapes (scalar float/uint, float4), but it is a real, unacknowledged divergence between the session that will eventually govern GPU binding (Task 7) and the one answering the name/type question today. Worth closing (trivially — copy the same `compilerOptionEntries` setup) or at minimum correcting the report's parity claim.

**F4 — Low. TDD evidence is narrative, not transcribed.**
The report describes following the brief's failing-tests → implement → green sequence and documents thorough final-state results (exact test/assertion counts, which cases exercise which contract), independently reproduced by this review. It does not show or quote an actual red run (e.g., "N cases failing" before implementation) — the red-state claim is asserted, not evidenced. Downgraded to Low given the strength of the final-state evidence.

**Informational (not counted as a finding):** `classifyFieldType()` classifies every plain `uint` field as `TextureIndex` with no `Unsupported` fallback for a hypothetical non-bindless `uint` (e.g. a flags/count field). This is D8's own documented Phase-3 scope (materials never put non-bindless-index `uint`s in `TParams`), not something Task 6 introduced or could have avoided within its own scope.

---

## 4. Independent verification performed (beyond reading the diff)

- Rebuilt `rx_material_tests`/`rx_material_gpu_tests` on `linux-native`; ran `ctest -R rx_material` — **2/2 passed**.
- Force-touched and recompiled `test_api_header_self_contained.cpp` from a clean object to confirm it's a genuine, currently-passing compile, not a stale artifact.
- Rebuilt both `rx_material_tests`/`rx_material_gpu_tests` on `windows-cross-zig` — both link to `.exe` cleanly.
- Recompiled `api_impl.cpp` and all three new test `.cpp` files with `-Wall -Wextra -Wpedantic -Wshadow` appended to their real `compile_commands.json` invocations — **zero warnings**, matching the report's claim.
- Wrote and ran a standalone ThreadSanitizer stress test of the exact `RxUnknownBase::addRef()/release()` ordering pattern (16 threads, 50,000 iters × 200 rounds) — **0 races, 0 leaks, exactly 200/200 correct single destructions**.
- Decoded all 5 embedded GUIDs' version/variant nibbles to confirm genuine-`uuidgen`-shaped v4 UUIDs, and confirmed all 5 are distinct.
- `git show 6c49910`, full diff and message, grepped for AI-attribution patterns — zero matches; author is the user's own identity.

---

## 5. Quality verdict

**4 findings — 1 High, 2 Medium, 1 Low.** Not approved as-is; the High finding (F1) is a direct, code-verifiable violation of one of this task's own binding ABI rules and should be fixed before sign-off (small, mechanical fix — wrap three method bodies in the same try/catch pattern already used four times elsewhere in the same file). F2/F3 are real but narrow-trigger robustness/consistency gaps worth addressing or explicitly re-flagging for Task 7 rather than leaving silently. F4 is a process/evidence nit. Everything else — the refcount ordering (scrutinized hardest, confirmed correct both analytically and empirically under TSAN), the COM identity rule, GUID/memcmp soundness, `setTexture` lifecycle, header self-containment, the `final`/non-virtual-destructor safety chain, the null-internal audit, and the AI-attribution check — is solid, well-tested, and independently reproduced.

---

## 6. Re-review — fix round 1 (`6c49910..0b0baab`, fix commit `0b0baab`)

**Scope:** the coordinator's fix diff spans three commits (`fc6e626`, `1f70719` — coordinator docs-only, unrelated to this task, out of scope per the coordinator's own instruction) and `0b0baab`, the actual fix. `git show --stat 0b0baab` confirms it touches exactly four files: `api_impl.cpp`, `test_api_contract.cpp`, `test_api_factory.cpp`, `task-6-report.md` — no docs/spec files, no unrelated code. **No scope creep.**

Re-verified independently (not just re-read the report): rebuilt `rx_material`/`rx_material_tests`/`rx_material_gpu_tests` from a clean touch on `linux-native` (real recompile, not a no-op), ran the full suite, ran full repo regression, recompiled every changed file with `-Wall -Wextra -Wpedantic -Wshadow`, rebuilt both test targets on `windows-cross-zig`, re-read `material_system.cpp`'s registration logic directly, and re-grepped the fix commit for AI attribution.

### F1 (High) — exception safety on the three setters: **CLOSED**

Confirmed in the actual diff, not just the report's description: `setFloat`/`setFloat4`/`setTexture` each now wrap their body (after the null-argument early-outs, correctly left outside the `try`) in `try/catch (const std::exception&) { ...; return RX_E_FAIL; }`, identical in shape to the existing `createInstance`/`loadMaterial`/factory pattern. `grep -n "try {\|catch ("` against the current `api_impl.cpp` confirms every method that can throw is now covered and every method left uncovered (`queryInterface` ×3, `name()`, `reloadChanged`, `addRef`/`release`) provably cannot throw (`memcmp`/pointer ops/atomics/`noexcept` `c_str()` only) — matches the report's audit table exactly, independently re-derived.

`setTexture`'s reorder (capture `previousTexture` → write `params_[name]` first → `addRef()` the new texture → `release()` the previous one, all *after* the possibly-throwing map write) is correctly exception-safe: if the map write throws, neither refcount has been touched yet, so a caught failure leaves every refcount exactly as it was on entry. **Bonus, unflagged-but-now-fixed latent bug:** the *old* code's `releaseStoredTextureIfAny()`-then-`addRef()` ordering had a real use-after-free on `setTexture(name, tex)` called twice with the *same* `tex` pointer when that pointer's refcount was exactly 1 (solely owned by the stored map entry) — the old code would `release()` it to 0 (destroying it) *before* the subsequent `addRef()` touches now-freed memory. The new write-before-addRef-before-release ordering closes this too, as a byproduct, without weakening anything F1 asked for.

The new "entry-point audit" test in `test_api_contract.cpp` covers every `RxResult` method reachable on a device-free (null-internal) system across normal/edge/malformed inputs and asserts `isDocumentedResult()` on each — since doctest's default exception trapping fails a `TEST_CASE` if an unhandled exception escapes it, this genuinely would have caught a regression, not just "didn't crash the process." The setters are *not* reachable on a null-internal system (an `IRxMaterialInstance` requires a real, device-backed `IRxMaterial`) — the implementer disclosed this architectural fact plainly rather than silently skipping it, and put the equivalent setter audit in `test_api_factory.cpp` against a real instance instead (sweeping a real name, unknown name, empty string, a 4096-byte name, and null args on all three setters). This is the correct, honest way to satisfy the coordinator's underlying intent given the null-internal constraint is a hard architectural fact, not a shortcut.

Independently re-verified: rebuilt from a clean touch, `rx_material_tests` **10 cases/50 assertions**, `rx_material_gpu_tests` **10 cases/143 assertions**, both **100% pass** — exact match to the report's claimed counts, not just trusted.

### F2 (Medium) — orphaned internal state on partial-failure: **CLOSED**

Confirmed the reorder in the actual code: `MaterialSystemImpl::loadMaterial()` now runs `ensureReflectionGlobalSession()`/`reflectMaterialParams()` **before** `internal_->loadMaterial(path)`. Independently re-read `material_system.cpp` (lines 762-801, unchanged by this fix — Task 5 code) to verify the report's load-bearing claim: `impl.materials.acquire(std::move(record))` — the only statement that actually makes a `MaterialRecord` reachable via `MaterialHandle` — is the *last* statement in `MaterialSystem::loadMaterial()`, after every possible throw point (session/module/compose/link/layout/reflect/`PipelineLayoutBuilder::build`/both codegen calls/both `vkCreateShaderModule` calls). This confirms, independently, that Task 5's `loadMaterial()` is genuinely all-or-nothing with respect to its registry — so the reorder does fully close the specific orphan window F2 identified: reflection now either fails before `internal_` is touched at all (nothing to orphan), or succeeds and `internal_->loadMaterial()` runs exactly once, and if *that* throws, Task 5's own code already guarantees nothing was registered.

The implementer's claim that a direct orphan-count assertion "wasn't possible" holds: `material_system.h` exposes only `detail::debugCompileCount()` (a compile-*attempt* counter, incremented regardless of success/failure — grepped and confirmed it cannot distinguish "orphaned" from "used") and no accessor for the registry's live size. The `debugLiveApiObjectCount()` seam that *is* used in the new bad-syntax regression test operates one layer up (Task 6's own `IRxUnknown`-rooted wrapper objects, not Task 5's internal `MaterialRecord`s) — the report discloses this boundary explicitly and correctly rather than overselling what the assertion proves. Given no lower-layer oracle exists without a Task 5 change (out of this task's scope), this is the correct, honestly-scoped test given what's actually observable today.

*Noted but explicitly out of scope for this re-review:* whether `PipelineLayoutBuilder::build()`'s returned descriptor/pipeline layout is itself cleaned up on the (pre-existing, Task-5-owned, unchanged-by-this-fix) codegen-failure unwind path in `material_system.cpp` is a question about code this fix diff does not touch — flagged here only for completeness, not as an open item against this fix round.

### F3 (Medium) — session-config divergence + inaccurate report claim: **CLOSED**

Confirmed field-for-field: `reflectMaterialParams()` now builds the identical `slang::CompilerOptionEntry`/`findCapability("spirv_1_3")` block `MaterialSystem::create()` uses, in the same place in the function, before `sessionDesc`/`createSession()`. The local-variable lifetime (`capabilityEntry`/`targetDesc` both stack-scoped for the duration of the one function call that uses them) is safe, matching the original's own pattern. The report's "What Task 7 must pick up" item 2 is corrected in place with an explicit strikethrough-style note ("this item originally claimed 'same session config' as already true; it was not... until the fix below") rather than silently rewritten — and it explicitly enumerates the one remaining, *structural* (not configuration) divergence: the two sessions are necessarily separate `ISession`/`IGlobalSession` instances, since `api_impl.cpp` cannot reach `MaterialSystem::Impl`'s private session. That's accurate and was already disclosed in the original report; nothing new to flag.

### F4 (Low) — TDD evidence honesty: **ACCEPTABLE, closed**

The report does not manufacture a fake red run. It states plainly that both new/extended tests pass identically before and after the F1-F3 code changes, explains precisely why (the swept inputs — real/unknown/empty/4096-byte names, null args — don't trigger `bad_alloc` under normal conditions, and fault-injection scaffolding to force one was explicitly out of scope per the coordinator's own instruction), and characterizes the new tests correctly as regression guards rather than proof-of-original-bug. This is the right call: honest disclosure of what the evidence does and doesn't show is worth more than a contrived red run, and matches this project's own "verify, don't overclaim" standard. Independently re-ran both binaries at the current commit and got the exact counts quoted (50/50, 143/143) — the transcripts in the report are real, not fabricated.

### New defects introduced by the fix: none found

Reviewed every changed line in `api_impl.cpp`/both test files for new bugs (self-overwrite refcount edge case in `setTexture` — traced through by hand, correct; loop-based setter audit against `test_unlit.slang`'s single `tint : float4` field — traced through by hand, `texture` never actually stored so its refcount stays untouched at 1 until the test's own `release()`, no leak/double-free; null-argument checks correctly left outside the new `try` blocks). Full regression (`ctest`, 14/14), both presets rebuilt clean from a forced touch (not a cached no-op), zero warnings under `-Wall -Wextra -Wpedantic -Wshadow` on every changed file, zero AI-attribution matches in the fix commit's message and diff, correct author identity.

### Re-review verdict

**All four findings addressed.** F1/F2/F3 fixed at the code level and independently verified (not just re-read); F4 resolved by honest disclosure rather than theater. No new defects, no scope creep in `0b0baab`.
