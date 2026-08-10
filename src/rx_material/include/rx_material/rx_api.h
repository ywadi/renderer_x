#pragma once

// rx_api.h -- RendererX's public COM-lite ABI surface for rx_material
// (the first public boundary this codebase ships) [spec Phase 3 design,
// docs/superpowers/specs/2026-08-10-phase3-render-graph-materials-design.md,
// D5]. This header is THE boundary: it is self-contained (only <cstdint>),
// pulls in no rx_*/STL/Vulkan/Slang type, and every symbol below follows
// the ABI rules D5 derives from the research survey of COM/ISlangUnknown/
// Diligent/chadaustin.me [R:M§1.3]:
//   - every interface is 100% pure virtual, single inheritance from
//     IRxUnknown only (no multiple/virtual inheritance, no data members,
//     no overloaded method names, no virtual destructor -- release()
//     plays that role instead);
//   - no exceptions, no RTTI (dynamic_cast/typeid), no STL type anywhere
//     in a signature -- errors are RxResult codes, identity/casting is
//     queryInterface() + GUID comparison;
//   - allocation and deallocation of anything crossing this boundary
//     happen on the renderer side only (release() is how a caller gives
//     memory back, never `delete`/`free`);
//   - every interface carries its own explicit GUID (kIID_IRx...,
//     generated with uuidgen, embedded as a literal below);
//   - every POD struct crossing the boundary is static_assert-pinned
//     immediately after its definition.
// This shape mirrors Slang's own `ISlangUnknown` (third_party/slang-
// prebuilt's include/slang.h) -- the direct in-repo precedent [R:M§1.5] --
// rather than inventing a new one: same three-method root
// (queryInterface/addRef/release), same GUID layout (RxGuid below is
// byte-for-byte SlangUUID's/COM's own GUID shape), same "no COM runtime
// required" property (queryInterface is a plain virtual call, nothing
// platform-specific).

#include <cstdint>

// Empty on x64 (the only architecture this project ships:
// zig-cc-cross-compiled *-windows-gnu and native Linux, both x86_64) --
// kept as an explicit, named marker on every boundary-crossing virtual
// method so the calling-convention decision is documented at each call
// site rather than assumed [R:M§1.3 point 7]. x64 has exactly one ABI-
// mandated calling convention (Microsoft's own x64 calling convention
// reference, cited in the design doc's research), so there is nothing
// for this macro to expand to.
#define RX_CALL /* empty on x64; kept for documentation of the boundary */

// Status codes crossing the ABI -- never an exception (rule 2 above).
// Internal C++ exceptions (std::runtime_error, std::out_of_range, ...)
// thrown by rx_material's internal MaterialSystem are caught at this
// boundary (api_impl.cpp) and mapped to one of these; RX_E_COMPILE is
// specifically a material compile/link/reflect failure, with the actual
// Slang diagnostic text logged via spdlog on the renderer side rather
// than carried across the boundary as a string (no STL string in a
// signature -- Phase 3 does not need structured diagnostics on the
// caller side, per the design doc).
typedef int32_t RxResult;
enum : RxResult {
    RX_OK = 0,
    RX_E_FAIL = -1,
    RX_E_INVALIDARG = -2,
    RX_E_NOTFOUND = -3,
    RX_E_COMPILE = -4,
    RX_E_NOINTERFACE = -5,
};

// Byte-for-byte the same layout as SlangUUID/Win32 GUID (uint32 + uint16
// + uint16 + 8 bytes) -- deliberate, not incidental: this is the exact
// shape every COM-lite precedent surveyed for D5 uses, and matching it
// means this type's bytes are meaningful if ever compared against a
// real COM/Slang GUID. No padding between members on any ABI this
// project targets (MSVC, Itanium/GCC/Clang, all agree on this exact
// field layout at natural alignment) -- pinned below regardless.
struct RxGuid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};
static_assert(sizeof(RxGuid) == 16, "RxGuid must stay pinned at 16 bytes -- every consumer on both sides of the "
                                     "ABI boundary depends on this exact size (D5).");
static_assert(alignof(RxGuid) == 4, "RxGuid's alignment must stay pinned -- a change here could shift its size via "
                                     "padding on some ABI.");

// --- IRxUnknown: the COM-lite root every interface below inherits ------
// Exactly ISlangUnknown's/IUnknown's three-method shape [R:M§1.5]. Every
// concrete object addRef()'d to 1 at construction (the COM convention:
// whoever gets the pointer back -- a factory's out-param, or
// queryInterface()'s out-param -- owns exactly one reference and must
// release() it). queryInterface() returns the SAME pointer (as
// IRxUnknown*, or as any other interface pointer this object supports)
// for every IID a given object answers to -- the COM identity rule --
// which single inheritance (never multiple) makes automatic: there is
// only ever one base-IRxUnknown sub-object per concrete instance, so no
// vtable-thunk pointer adjustment can ever make two queries for the same
// object disagree.
struct IRxUnknown {
    virtual RxResult RX_CALL queryInterface(const RxGuid& iid, void** outObject) = 0;
    virtual uint32_t RX_CALL addRef() = 0;
    virtual uint32_t RX_CALL release() = 0;
};
static constexpr RxGuid kIID_IRxUnknown = {0x780faed9, 0xc2e3, 0x4fcc, {0xae, 0xe3, 0x79, 0x4a, 0xa4, 0x5d, 0xca, 0xf4}};

// --- IRxTexture: opaque handle wrapping an internal renderer resource --
// No methods beyond IRxUnknown in Phase 3 -- it exists purely so
// IRxMaterialInstance::setTexture() below has a real interface-typed
// parameter to bind against. It always wraps a renderer-owned
// rhi::Texture2D + bindless-table handle; nothing in Task 6's surface
// creates one (no rxCreateTexture factory yet) -- a future task adds the
// creation path once texture loading has its own public surface.
struct IRxTexture : IRxUnknown {};
static constexpr RxGuid kIID_IRxTexture = {0x927ce6e7, 0x3fcd, 0x47e8, {0x8b, 0x20, 0xef, 0xdd, 0xc5, 0x32, 0x26, 0xde}};

// --- IRxMaterialInstance: one material's bound-parameter set -----------
// setFloat/setFloat4/setTexture validate `name` against the owning
// IRxMaterial's reflected parameter set and the value's shape against
// that parameter's reflected type:
//   RX_OK             -- name known, type matches; value stored.
//   RX_E_NOTFOUND      -- no reflected parameter named `name` on this
//                         material.
//   RX_E_INVALIDARG    -- `name`/`value`/`texture` is null, OR `name` is
//                         a real reflected parameter of a DIFFERENT type
//                         than this setter binds.
// Task 6 stores validated values into a CPU-side blob owned by the
// instance object (never touching a live GPU descriptor); a future task
// connects that blob to the actual per-instance descriptor set/uniform
// buffer this parameter block needs at draw time -- see api_impl.cpp's
// own header comment for the exact handoff.
struct IRxMaterialInstance : IRxUnknown {
    virtual RxResult RX_CALL setFloat(const char* name, float value) = 0;
    virtual RxResult RX_CALL setFloat4(const char* name, const float value[4]) = 0;
    virtual RxResult RX_CALL setTexture(const char* name, IRxTexture* texture) = 0;
};
static constexpr RxGuid kIID_IRxMaterialInstance = {
    0x36957adc, 0x1777, 0x4093, {0xaa, 0x97, 0x0c, 0x30, 0xc0, 0xb9, 0xc5, 0x75}};

// --- IRxMaterial: one loaded Slang material module ----------------------
struct IRxMaterial : IRxUnknown {
    // Allocates a new IRxMaterialInstance bound to this material (renderer-
    // side allocation, per the boundary rules -- the caller releases it
    // like any other IRxUnknown). RX_E_INVALIDARG if outInstance is null.
    virtual RxResult RX_CALL createInstance(IRxMaterialInstance** outInstance) = 0;
    // The material's own name (its Slang module's file stem). Valid for
    // this IRxMaterial's own lifetime -- never freed/reallocated out from
    // under a caller that only holds the returned pointer, no separate
    // release needed for the string itself.
    virtual const char* RX_CALL name() = 0;
};
static constexpr RxGuid kIID_IRxMaterial = {0xfeed2ffc, 0xac13, 0x40cc, {0x9c, 0x4c, 0x26, 0x90, 0xcb, 0x6a, 0x59, 0xf6}};

// --- IRxMaterialSystem: the entry point into rx_material ----------------
struct IRxMaterialSystem : IRxUnknown {
    // Loads the Slang module at `slangModulePath` as a material (see the
    // internal rx::material::MaterialSystem::loadMaterial() this forwards
    // to for the full compile/reflect contract). RX_E_INVALIDARG if
    // `slangModulePath`/`outMaterial` is null; RX_E_COMPILE (diagnostics
    // logged via spdlog on the renderer side, never thrown across this
    // boundary) if the module fails to load/compile/link/reflect.
    virtual RxResult RX_CALL loadMaterial(const char* slangModulePath, IRxMaterial** outMaterial) = 0;
    // Declared now for GUID/vtable stability; Task 7 wires the actual
    // hot-reload behavior (watch loaded modules' source files, re-
    // compile/re-link changed ones, invalidate affected pipeline-cache
    // entries). Task 6's implementation is a documented no-op that
    // always returns RX_OK -- see api_impl.cpp's own comment on this
    // method for why that is this task's explicit, planned sequencing
    // rather than a stub left unfinished by oversight.
    virtual RxResult RX_CALL reloadChanged() = 0;
};
static constexpr RxGuid kIID_IRxMaterialSystem = {
    0x8739717f, 0xea0e, 0x4484, {0x82, 0x9e, 0x68, 0x9c, 0x07, 0x06, 0x1b, 0xbc}};

// --- Factory: the in-process bridge into an existing internal system ----
// The standalone DLL artifact is deferred (D5) -- Phase 3 proves the ABI
// shape inside the normal static-library build rather than shipping a
// real rx.dll yet. `internalMaterialSystem` is a bridge pointer to an
// already-constructed `rx::material::MaterialSystem*`: this boundary
// treats it as opaque (a `void*`, exactly like the ABI rules require --
// no rx_material internal type name may appear in this header), and does
// NOT require it to be non-null. An instance created with a null
// `internalMaterialSystem` answers queryInterface/addRef/release/
// reloadChanged normally (none of those touch the internal system) but
// loadMaterial() on it returns RX_E_FAIL -- a deliberate, defensive
// design choice that keeps this factory usable for device-free ABI
// contract testing (queryInterface identity, refcount round-trips) with
// no real rx::rhi::Device anywhere nearby, never a null-pointer crash.
struct RxMaterialSystemDesc {
    void* internalMaterialSystem;  // rx::material::MaterialSystem*, or null (see above)
};
static_assert(sizeof(RxMaterialSystemDesc) == sizeof(void*),
              "RxMaterialSystemDesc must stay exactly one pointer wide -- it is passed by pointer across the ABI "
              "and a caller may construct it as a plain aggregate on either side.");

// RX_E_INVALIDARG if `desc` or `outSystem` is null. On success,
// `*outSystem` is a new IRxMaterialSystem with refcount 1 (the caller
// owns that reference and must release() it); on failure `*outSystem` is
// set to null.
extern "C" RxResult rxCreateMaterialSystem(const RxMaterialSystemDesc* desc, IRxMaterialSystem** outSystem);
