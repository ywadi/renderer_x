#pragma once
// Internal (non-ABI) seam for rx_api.h's COM-lite implementation objects
// (api_impl.cpp) -- explicitly NOT part of the ABI surface: never included
// by rx_api.h itself, never crosses the boundary, and names no rx_api.h
// type in a way that would make it part of that contract. Mirrors
// material_system.h's own `detail::debugCompileCount()` carve-out (see
// that header's comment for the same rationale).
//
// Two different kinds of seam live here:
//   1. debugLiveApiObjectCount() -- genuinely test-only, exposed purely so
//      test_api_contract.cpp's refcount-round-trip case can assert that a
//      COM-lite object is destroyed exactly once when its refcount reaches
//      zero (there is no other way to observe object lifetime from outside
//      api_impl.cpp's own translation unit).
//   2. materialHandle()/materialInstanceBlobData()/materialInstanceBlobSize()
//      [Task 7] -- a real, production draw-time BRIDGE, not a test seam:
//      MaterialSystem::getPipeline()/bindInstance() are deliberately NOT
//      part of the ABI surface this phase (D5/D11 -- draw submission is
//      not public in Phase 3), yet a caller driving an actual draw (sample
//      06, Task 8) still needs to get from an IRxMaterial*/
//      IRxMaterialInstance* it obtained entirely through rx_api.h to the
//      internal rx::material::MaterialHandle / raw parameter-blob bytes
//      those two internal methods need. This header is that bridge: a
//      caller that already holds the same internal
//      rx::material::MaterialSystem* it passed via
//      RxMaterialSystemDesc::internalMaterialSystem uses these three
//      functions to build a `rx::material::InstanceBinding` and call
//      `bindInstance()` directly. Safe precisely because every
//      IRxMaterial*/IRxMaterialInstance* reachable at all was created by
//      THIS SAME api_impl.cpp (there is no other factory for either) --
//      the unchecked downcast inside each function's implementation
//      relies on that invariant, not on any caller-supplied guarantee.
#include <cstddef>
#include <cstdint>

#include <rx_material/material_system.h>
#include <rx_material/rx_api.h>

namespace rx::material::detail {

// Number of live IRxUnknown-rooted API objects (MaterialSystemImpl,
// MaterialImpl, MaterialInstanceImpl, TextureImpl) this process has
// constructed but not yet destroyed. Incremented in RxUnknownBase's
// constructor, decremented in its destructor -- i.e. this tracks
// construction/destruction pairs, not refcounts themselves.
[[nodiscard]] uint64_t debugLiveApiObjectCount();

// [Task 7] `material`'s internal rx::material::MaterialHandle -- the
// value MaterialSystem::getPipeline()/bindInstance()'s own
// InstanceBinding::material field needs. `material` must be a real,
// non-null IRxMaterial obtained from IRxMaterialSystem::loadMaterial().
[[nodiscard]] rx::material::MaterialHandle materialHandle(IRxMaterial* material);

// [Task 7] `instance`'s raw CPU-side parameter blob -- exactly
// `materialInstanceBlobSize(instance)` bytes, laid out per the owning
// material's own MaterialSystem::materialParams() offsets (setFloat/
// setFloat4/setTexture write directly into these bytes). Together these
// two accessors are exactly what MaterialSystem::InstanceBinding::
// paramData/paramSize need. `instance` must be a real, non-null
// IRxMaterialInstance obtained from IRxMaterial::createInstance(). The
// returned pointer is valid only as long as `instance` itself has not
// been released and no further setFloat/setFloat4/setTexture call has
// resized it (never happens today -- the blob is sized once, at
// construction, to the material's own fixed paramBlockSize() -- but
// stated here for the caller's own safety margin) -- callers should copy
// out of it (or call bindInstance() with it) immediately, not retain it.
[[nodiscard]] const void* materialInstanceBlobData(IRxMaterialInstance* instance);
[[nodiscard]] size_t materialInstanceBlobSize(IRxMaterialInstance* instance);

}  // namespace rx::material::detail
