### Task 3: Tracy integration

**Files:** Create `src/rx_core/include/rx_core/profile.h` (RX_ZONE macros wrapping Tracy, no-op when disabled); vendor Tracy client (pinned tag, TRACY_ENABLE option, ON in dev presets); modify frame-path files to add zones (FrameSync acquire/present, Executor::execute + per-pass zones using pass names, MaterialSystem::getPipeline/loadMaterial, Uploader submits); GPU ctx: `src/rx_rhi_vk/tracy_gpu.{h,cpp}` — TracyVkContextCalibrated when VK_EXT_calibrated_timestamps present else TracyVkContext; collect per frame.
**Constraints:** zones are cheap macros — no allocations, no behavior change when disconnected; GPU ctx guarded by extension query (lavapipe support empirically checked in-task and documented either way); windows-cross build verified.
**Steps:** vendor+option wiring → zone macros + placements → GPU ctx guarded → verify: connect Tracy locally, capture a sample-05 run, screenshot/txt evidence in report; suite green both presets → commit.

