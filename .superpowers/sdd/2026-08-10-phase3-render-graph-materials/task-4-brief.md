### Task 4: Sample 05_multipass (shadow + forward + tonemap)

**Files:**
- Create: `samples/05_multipass/main.cpp`, `samples/05_multipass/CMakeLists.txt`, `shaders/multipass/shadow.vert.slang`, `shaders/multipass/lit.vert.slang`, `shaders/multipass/lit.frag.slang`, `shaders/multipass/tonemap.vert.slang`, `shaders/multipass/tonemap.frag.slang`
- Modify: `samples/CMakeLists.txt`, `tools/package_samples.sh` (add 05 dir), `.github/workflows/ci.yml` (gate list — follow how 04 was added), `samples/README.md`

**Scene/pipeline spec:** ground plane + one cube + one sphere (reuse the procedural mesh generation approach from `samples/03_bindless_mesh`), directional light at fixed elevation. Graph: `shadow` (depth-only, Absolute 1024×1024 D32_SFLOAT "shadowmap") → `forward` (reads "shadowmap" sampled with compare or manual PCF-1 tap; writes SwapchainRelative R16G16B16A16_SFLOAT "hdr" + D32 depth "depth"; Lambert × shadow factor, light-space matrix via push constants/bindless buffer per the 03 pattern) → `tonemap` (reads "hdr", writes backbuffer; Reinhard `c/(1+c)` then linear→sRGB is handled by the UNORM/sRGB swapchain as in existing samples — output plain Reinhard). Zero hand-written barriers/transitions in the sample (D10) — grep-able acceptance: `vkCmdPipelineBarrier2` must not appear in `samples/05_multipass/`.

**Steps:**
- [ ] **1. Shaders + sample** wired through `RenderGraph`/`Executor` + existing `FrameSync` loop (acquire → executor.execute into frame cmd → submit/present). Headless mode: fixed light/camera, 3 frames, readback, assert: (a) a specified ground-plane pixel inside the cube's shadow is darker than a specified lit ground pixel by >2×; (b) lit pixel is non-trivially bright (>0.2); (c) all readback values ≤ 1.0 (tonemapped). `--present` animates the light azimuth; `--validate` opt-in flag identical to samples 01-04.
- [ ] **2. ctest gate** (headless + `--validate`) green locally, zero validation errors.
- [ ] **3. Packaging + CI**: extend `tools/package_samples.sh` and ci.yml gate lists; per-sample dir ships the `shaders/multipass/*.slang` files + Slang runtime libs exactly as 02 does.
- [ ] **4. Both presets build; run packaged binary from an unzipped layout locally.**
- [ ] **5. Commit** `feat: add sample 05_multipass driving shadow+forward+tonemap through rx_graph`.

