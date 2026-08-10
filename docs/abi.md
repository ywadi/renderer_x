# RendererX Public ABI Boundary Rules

RendererX's public interface (`rx_api.h`, today built into the static
libraries; the standalone DLL artifact is deferred per spec D5) follows a
COM-lite pattern for ABI stability
and cross-compiler compatibility. This document establishes the rules for future
interface additions.

## Pattern Summary

Every interface in the public ABI (defined in `rx_api.h`) adheres to these structural
constraints, derived from research into COM, Slang's ISlangUnknown, and precedents
like Diligent Engine:

1. **100% pure virtual, single inheritance**: Every interface inherits only from
   `IRxUnknown`, the three-method root (queryInterface/addRef/release). No data
   members, no overloads, no virtual destructors — release() replaces the destructor.

2. **Explicit GUID per interface version**: Every interface carries a unique,
   immutable GUID (128-bit identifier). queryInterface() is the only casting
   mechanism; dynamic_cast and typeid are forbidden.

3. **No exceptions, no RTTI**: Internal C++ exceptions are caught at the boundary
   and mapped to RxResult error codes. Callers never see std::exception or
   type_info.

4. **No STL, no Vulkan/Slang types**: Signatures use only fixed-width POD types
   (uint32_t, void*) and custom structs. All POD structs crossing the boundary
   are pinned via static_assert for size/alignment.

5. **Renderer-side allocation only**: Callers receive references to renderer-owned
   objects through factory methods and addRef/release pairs. A caller never calls
   delete or free on anything from the DLL.

## Boundary Rules

### Reference Counting

- Every object returned from a factory method arrives with refcount = 1.
- The caller owns that reference and must eventually call release().
- Calling addRef/release follows standard COM discipline.

### Parameter Blocks (POD Structs)

- Every struct crossing the boundary must be declared with static_assert checks:
  ```cpp
  struct MyParams { /* fixed-width fields only */ };
  static_assert(sizeof(MyParams) == expectedBytes, "...reason...");
  static_assert(alignof(MyParams) == expectedAlign, "...reason...");
  ```
- No implicit padding. All fields must be explicitly laid out.
- All integer types must be fixed-width (int32_t, uint64_t, etc.), never int or
  size_t.

### Error Handling

- Methods return RxResult (int32_t): RX_OK, RX_E_FAIL, RX_E_INVALIDARG,
  RX_E_NOTFOUND, RX_E_COMPILE, RX_E_NOINTERFACE.
- Never throw across the boundary.
- Diagnostic text (compilation errors, etc.) is logged internally via spdlog,
  not carried as a string parameter.

## Why This Shape

The DLL is built by zig's cross-compiler (zig cc, targeting Windows-GNU ABI) but
consumed by MSVC clients (D5, §1.3). Three incompatibilities would break a naive C++
ABI without explicit rules:

1. **Name mangling**: MSVC and GCC mangle virtual method names differently. COM-lite
   avoids this by using only C-visible symbols (plain virtual functions with
   stable GUID-based queries instead of C++ RTTI).

2. **Exception handling**: MSVC's SEH and GCC's DWARF unwinding are incompatible.
   Stopping exceptions at the boundary (catch and return RxResult) eliminates the
   problem.

3. **Layout assumptions**: std::string, std::vector, and other STL types have
   ABI-dependent layouts (pointer size, allocator strategy, etc.). Using only POD
   and static_assert-pinned structs makes layout portable.

The COM-lite approach is proven across many cross-compiler boundaries: Windows
COM itself (still the standard after 30+ years), Slang (which RendererX already
links), and Diligent Engine. Reference rx_api.h for the canonical example.

## Pre-Release Interface Evolution

**Before the first SDK release (v1.0)**, GUIDs and method signatures can change
freely:

- Add new methods to an interface by appending them (no renumbering vtable indices).
- Create a new interface version (GUIDv2) if you need to remove or reorder methods
  from an existing interface.
- Existing interfaces that ship in a Phase can keep being iterated up until release.

**After the first SDK release**, interface GUIDs become immutable:

- Create a new versioned interface (IRxMaterialV2) rather than modifying the
  original IRxMaterial.
- Old interfaces stay in the header forever — clients may still link against them.
- Bump the major version number when introducing breaking interface changes.

This project is currently pre-v1.0 (Phase 3 in development). Interface stability
is a concern only after the first public release.
