# Phase 3 Research: DLL ABI Boundary Patterns and Material System Design

Scope: RendererX Phase 3 public API surface (`IMaterial`, `IShaderModule`, `ITexture`, `IMesh`).
Context: Vulkan 1.3-only renderer shipped as a single DLL to third-party engines, most likely
MSVC-built on Windows; our Windows binaries are cross-compiled from Linux with `zig cc`, which
targets the `*-windows-gnu` ABI by default. Shaders are Slang v2026.14.x with runtime compilation
+ reflection already in place (`rx_shader`).

Research only. No code changes. Every load-bearing claim below is cited; anything I could not
independently verify is explicitly marked **UNVERIFIED**.

---

## 1. DLL ABI boundary patterns

### 1.1 Precedent survey

#### (a) Flat C ABI + opaque handles — bgfx

bgfx's primary API is C++ ("using basic C++ that looks like C"), but the project maintains a
parallel **C99 API** built around a single generated struct, `bgfx_interface_vtbl_t`, which is a
plain struct of function pointers mirroring every C++ API entry point. A consumer calls the one
exported function `bgfx_get_interface(version)` to retrieve this vtable, rather than linking
against named symbols directly — Branimir Karadžić's own description: "import single shared
library function `bgfx_get_interface`, call it with version of bgfx API header used and get whole
API" ([bgfx is switching to IDL to generate API](https://bkaradzic.github.io/posts/idl/)). This is
structurally a **COM-like pattern without inheritance or reference counting**: one flat struct of
function pointers, versioned by an integer, not a vtable hierarchy.

GPU resources (buffers, textures, shaders, programs) are never passed as pointers; they are
opaque 16-bit **handles** validated internally by bgfx, which sidesteps lifetime/ownership issues
across the boundary entirely (see the generated header,
[`bgfx/include/bgfx/c99/bgfx.h`](https://github.com/bkaradzic/bgfx/blob/master/include/bgfx/c99/bgfx.h),
and the [API reference](https://deepwiki.com/bkaradzic/bgfx/2.1-api-reference) — note the latter
is a third-party auto-generated wiki, lower confidence than the primary source).

Critically, bgfx did **not** hand-write the C99 layer and its N language bindings (C#, Beef, Zig)
forever — it moved to a Lua-based **IDL** that generates the C++ header, the C99 wrapper, and the
shared-library shim from one description, specifically because "adding/changing API for C99
bindings was changing header declarations of function and function type... That's two
definitions, one implementation, and one list of functions" — i.e. hand-maintained parallel ABI
surfaces rot ([bgfx is switching to IDL to generate API](https://bkaradzic.github.io/posts/idl/)).
This is directly relevant to RendererX: whatever boundary shape we pick, plan for codegen (from
Slang reflection or an IDL) rather than hand-synced headers, or the C ABI and the ergonomic C++
header will drift.

#### (b) COM-style pure-virtual interfaces — Diligent Engine, D3D/COM, Slang

**Diligent Engine** implements a custom COM-style reference-counted object model rather than using
C++ smart pointers, and its own docs state the reason directly: "the engine is packed into
dynamic libraries, which can be used in any language or system, and thus needs to provide simple
and efficient method for managing object lifetimes... C++ smart pointers maintain internal
reference counters, but do not allow accessing them directly. The counters can only be changed by
creating new smart pointers, which limits their application to C++ only"
([Reference Counting – Diligent Graphics](http://diligentgraphics.com/diligent-engine/architecture/cross-platform/reference-counting/)).
Its architecture separates the object interface (`IObject`, providing `AddRef`/`Release`/
`QueryInterface`) from the reference-counting bookkeeping object (`IReferenceCounters`, which
supports weak pointers), with `RefCntAutoPtr`/`RefCntWeakPtr` as C++-side convenience wrappers
([`IReferenceCounters` API reference](https://diligentgraphics.com/doc/class_diligent_1_1_i_reference_counters.html)).
This is presented in general engineering discussion as a textbook "COM-Lite" implementation for a
graphics engine (search-aggregated characterization; treat as informed community consensus rather
than a Diligent self-description).

**Windows COM / `IUnknown`** is the ur-precedent: "IUnknown, a simple interface that represents a
binary stable ABI for calling methods on objects across a set of languages and a way of managing
the lifetime of objects used across multiple modules" ([What's a vtable? What's an IUnknown? —
TimDbg](https://www.timdbg.com/posts/vtables/); background on the interface itself:
[IUnknown — Wikipedia](https://en.wikipedia.org/wiki/IUnknown)). D3D's own interfaces
(`ID3D11Device`, `ID3D12Device`, etc.) are COM interfaces built on exactly this pattern — this is
why D3D headers work unmodified from MSVC, MinGW, and Rust bindgen alike.

**Slang** ships its own "COM-lite" interface, `ISlangUnknown`, defined with the same three-method
shape as `IUnknown`:

```cpp
virtual SLANG_NO_THROW SlangResult SLANG_MCALL
queryInterface(SlangUUID const& uuid, void** outObject) = 0;
virtual SLANG_NO_THROW uint32_t SLANG_MCALL addRef() = 0;
virtual SLANG_NO_THROW uint32_t SLANG_MCALL release() = 0;
```

with the header stating explicitly: "This interface definition is compatible with the COM
`IUnknown`, and uses the same UUID, but Slang does not require applications to use or initialize
COM" ([`include/slang.h`](https://raw.githubusercontent.com/shader-slang/slang/master/include/slang.h)).
Slang provides `QueryInterface`/`AddRef`/`Release` aliases for COM-familiarity and a `ComPtr<T>`
smart-pointer convenience type, while stating the design does not depend on any Windows COM
runtime aspect — i.e. it works identically on Linux/macOS where there is no COM runtime at all.
This is the strongest same-domain precedent available to us: **Slang, the shader compiler we are
already embedding, ships its public C++ API as COM-lite pure-virtual interfaces specifically so it
can be consumed identically across compilers and platforms.**

#### (c) Exporting real C++ classes — why it is fragile

Exporting concrete classes (non-abstract, with data members, inline methods, `std::vector`/
`std::string` in signatures, real destructors) across a DLL boundary depends on the *entire* C++
ABI matching between builder and consumer, and "there is no C++ ABI" as a cross-vendor standard —
only per-compiler-family conventions (Itanium ABI for GCC/Clang, a separate one for MSVC)
([Windows GNU/MinGW and MSVC binary C ABI compatibility guarantees —
Ziggit](https://ziggit.dev/t/windows-gnu-mingw-and-msvc-binary-c-abi-compatibility-guarantees/6903),
quoting a community answer: "For C++ you must have the same C++ library and the same exception
(unwind) library for all of your code and libraries... these are C++ problems, since there is no
C++ ABI"). Concretely, the things that break:

- **Name mangling** differs between MSVC and Itanium-ABI compilers, so you cannot even link a
  plain (non-`extern "C"`) C++ symbol across the two toolchains.
- **Exception-handling ABI** differs (SEH-based tables for MSVC vs. DWARF/Itanium unwind tables
  for GCC/Clang/MinGW). The CERT C++ coding standard states the rule plainly: "Throw an exception
  across an execution boundary only when both sides of the execution boundary use the same ABI for
  exception handling," and gives the exact failure mode relevant to us — a GCC-compiled library
  using the DWARF/Itanium ABI throwing to an MSVC application using SEH results in an uncaught
  exception and program termination
  ([ERR59-CPP — SEI CERT C++ Coding Standard](https://cmu-sei.github.io/secure-coding-standards/sei-cert-cpp-coding-standard/rules/exceptions-and-error-handling-err/err59-cpp)).
- **RTTI is per-module.** `dynamic_cast`/`typeid` comparisons across a DLL boundary can silently
  fail (return null / compare unequal) because `type_info` is generated per translation unit /
  module rather than being globally unified — a commonly hit, well-documented failure mode
  ([forum discussion of the exact symptom](https://www.daniweb.com/programming/software-development/threads/371696/access-violation-and-polymorphism-problem)).
- **STL containers/strings are not ABI-stable even within one vendor** across debug/release,
  standard-library versions, or allocator configuration; libstdc++ explicitly documents an ABI
  versioning/compatibility policy precisely because container layouts change
  ([GCC libstdc++ ABI Policy and Guidelines](https://gcc.gnu.org/onlinedocs/gcc-13.4.0/libstdc++/manual/manual/abi.html)).
  MSVC STL and libstdc++/libc++ are never layout-compatible with each other at all.
- **Allocator/CRT mismatch**: freeing memory allocated by a different CRT/heap than the one that
  allocated it is a classic Windows DLL bug; the fix is a strict rule that allocation and
  deallocation happen on the same side of the boundary. Chad Austin's widely cited "Binary-
  compatible C++ Interfaces" guide states this directly: "Don't allocate memory on one side of the
  DLL boundary and free it on the other" ([Binary-compatible C++ Interfaces —
  chadaustin.me](https://chadaustin.me/cppinterface.html)). The same page gives the fuller rule set
  used by real shipping engines: interface classes must be *completely* abstract (every method pure
  virtual), all free functions `extern "C"`, an explicit calling convention (historically
  `__stdcall` on x86), no standard library types in the interface, no exceptions across the
  boundary, no virtual destructors (use an explicit `destroy()`/placement-`operator delete` pattern
  instead), and — a subtle one directly relevant to interface design discipline — **no overloaded
  methods in the interface**, because "different compilers order them within the vtable
  differently."
- **Struct layout/packing**: any POD struct crossing the boundary must have explicit, pragma-
  pinned packing/alignment and no compiler-dependent padding assumptions, since MSVC and GCC/Clang
  can differ on default alignment for edge cases (bitfields, `#pragma pack` behavior, empty base
  classes).

Net effect: exporting real (non-COM-shaped) C++ classes only works if both sides are built by the
*same compiler, same version, same STL, same CRT/runtime linkage, same build config* — which is
precisely the constraint RendererX cannot make, because the DLL is zig-cc/MinGW-built and
consumers are MSVC.

### 1.2 THE LOAD-BEARING QUESTION: is the MinGW/zig-cc vtable layout for a COM-shaped interface reliably identical to MSVC's, on Windows x64?

**Short answer: for the specific narrow subset of C++ that COM itself is restricted to — single
inheritance from a pure-virtual base, no data members, no multiple/virtual inheritance, no
overloaded methods, no virtual destructor — yes, and this is not a theoretical claim but a 20+
year empirical fact baked into how Windows itself works. Outside that subset, no.**

Evidence, assembled from primary/authoritative sources:

1. **The x64 Windows calling convention is a platform ABI, not a compiler-specific convention.**
   Microsoft's own reference describes it as "the standard processes and conventions that one
   function (the caller) uses to make calls into another function (the callee) in x64 code" — a
   single fixed 4-register fast-call convention (`RCX`/`RDX`/`R8`/`R9`, `XMM0`–`XMM3`) that applies
   uniformly to every function on Windows x64, with no MSVC-only variant
   ([x64 calling convention — Microsoft Learn](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170)).
   This matters because it means a virtual call — `this` in `RCX`, remaining args following — is
   dispatched identically regardless of which compiler emitted the call site or the callee, as
   long as both target Windows x64. This is a structural difference from x86, where MSVC and GCC
   historically diverged on `thiscall` (GCC only added a compatible `__thiscall` calling convention
   in GCC 4.6, explicitly to be able to call VC++-generated class methods —
   [GCC mailing list thread, Dec 2007](https://gcc.gnu.org/legacy-ml/gcc-help/2007-12/msg00216.html)
   discusses exactly this gap). On x64 there is no equivalent gap to patch because there is only
   one convention to begin with.
2. **mingw-w64 ships full production headers for Windows COM** — `combaseapi.h`, `objidlbase.idl`,
   `Unknwn.h`, `objbase.h` — enabling GCC/MinGW-built code to call `CoCreateInstance` and invoke
   virtual methods on COM objects that live inside Microsoft-built system DLLs (`combase.dll`,
   `ole32.dll`) and to consume Microsoft's own COM-shaped interfaces (DirectX/DXGI, WIC, MMDevice,
   XAudio2, Shell, etc.) ([`combaseapi.h` in
   mingw-w64](https://github.com/msys2-contrib/mingw-w64/blob/master/mingw-w64-headers/include/combaseapi.h),
   [`objidlbase.idl`](https://github.com/Alexpux/mingw-w64/blob/master/mingw-w64-headers/include/objidlbase.idl)).
   This is the crux of the evidence: **every MinGW-built Windows application that touches COM at
   all is a running, decades-long production proof that a GCC/Itanium-internals compiler correctly
   calls through vtables produced by an MSVC-built callee**, and (via user-implemented COM servers)
   the reverse direction as well. `zig cc` targeting `*-windows-gnu` uses this exact mingw-w64
   headers/CRT foundation.
3. **Slang itself is a live, shipping instance of this exact pattern already inside RendererX's
   toolchain** — `ISlangUnknown`/COM-lite interfaces, explicitly designed to be ABI-stable across
   compilers without requiring the Windows COM runtime
   ([`include/slang.h`](https://raw.githubusercontent.com/shader-slang/slang/master/include/slang.h)).
   Since RendererX already links Slang and calls through `ISlangUnknown`-derived interfaces today,
   we are already depending on this property.
4. **zig's own stated compatibility claim.** Zig's Windows-GNU target was made the default ABI for
   `zig cc`/`zig build` starting at Zig 0.8.0 specifically so Zig works without an MSVC install,
   using mingw-w64 to provide libc — a change proposed and accepted precisely on the strength of
   this claim: "this C ABI is binary-compatible with MSVC-compiled code" for the C-level ABI,
   though "the header files are not source-compatible with MSVC libc header files"
   ([`ziglang/zig` issue #6565](https://github.com/ziglang/zig/issues/6565), accepted, milestone
   0.8.0). This confirms the premise in the task brief (`zig cc` defaults to `windows-gnu`) is
   accurate. Community discussion of the same claim adds the caveats that matter for us: mixing
   Universal CRT and legacy `msvcrt.dll`-linked binaries is unsafe (heap mismatch), and C `long
   double` size differs between the two ABIs — but nothing in that thread contradicts C-level (and,
   by the argument above, COM-subset virtual-call-level) compatibility; it explicitly and
   separately calls out that **C++ is not covered by any such guarantee**: "For C++ you must have
   the same C++ library and the same exception (unwind) library for all of your code and
   libraries... these are C++ problems, since there is no C++ ABI"
   ([Ziggit thread](https://ziggit.dev/t/windows-gnu-mingw-and-msvc-binary-c-abi-compatibility-guarantees/6903)).

**Important calibration — what is *not* proven by the above:** I found no primary source (Microsoft,
LLVM, GCC, or Zig) that formally *specifies* "GCC/Clang/Zig and MSVC vtable layouts are guaranteed
identical on Windows x64" as a written cross-vendor standard. What exists is (a) a shared,
OS-mandated calling convention that removes the x86-style thiscall mismatch entirely, and (b) an
overwhelming, multi-decade empirical track record (mingw-w64's COM interoperability, Slang's own
design) that the *narrow* COM-legal subset of vtable shapes is safe in practice. Treat this as
**empirically validated, not formally standardized** — the mitigation is to stay religiously
inside the COM-legal subset (see rules below), not to assume general C++ ABI compatibility. I
found no citable case study of a project specifically stating "we build our DLL with zig cc
targeting windows-gnu and ship COM-style interfaces to MSVC consumers"; that exact combination is
**UNVERIFIED** as a named precedent, though every constituent claim it depends on is independently
verified above.

Also relevant and independently verified: GCC/Itanium and MSVC *do* differ in general vtable
layout as soon as you leave the COM-legal subset — multiple inheritance, virtual inheritance, and
RTTI/type_info representation are laid out differently between the Itanium C++ ABI (used by
GCC/Clang/MinGW) and MSVC's ABI ([C++ ABI Deep Dive](https://chenmiaoi.github.io/modern_cpp/en/topics/abi.html),
secondary source; primary Itanium vtable layout spec at
[Itanium C++ ABI](https://itanium-cxx-abi.github.io/cxx-abi/abi.html)). This is exactly why COM's
authors restricted interfaces to single inheritance, pure-virtual-only, no data members: it is the
intersection where the two families of ABI agree.

### 1.3 Standard rules for the boundary (synthesized, all independently cited above)

1. Interface classes: 100% pure virtual, single inheritance only, from a common root
   (`IUnknown`/`ISlangUnknown`-shaped: `queryInterface`/`addRef`/`release`), no data members, no
   overloaded method names, no virtual destructor (use `Release()`/an explicit `destroy` method
   instead) — [chadaustin.me](https://chadaustin.me/cppinterface.html).
2. No exceptions cross the boundary — return error codes / `SlangResult`-style status codes
   instead — [ERR59-CPP](https://cmu-sei.github.io/secure-coding-standards/sei-cert-cpp-coding-standard/rules/exceptions-and-error-handling-err/err59-cpp).
3. No RTTI (`dynamic_cast`/`typeid`) relied upon across the boundary — use `queryInterface` +
   GUID/UUID comparison instead, exactly as COM and Slang do — failure mode documented at
   [DaniWeb](https://www.daniweb.com/programming/software-development/threads/371696/access-violation-and-polymorphism-problem).
4. No STL (or any non-trivial, non-POD C++ type) in signatures — libstdc++'s own ABI policy
   underscores why container layout cannot be assumed stable even within one vendor
   ([GCC libstdc++ ABI Policy](https://gcc.gnu.org/onlinedocs/gcc-13.4.0/libstdc++/manual/manual/abi.html)).
5. Allocation and deallocation of anything crossing the boundary must happen on one consistent
   side — [chadaustin.me](https://chadaustin.me/cppinterface.html); PhysX enforces this exact rule
   by requiring the *application* to supply a `PxAllocatorCallback` implementing `allocate()`/
   `deallocate()`, so all engine-side allocation funnels through app-provided calls
   ([PhysX API Basics](https://nvidia-omniverse.github.io/PhysX/physx/5.6.0/docs/API.html)).
6. Struct layout/packing discipline: any POD struct on the boundary needs explicit, pinned
   alignment/packing, no reliance on default padding matching between toolchains.
7. Explicit calling convention on every boundary-crossing function (historically `__stdcall` on
   x86; on x64 this is moot given the single-ABI point above, but should still be pinned via
   `extern "C"` linkage discipline) — [chadaustin.me](https://chadaustin.me/cppinterface.html).
8. Version the interface surface explicitly (bgfx's integer version passed into
   `bgfx_get_interface`, or GUID-per-interface-version as COM/Slang/Diligent do) rather than
   assuming struct/vtable shape stays fixed forever.

### 1.4 Real products' choices

| Product | Boundary shape | Notes | Source |
|---|---|---|---|
| bgfx | C++ internally; generated flat C99 vtable struct (`bgfx_interface_vtbl_t`) + opaque 16-bit handles | IDL-generated to avoid hand-sync drift | [bkaradzic.github.io](https://bkaradzic.github.io/posts/idl/) |
| Diligent Engine | COM-style pure-virtual interfaces (`IObject`, `IReferenceCounters`, `RefCntAutoPtr`) | Explicitly chosen because "packed into dynamic libraries... used in any language" | [diligentgraphics.com](http://diligentgraphics.com/diligent-engine/architecture/cross-platform/reference-counting/) |
| Direct3D | COM interfaces (`IUnknown`-derived) | The original precedent COM itself was built for | [IUnknown — Wikipedia](https://en.wikipedia.org/wiki/IUnknown) |
| Slang | `ISlangUnknown` COM-lite, `ComPtr<T>` | "compatible with the COM `IUnknown`... does not require applications to use or initialize COM" | [slang.h](https://raw.githubusercontent.com/shader-slang/slang/master/include/slang.h) |
| PhysX | Abstract `Px*`-prefixed interface classes, factory functions, `release()` lifetime, app-supplied `PxAllocatorCallback` | No `new`/`delete` across the boundary; app owns allocation | [PhysX API Basics](https://nvidia-omniverse.github.io/PhysX/physx/5.6.0/docs/API.html) |
| FMOD | Flat C API (`FMOD_SYSTEM*` opaque handle) wrapping an internal C++ core; "the C header is actually a wrapper for the C++ interface" | Chosen because FMOD is closed-source and must support arbitrary compilers/languages | [FMOD API discussion](https://javierzumer.com/blog/2021/7/31/introduction-to-the-fmod-api), [Handle System docs](https://documentation.help/FMOD-Studio-API/handles.html) |
| OpenXR loader | Pure C API; per-`XrInstance` dispatch tables retrieved via `xrGetInstanceProcAddr` | Function-pointer table pattern, not vtables/classes at all | [OpenXR Loader spec](https://registry.khronos.org/OpenXR/specs/1.0/loader.html) |
| Vulkan | Pure C API; `Vulkan-Hpp` is an optional, separately maintained C++ header layered on top | C is the ABI; C++ is convenience only, never the wire format | [Khronos introduces Vulkan-Hpp](https://www.khronos.org/news/permalink/khronos-introduces-vulkan-hpp-open-source-vulkan-c-api) |

### 1.5 Assessment for RendererX

Given the constraint that the DLL is built with zig cc (`*-windows-gnu`, Itanium C++ internals,
Windows x64 calling convention) and consumed by MSVC, the reliably-safe boundary is the
**COM-lite pure-virtual-interface pattern**, not a flat C-only API and not real exported C++
classes:

- A flat C ABI (bgfx/FMOD/Vulkan/OpenXR-style) is *maximally* safe but throws away C++ ergonomics
  entirely (no `IFoo->method()` call syntax, no natural polymorphism for engine-side extension) —
  appropriate for Vulkan/OpenXR because they target C and many languages, less natural for a
  C++-first material/mesh/texture surface aimed at C++ game engines.
- Real exported C++ classes are unsafe across this specific toolchain pairing per §1.1(c)/§1.2.
- COM-lite pure-virtual interfaces (Slang's own `ISlangUnknown` pattern, Diligent's `IObject`) give
  C++ call syntax on both sides while staying inside the empirically-safe vtable subset, and this
  is *already* the pattern RendererX depends on transitively through Slang. Wrapping the interface
  vtable with a small set of `extern "C"` factory/entry functions (to avoid C++ name-mangling
  issues on the handful of boundary-crossing free functions, e.g. `rxCreateDevice`) combines both
  patterns the way Diligent and Slang both do in practice.

Recommendation (research-level, not a decision): `IMaterial`/`IShaderModule`/`ITexture`/`IMesh`
as `ISlangUnknown`-shaped COM-lite interfaces (queryInterface/addRef/release root, GUID per
interface version, factory functions returning interface pointers, app or engine-side allocator
symmetry enforced by construction), with the existing Slang COM-lite pattern as the direct
in-repo precedent to mirror rather than invent a new shape.

---

## 2. Material system design references

### 2.1 Filament's material system

Filament materials are authored offline in a JSON-like `.mat` format ("a format loosely based on
JSON that we call JSONish") with three blocks — `material { }` (metadata: parameters, shading
model, blending, etc.), optional `vertex { }`, and `fragment { }` containing raw GLSL with C++-style
comments ([Filament Materials Guide](https://google.github.io/filament/Materials.md.html)).
Parameters are declared as typed material parameters (scalars, vectors, matrices, samplers) and
surfaced to shader code as `materialParams_<name>` (for samplers) or fields of a `materialParams`
struct (everything else); **constants** are a distinct, compile-time-only specialization mechanism
("constant parameters allow the compiler to generate more efficient code," surfaced as
`materialConstant_<name>`) — i.e. Filament already separates *runtime-bound* parameters from
*compile-time* specialization inputs, which is directly relevant to how we should split
`IMaterial`'s parameter block from its permutation-selecting knobs.

Filament ships five shading models (lit/PBR, subsurface, cloth, unlit, legacy
specular-glossiness) and generates **shader variants along independent bitmask dimensions** —
directional lighting, dynamic lighting, shadow receiver, skinning, fog, VSM, SSR, stereo, etc. —
compiled offline by the `matc` tool into a **material package** containing metadata plus
platform-specific shader blobs for every variant combination the target platform needs
([Filament Materials Guide](https://google.github.io/filament/Materials.md.html)). In practice the
variant bitmask is 8 bits wide (256 theoretical combinations) but a real material only ships the
subset actually reachable — one community explanation from a Filament engineer states "128
combinations theoretically, but only 48 variants used, 80 reserved," and notes some dimensions
only affect certain stages (e.g. skinning bits are irrelevant to fragment shaders, so those
variants collapse) ([`google/filament` discussion #5410](https://github.com/google/filament/discussions/5410)).
Applications can further shrink the compiled set via a `variantFilter`/`UserVariantFilterMask` to
skip variants "the application guarantees will never be needed... reducing the overall size of
the material" and avoiding "expensive compilation of all possible variants"
([Filament Materials Guide](https://google.github.io/filament/Materials.md.html)). At runtime, a
`MaterialInstance` is "a reference to a material and a set of values for the different values of
that material," and variant selection based on scene state (shadows enabled, skinning present,
etc.) is deliberately invisible to the caller — the engine picks the pre-compiled variant, the
application never juggles permutations directly (same source).

**Portable design takeaways for RendererX**: (1) separate *bound parameters* from *specialization
constants* explicitly, at the material-definition level, not just in the shader; (2) treat
variants as an orthogonal bitmask of independent boolean/small-enum axes rather than an ad hoc
combinatorial list, so tooling can enumerate/reason about the space; (3) give the material system
an explicit filter mechanism so a specific game/engine integration can prune variants it will never
hit, because the raw cross-product is intractable to always fully compile; (4) keep permutation
selection an internal runtime concern hidden behind the material instance, not something the
engine-facing API exposes as a per-draw choice.

### 2.2 Slang's material-modularity story (v2026.x)

Two complementary language mechanisms matter for a material system: **interfaces + generics**
(Swift/Rust-protocol-flavored, not C++ templates) and **existential ("any") types + link-time
specialization**.

- **Interfaces and generics**: Slang interfaces look like C# interfaces/Swift protocols; a type
  conforms via `: InterfaceName` and must implement the declared methods, with support for default
  implementations, associated types (`associatedtype Iterator : IIterator`, resembling Swift's
  `associatedtype`/Rust's `type`), and generic constraints spelled with `where T : IFace` so the
  compiler can typecheck the generic body *before* specialization — "it is important to associate a
  generic type parameter with a type constraint" ([Interfaces and Generics — Slang user
  guide](https://shader-slang.org/slang/user-guide/interfaces-generics)). When the concrete type is
  statically known, Slang generic code specializes to zero-overhead code identical to hand-written
  code for that type; only when the concrete type genuinely cannot be resolved statically
  (interface-typed return values, heterogeneous collections) does the compiler fall back to
  runtime dynamic dispatch, and even then only for types free of opaque/resource-typed fields (same
  source).
- **Existential types / `[anyValueSize(N)]` / dynamic dispatch**: Slang's design doc for
  existential types describes encoding an interface-typed value as a bundled tuple of "a concrete
  type `T`, a witness table `W` (function pointers proving the type implements the interface), and
  the actual object value" via IR ops `makeExistential`/`extractExistential`
  ([`docs/design/existential-types.md`](https://github.com/shader-slang/slang/blob/master/docs/design/existential-types.md)).
  When creation and extraction happen within the same scope the compiler can fold this back to
  static specialization at zero cost; when the value genuinely escapes into runtime-polymorphic
  code, the witness table gives real dynamic dispatch. Because value types need to be copied by
  value, the interface declaration carries a fixed maximum payload size annotation
  (`[anyValueSize(16)] interface IFoo`, observed in Slang's own issue tracker discussing
  `DescriptorHandle<T>`/dynamic-dispatch packing:
  [`shader-slang/slang` issue #9131](https://github.com/shader-slang/slang/issues/9131)) — this is
  the mechanism that lets a material's payload be boxed generically without knowing the concrete
  material type ahead of time, at the cost of a size cap and an indirect call.
- **`ParameterBlock<T>`**: laid out by the compiler to hold "zero or one buffer for ordinary data
  and zero or one descriptor set/table for descriptors"
  ([`ParameterBlock<T>` — Slang stdlib reference](http://shader-slang.org/stdlib-reference/types/parameterblock-09/)) —
  i.e. it is Slang's native primitive for exactly the "typed parameter block bound per pass/per
  material" shape a Phase-3 `IMaterial` needs, without hand-rolled descriptor-set bookkeeping.
- **Module system + link-time specialization**: Slang modules use `export`/`extern` to control
  cross-module visibility, can be precompiled to a portable binary IR (`.slang-module`) and shipped
  independently, and are combined at runtime via `IComponentType`: `ISession::loadModule()` to load,
  `ISession::createCompositeComponentType()` to combine, and `IComponentType::link()`/
  `linkWithOptions()` to resolve cross-module references and specialize generic/existential
  parameters — explicitly documented as "a recommended approach for shader specialization compared
  to preprocessor based specialization"
  ([Compiling Code with Slang](https://shader-slang.org/slang/user-guide/compiling)). This module
  system is what lets each material's Slang source be an independently authored/compiled unit,
  combined against the engine's shared interfaces and per-pass code at link time rather than via
  textual `#include`/`#define` permutation generation.
- **Real-world material use of this pattern**: NVIDIA's Falcor research renderer already builds its
  material system directly on these Slang mechanisms: "generics with interface constraints,
  associated types, and interface/structure extensions... The path tracer in Falcor calls the
  `eval`, `sample`, and `evalPdf` material interface multiple times," and Slang more broadly is
  described as used in production/research contexts including NVIDIA Omniverse and RTX Remix
  (search-aggregated summary of NVIDIA's public materials; primary sources:
  [NVIDIA Slang Vulkanised 2024 talk (Theresa Foley)](https://vulkan.org/user/pages/09.events/vulkanised-2024/vulkanised-2024-thereas-foley-nvidia.pdf),
  [Falcor repository](https://github.com/NVIDIAGameWorks/Falcor)). This is a direct, load-bearing
  precedent for "material = Slang module implementing a material interface": it is not a novel
  idea we would be pioneering, it is the mechanism Slang's own primary sponsor built its reference
  renderer's material system on.

**The paper underpinning all of this** is He, Foley, Hofstee, Long, and Fatahalian, "Shader
Components: Modular and High Performance Shader Development," ACM Transactions on Graphics /
SIGGRAPH 2017, which introduced "shader components" — first-class modular units that bundle a unit
of shader logic together with the parameters that must be bound when that logic is used — as the
answer to modern engines organizing *parameters* into efficient frequency-based modules while
having "no corresponding primitives to organize shader logic into modules," which forces complex
shaders into "a monolithic block of parameters"
([project page](http://graphics.cs.cmu.edu/projects/shadercomp/),
[PDF](http://graphics.cs.cmu.edu/projects/shadercomp/he17_shadercomp.pdf),
[ACM DL entry](https://dl.acm.org/doi/10.1145/3072959.3073648)). Slang's interfaces/generics +
`ParameterBlock` + module system is the production evolution of exactly this idea (the paper's
components were first implemented as an extension to the Spire language, Slang's direct
predecessor).

### 2.3 Mapping `IMaterial` (inherit/override/extend) onto Slang, and hot-reload implications

- **Inherit**: a Slang `interface IMaterial { ... }` with default method implementations is the
  natural analogue of "base material behavior a concrete material can inherit" — conforming
  material modules only need to override what differs, matching Slang's documented support for
  interface default implementations
  ([Interfaces and Generics](https://shader-slang.org/slang/user-guide/interfaces-generics)).
- **Override**: ordinary Slang method overriding on a conforming struct/module, resolved statically
  whenever the engine can specialize the concrete material type at link time (the common case for a
  render-graph pass that knows its material set up front), falling back to witness-table dynamic
  dispatch only where the engine genuinely needs heterogeneous materials behind one interface
  pointer (e.g. a bindless material array walked by index) — same existential-types mechanism as
  §2.2.
- **Extend**: Slang's module/import system plus associated types gives a path for a material to
  compose in optional capability modules (e.g. an optional clear-coat extension) without the base
  interface needing to know about every possible extension up front — this is the same "retroactive
  extension"/module-composition capability referenced in the interfaces-and-generics guide.
- **Hot reload → material→pipeline caching**: because Slang's model is "load module → compose →
  link → generate target code," and linking/specialization is explicitly the step Slang recommends
  doing at *runtime* rather than via preprocessor permutation
  ([Compiling Code with Slang](https://shader-slang.org/slang/user-guide/compiling)), a hot-reload
  event is naturally scoped to "reload one module, re-run composition+link for every
  `(material, pass)` combination that referenced it, and only recompile the pipelines that changed"
  — provided the material→pipeline cache is keyed on module content hash (not just material
  identity), so a reload invalidates exactly the cache entries derived from the changed module and
  nothing else. This is a design implication, not something Slang states outright — flagged as
  synthesis rather than a direct citation.

---

## 3. Recommendation inputs: keying and caching material→pipeline permutations

### 3.1 Filament's approach (already detailed in §2.1)

Key = orthogonal bitmask of independent shading-relevant axes (lighting mode, shadow receiver,
skinning, fog, VSM, SSR, stereo, ...), computed at *material compile time* (`matc`) so the packaged
material already contains every needed permutation as a precompiled blob; runtime variant
*selection* (not compilation) is driven by renderer/scene state and is hidden inside `MaterialInstance`
([Filament Materials Guide](https://google.github.io/filament/Materials.md.html)). Filament caps
the combinatorial blow-up with an explicit variant filter mask apps can use to declare which axes
they will never need ([`variantFilter`/`UserVariantFilterMask`](https://google.github.io/filament/Materials.md.html)).
This is an **offline, exhaustive-within-a-declared-subset** strategy — it trades build-time/package
size for zero runtime shader-compile stutter.

### 3.2 Granite / Fossilize's approach

Granite (Themaister's personal Vulkan renderer, an active render-graph implementation) links its
render-graph resource/barrier tracking directly against `vkCmdPipelineBarrier`/`VkEvent`-driven
synchronization per pass, but — per the fetched render-graph deep-dive — that layer deliberately
does **not** own pipeline/shader-variant selection; it is purely about resource lifetime and
barriers ([Render graphs and Vulkan — a deep dive](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/)).
Pipeline object creation/caching is handled by a separate layer: Granite requests `Shader` objects
from its `Device` by SPIR-V blob, builds reflection via SPIRV-Cross, and links multiple `Shader`
objects into a `Program`, which in turn drives pipeline-layout and descriptor-set-allocator
construction; sort keys used for draw submission explicitly include a `pipeline_hash` alongside a
`draw_hash` (search-aggregated summary of
[`Granite/renderer/render_queue.cpp`](https://github.com/Themaister/Granite/blob/master/renderer/render_queue.cpp)).
For the actual Vulkan pipeline *creation* cost problem, Granite integrates Valve's **Fossilize**
project, which serializes "all information needed to create pipelines in a GPU and driver
independent way," so that "a Fossilize database can be shipped as part of an application to
pre-warm all historically observed pipelines... at `Vulkan::Device` creation time," explicitly to
avoid runtime pipeline-compile stutter by pre-warming hashmaps with previously observed
(shader-module + pipeline-state) combinations
([A tour of Granite's Vulkan backend – Part 6](https://themaister.net/blog/2019/05/01/a-tour-of-granites-vulkan-backend-part-6/),
[Fossilize README](https://github.com/ValveSoftware/Fossilize/blob/master/README.md)). This is a
**runtime hashmap keyed by full pipeline-state-object content, backed by an offline-recorded
warm-cache** strategy — it does not require exhaustively enumerating variants ahead of time the way
Filament does; instead it accepts first-use compilation cost once, then serializes/replays to
avoid paying it again. Granite's own blog also flags that this space (mesh-rendering permutations
specifically) is an open, ongoing problem — "permutation hell is starting to become a problem" is
explicitly discussed in a 2024 post on modernizing Granite's mesh rendering (referenced in search
results; not independently fetched in full — treat the exact wording as **UNVERIFIED** beyond the
search snippet, though the general point that permutation counts are a live pain point for Granite
is corroborated by the existence of that blog post title itself).

### 3.3 Minimal variant/specialization scheme recommendation (synthesis)

Combining the two precedents plus Slang's own mechanisms (§2.2), a Phase-3-sized scheme that does
not paint RendererX into a corner:

1. **Key pipeline variants on (material-module content hash, pass-signature hash, and a small
   orthogonal specialization-constant bitmask)** — mirroring Filament's independent-axis bitmask
   idea, but generated from the render graph's pass declarations (per-pass state: attachment
   formats, sample count, depth/stencil usage, bindless-set layout) rather than a fixed hand-curated
   axis list, since RendererX's render graph is the actual source of per-pass variability.
2. **Resolve material specialization via Slang link-time specialization, not preprocessor
   permutation** — each material is a Slang module implementing the `IMaterial`-equivalent
   interface; the render graph supplies the pass-specific `IComponentType`s (entry points,
   pass-level parameter blocks) and the engine calls `createCompositeComponentType` +
   `linkWithOptions` once per unique `(material, pass-signature)` pair, caching the resulting target
   code / `VkPipeline` keyed by that pair's hash — directly following Slang's documented
   recommended workflow
   ([Compiling Code with Slang](https://shader-slang.org/slang/user-guide/compiling)).
3. **Reserve existential/`[anyValueSize]` dynamic dispatch for the specific case that needs
   heterogeneity behind one dispatch site** — e.g. a bindless material array iterated by a single
   shared shader — rather than defaulting to it everywhere; default to static specialization
   (Filament-style precompiled permutations, but computed lazily/on-demand rather than
   ahead-of-time-exhaustive) for the common per-pass draw path, since static specialization is
   zero-overhead per Slang's own documented behavior when the concrete type is resolvable
   ([existential-types.md](https://github.com/shader-slang/slang/blob/master/docs/design/existential-types.md),
   [Interfaces and Generics](https://shader-slang.org/slang/user-guide/interfaces-generics)).
4. **Adopt Fossilize-style persistence, not Filament-style exhaustive precompilation**, for
   RendererX's actual pipeline objects (as opposed to Slang target-code generation): let variants
   compile lazily on first observed `(material, pass)` combination, hash and cache the resulting
   `VkPipeline`/target code in-process, and support serializing that cache to disk for warm-start —
   this scales better than Filament's exhaustive bitmask-product approach for a render-graph-driven
   renderer where the *pass* side of the key is generated by graph topology rather than a small
   fixed enum, and avoids ahead-of-time combinatorial blow-up between materials × passes ×
   render-graph configurations.
5. **Explicitly separate "bound parameters" (the `ParameterBlock<T>` instance data, changeable
   without recompilation) from "specialization inputs" (module choice, link-time constants,
   `[anyValueSize]` boxing decisions)** at the `IMaterial` API level, mirroring Filament's
   parameter-vs-constant split (§2.1) — this is what keeps hot material-property edits cheap
   (parameter block update only) while keeping structural material changes (shader logic swaps)
   correctly routed through the module-reload → re-link → re-cache path (§2.3).

This gives RendererX a graph-driven, lazily-populated pipeline cache keyed by content hashes
(material module + pass signature + specialization bitmask) rather than a fixed enumerated variant
space, with a documented escape hatch (existential dynamic dispatch) for the cases where static
permutation would blow up — which is the "doesn't paint us into a corner" property the question
asked for, without requiring us to build Filament's offline `matc`-equivalent tooling in Phase 3.

---

## Open items / explicitly UNVERIFIED

- No formal cross-vendor specification stating MSVC and GCC/Clang/Zig produce byte-identical
  vtables on Windows x64 for COM-legal interface shapes — this is treated as empirically proven
  (via mingw-w64's COM interoperability track record) rather than formally standardized. See §1.2.
- No named project found that explicitly states "we build our DLL with `zig cc` targeting
  `windows-gnu` and ship COM-style interfaces to MSVC consumers" as a stated architecture decision.
  The constituent facts (zig-cc/windows-gnu C-ABI compatibility with MSVC; COM-subset vtable
  interoperability between MinGW and MSVC) are each independently verified, but this exact
  combination as a citable precedent is **UNVERIFIED**.
- Exact current release patch (`v2026.14.1`) of Slang was not independently confirmed; GitHub shows
  a `v2026.14` tag (July 24) and `v2026.9.1`/`v2026.1.2` as nearby releases, consistent with a
  `v2026.14.1` patch existing but not directly observed —
  [releases list](https://github.com/shader-slang/slang/releases).
- Granite's 2024 "permutation hell" mesh-rendering blog post content was only seen via search
  snippet, not fetched directly; treat exact wording as **UNVERIFIED**, though the existence and
  general thrust of the post (permutation counts becoming a real problem for Granite) is corroborated
  by its title and by Fossilize's own stated purpose.
- Diligent Engine's own docs were fetched successfully and are treated as primary source; a couple
  of surrounding characterizations ("textbook COM-Lite implementation") came from aggregated web
  search summaries rather than a single fetched page, and are flagged as such inline.
