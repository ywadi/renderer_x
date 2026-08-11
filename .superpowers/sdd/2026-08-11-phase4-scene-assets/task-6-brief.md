### Task 6: Present-mode control (seed 1)

**Files:** Modify `src/rx_rhi_vk/device.{h,cpp}` (`Device::setPresentMode(PresentMode)` — enum VsyncOn/VsyncOff; recreates swapchain via existing recreate path with vkb `set_desired_present_mode` ladder: VsyncOn=FIFO; VsyncOff=MAILBOX→IMMEDIATE→FIFO-with-warning [R:present]; explicit default = current behavior made explicit as VsyncOn? **No** — explicit default FIFO for samples without the flag, MAILBOX-preference removed so behavior is *chosen*, documented); all six samples gain `--vsync on|off` (default on) parsed like `--validate`; `samples/README.md` rows.
**Steps:** device-free arg-parse tests where samples have them; manual+gate verification: headless unaffected; present-mode toggle exercised in sample 07 (Task 7 consumes); recreate-on-toggle validated (resize test pattern reused); zero validation errors → commit.

