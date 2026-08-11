#pragma once
#include <volk.h>

// tracy_gpu.h -- the ONLY header in this codebase that may include a Tracy
// Vulkan header (<tracy/TracyVulkan.hpp>) [Phase 4 Stage 0 Task 3, spec D3].
// This mirrors rx_core/profile.h's own rule for CPU zones, extended to
// rx_rhi_vk's Vulkan-coupled GPU-zone seam: every OTHER rx_rhi_vk/rx_graph
// header (device.h, executor.h, ...) stays Tracy-free; a .cpp needing a GPU
// zone includes THIS header, never <tracy/TracyVulkan.hpp> directly. Two
// seams, each the sole owner of its own domain -- GPU zones belong here,
// not in rx_core, which stays Vulkan-free.
//
// TRACY_ENABLE is the identical PUBLIC compile definition rx_core/profile.h
// documents (propagated transitively from src/rx_core/CMakeLists.txt's
// own `if(RX_TRACY)` guard). When it is OFF, <tracy/TracyVulkan.hpp> is not
// even reachable on the include path (Tracy was never fetched -- see
// third_party/CMakeLists.txt's own `if(RX_TRACY)` guard), so this header
// must not name it at all in that branch -- every macro below is defined as
// a bare no-op directly instead, and GpuProfileContext degrades to a plain
// `void*` that createGpuProfileContext() always returns null for. Callers
// (executor.cpp) never need their own `#ifdef TRACY_ENABLE`: every symbol
// below exists and is safe to call unconditionally in both configurations.
#ifdef TRACY_ENABLE
#include <tracy/TracyVulkan.hpp>
#endif

namespace rx::rhi {

class Device;

#ifdef TRACY_ENABLE

using GpuProfileContext = TracyVkCtx;

// RX_GPU_ZONE_DYNAMIC(ctx, varname, cmd, nameText) -- GPU zone spanning the
// rest of the enclosing block, named from a RUNTIME string (not required to
// be a compile-time literal) [task-3 dispatch item 4: per-pass zones keyed
// by a render-graph pass's own std::string-backed name]. Tracy's own
// documented mechanism for exactly this case is the "Transient GPU zone"
// (Tracy user manual, "Transient GPU zones": "Transient zones... are
// available in OpenGL, Vulkan, Direct3D 11/12 and WebGPU macros" -- verified
// directly against the vendored v0.14.0 manual/tracy.tex before writing
// this) -- TracyVkZoneTransient(ctx, varname, cmdbuf, name, active), whose
// `name` parameter is a runtime `const char*` Tracy copies into its own
// buffer, unlike TracyVkZone's compile-time-literal `name`. `varname` names
// the RAII scope variable (needed because more than one GPU zone macro in
// one scope would otherwise collide on Tracy's own default variable name --
// see the manual's "Multiple zones in one scope" section).
//
// `active` is deliberately computed here as `(ctx) != nullptr`, NOT a bare
// `true`: verified directly against the vendored v0.14.0 source
// (TracyVulkan.hpp's VkCtxScope constructor) that `active == false` is what
// makes that constructor return immediately, BEFORE it would otherwise
// unconditionally dereference `ctx` (`ctx->NextQueryId()`, no null-guard of
// its own) -- a null ctx with `active` hardcoded true would be a real
// null-pointer-dereference crash the moment createGpuProfileContext()
// returns null (RX_TRACY off, or any context-creation failure). Folding the
// null check into `active` here is what makes this macro genuinely safe to
// call with a null `ctx`, matching this header's own "safe on a null
// context" claim on createGpuProfileContext() above.
//
// COST AND THE TRACY_ON_DEMAND TRADEOFF [task-3 fix round 1, Medium
// finding] -- the GPU-side twin of the identical note on
// rx_core/profile.h's RX_ZONE_DYNAMIC_NAME; read that one for the full
// explanation, this is the short version. The dynamic-name `VkCtxScope`
// constructor this macro drives (public/tracy/TracyVulkan.hpp) calls
// `Profiler::AllocSourceLocation(...)` -- a real `tracy_malloc()` +
// `memcpy()` -- on every invocation, unconditionally, UNLESS
// `TRACY_ON_DEMAND` changes its `m_active` gate to
// `is_active && GetProfiler().IsConnected()`. This project's vendored
// Tracy is built with `-DTRACY_ON_DEMAND=ON` (third_party/CMakeLists.txt)
// specifically so this per-pass GPU zone (like its CPU-side counterpart)
// only pays that allocation cost while a profiler is actually connected
// and measuring -- disconnected, `active` is already false from the
// `(ctx) != nullptr` computation above being combined with the connection
// check Tracy itself adds, so the constructor returns before allocating
// (and before even the `vkCmdWriteTimestamp` call). Same disclosed
// tradeoff as the CPU-side macro: on-demand means no pre-connection
// buffering, so a profiler attaching mid-run sees GPU zones from that
// point forward only.
#define RX_GPU_ZONE_DYNAMIC(ctx, varname, cmd, nameText) \
    TracyVkZoneTransient(ctx, varname, cmd, nameText, (ctx) != nullptr)

// RX_GPU_COLLECT(ctx, cmd) -- collects this frame's GPU timestamp queries
// (Tracy's TracyVkCollect, `ctx->Collect(cmd)` -- no null-guard of its own,
// verified directly against the vendored v0.14.0 source, same as
// VkCtxScope's constructor above) -- call once per frame, once every
// TracyVkZone/RX_GPU_ZONE_DYNAMIC this frame recorded on `cmd` has closed.
// Per Tracy's manual: `cmd` must be in the recording state and OUTSIDE a
// render pass instance (i.e. after every vkCmdEndRendering this frame has
// already run). The null check is wrapped INSIDE this macro (do/while(0)
// so it still behaves like one statement at the call site) so a null `ctx`
// (RX_TRACY off already routes here to nothing, but also any
// createGpuProfileContext() failure while RX_TRACY is on) is silently
// skipped rather than dereferenced.
//
// COST [task-3 fix round 1, Low finding]: `VkCtx::Collect()`'s own body
// (public/tracy/TracyVulkan.hpp, verified directly) does a real
// `vkGetQueryPoolResults` readback every call when there is pending data --
// which there always is, every frame, once any pass has recorded a GPU
// zone -- UNLESS `TRACY_ON_DEMAND` is defined, in which case `Collect()`
// gains its own `if (!GetProfiler().IsConnected()) { <cheap query-pool
// reset only>; return; }` early-out before ever touching the real readback
// path. Covered by the identical `-DTRACY_ON_DEMAND=ON` build flag as
// RX_GPU_ZONE_DYNAMIC above -- no additional wiring needed here.
#define RX_GPU_COLLECT(ctx, cmd) \
    do {                         \
        if ((ctx) != nullptr) {  \
            TracyVkCollect(ctx, cmd); \
        }                        \
    } while (0)

// RX_GPU_CONTEXT_DESTROY(ctx) -- destroys a context created by
// createGpuProfileContext() below (Tracy's TracyVkDestroy, `ctx->~VkCtx()`
// -- no null-guard of its own either). Same null-safe wrapping as
// RX_GPU_COLLECT above, for the identical reason.
#define RX_GPU_CONTEXT_DESTROY(ctx) \
    do {                            \
        if ((ctx) != nullptr) {     \
            TracyVkDestroy(ctx);    \
        }                           \
    } while (0)

#else

using GpuProfileContext = void*;

#define RX_GPU_ZONE_DYNAMIC(ctx, varname, cmd, nameText)
#define RX_GPU_COLLECT(ctx, cmd)
#define RX_GPU_CONTEXT_DESTROY(ctx)

#endif  // TRACY_ENABLE

// Creates `device`'s Tracy GPU-zone context [spec D3]: TracyVkContextCalibrated
// when VK_EXT_calibrated_timestamps was enabled on `device`'s logical device
// at creation time (device.calibratedTimestampsEnabled(); see device.cpp's
// own optional, guarded enable_extension_if_present() call -- this function
// never touches Vulkan device-creation itself), else plain TracyVkContext.
// Returns nullptr when RX_TRACY/TRACY_ENABLE is off, or on any underlying
// context-creation failure -- RX_GPU_ZONE_DYNAMIC/RX_GPU_COLLECT/
// RX_GPU_CONTEXT_DESTROY above are all safe on a null context regardless
// (compiled away entirely when TRACY_ENABLE is off; each wraps its own null
// check when it is on), so callers never need their own #ifdef TRACY_ENABLE
// or null check.
//
// Allocates and fully consumes one short-lived VkCommandPool/VkCommandBuffer
// of its own (against `device`'s graphics queue/family) to perform the
// handful of calibration submissions Tracy's context constructor issues
// synchronously -- neither is retained by the returned context or by this
// function past return (Tracy's own manual: "[the command buffer] will be
// in the executable state on exit from the initialization function"; the
// VkCtx type itself keeps no member referencing the queue/cmdbuf it was
// constructed with, verified directly against the vendored v0.14.0 source
// before writing this).
[[nodiscard]] GpuProfileContext createGpuProfileContext(Device& device);

}  // namespace rx::rhi
