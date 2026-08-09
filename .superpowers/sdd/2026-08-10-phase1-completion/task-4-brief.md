### Task 4: Slang prebuilt fetch + triangle shaders (parallel-safe lane)

**Files:**
- Create: `tools/fetch_slang.cmake`, `tools/fetch_slang_test.sh`
- Create: `shaders/triangle.vert.slang`, `shaders/triangle.frag.slang`, `shaders/CMakeLists.txt`, `shaders/tests/spirv_validity_test.cpp`
- Modify: root `CMakeLists.txt` (add `include(tools/fetch_slang.cmake)` before `add_subdirectory(shaders)`; add `add_subdirectory(shaders)`)

**Interfaces:**
- Produces: `RX_SLANGC` cache variable (path to fetched `slangc`); build targets `triangle_shaders` (custom target producing `${CMAKE_BINARY_DIR}/shaders/triangle.vert.spv` + `triangle.frag.spv`), cache variables `RX_TRIANGLE_VERT_SPV`/`RX_TRIANGLE_FRAG_SPV` (INTERNAL, absolute paths); ctest `shader_spirv_test`.

**Fetch script** (`tools/fetch_slang.cmake`): Slang `2026.14.1` prebuilt release archives from `https://github.com/shader-slang/slang/releases/download/v2026.14.1/` — `slang-2026.14.1-linux-x86_64-glibc-2.27.tar.gz` for Linux host; extract into `third_party/slang-prebuilt/<platform>/` with a `.rx-fetched` marker for idempotency; `file(DOWNLOAD ... STATUS)` checked, `FATAL_ERROR` on failure naming the URL; `file(ARCHIVE_EXTRACT)`. **Verify the archive's internal layout before assuming `bin/slangc`** — list the extracted tree and set `RX_SLANGC` to the real path (archives may or may not have a top-level directory). Note: slangc runs on the HOST, so always fetch the host (Linux) archive for the compiler even under the windows-cross-zig preset — shader compilation is host-side tooling; guard accordingly (`CMAKE_HOST_SYSTEM_NAME`, not `CMAKE_SYSTEM_NAME`).

**Shaders:** vertex shader generates a triangle from `SV_VertexID` (3 hardcoded NDC positions: `(0,-0.5) (0.5,0.5) (-0.5,0.5)`), solid white color; fragment returns it. Slang syntax: `[shader("vertex")]` / `[shader("fragment")]` entry points named `main`.

**Compile:** `add_custom_command` invoking `${RX_SLANGC} <src> -target spirv -profile sm_6_0 -entry main -o <out>`. **If this pinned slangc rejects any flag, run `${RX_SLANGC} -h`, adapt, and record the actual working flags in your report** — flag drift across Slang releases is expected; the build-and-run gate exists to catch it. The `.spv` outputs must be regenerated when the `.slang` sources change (DEPENDS).

**Test:** `shader_spirv_test` reads both `.spv` files, asserts the SPIR-V magic number `0x07230203` (first 4 bytes, little-endian). Paths injected via `target_compile_definitions`.

**Verify:** `cmake --preset linux-native && cmake --build --preset linux-native && ctest --preset linux-native -R shader_spirv_test`; `./tools/fetch_slang_test.sh` (asserts `slangc -v` reports 2026.14.1); re-configure a second time and confirm no re-download (marker works); `windows-cross-zig` still configures+builds. Commit clean.

---

