#pragma once
// Thread-affinity (D5, Phase 4): the plain data types below (RxMemoryReport
// and friends) carry no thread affinity at all -- they are snapshots, safe
// to copy/read from anywhere. MemoryAccounting's record()/release() are
// internally thread-safe (atomic counters) but are, in practice, only ever
// called from Buffer/Texture2D create/destroy on the main thread, per the
// existing GPU-object-mutation contract -- see docs/threading.md.
// Allocator::report()/setCurrentFrameIndex() (buffer.h) follow that same
// convention (called once per frame from the main/render thread, alongside
// FrameSync's own per-frame bookkeeping).
//
// volk first, for VkDeviceSize/VkResult/VK_MAX_MEMORY_HEAPS etc -- this
// header intentionally does NOT include <vk_mem_alloc.h> (unlike buffer.h/
// texture.h): everything declared here is meant to be a plain, VMA-free,
// host-facing surface (D24(c): "a host-facing POD memory report... feeding
// HUD + Tracy plots") that a caller (a sample's HUD, the future SDK caps
// surface) can consume without pulling in VMA at all. The code that
// actually calls vmaGetHeapBudgets()/vmaSetCurrentFrameIndex() to BUILD an
// RxMemoryReport lives in rx_rhi_vk/buffer.h's Allocator (buffer.cpp),
// which already depends on VMA for other reasons.
#include <volk.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rx::rhi {

// [Phase 4 Task 10, spec D24(a)] Every GPU allocation this engine makes is
// attributed to exactly one of these five categories at the point it is
// created -- VMA itself has no category concept (verified against the
// vendored VMA 3.4.0 header: its nearest primitive is a per-allocation
// debug NAME string, never aggregated into anything), so this enum plus
// MemoryAccounting below is this engine's own ledger, threaded through the
// five VMA/Vulkan allocation choke points in buffer.cpp/texture.cpp.
//
// Threading (D24/D5): callers of Allocator::createHostVisibleBuffer()/
// createDeviceLocalBuffer()/createUploadRingBuffer()/Texture2D::create()
// pass a category explicitly (default MemoryCategory::Internal for callers
// that don't yet care -- see buffer.h's own comment on why the default is
// deliberately generic, not a guess at a "real" category). GeometryPool
// (Task 12) and TextureCache (Task 14) are the two NOT-YET-BUILT
// subsystems this ticket's own gate matrix (#27, row 10) names as the
// intended real GeometryPool/Texture taggers; Uploader's staging ring
// buffer (upload.cpp) is the one EXISTING call site this task itself wires
// to MemoryCategory::Staging.
enum class MemoryCategory : uint8_t {
    GeometryPool = 0,
    Texture = 1,
    Transient = 2,
    Staging = 3,
    Internal = 4,
};

inline constexpr size_t kMemoryCategoryCount = 5;

// Human-readable name for logging/HUD use -- covers every MemoryCategory
// value; "UNKNOWN" is unreachable for any value produced by this enum but
// kept as a defensive default rather than UB on an out-of-range cast.
const char* memoryCategoryName(MemoryCategory category);

// Bytes/count currently attributed to one MemoryCategory, per
// MemoryAccounting's own ledger below.
struct MemoryCategoryStats {
    uint64_t bytes = 0;
    uint32_t count = 0;
};

// [Phase 4 Task 10, gate ruling #27] Which of the two budget SOURCES a
// report's per-heap `budgetBytes` numbers came from -- explicit, not
// inferred by the reader from whether the numbers "look real": either the
// genuine `VK_EXT_memory_budget` driver query (RealExtension), or this
// engine's own heap-size estimate when that extension is unavailable or
// was deliberately not enabled (HeapSizeFallback). See Allocator::report()
// (buffer.cpp) for exactly how each is computed.
enum class MemoryBudgetSource : uint8_t {
    RealExtension,
    HeapSizeFallback,
};

const char* memoryBudgetSourceName(MemoryBudgetSource source);

// [Phase 4 Task 10, gate ruling #27] Per-heap usage/budget -- NEVER a
// fixed-count array: RADV APUs (this project's own Steam Deck floor
// hardware) expose multiple memory heaps by default (verified via Mesa's
// `radv_enable_unified_heap_on_apu` driconf option, matrix-issue27 row 11
// -- its very existence proves RADV's APU default is multi-heap), so
// RxMemoryReport::heaps below is sized to whatever
// VkPhysicalDeviceMemoryProperties::memoryHeapCount actually reports on
// the running device, never assumed.
struct MemoryHeapReport {
    uint32_t heapIndex = 0;
    VkDeviceSize heapSizeBytes = 0;
    VkDeviceSize usageBytes = 0;
    VkDeviceSize budgetBytes = 0;
};

// [Phase 4 Task 10, spec D24(c)] Host-facing POD memory report: per-category
// bytes/counts (MemoryAccounting's ledger, snapshotted), per-heap usage/
// budget (vmaGetHeapBudgets()), and the explicit budget-source flag above.
// Precedent: bgfx's `struct Stats` (bgfx.h) ships an analogous flat POD
// (textureMemoryUsed/gpuMemoryUsed/gpuMemoryMax alongside frame-timing
// fields) read by the host once per frame -- this struct is the same shape,
// widened to per-category (bgfx has only texture/rt/transient) and
// per-heap (bgfx's gpuMemoryUsed/gpuMemoryMax are single aggregate
// numbers) plus the explicit source flag bgfx's own docs flag as
// ambiguous ("unavailable on some backends", no fallback signal) on
// purpose. Copyable, trivially comparable field-by-field; safe to snapshot
// and hand to a HUD or a log line without holding onto any live Vulkan
// state.
struct RxMemoryReport {
    std::array<MemoryCategoryStats, kMemoryCategoryCount> categories{};
    std::vector<MemoryHeapReport> heaps;
    MemoryBudgetSource budgetSource = MemoryBudgetSource::HeapSizeFallback;

    const MemoryCategoryStats& category(MemoryCategory cat) const {
        return categories[static_cast<size_t>(cat)];
    }
};

// Compact, single-line summary of `report` for a log message (used by
// Allocator::noteAllocationFailure() below to satisfy D24(d)'s "loud, named
// failure with the report attached" -- the report is literally inlined
// into the same error line, plus captured verbatim for programmatic
// inspection via Allocator::lastAllocationFailureReport()).
std::string summarizeMemoryReport(const RxMemoryReport& report);

// [Phase 4 Task 10, spec D24(a)] The ledger itself: an atomic
// bytes/count counter per MemoryCategory. Deliberately Vulkan-free and
// side-effect-free beyond its own counters -- record()/release() are
// called by Buffer/Texture2D at the moment their underlying VMA allocation
// is created/destroyed (buffer.cpp/texture.cpp), each holding a
// std::shared_ptr<MemoryAccounting> back to the Allocator that created
// them so the ledger survives exactly as long as any resource still
// attributed to it -- not tied to the owning Allocator object's own
// lifetime, which can move.
//
// Thread-safety: every counter is a std::atomic, so record()/release() are
// safe to call from any thread -- defense in depth. Production code today
// only ever creates/destroys Buffer/Texture2D from the main thread (D5,
// existing GPU-object-mutation contract; see docs/threading.md), so this
// is not a relaxation of that contract, just a cheap extra guarantee.
class MemoryAccounting {
public:
    MemoryAccounting() = default;
    MemoryAccounting(const MemoryAccounting&) = delete;
    MemoryAccounting& operator=(const MemoryAccounting&) = delete;

    void record(MemoryCategory category, uint64_t bytes);
    void release(MemoryCategory category, uint64_t bytes);

    MemoryCategoryStats snapshot(MemoryCategory category) const;

    // True iff any category still carries a nonzero count -- the
    // teardown leak/balance check (D24, matrix-issue27 row 16):
    // Allocator's destructor (buffer.cpp) calls this and logs a named
    // RX_LOG_ERROR naming every nonzero category if it is ever true at the
    // point an Allocator is torn down, mirroring DeletionQueue's own
    // "destroyed with N never flushed" destructor warning
    // (deletion_queue.h/.cpp) -- the same established pattern in this
    // codebase, not a new one.
    bool hasOutstanding() const;

private:
    struct Counter {
        std::atomic<uint64_t> bytes{0};
        std::atomic<uint32_t> count{0};
    };

    std::array<Counter, kMemoryCategoryCount> counters_{};
};

// [Phase 4 Task 10, spec D24(d)] Which class of failure an allocation
// call ended in -- named and distinguishable, replacing every allocation
// site's previous generic "vmaCreateBuffer failed" catch-all (verified:
// zero OOM-specific handling existed anywhere in this repository before
// this task, buffer.cpp:59-64/90-96/126-131, texture.cpp:149-153).
enum class AllocationFailureKind : uint8_t {
    None,
    OutOfDeviceMemory,
    OutOfHostMemory,
    Other,
};

const char* allocationFailureKindName(AllocationFailureKind kind);

// Pure classification, deliberately independent of any live
// VkDevice/Allocator -- lets AllocationFailureKind's mapping be unit
// tested device-free (oom_handling_test.cpp) even though forcing a REAL
// vmaCreateBuffer/vmaCreateImage failure needs a live device (see that
// test file's own comment on `VmaAllocatorCreateInfo::pHeapSizeLimit`,
// the mechanism this task's own GPU tests use for that).
AllocationFailureKind classifyAllocationFailure(VkResult result);

// ---------------------------------------------------------------------
// EVICTION CONTRACT (D24(e), card #27, gate ruling 2026-08-18)
// ---------------------------------------------------------------------
// This task owns the CONTRACT TEXT below plus a minimal synthetic proof
// that the mechanism composes (src/rx_rhi_vk/tests/eviction_contract_test.cpp).
// It does NOT implement real call sites -- those are Task 13's
// (rx::asset::Registry mesh()/material()/texture() accessors) and Task
// 19's (DrawListBuilder's material/mesh handle resolution) acceptance
// criteria, implemented AGAINST these exact rules:
//
// (1) RESIDENCY-TOLERANT RESOLVE. A resolve call on a handle whose
//     resource has been evicted returns a DEFINED fallback (the D11
//     fallback-asset pattern), never a dangling reference and never UB.
//     "Evicted" here means: the handle is still a validly-issued,
//     non-stale handle (see (3)), but the resource it names is
//     temporarily unavailable because it was evicted and has not yet
//     been reclaimed.
//
// (2) OBSERVABLE NON-RESIDENCY. The resolve call's signature makes
//     non-residency observable to the caller -- a bool/enum
//     out-parameter or equivalent return shape -- never a silent
//     substitution the caller has no way to detect. (A caller that
//     doesn't check the flag still gets a safe, defined fallback value
//     per (1); a caller that DOES check it can react, e.g. trigger a
//     reload.)
//
// (3) ONE GENERATIONAL MECHANISM, NOT TWO. Once an eviction is actually
//     RECLAIMED (the slot freed, the handle's generation bumped -- see
//     (c) below), a resolve against the OLD (pre-eviction) handle is
//     caught by the SAME generational-handle staleness check the D6
//     Handle<Tag>/HandlePool<Tag,T> pattern already uses for an
//     ordinary dangling handle (rx_core/include/rx_core/handle.h) -- NOT
//     a second, bespoke "was this specific handle ever evicted"
//     side-table. The only state genuinely new to eviction is the
//     transient "evicted but not yet reclaimed" flag a resolve consults
//     during the deferred window between evict() and reclaim -- once
//     that window closes, staleness detection reduces to the existing
//     D6 mechanism with zero new bookkeeping.
//
// (b) HANDLE-MEDIATED REFERENCES ONLY. No new Phase-4 API surface may
//     return or accept a raw pointer or bare integer index to a
//     pool-resident/evictable resource for a caller to hold onto past
//     the call that produced it -- every persisted reference to an
//     evictable resource is a generational handle (core::Handle<Tag>).
//     Enforced at review, not by an automated test -- this text is the
//     exact wording Task 13/19/21/3/14 reviewers check new API surfaces
//     against.
//
// (c) WORKING DEFERRED-EVICTION PATH. evict(handle) (1) immediately
//     marks the handle's slot non-resident (so (1)/(2) above hold from
//     that instant), (2) retires the underlying GPU resource's real
//     destructor into the existing rx::rhi::DeletionQueue
//     (deletion_queue.h) tagged with the current frame index, and (3)
//     only frees the slot for reuse (bumping its generation, per (3)
//     above) once DeletionQueue::onFrameFenceSignaled() actually runs
//     that retired destructor -- i.e. the resource is provably NOT
//     destroyed while a frame that may still reference it is in flight,
//     using the exact fence-gated mechanism this codebase already ships
//     for buffer/image teardown, not a new one.
//
// src/rx_rhi_vk/tests/eviction_contract_test.cpp builds a MINIMAL
// synthetic handle table (real rx::core::HandlePool<Tag,T> + a real
// rx::rhi::DeletionQueue, no Vulkan device needed at all) and exercises
// resolve-while-resident, resolve-while-evicted-but-not-yet-reclaimed,
// resolve-after-reclaim, and slot-reuse-after-reclaim end to end against
// clauses (1)-(3)/(c) above -- this is the "contract" test this task
// owns; Task 13/19 add their own resolve-call-site tests against real
// assets/draw lists.

}  // namespace rx::rhi
