### Task 6: COM-lite public surface (rx_api.h)

**Files:**
- Create: `src/rx_material/include/rx_material/rx_api.h` (the ONLY header a public consumer needs; self-contained: no rx_*, STL, or Vulkan includes — C types + pure-virtual interfaces only), `src/rx_material/api_impl.cpp`
- Create: `src/rx_material/tests/test_api_contract.cpp` (device-free target where possible; factory tests in the GPU target)
- Modify: `src/rx_material/CMakeLists.txt`

**Interfaces (produces — the ABI surface, D5; mirror Slang's `ISlangUnknown` shape [R:M§1.5]):**

```cpp
// rx_api.h — ABI rules (D5, R:M§1.3): pure-virtual single-inheritance only, no overloads,
// no data members, no exceptions/RTTI/STL across the boundary, renderer-side allocation only,
// explicit GUID per interface version, POD structs static_assert-pinned.
#include <cstdint>
#define RX_CALL /* empty on x64; kept for documentation of the boundary */

typedef int32_t RxResult;
enum : RxResult { RX_OK = 0, RX_E_FAIL = -1, RX_E_INVALIDARG = -2, RX_E_NOTFOUND = -3,
                  RX_E_COMPILE = -4, RX_E_NOINTERFACE = -5 };
struct RxGuid { uint32_t data1; uint16_t data2, data3; uint8_t data4[8]; };

struct IRxUnknown {
    virtual RxResult RX_CALL queryInterface(const RxGuid& iid, void** outObject) = 0;
    virtual uint32_t RX_CALL addRef() = 0;
    virtual uint32_t RX_CALL release() = 0;
};
struct IRxTexture : IRxUnknown { /* opaque: wraps rhi::Texture2D + bindless handle */ };
struct IRxMaterialInstance : IRxUnknown {
    virtual RxResult RX_CALL setFloat(const char* name, float value) = 0;
    virtual RxResult RX_CALL setFloat4(const char* name, const float value[4]) = 0;
    virtual RxResult RX_CALL setTexture(const char* name, IRxTexture* texture) = 0;
};
struct IRxMaterial : IRxUnknown {
    virtual RxResult RX_CALL createInstance(IRxMaterialInstance** outInstance) = 0;
    virtual const char* RX_CALL name() = 0;                    // valid for material lifetime
};
struct IRxMaterialSystem : IRxUnknown {
    virtual RxResult RX_CALL loadMaterial(const char* slangModulePath, IRxMaterial** outMaterial) = 0;
    virtual RxResult RX_CALL reloadChanged() = 0;              // Task 7 wires this; declared now (GUID stability)
};
// In-process bridge factory (standalone-DLL packaging deferred, D5): desc carries
// pointers to live internal objects; a future rx.dll adds public device creation.
struct RxMaterialSystemDesc { void* internalMaterialSystem; };  // rx::material::MaterialSystem*
extern "C" RxResult rxCreateMaterialSystem(const RxMaterialSystemDesc* desc, IRxMaterialSystem** outSystem);
```

GUIDs: one `static constexpr RxGuid kIID_IRx...` per interface (generate with `uuidgen`, embed literal values). Implementation classes in `api_impl.cpp` hold `std::atomic<uint32_t>` refcounts; `queryInterface` supports each interface's own IID + `IRxUnknown` (COM identity rule: same object pointer for IRxUnknown from any interface of the object). Errors are codes; internal exceptions caught at the boundary and mapped (`RX_E_COMPILE` carries diagnostics via `spdlog` — no strings across ABI in Phase 3).

**Steps:**
- [ ] **1. Failing tests** (`test_api_contract.cpp`): QI-identity (IRxUnknown* from IRxMaterial == from its IRxMaterialInstance's parent? NO — identity is per-object: assert IRxUnknown from the SAME object via different IIDs compares equal); refcount round-trip (create → addRef → release ×2 → destroyed exactly once, verified via instance counter for tests); unknown IID → `RX_E_NOINTERFACE` and `*outObject == nullptr`; null out-params → `RX_E_INVALIDARG`; `static_assert(sizeof(RxGuid) == 16)` and `sizeof(RxMaterialSystemDesc) == sizeof(void*)`; header self-containment (a test TU that includes ONLY rx_api.h and compiles). Factory + loadMaterial happy path in the GPU target using `test_unlit.slang`.
- [ ] **2. Verify failure. 3. Implement. 4. Green both presets.**
- [ ] **5. Commit** `feat: add COM-lite public material API surface`.

