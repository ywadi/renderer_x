#pragma once

// rx_core/profile.h -- the engine's ONLY CPU-zone Tracy seam [spec D3,
// Phase 4 Stage 0 Task 3]. Every other rx_* public header must consume
// profiling exclusively through the RX_ZONE*/RX_FRAME_MARK/RX_PLOT macros
// below, never by naming a Tracy type/macro directly -- this is the ONE
// header, project-wide, that may #include a Tracy CPU header. (rx_rhi_vk's
// tracy_gpu.h is the separate, equally exclusive seam for the Vulkan
// GPU-zone API -- TracyVulkan.hpp is never named outside that one file
// either. Two seams, each the sole owner of its own domain, not one seam
// covering both -- GPU-zone macros are inherently Vulkan-coupled and belong
// in rx_rhi_vk, not rx_core, which stays Vulkan-free.)
//
// TRACY_ENABLE is a PUBLIC compile definition on rx_core (see
// src/rx_core/CMakeLists.txt), set if and only if the top-level RX_TRACY
// CMake option is ON (third_party/CMakeLists.txt's own `if(RX_TRACY)` guard
// decides whether Tracy is even fetched/built at all). Because rx_core is a
// public, transitive dependency of every other library and sample in this
// engine, that one definition point is what makes every RX_ZONE*/
// RX_FRAME_MARK/RX_PLOT call site in the whole codebase agree on whether
// Tracy is compiled in, with no per-library wiring required.
//
// When TRACY_ENABLE is OFF, <tracy/Tracy.hpp> is not even reachable on the
// include path -- the dependency was never fetched (RX_TRACY was OFF at
// configure time) -- so this header must not name a Tracy header or symbol
// at all in that branch. The #else block below defines every macro as a
// bare no-op directly, matching Tracy's own "compiles to nothing when
// disabled" contract (public/tracy/Tracy.hpp's own `#ifndef TRACY_ENABLE`
// branch, verified directly against the vendored v0.14.0 tag before writing
// this) bit for bit, without depending on that header being present.
#ifdef TRACY_ENABLE

#include <tracy/Tracy.hpp>

// RX_ZONE -- scoped CPU zone for the remainder of the enclosing block,
// named automatically after the enclosing function (Tracy's own
// ZoneScoped: records function name, source file, and line at the call
// site). Statement-style, no parentheses at the call site (`RX_ZONE;`),
// mirroring Tracy's own ZoneScoped convention exactly -- this is an
// object-like macro, not a function-like one, so it can expand to a local
// RAII variable declaration.
#define RX_ZONE ZoneScoped

// RX_ZONE_NAMED(nameLiteral) -- scoped CPU zone with a fixed, compile-time
// display name (Tracy's ZoneScopedN; `nameLiteral` must be a string literal
// with static storage duration -- Tracy retains only the pointer, never
// copies the text). Use RX_ZONE_DYNAMIC_NAME below, inside the same scope,
// to additionally attach a runtime-known name to this same zone.
#define RX_ZONE_NAMED(nameLiteral) ZoneScopedN(nameLiteral)

// RX_ZONE_DYNAMIC_NAME(text, size) -- sets a per-call, runtime-known name
// on the CURRENTLY ACTIVE zone (i.e. the nearest enclosing RX_ZONE/
// RX_ZONE_NAMED in this same scope) [task-3 dispatch item 4: per-pass zones
// keyed by a render-graph pass's own std::string-backed name, which is
// never a compile-time literal]. Tracy's own documented idiom for exactly
// this case (Tracy user manual, "Marking frames" -> zone-naming
// subsection: "If you want to set zone name on a per-call basis, you may
// do so using the ZoneName(text, size) macro" -- verified directly against
// the vendored v0.14.0 manual/tracy.tex before writing this). `text` need
// not be null-terminated (an explicit `size` is always passed); Tracy
// copies the bytes into its own buffer, unlike a zone's static NAME
// literal above, so `text`'s lifetime need only cover this call, not the
// zone's own duration. The one addition to the 4-macro list this task's
// resolutions named (RX_ZONE/RX_ZONE_NAMED/RX_FRAME_MARK/RX_PLOT) -- added
// because dispatch item 4 explicitly requires dynamic per-pass zone naming,
// and profile.h is this project's one CPU-zone include point, so the
// wrapper belongs here rather than as a raw Tracy call in a .cpp.
//
// COST AND THE TRACY_ON_DEMAND TRADEOFF [task-3 fix round 1, Medium
// finding] -- read this before adding a new call site. Unlike every other
// macro on this page, this one is NOT a bare ring-buffer write: Tracy's own
// underlying `ScopedZone::Name()` (public/client/TracyScoped.hpp, verified
// directly against the vendored v0.14.0 source) does a real
// `tracy_malloc()` + `memcpy()` on every call, because the text is a
// runtime value Tracy must copy rather than a literal it can retain by
// pointer. Left unguarded, that would run on every call regardless of
// whether anything is listening -- directly contradicting D3's "passive
// (~no cost) until a profiler connects" for this one macro, which is
// exactly what an earlier pass of this task shipped and a fix-round review
// caught. Fixed at the dependency-build level, not here: this project's
// vendored Tracy is built with `-DTRACY_ON_DEMAND=ON`
// (third_party/CMakeLists.txt), which changes `ScopedZone`'s (and every
// zone type's) `m_active` gate from a bare `is_active` to
// `is_active && GetProfiler().IsConnected()`, AND makes `Name()` itself
// re-check the live connection id before doing any work -- so with no
// profiler attached, this macro now costs a few branches and returns,
// never reaching `tracy_malloc`. The allocation only happens WHILE a
// profiler is actually connected and actively measuring, which is the
// accepted, intended cost of real-time profiling, not a design flaw.
//
// The tradeoff that comes with `TRACY_ON_DEMAND`, disclosed rather than
// hidden: the client buffers nothing before a profiler connects, so
// capture HISTORY STARTS AT CONNECTION TIME -- attaching mid-run shows
// zones from that moment forward only, never retroactively. Acceptable for
// this project's own use (interactive local profiling sessions, not
// after-the-fact forensic capture of a run nobody was watching), and
// matches Tracy's own documented on-demand model exactly.
#define RX_ZONE_DYNAMIC_NAME(text, size) ZoneName(text, size)

// RX_FRAME_MARK -- marks the end of one rendered frame (Tracy's FrameMark).
// Statement-style, no parentheses (`RX_FRAME_MARK;`), exactly like RX_ZONE
// above and Tracy's own FrameMark. Call once per frame, in every sample's
// frame loop (present and headless paths alike) [task-3 dispatch item 4].
#define RX_FRAME_MARK FrameMark

// RX_PLOT(nameLiteral, value) -- plots a named numeric time series (Tracy's
// TracyPlot; `nameLiteral` must be a string literal with static storage
// duration, same requirement as RX_ZONE_NAMED's name). `value` may be any
// type TracyPlot itself accepts (integral or floating-point).
#define RX_PLOT(nameLiteral, value) TracyPlot(nameLiteral, value)

#else

#define RX_ZONE
#define RX_ZONE_NAMED(nameLiteral)
#define RX_ZONE_DYNAMIC_NAME(text, size)
#define RX_FRAME_MARK
#define RX_PLOT(nameLiteral, value)

#endif  // TRACY_ENABLE
