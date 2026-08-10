# Task 7 Report: sample_04_streaming (eviction-while-in-flight safety proof)

## Summary

Implemented `samples/04_streaming`: 24 procedurally-generated flat-hue
textures competing for a resident budget of 8 bindless slots, streamed
in / evicted-oldest on a timer, with the evicted `Texture2D` retired into
`rx::rhi::DeletionQueue` tagged with the current frame number rather than
destroyed on the spot. The headless gate runs 60 real frames through a
genuine 2-frames-in-flight offscreen loop (not a serialized one — real
CPU/GPU overlap is exercised on purpose, since a serialized loop would
never expose an eviction bug) with deferred readback probes at every grid
cell, asserting all 24 logical textures were observed resident at some
point and zero unexpected validation errors. `--present` mode shows the
same 24-cell grid streaming continuously at ~1 texture/second, verified
visually on real hardware (screenshots below). Both presets fully green
(9/9 ctest on linux-native and windows-cross-zig, including
`sample_04_streaming_headless`). One real bug (sRGB double-encoding) and
one real resource-leak-on-failure bug were found and fixed during
development; both are documented below and in `main.cpp`.

## 1. The eviction-while-in-flight mechanism (the actual point of this task)

**NOTE — this section describes the ORIGINAL mechanism as first
implemented; step 2 had a Critical bug (the descriptor-slot rewrite
itself was not frame-lag-protected, only the destruction was). See
"Fix round" at the end of this report for the corrected mechanism and
the full analysis — kept here, uncorrected apart from the one phrasing
fix below, as the historical record of what was reviewed.**

`evictOldestAndStreamInNext()` (`samples/04_streaming/main.cpp`):

1. `bindlessTable.release(victimHandle)` — pure host-side bookkeeping
   (per `bindless.h`'s RELEASE-SAFETY CONTRACT: never touches the GPU,
   never rewrites the descriptor, never touches the victim's
   `Texture2D`).
2. Create + upload the incoming texture, then
   `bindlessTable.registerSampledImage(...)` — this **rewrites** the
   slot's descriptor immediately, **deterministically** reusing the
   just-freed index (`BindlessTable`'s free list is LIFO — not merely
   "very likely," which is how an earlier draft of this report
   mischaracterized it; the determinism is exactly what made the
   resulting race reproducible every single eviction cycle, not a rare
   coincidence) — though the code never assumed the index would be the
   same for correctness purposes, it always used whatever handle
   `register()` actually returned.
3. The victim's **old** `Texture2D` is wrapped in a `shared_ptr` (for
   `std::function` copy-constructibility, per `deletion_queue.h`'s own
   documented pitfall) and retired via
   `deletionQueue.retire([holder]{}, currentFrameNumber)` — `
   currentFrameNumber` is the caller's `frameSync.frameNumber()` at the
   moment of eviction, **before** this frame's draws are recorded.

**Why that specific frame tag is correct (not off by one either way)** —
documented at length in `main.cpp`'s header comment
("FRAME-LAG SAFETY ARGUMENT"):
- Any command buffer submitted strictly before frame N (frame N-1 and
  earlier) was recorded while the slot still held the OLD descriptor
  contents; it may still be executing on the GPU.
- Frame N itself (and every later frame) is only ever recorded with the
  slot already holding the NEW contents (the rewrite happens before
  recording), so frame N never depends on the old resource surviving.
- This project submits every frame to exactly one graphics queue
  throughout, and Vulkan queue submissions signal fences in submission
  order — so once frame N's fence is confirmed signaled, frame N-1 (and
  everything earlier) is guaranteed to have already completed too. This
  is the same assumption every frames-in-flight double-buffering scheme
  in this codebase already relies on (`frame_sync.h`/`deletion_queue.h`).
- `DeletionQueue::retire(..., N)` only runs once `onFrameFenceSignaled(N)`
  is called, which only happens once frame N's own fence has been waited
  on and confirmed — by construction, after frame N-1 has also completed.

Headless mode does **not** use `CommandContext::runOnce()` (which is
fully synchronous, one submission at a time) — it hand-rolls a real
`FrameSync`-driven, 2-frames-in-flight loop against 2 dedicated offscreen
color images (not 1), specifically so genuine CPU/GPU overlap is exercised.
A fully-serialized "wait every frame" loop would make the
zero-validation-errors assertion vacuously true even for a broken
implementation that destroyed resources immediately — the brief's own
warning that this sample "is the entire point" was taken literally: real
overlap had to be present for the test to mean anything.

Because probing needs to know what SHOULD be on screen for a frame
recorded up to 2 frames ago (by which point live residency state has
moved on), each offscreen slot carries a `residentAtRecordTime`
snapshot captured at record time and consulted only once that slot's
fence confirms the frame is done — this is the piece that made "genuine
overlap + correct probing" both true at once without conflating them.

## 2. Grid / bindless design

24 fixed grid cells (one per **logical texture**, not per physical slot)
in a 6x4 layout under a static orthographic camera. A cell draws only
while its texture is resident; non-resident cells are skipped entirely in
`recordGridDraws()` — this is exactly what
`VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` (set on `BindlessTable`'s
bindings, `bindless.h`) makes valid: a bound descriptor set may contain
slots no draw call this frame indexes.

The shader's set 0 uses only 2 of `BindlessTable`'s 3 fixed bindings
(images + samplers, no storage buffers) — a deliberate scope reduction
from 03_bindless_mesh: this grid/camera are completely static, so there
is nothing for a double-buffered bindless transform buffer to protect;
per-object transforms are a push-constant `mvp` instead (72 logical
bytes, padded to 80 to match Slang's HLSL-cbuffer-style block-size
rounding — see finding below). `PipelineLayoutBuilder`'s external-set-0
substitution explicitly supports a "strict subset of the three slots",
so this required no new plumbing.

`BindlessTable::Capacities{sampledImages=16, samplers=4, storageBuffers=1}`
— `storageBuffers=1` exists purely because `BindlessTable::create()`
rejects any capacity of 0 outright (it always builds all 3 fixed
bindings); this shader never declares or indexes that binding at all.

No depth buffer: the 24 cells are coplanar and non-overlapping in screen
space by construction, so there is nothing for a depth test to resolve —
unlike 03_bindless_mesh's overlapping 3D objects.

## 3. Two real bugs found via direct empirical verification

**Push-constant size mismatch (would have under-covered the push range):**
`reflect()` reported the one push range as 80 bytes; a hand-computed
`sizeof(PushConstants){mat4; uint; uint;}` is 72. Root cause: Slang pads
a push-constant `ConstantBuffer<T>`'s total block size up to a 16-byte
multiple (HLSL cbuffer-style packing), which the shipped `slang.h`'s
behavior — not the research file, which didn't cover this — actually
exhibits. Fixed by adding explicit `uint32_t _padding[2]` to the C++
struct so `sizeof(PushConstants) == 80` matches exactly, and
`vkCmdPushConstants`'s declared size always covers fully-initialized
(zero-padded, via `PushConstants push{};`) bytes rather than reading past
the struct's real C++ layout. Documented in `main.cpp` next to the struct
and the reflection-count check.

**sRGB double-encoding silently defeated the headless color-probe
assertion:** initial runs showed 0/24 textures observed resident despite
correct rendering (confirmed via a debug PPM dump + pixel-exact
comparison). Root cause: `device->swapchainFormat()` — used for both the
offscreen target and the real swapchain, matching every other sample's
"one pipeline, one format" discipline — resolves to
`VK_FORMAT_B8G8R8A8_SRGB` on this development machine's NVIDIA driver.
Writing a value sampled from a plain-UNORM texture into an `_SRGB` color
attachment makes the GPU apply the linear→sRGB transfer function before
storing the encoded bytes, regardless of the sampled texture's own
format. A raw-byte readback therefore sees the sRGB-encoded fill color,
not the uploaded byte value. Verified by hand-computing the sRGB encode
of logical texture 0's fill color `(242,36,36)` → `≈(249,105,105)`,
which matched the probed pixel (after accounting for the render target's
B/R channel order) exactly. Fixed by comparing each probe against **both**
the linear fill color and its sRGB-encoded form (each in both possible
channel orders — 4 combinations total) rather than hardcoding an
assumption about which format the driver picked; this keeps the
assertion correct on any platform/driver regardless of whether it
resolves an `_SRGB` or plain `UNORM` swapchain format. Documented at
length next to `srgbEncodeColor()` in `main.cpp` since no earlier sample
had reason to discover or document this (they either compare colors only
against each other, or never assert exact expected values at all).

**Resource leak on `createScene()` failure paths (validation-caught):**
an early return path after `vkCreateSampler`/`registerSampler` succeeded
but a later step failed left the raw `VkSampler` handle undestroyed,
caught immediately by `VUID-vkDestroyDevice-device-00378` the first time
a failure path was actually exercised (during the push-constant-size
debugging above). Fixed by calling `destroyScene()` (which itself drains
the `DeletionQueue` via `flushAll()` before destroying the
pipeline/sampler) on every `createScene()` failure path reached after the
sampler is created.

## 4. Verification performed

- `ctest --preset linux-native`: 9/9 green, including
  `sample_04_streaming_headless` (~0.5s). Ran the sample binary directly
  5 consecutive times outside ctest — zero unexpected validation errors
  each run, `24 / 24 logical textures observed resident at some point`
  every time.
- `windows-cross-zig`: configures + builds cleanly (only the new sample
  needed compiling; everything else was already current); `ctest` 9/9
  green there too (the whole suite runs under Wine in this environment).
- `--present` mode: run against the real NVIDIA RTX 2080 on this
  development machine (`DISPLAY=:1`), screenshotted twice ~2s apart via
  `import -window`. Confirmed visually: 8 flat-colored grid cells,
  smoothly cycling through the HSV wheel, shifting one cell per second as
  textures stream in/evict; window closes cleanly (exit 0, "window closed
  cleanly" logged) with zero unexpected validation errors in the log.
  (An earlier screenshot attempt caught the window before its first
  Vulkan present — showing whatever was on the desktop underneath, not a
  bug in the sample — and a separate attempt was defeated by `pkill -f`
  matching its own invoking shell's command line and killing the
  capture script itself; both were tooling mistakes on my side, resolved
  by using `pkill -x` and polling for the window before adding a fixed
  post-creation buffer.)
- Compiled standalone with `-Wall -Wextra`: exactly one warning
  (`-Wmissing-field-initializers` on `Scene scene{std::move(*bindlessTable)}`),
  which is the pre-existing, already-shipped, already-reviewed pattern
  `03_bindless_mesh/main.cpp` uses identically (verified by compiling it
  the same way) — not a new defect, left consistent with that precedent.

## 5. Deviations / notes for the coordinator

- Push-constant transform instead of a bindless storage buffer (see §2)
  — a deliberate scope reduction given this sample's static grid/camera,
  not a shortcut on the sample's actual subject. Flagged in case the
  coordinator wanted every sample to exercise all 3 `BindlessTable`
  bindings; `PipelineLayoutBuilder`'s subset-compatible external-set-0
  check already supports this without any new plumbing.
- `NonUniformResourceIndex()` is never used (every index is a per-draw
  push-constant, uniform across the whole draw) — no `spirv-val` run
  required per the plan's Global Constraints, same as 03_bindless_mesh.
- No new engine-layer changes were needed at implementation time —
  `BindlessTable`, `DeletionQueue`, `Uploader`, `Texture2D`,
  `FrameSync::frameNumber()`, and `transitionImage`'s aspect parameter
  were all already exactly sufficient for this sample's needs. (One
  engine-layer *doc* change — tightening `bindless.h`'s RELEASE-SAFETY
  CONTRACT comment — was made in the fix round below, sanctioned by the
  coordinator; no engine *behavior* changed.)

## Files touched

- Create: `samples/04_streaming/main.cpp`, `samples/04_streaming/CMakeLists.txt`
- Modify: `CMakeLists.txt` (root, `add_subdirectory`), `samples/README.md`
  (new `## 04_streaming` section + build/run instructions)
- Modify (fix round): `src/rx_rhi_vk/include/rx_rhi_vk/bindless.h`
  (RELEASE-SAFETY CONTRACT doc tightening, no behavior change),
  `samples/04_streaming/main.cpp` (descriptor-rewrite fix),
  `samples/README.md` (04_streaming section restatement)

## Fix round: Critical + Important review findings

Review came back **Needs fixes**: 1 Critical + 1 Important + 1 Minor
(phrasing).

**CRITICAL — the descriptor-slot REWRITE was not frame-lag-protected,
only the destruction was.** The original `evictOldestAndStreamInNext()`
(§1 above) released the victim's handle and then immediately called
`registerSampledImage()` for the incoming texture. `BindlessTable`'s
free list is LIFO (`rx_core/handle.h`'s `HandlePool::acquire()` pops
`freeList_.back()`; `release()` pushes to `freeList_.back()`) — verified
directly against the header before writing the fix — so that
immediate register call **deterministically** rewrote the exact
just-freed physical slot via `vkUpdateDescriptorSets`. At that point
only frame N-2 was fence-confirmed; frame N-1 could still be executing
a draw recorded against the OLD descriptor in that exact slot and
dynamically sampling it. Vulkan's `UPDATE_AFTER_BIND`/`PARTIALLY_BOUND`/
`UPDATE_UNUSED_WHILE_PENDING` rules permit rewriting only descriptors
NOT dynamically used by pending command buffers — this one was. Bounded
consequence (the new texture's data is always fully uploaded before its
descriptor is ever written, so this was never a use-after-free — a
stale in-flight frame would silently sample the new texture's data one
frame early), but it is a genuine spec violation, invisible to
validation layers, and it directly contradicted this sample's own
stated purpose.

**Fix:** both halves of an eviction — destroying the victim's old
`Texture2D` AND registering the incoming texture into the (deterministically
reused) slot — are now deferred into ONE `DeletionQueue::retire()`
call, tagged with the same `currentFrameNumber` as before. Concretely,
`evictOldestAndStreamInNext()` now: releases the victim's handle and
clears its grid-cell bookkeeping immediately (host-side only, still
safe per the RELEASE-SAFETY CONTRACT); creates + uploads the incoming
texture's `Texture2D` immediately (a brand-new `VkImage` has no
descriptor slot at all yet, so there is no in-flight hazard whatsoever
in that half); then retires ONE callback, tagged with
`currentFrameNumber`, that calls `registerSampledImage()` for the
incoming texture and only THEN marks its grid cell resident (adds it to
`residencyOrder`, wires up `scene.textures[incoming]`). `LogicalTextureSlot::texture`
changed from `std::optional<Texture2D>` to `std::shared_ptr<Texture2D>`
so both the outgoing (destroy) and incoming (register) textures could
be captured into that one retire callback without an extra wrap step.
Documented at length in `main.cpp`'s header comment under a new
"DESCRIPTOR REWRITE SAFETY" section (added alongside the existing
"FRAME-LAG SAFETY ARGUMENT," which the destruction half already had
right, per the review). Net effect: the incoming texture's grid cell
becomes resident/drawable 2 frames after the eviction decision, not
immediately — this shifts the sample's internal timing but not its
headless-gate assertion (still "every one of the 24 textures observed
resident at some point"); re-verified the margin is still comfortable
(the last never-before-seen texture, index 23, becomes visible at frame
50 of a 60-frame run — 10 frames of margin before the run ends) and
confirmed empirically (still `24 / 24` every run, see Verification
below) rather than trusting the arithmetic alone.

**IMPORTANT — `bindless.h`'s RELEASE-SAFETY CONTRACT overclaimed.**
Tightened the comment (lines ~112-125 pre-fix) to state the missing
load-bearing condition explicitly: rewriting a bound descriptor via
`UPDATE_AFTER_BIND | PARTIALLY_BOUND` is valid only for a slot NOT
dynamically used by a pending command buffer, release()-then-register()
into the same LIFO-freed index is the deterministic (not rare) case
this matters for, and callers must defer the rewrite itself to a
fence-confirmed point (DeletionQueue-style) — pointing at
`samples/04_streaming` as the worked example. Doc-only change, no
behavior change, sanctioned by the coordinator as an engine-file edit
in scope for this task's fix round.

**MINOR — report phrasing.** §1 above originally said the immediate
`registerSampledImage()` call was "very likely" reusing the just-freed
index; corrected in place to "deterministically" (LIFO free list), which
is exactly what made the race reproducible every cycle rather than an
occasional flake.

**Verification after the fix round:**
- Compiled clean (`cmake --build --preset linux-native --target
  sample_04_streaming`); the `bindless.h` doc-only change triggered a
  rebuild of `rx_rhi_vk`, `rx_rhi_vk_tests`, and `sample_03_bindless_mesh`
  (headers are transitively included) — all still built clean.
- Headless gate re-run 5 consecutive times directly: `24 / 24 logical
  textures observed resident at some point` and zero unexpected
  validation errors every time (matches the pre-fix baseline exactly).
- `ctest --preset linux-native`: 9/9 green (including
  `rx_rhi_vk_tests` and `sample_03_bindless_mesh_headless`, confirming
  the `bindless.h` doc edit didn't regress anything it touches
  transitively).
- `cmake --build --preset windows-cross-zig` + `ctest --preset
  windows-cross-zig`: 9/9 green.
- `-Wall -Wextra` standalone compile: exactly the same one pre-existing
  warning as before the fix (`-Wmissing-field-initializers` on the
  `Scene scene{...}` aggregate init, identical to `03_bindless_mesh`'s
  own shipped pattern) — no new warnings from the restructuring.
- `--present` mode re-run on the same real hardware (NVIDIA RTX 2080,
  `DISPLAY=:1`), screenshotted twice ~2s apart: visually identical grid
  cycling behavior to the pre-fix screenshots, zero unexpected
  validation errors, clean shutdown.
- `git log --format='%B'` grepped for AI-attribution strings on the fix
  commit(s): no matches.

No further deviations.
