// rx_api.h self-containment proof [Task 6, spec Phase 3 design D5, R:M
// §1.3 point 4]: this translation unit includes NOTHING else -- not
// <doctest/doctest.h>, not any rx_*/STL/Vulkan/Slang header. See rx_api.h's
// own top comment for the ABI rule this pins: the boundary header must be
// usable by a consumer that links against nothing this codebase itself
// depends on. A bug that made rx_api.h implicitly rely on some OTHER
// header having been included first (a std:: type used without the
// header that declares it, say) would fail to compile HERE even though
// it might accidentally keep compiling in every other .cpp in this test
// binary (which all include <doctest/doctest.h> and other headers
// first, silently supplying whatever was missing). Successful
// compilation of this one file into an object IS the test; there is
// nothing further to assert at runtime, so it deliberately contributes
// no TEST_CASE and needs no doctest include of its own.
#include <rx_material/rx_api.h>

static_assert(sizeof(RxGuid) == 16, "rx_api.h's own pinned RxGuid size -- re-checked here so this self-containment "
                                     "TU actually exercises the type, not just names it");
static_assert(alignof(RxGuid) == 4, "rx_api.h's own pinned RxGuid alignment -- re-checked here for the same reason");
static_assert(sizeof(RxMaterialSystemDesc) == sizeof(void*),
              "rx_api.h's own pinned RxMaterialSystemDesc size -- re-checked here for the same reason");
// [Stage 0 audit F7] RxMaterialSystemDesc/RxTextureDesc's own pinned
// alignments -- re-checked here for the identical reason every sizeof
// pin in this file already is (this file's own header comment).
static_assert(alignof(RxMaterialSystemDesc) == alignof(void*),
              "rx_api.h's own pinned RxMaterialSystemDesc alignment -- re-checked here for the same reason");
// [Task 7] RxTextureDesc's own pinned size -- same re-check discipline.
static_assert(sizeof(RxTextureDesc) == 32, "rx_api.h's own pinned RxTextureDesc size -- re-checked here for the "
                                            "same reason");
static_assert(alignof(RxTextureDesc) == 8,
              "rx_api.h's own pinned RxTextureDesc alignment -- re-checked here for the same reason");

// [Stage 0 audit F7] "No GUID-uniqueness test exists across the five
// kIID_* constants (manually verified unique)" -- replaced here with an
// enforced compile-time proof instead of a one-time manual check.
// `guidEquals()` compares RxGuid's 4 members field-by-field (no padding to
// worry about -- see the pinned static_asserts above) so every one of the
// 10 pairwise comparisons below is a plain constexpr equality, no
// runtime/doctest dependency needed -- fitting this file's own established
// "successful compilation IS the test" style.
constexpr bool guidEquals(const RxGuid& a, const RxGuid& b) {
    return a.data1 == b.data1 && a.data2 == b.data2 && a.data3 == b.data3 && a.data4[0] == b.data4[0] &&
           a.data4[1] == b.data4[1] && a.data4[2] == b.data4[2] && a.data4[3] == b.data4[3] &&
           a.data4[4] == b.data4[4] && a.data4[5] == b.data4[5] && a.data4[6] == b.data4[6] &&
           a.data4[7] == b.data4[7];
}

static_assert(!guidEquals(kIID_IRxUnknown, kIID_IRxTexture), "kIID_IRxUnknown/kIID_IRxTexture must differ");
static_assert(!guidEquals(kIID_IRxUnknown, kIID_IRxMaterialInstance),
              "kIID_IRxUnknown/kIID_IRxMaterialInstance must differ");
static_assert(!guidEquals(kIID_IRxUnknown, kIID_IRxMaterial), "kIID_IRxUnknown/kIID_IRxMaterial must differ");
static_assert(!guidEquals(kIID_IRxUnknown, kIID_IRxMaterialSystem),
              "kIID_IRxUnknown/kIID_IRxMaterialSystem must differ");
static_assert(!guidEquals(kIID_IRxTexture, kIID_IRxMaterialInstance),
              "kIID_IRxTexture/kIID_IRxMaterialInstance must differ");
static_assert(!guidEquals(kIID_IRxTexture, kIID_IRxMaterial), "kIID_IRxTexture/kIID_IRxMaterial must differ");
static_assert(!guidEquals(kIID_IRxTexture, kIID_IRxMaterialSystem),
              "kIID_IRxTexture/kIID_IRxMaterialSystem must differ");
static_assert(!guidEquals(kIID_IRxMaterialInstance, kIID_IRxMaterial),
              "kIID_IRxMaterialInstance/kIID_IRxMaterial must differ");
static_assert(!guidEquals(kIID_IRxMaterialInstance, kIID_IRxMaterialSystem),
              "kIID_IRxMaterialInstance/kIID_IRxMaterialSystem must differ");
static_assert(!guidEquals(kIID_IRxMaterial, kIID_IRxMaterialSystem),
              "kIID_IRxMaterial/kIID_IRxMaterialSystem must differ");

namespace {

// Naming every declared symbol once (interfaces, GUIDs, the struct, the
// factory) so this file's only job -- proving the header ALONE is
// sufficient to name everything it declares -- cannot be quietly
// defeated by an otherwise-unreferenced declaration being subtly wrong
// in a way only a real consumer would ever hit (e.g. a typo'd type in a
// method no test elsewhere calls).
using UnknownPtr = IRxUnknown*;
using TexturePtr = IRxTexture*;
using MaterialInstancePtr = IRxMaterialInstance*;
using MaterialPtr = IRxMaterial*;
using MaterialSystemPtr = IRxMaterialSystem*;

[[maybe_unused]] const RxGuid& kUnknownGuidRef = kIID_IRxUnknown;
[[maybe_unused]] const RxGuid& kTextureGuidRef = kIID_IRxTexture;
[[maybe_unused]] const RxGuid& kMaterialInstanceGuidRef = kIID_IRxMaterialInstance;
[[maybe_unused]] const RxGuid& kMaterialGuidRef = kIID_IRxMaterial;
[[maybe_unused]] const RxGuid& kMaterialSystemGuidRef = kIID_IRxMaterialSystem;

using FactoryFn = RxResult (*)(const RxMaterialSystemDesc*, IRxMaterialSystem**);
[[maybe_unused]] constexpr FactoryFn kFactory = &rxCreateMaterialSystem;

// [spec Phase 4 design D23, seed 13] rxSetLogCallback's own types --
// named here for the identical "cannot be quietly defeated by an
// otherwise-unreferenced declaration" reason as everything else in this
// file (this header's own comment above).
static_assert(sizeof(RxLogSeverity) == sizeof(int32_t),
              "rx_api.h's own pinned RxLogSeverity size -- re-checked here for the same reason as RxGuid/"
              "RxMaterialSystemDesc/RxTextureDesc above");
[[maybe_unused]] constexpr RxLogSeverity kTraceSeverity = RX_LOG_TRACE;
[[maybe_unused]] constexpr RxLogSeverity kDebugSeverity = RX_LOG_DEBUG;
[[maybe_unused]] constexpr RxLogSeverity kInfoSeverity = RX_LOG_INFO;
[[maybe_unused]] constexpr RxLogSeverity kWarnSeverity = RX_LOG_WARN;
[[maybe_unused]] constexpr RxLogSeverity kErrorSeverity = RX_LOG_ERROR;
[[maybe_unused]] RxLogCallback kUnusedLogCallback = nullptr;
using SetLogCallbackFn = RxResult (*)(RxLogCallback, void*);
[[maybe_unused]] constexpr SetLogCallbackFn kSetLogCallback = &rxSetLogCallback;

// [Task 7] RxTextureDesc/RxFormat/IRxMaterialSystem::createTexture2D --
// named here too, for the identical "cannot be quietly defeated by an
// otherwise-unreferenced declaration" reason this file's own header
// comment states.
[[maybe_unused]] RxTextureDesc kUnusedTextureDesc{};
[[maybe_unused]] constexpr RxFormat kUnormFormat = RX_FORMAT_RGBA8_UNORM;
[[maybe_unused]] constexpr RxFormat kSrgbFormat = RX_FORMAT_RGBA8_SRGB;
using CreateTexture2DFn = RxResult (IRxMaterialSystem::*)(const RxTextureDesc*, IRxTexture**);
[[maybe_unused]] constexpr CreateTexture2DFn kCreateTexture2D = &IRxMaterialSystem::createTexture2D;

}  // namespace
