#pragma once
// Test-only seam for rx_api.h's COM-lite implementation objects
// (api_impl.cpp) -- explicitly NOT part of the ABI surface: never
// included by rx_api.h itself, never crosses the boundary, and names no
// rx_api.h type. Mirrors material_system.h's own
// `detail::debugCompileCount()` carve-out (see that header's comment for
// the same rationale): exposed purely so test_api_contract.cpp's
// refcount-round-trip case can assert that a COM-lite object is
// destroyed exactly once when its refcount reaches zero -- there is no
// other way to observe object lifetime from outside api_impl.cpp's own
// translation unit (queryInterface/addRef/release only ever report
// refcounts, never "was delete actually called").
#include <cstdint>

namespace rx::material::detail {

// Number of live IRxUnknown-rooted API objects (MaterialSystemImpl,
// MaterialImpl, MaterialInstanceImpl) this process has constructed but
// not yet destroyed. Incremented in RxUnknownBase's constructor,
// decremented in its destructor -- i.e. this tracks construction/
// destruction pairs, not refcounts themselves.
[[nodiscard]] uint64_t debugLiveApiObjectCount();

}  // namespace rx::material::detail
