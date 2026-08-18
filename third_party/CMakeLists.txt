include("${CMAKE_SOURCE_DIR}/cmake/DepCache.cmake")

set(RX_SPDLOG_TAG "v1.17.0")
rx_add_cached_dependency(
  NAME spdlog
  REPO https://github.com/gabime/spdlog.git
  TAG ${RX_SPDLOG_TAG}
  CMAKE_ARGS -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF
)
find_package(spdlog REQUIRED PATHS "${spdlog_CACHE_DIR}" NO_DEFAULT_PATH)

# CMake 3.22 has no find_package(... GLOBAL) (that arrived in 3.24), and
# imported targets are directory-scoped by default: without this, the
# spdlog::spdlog target created here would be invisible to sibling
# add_subdirectory() calls (e.g. tools/dep_cache_smoketest) that need to
# link against it. Promote it to global scope explicitly instead.
set_target_properties(spdlog::spdlog PROPERTIES IMPORTED_GLOBAL TRUE)

include(FetchContent)

set(RX_DOCTEST_TAG "v2.5.3")
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG ${RX_DOCTEST_TAG}
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(doctest)

set(RX_GLM_TAG "1.0.3")
FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG ${RX_GLM_TAG}
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)

# Vulkan-Headers: header-only, no loader library involved. Deliberately NOT
# found via find_package(Vulkan): that bundled FindVulkan.cmake (CMake 3.22
# here) gates Vulkan_FOUND -- and therefore Vulkan::Headers existing at all
# -- behind ALSO locating the platform's Vulkan *loader* import library
# (vulkan-1.lib on Windows), which does not exist anywhere when
# cross-compiling to Windows on this Linux host. Nothing in this project
# links the loader at compile/link time: the RHI layer (Task 6+) loads
# Vulkan entry points dynamically at runtime via volk, so only the headers
# (types/constants/signatures) are ever needed.
#
# Built through the dep-cache (rx_add_cached_dependency), not a plain
# FetchContent, so it produces a real installed CMake package
# (VulkanHeadersConfig.cmake under <prefix>/share/cmake/VulkanHeaders). That
# matters beyond this project's own consumption: vk-bootstrap (added below)
# is itself built through the dep-cache as a fully separate `cmake -S -B`
# subprocess, which cannot see any in-tree FetchContent target from this
# configure. vk-bootstrap's own CMakeLists.txt looks for Vulkan::Headers by
# trying, in order, find_package(VulkanHeaders CONFIG), then
# find_package(Vulkan) (the loader-requiring one -- broken when
# cross-compiling to Windows here, same as above), then finally FetchContent
# as a last resort -- but the find_package(Vulkan) and FetchContent fallback
# paths both explicitly set VK_BOOTSTRAP_INSTALL OFF (upstream's own comment:
# "If we had to use a direct path to get the headers, disable installing"),
# which would leave nothing for this project's own
# find_package(vk-bootstrap ...) below to find. Feeding vk-bootstrap's
# subprocess build a CMAKE_PREFIX_PATH pointing at this installed
# VulkanHeaders package makes its first find_package(VulkanHeaders CONFIG)
# attempt succeed, so it never reaches the problematic fallbacks and installs
# normally. Verified directly against vk-bootstrap's pinned commit's
# CMakeLists.txt -- see the vk-bootstrap block below.
#
# Pinned to vulkan-sdk-1.4.357.0 -- the same SDK version already pinned for
# volk below, and (verified directly against its source) the exact
# Vulkan-Headers version (gen/CurrentBuildVulkanVersion.cmake:
# VK_BOOTSTRAP_SOURCE_HEADER_VERSION_GIT_TAG "v1.4.357", the same commit as
# this tag) that vk-bootstrap's pinned commit's generated dispatch tables
# (VkBootstrapDispatch.h/VkBootstrapFeatureChain.h) require: an earlier
# attempt pinning the older vulkan-sdk-1.3.280.0 (matching this machine's
# driver-reported Instance Version, and what rx_platform's Task 5.6 pinned)
# failed to compile vk-bootstrap's isolated sub-build with "unknown type
# name 'VkDepthClampModeEXT'" and similar errors -- structs/PFN_ typedefs
# for extensions newer than 1.3.280 that vk-bootstrap's generated code
# references unconditionally. Newer headers are additive/backward-compatible
# (declaring a type doesn't require using it or the driver supporting it),
# and this project's own requested instance API version stays pinned at
# 1.3.0 via require_api_version() in rx_rhi_vk regardless of header version,
# so this bump does not pull in any Vulkan 1.4 *feature* requirement --
# consistent with the Vulkan 1.3-only baseline constraint.
set(RX_VULKAN_HEADERS_TAG "vulkan-sdk-1.4.357.0")
rx_add_cached_dependency(
  NAME Vulkan-Headers
  REPO https://github.com/KhronosGroup/Vulkan-Headers.git
  TAG ${RX_VULKAN_HEADERS_TAG}
  CMAKE_ARGS -DVULKAN_HEADERS_ENABLE_TESTS=OFF -DVULKAN_HEADERS_ENABLE_MODULE=OFF
)
find_package(VulkanHeaders CONFIG REQUIRED PATHS "${Vulkan-Headers_CACHE_DIR}" NO_DEFAULT_PATH)

# Same CMake 3.22 limitation as spdlog above: find_package()'s imported
# targets are directory-scoped by default, so promote Vulkan::Headers to
# global scope for sibling directories (rx_platform, rx_rhi_vk) to link.
set_target_properties(Vulkan::Headers PROPERTIES IMPORTED_GLOBAL TRUE)

set(RX_SDL3_TAG "release-3.4.14")
rx_add_cached_dependency(
  NAME SDL3
  REPO https://github.com/libsdl-org/SDL.git
  TAG ${RX_SDL3_TAG}
  CMAKE_ARGS -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF
)
find_package(SDL3 REQUIRED PATHS "${SDL3_CACHE_DIR}" NO_DEFAULT_PATH)

# Same CMake 3.22 limitation as spdlog above. Unlike spdlog::spdlog, SDL3's
# own config makes "SDL3::SDL3" an ALIAS (to SDL3::SDL3-static here, since
# we build static-only) rather than a real imported target, and
# set_target_properties() cannot be applied to an ALIAS target directly.
# Worse, an ALIAS is scoped to the directory it was created in regardless of
# whether the target it points to is global, so "SDL3::SDL3" itself stays
# invisible outside this directory even after the fix below. Promote the
# real underlying imported targets to global scope instead, and have
# consumers (src/rx_platform) link against "SDL3::SDL3-static" directly
# rather than the alias.
set_target_properties(SDL3::SDL3-static PROPERTIES IMPORTED_GLOBAL TRUE)
set_target_properties(SDL3::Headers PROPERTIES IMPORTED_GLOBAL TRUE)

# volk: dynamic Vulkan function loader. Its own CMakeLists.txt defines a
# `volk` STATIC target that unconditionally compiles volk.c, and only tries
# to make Vulkan headers visible to that target via, in order,
# find_package(Vulkan) (the loader-requiring one this project avoids
# elsewhere), $ENV{VULKAN_SDK}, or a target literally named "Vulkan-Headers"
# (not this project's "Vulkan::Headers") -- none of which resolve here, so
# that target would fail to compile (missing <vulkan/vulkan.h>) the moment
# anything triggers a default/"all" build. This project never needs volk's
# own target anyway: rx_rhi_vk compiles volk.c straight into itself (see
# src/rx_rhi_vk/CMakeLists.txt) and gets Vulkan::Headers transitively through
# vk-bootstrap::vk-bootstrap. So fetch the source only, via
# FetchContent_Populate, and deliberately skip FetchContent_MakeAvailable's
# implicit add_subdirectory (and therefore volk's CMakeLists.txt) entirely.
#
# FetchContent_Populate() is a function, so it sets volk_SOURCE_DIR as a
# normal variable scoped only to this directory (CMakeLists.txt is a macro's
# caller here, not a function's, but the PARENT_SCOPE set inside
# FetchContent_Populate still only reaches up to *this* directory's scope,
# not sideways into sibling add_subdirectory()s). src/rx_rhi_vk, which needs
# volk_SOURCE_DIR to locate volk.c/volk.h, is a *sibling* of this directory
# under the root CMakeLists.txt, not a descendant of it -- so without an
# explicit PARENT_SCOPE propagation here too, volk_SOURCE_DIR would be empty
# there. Propagate it up to the root scope, which src/rx_rhi_vk (added later
# from the root CMakeLists.txt) inherits.
set(RX_VOLK_TAG "vulkan-sdk-1.4.357.0")
FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG ${RX_VOLK_TAG}
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(volk)
if(NOT volk_POPULATED)
  FetchContent_Populate(volk)
endif()
set(volk_SOURCE_DIR "${volk_SOURCE_DIR}" PARENT_SCOPE)

# VMA (Vulkan Memory Allocator): a single-header-style library whose actual
# implementation only gets compiled wherever some translation unit defines
# VMA_IMPLEMENTATION before including vk_mem_alloc.h -- that TU is
# src/rx_rhi_vk/src/vma_impl.cpp (see its own comment for why it must be the
# *only* one). Every other consumer (src/rx_rhi_vk/src/buffer.cpp, this
# library's public headers) just #includes vk_mem_alloc.h for declarations.
# VMA's own CMakeLists.txt is deliberately never added via
# add_subdirectory(): it exists to build VMA's sample application (which
# pulls in GLFW and other dependencies this project has no use for and does
# not want to fetch), not a library target this project would link against
# -- there is nothing to compile as a separate target; the implementation is
# a single TU already covered by rx_rhi_vk's own sources. So, exactly like
# volk above: fetch the source only, via FetchContent_Populate, and
# deliberately skip FetchContent_MakeAvailable's implicit add_subdirectory
# (and therefore VMA's CMakeLists.txt) entirely.
#
# Same CMake 3.22 sibling-scope reason as volk above: FetchContent_Populate()
# is a function, so it sets vma_SOURCE_DIR as a normal variable scoped only
# to this directory. src/rx_rhi_vk, which needs vma_SOURCE_DIR to locate
# include/vk_mem_alloc.h, is a *sibling* of this directory under the root
# CMakeLists.txt, not a descendant of it -- so without an explicit
# PARENT_SCOPE propagation here too, vma_SOURCE_DIR would be empty there.
# Propagate it up to the root scope, which src/rx_rhi_vk (added later from
# the root CMakeLists.txt) inherits.
set(RX_VMA_TAG "v3.4.0")
FetchContent_Declare(vma
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG ${RX_VMA_TAG}
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(vma)
if(NOT vma_POPULATED)
  FetchContent_Populate(vma)
endif()
set(vma_SOURCE_DIR "${vma_SOURCE_DIR}" PARENT_SCOPE)

# stb (stb_image.h): header-only image decoder, used by rx_rhi_vk's texture
# upload path (Task 4, rx::rhi::Texture2D/Uploader) to decode PNG/JPG/etc.
# pixel data from disk into raw RGBA8 bytes -- the spec's Fixed decision
# #10: "stb_image ... Public upload surface is 'pixels + format + extent
# in -> bindless handle out' so a libktx backend can slot in later without
# touching descriptor code" [R:C3]. Exactly like volk/VMA above: stb's repo
# has no CMakeLists.txt at all (it's a loose collection of public-domain
# single-header libraries, not a buildable project) and no version tags
# (all releases are untagged commits on `master`), so this fetches the
# source only via FetchContent_Populate (never FetchContent_MakeAvailable,
# which would have nothing to add_subdirectory anyway) and PARENT_SCOPE-
# propagates stb_SOURCE_DIR up to the root scope for src/rx_rhi_vk (a
# sibling directory under the root CMakeLists.txt, not a descendant of this
# one) to consume -- same CMake 3.22 sibling-scope reason documented above
# volk/VMA. The implementation itself is compiled exactly once, in
# src/rx_rhi_vk/src/stb_impl.cpp (STB_IMAGE_IMPLEMENTATION defined there
# only), mirroring vma_impl.cpp's own single-TU-implementation discipline:
# defining STB_IMAGE_IMPLEMENTATION in more than one TU is a
# duplicate-symbol link error.
set(RX_STB_TAG "2c980bb59875b0d32144a71867fbdebb2f77cd20") # nothings/stb has no version tags; pinned commit, master as of this task's date
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG ${RX_STB_TAG}
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
  FetchContent_Populate(stb)
endif()
set(stb_SOURCE_DIR "${stb_SOURCE_DIR}" PARENT_SCOPE)

# vk-bootstrap: instance/device/swapchain setup helper, a real compiled
# library, so (unlike volk/GLM/doctest) it goes through the dependency cache
# rather than plain FetchContent.
#
# Investigated risk (flagged from Task 5.6, the rx_platform find_package(Vulkan)
# fix): does vk-bootstrap's own CMakeLists.txt (checked directly against the
# pinned commit) require the Vulkan loader for its *library* target, not just
# its tests/examples (already disabled below via VK_BOOTSTRAP_TEST=OFF)? Yes,
# conditionally: it needs a Vulkan::Headers target and tries, in order,
# (1) find_package(VulkanHeaders CONFIG), (2) find_package(Vulkan) -- the
# loader-requiring one, broken here when cross-compiling to Windows -- (3)
# FetchContent as a last resort. Paths (2) succeeding and (3) both explicitly
# set VK_BOOTSTRAP_INSTALL OFF ("If we had to use a direct path to get the
# headers, disable installing"), which would make `cmake --build --target
# install` install nothing, leaving no vk-bootstrap-config.cmake for the
# find_package(vk-bootstrap ...) below to find -- on Windows for sure, and
# possibly even on Linux depending on what find_package(Vulkan) locates on
# this host. Fix: pass CMAKE_PREFIX_PATH pointing at this project's own
# dep-cache install of Vulkan-Headers (above) into vk-bootstrap's isolated
# sub-build, so its first attempt, find_package(VulkanHeaders CONFIG),
# succeeds directly and it never reaches the install-disabling fallbacks --
# on both linux-native and windows-cross-zig. Verified by building both
# presets from a clean .deps-cache and confirming vk-bootstrap-config.cmake
# is installed and find_package(vk-bootstrap ...) below succeeds on each.
set(RX_VK_BOOTSTRAP_COMMIT "556b79b165386f6c1a18362d30f2a076fdaa2778")
rx_add_cached_dependency(
  NAME vk-bootstrap
  REPO https://github.com/charles-lunarg/vk-bootstrap.git
  TAG ${RX_VK_BOOTSTRAP_COMMIT}
  CMAKE_ARGS -DVK_BOOTSTRAP_TEST=OFF "-DCMAKE_PREFIX_PATH=${Vulkan-Headers_CACHE_DIR}"
)
find_package(vk-bootstrap REQUIRED PATHS "${vk-bootstrap_CACHE_DIR}" NO_DEFAULT_PATH)

# Same CMake 3.22 limitation as spdlog/SDL3/Vulkan-Headers above.
set_target_properties(vk-bootstrap::vk-bootstrap PROPERTIES IMPORTED_GLOBAL TRUE)

# enkiTS: the engine's sole task scheduler [spec D2] -- src/rx_task wraps
# it thinly as rx::task::Scheduler (Phase 4 Stage 0, Task 2). Zlib license
# (Doug Binks, see the pinned tag's own License.txt -- permissive, no
# attribution requirement beyond "appreciated but not required").
#
# Pinned v1.12 -- the current tag on github.com/dougbinks/enkiTS as of this
# task (verified directly: `git ls-remote --tags`), matching [R:threading]'s
# own version citation. Note enkiTS's CMakeLists.txt project() call still
# reports VERSION "1.11" (an upstream tag/project-version mismatch, not a
# mistake on this project's part -- verified directly against the checked-
# out v1.12 tag's own CMakeLists.txt).
#
# Routed through the dep-cache like spdlog/Vulkan-Headers/SDL3/vk-bootstrap
# above, not vendored as source under third_party/ [task-2 resolution 1]:
# enkiTS's own CMakeLists.txt already installs a clean, real CMake package
# (ENKITS_INSTALL=ON produces enkiTSConfig.cmake exporting an `enkiTS::
# enkiTS` imported target with correct INTERFACE_INCLUDE_DIRECTORIES/
# INTERFACE_COMPILE_DEFINITIONS) -- verified directly by configuring,
# building, and installing this exact tag through BOTH this project's
# toolchains (linux-native and windows-cross-zig) from a clean prefix, then
# linking and RUNNING a real enki::TaskScheduler/TaskSet program against
# each installed tree (the windows-cross-zig one under Wine) -- both
# produced a working static library with no source-level or link-level
# changes needed. Given a dependency whose own install already "exports
# cleanly through rx_add_cached_dependency" (the resolution's own bar), that
# beats hand-vendoring the two source files under third_party/ for no
# benefit.
#
# ENKITS_BUILD_C_INTERFACE/ENKITS_BUILD_EXAMPLES=OFF: this project only
# ever calls the C++ API (TaskScheduler.h), and enkiTS's example
# executables would otherwise build unconditionally and add nothing this
# project links against. ENKITS_BUILD_SHARED=OFF: matches every other
# compiled dependency here (spdlog/SDL3) being linked statically.
set(RX_ENKITS_TAG "v1.12")
rx_add_cached_dependency(
  NAME enkiTS
  REPO https://github.com/dougbinks/enkiTS.git
  TAG ${RX_ENKITS_TAG}
  CMAKE_ARGS -DENKITS_INSTALL=ON -DENKITS_BUILD_C_INTERFACE=OFF -DENKITS_BUILD_EXAMPLES=OFF -DENKITS_BUILD_SHARED=OFF
)
find_package(enkiTS CONFIG REQUIRED PATHS "${enkiTS_CACHE_DIR}" NO_DEFAULT_PATH)

# Same CMake 3.22 limitation as spdlog/SDL3/Vulkan-Headers/vk-bootstrap
# above: promote to global scope for src/rx_task (a sibling directory) to
# link.
set_target_properties(enkiTS::enkiTS PROPERTIES IMPORTED_GLOBAL TRUE)

# Tracy: the engine's profiler client [spec D3, Phase 4 Stage 0 Task 3].
# Client-only -- this pulls in exactly one compiled translation unit
# (public/TracyClient.cpp) and the client-side headers; the profiler
# UI/server (profiler/, server/, capture/, python/, meson.build's own
# separate "tracy-profiler" target) is never built here, never
# add_subdirectory'd, and never linked by anything in this project. BSD-3
# license (see the pinned tag's own LICENSE file -- permissive, no
# attribution requirement beyond the standard copyright-notice retention;
# recorded here per this task's own resolution to note the license at
# vendoring time).
#
# Entirely guarded behind RX_TRACY (declared as a top-level option() in the
# root CMakeLists.txt, before this file is add_subdirectory'd, so it is
# already set by the time this line runs): when OFF, this whole block never
# executes -- Tracy is never git-cloned, never configured, never compiled,
# and the `Tracy::TracyClient` target never exists -- rather than being
# built-but-disabled-at-runtime. This is what makes "RX_TRACY=OFF -> zero
# Tracy symbols in every binary" a structural guarantee instead of a
# TRACY_ENABLE-macro convention that could bit-rot.
#
# Pinned v0.14.0 -- the current tag on github.com/wolfpld/tracy as of this
# task (verified directly: `git ls-remote --tags`, matching [R:threading]'s
# own ">= v0.11" citation and its "VK_EXT_calibrated_timestamps added in
# v0.11.0" fact -- v0.14.0 carries that support unchanged).
#
# Routed through the dep-cache like spdlog/Vulkan-Headers/SDL3/
# vk-bootstrap/enkiTS above, not hand-vendored as loose source under
# third_party/: Tracy's own CMakeLists.txt (verified directly against the
# pinned tag) already builds a real `TracyClient` target and installs a
# clean CMake package (`install(EXPORT TracyConfig ...)` +
# `configure_package_config_file` producing TracyConfig.cmake, exporting a
# `Tracy::TracyClient` imported target with correct
# INTERFACE_INCLUDE_DIRECTORIES) -- exactly the "exports cleanly through
# rx_add_cached_dependency" bar the enkiTS vendoring precedent set, so
# hand-vendoring the source under third_party/ instead would add nothing.
#
# TRACY_ENABLE=ON IS passed here, deliberately matching (never drifting
# from) src/rx_core/CMakeLists.txt's own `if(RX_TRACY)
# target_compile_definitions(rx_core PUBLIC TRACY_ENABLE)`: TracyClient.cpp
# itself (verified directly by reading it) is guarded top-to-bottom by
# `#ifdef TRACY_ENABLE` -- built WITHOUT it, the compiled Tracy::TracyClient
# library would be a near-empty stub (no profiler thread, no networking, no
# real zone bookkeeping), and this project's own code (which DOES see
# TRACY_ENABLE, from rx_core's PUBLIC definition) would emit real calls
# into `tracy::` symbols that stub build never defines -- an undefined-
# reference link error, not a silent no-op. The two must always agree; both
# are driven by the identical RX_TRACY guard at the CMake level so they
# structurally cannot disagree. TRACY_STATIC=ON matches every other
# compiled dependency here being linked statically.
#
# TRACY_ON_DEMAND=ON [task-3 fix round 1, Medium+Low finding]: turned ON
# here -- NOT Tracy's own default (OFF) -- because leaving it off left two
# real gaps against D3's own "passive (~no cost) until a profiler connects"
# text, both traced directly against the vendored source before this fix:
#   - ScopedZone::Name()/VkCtxScope's dynamic-name constructor (backing
#     RX_ZONE_DYNAMIC_NAME/RX_GPU_ZONE_DYNAMIC -- rx_core/profile.h,
#     rx_rhi_vk/tracy_gpu.h) tracy_malloc()+memcpy() unconditionally on
#     every call when on-demand is off, since `m_active` is then just
#     `is_active` with no connection check at all
#     (public/client/TracyScoped.hpp, public/tracy/TracyVulkan.hpp).
#   - VkCtx::Collect() (backing RX_GPU_COLLECT) always does a real
#     vkGetQueryPoolResults readback, every frame, with no connection
#     check either (public/tracy/TracyVulkan.hpp).
# With TRACY_ON_DEMAND defined, BOTH of those gain the exact early-out this
# project wants for free, straight from Tracy's own source (no code on this
# project's side needed): every `m_active` initializer becomes
# `is_active && GetProfiler().IsConnected()`, and Collect() gets its own
# `#ifdef TRACY_ON_DEMAND if (!GetProfiler().IsConnected()) { <cheap
# query-pool reset only>; return; }` branch. Disconnected, both dynamic-name
# macros and the per-frame Collect() now cost a few branches and nothing
# more -- no allocation, no GPU readback -- restoring "passive (~no cost)
# until a profiler connects" for the two sites that violated it. The plain
# RX_ZONE/RX_ZONE_NAMED/RX_FRAME_MARK/RX_PLOT macros were already this cheap
# either way (a lock-free ring-buffer write, no allocation, regardless of
# TRACY_ON_DEMAND) and are unaffected by this flag other than gaining the
# identical connection check.
#
# TRADEOFF, disclosed (not free): on-demand means the client buffers
# nothing before a profiler connects -- capture history starts at the
# moment of connection, not at process startup. A profiler that attaches
# mid-run sees only frames/zones from that point forward; whatever ran
# before connecting is not retroactively visible. Documented again at each
# dynamic-name macro's own declaration (profile.h, tracy_gpu.h) since that
# is where a reader is most likely to reason about zone cost.
if(RX_TRACY)
    set(RX_TRACY_TAG "v0.14.0")
    rx_add_cached_dependency(
      NAME tracy
      REPO https://github.com/wolfpld/tracy.git
      TAG ${RX_TRACY_TAG}
      # TRACY_NO_BROADCAST + TRACY_ONLY_LOCALHOST [CI Wine hang, runs
      # 31512073559/31521284064]: the on-demand client's first zone spins up
      # its network layer, and its UDP discovery broadcast wedges process
      # EXIT under CI's cold-prefix Wine (tests print SUCCESS, the process
      # never dies, ctest times out at the new 120s guard) — exactly and
      # only the three test binaries that fire a Tracy macro hung; the four
      # zone-free ones passed. Broadcast is LAN-discovery convenience we do
      # not use (we connect directly); localhost-only is CI hygiene. Local
      # warm-prefix Wine exits fine either way, which is why this never
      # reproduced off-CI.
      CMAKE_ARGS -DTRACY_ENABLE=ON -DTRACY_STATIC=ON -DTRACY_ON_DEMAND=ON -DTRACY_NO_BROADCAST=ON -DTRACY_ONLY_LOCALHOST=ON
    )
    find_package(Tracy CONFIG REQUIRED PATHS "${tracy_CACHE_DIR}" NO_DEFAULT_PATH)

    # Same CMake 3.22 limitation as spdlog/SDL3/Vulkan-Headers/vk-bootstrap/
    # enkiTS above: promote to global scope for src/rx_core (a sibling
    # directory) to link.
    set_target_properties(Tracy::TracyClient PROPERTIES IMPORTED_GLOBAL TRUE)
endif()

# ---------------------------------------------------------------------------
# Phase 4 Stage 1 Task 13 -- glTF import: fastgltf + meshoptimizer +
# MikkTSpace + Draco (spec D7, gate matrix-issue02 as amended by
# gate/rulings-2026-08-18.md §#2). Gate ruling #2 CONFIRMED matrix Conflict
# C1: contrary to the plan's original text, none of these were vendored
# before this task -- `third_party/CMakeLists.txt` up to this point
# enumerates spdlog/doctest/glm/Vulkan-Headers/SDL3/volk/VMA/stb/
# vk-bootstrap/enkiTS/Tracy only.

# fastgltf: the glTF 2.0 parser [spec D7 -- "5-7x faster than cgltf, full
# KHR extension surface"]. MIT license (LICENSE.md: "Copyright (c) 2022 -
# 2025 Sean Apeler", verified directly at the pinned tag). Pinned v0.9.0
# (2025-07-08 -- current latest tag; no v0.10 exists as of this task, gate
# matrix C7). Exports a real CMake package (fastgltf-config.cmake via
# `install(EXPORT fastgltf-targets ... NAMESPACE fastgltf::)`, verified
# directly against the pinned tag's CMakeLists.txt) -- routed through the
# dep-cache like spdlog/SDL3/Vulkan-Headers/vk-bootstrap/enkiTS/Tracy
# above, not hand-vendored as loose source.
#
# FASTGLTF_COMPILE_AS_CPP20=ON [gate matrix row 1, BINDING]: this repo is
# C++20 throughout (root CMakeLists.txt: CMAKE_CXX_STANDARD 20); fastgltf
# defaults to C++17 (`cxx_std_17` via its own FASTGLTF_COMPILE_TARGET
# variable) unless this option is set, in which case
# target_compile_features(fastgltf PUBLIC cxx_std_20) is used instead --
# verified directly against the pinned tag's top-level CMakeLists.txt.
# FASTGLTF_ENABLE_TESTS/EXAMPLES/DOCS=OFF: this project never builds
# fastgltf's own Catch2 test suite or GL/GLFW-based example viewer.
# FASTGLTF_ENABLE_DEPRECATED_EXT=OFF (fastgltf's own default -- set
# explicitly here per gate matrix §2B's Archived-extensions row: "Build
# fastgltf with the deprecated gate OFF" -- KHR_materials_pbrSpecularGlossiness
# and friends hit the generic unknown-extension logging path instead of a
# silently-parsed struct).
#
# EMBEDDED SIMDJSON (Apache-2.0) -- NOT a separate vendoring step: fastgltf
# has no `simdjson::simdjson` target visible at its own configure time (it
# is built in total isolation by rx_add_cached_dependency's own `cmake -S
# -B` subprocess, see DepCache.cmake), so its own `cmake/dependencies.cmake`
# (verified directly against the pinned tag) takes its embedded-copy
# fallback path: `file(DOWNLOAD ...)` fetches simdjson's single-header
# amalgamation (simdjson.h/simdjson.cpp) directly from
# `raw.githubusercontent.com/simdjson/simdjson/v3.12.3/singleheader/` --
# note SIMDJSON_TARGET_VERSION="3.12.3" is hardcoded in fastgltf v0.9.0's
# own dependencies.cmake, so this is pinned transitively through the
# fastgltf pin above, not a separate moving target this project tracks --
# and compiles it straight into the `fastgltf` target
# (fastgltf_add_source_directory). simdjson is Apache License 2.0 (see
# https://github.com/simdjson/simdjson/blob/v3.12.3/LICENSE, verified
# directly) -- recorded here per the gate ruling's "+ embedded simdjson
# license" requirement. Both licenses (fastgltf MIT + simdjson Apache-2.0)
# apply to the resulting `fastgltf::fastgltf` static library this project
# links.
# TOOLCHAIN FIX [this task, verified directly against the zig-bundled
# Clang 21.1.0 both presets' zig-cxx-* wrappers invoke -- see
# cmake/zig-wrappers/zig-cxx-linux: even "linux-native" compiles through
# `zig c++ -target x86_64-linux-gnu`, not the system compiler]: a plain
# `cmake --build` of this dependency fails to compile TWO unrelated
# always_inline/target-attribute mismatches under this Clang version,
# neither fixable without patching vendored source (this project's
# dep-cache has no patch step, and neither is worth adding one for):
#   1. simdjson v3.12.3's "icelake" (AVX-512) kernel marks its per-function
#      `__attribute__((target("avx512f,avx512bw,avx512vbmi2,...")))` regions
#      without the separate "evex512" feature LLVM now tracks independently
#      (added between the LLVM version simdjson 3.12.3 targeted and
#      Clang 21) -- always_inline AVX-512 intrinsics inside those regions
#      fail to compile ("requires target feature 'evex512'... compiled
#      without support for 'evex512'"). Fixed by
#      `-DSIMDJSON_IMPLEMENTATION_ICELAKE=0` (simdjson's own documented
#      opt-out, simdjson.h:9452 `#ifndef` guard verified directly) --
#      simdjson still runtime-dispatches to its haswell(AVX2)/westmere
#      (SSE4.2)/fallback kernels, all of which compile and run cleanly.
#      Zero correctness impact, and no measured performance impact on any
#      hardware this project targets: neither desktop CI/dev machines nor
#      the Steam Deck's Zen 2 APU expose AVX-512 at all, so the icelake
#      kernel would never be SELECTED at runtime on this project's actual
#      hardware floor even if it did compile.
#   2. fastgltf.cpp's own `sse_crc32c` (src/fastgltf.cpp, verified
#      directly) is marked `[[gnu::target("sse4.2")]]` and calls
#      always_inline `_mm_crc32_u32`/`_mm_crc32_u16`/`_mm_crc32_u8`, which
#      Clang's own intrinsic headers mark as requiring the SEPARATE
#      "crc32" target feature -- this Clang version does not imply crc32
#      from sse4.2 within a function `target()` attribute string (real
#      Intel silicon bundles both, but LLVM models them as independent
#      feature bits). Per Clang's own documented semantics, a per-function
#      `target()` attribute is ADDITIVE to the translation unit's
#      command-line baseline features, so passing `-mcrc32` as an ordinary
#      compile flag (harmless on any x86_64 target regardless of runtime
#      support -- this only affects what the COMPILER is willing to
#      generate, and `sse_crc32c` is itself only ever CALLED after an
#      explicit `sse4::supported_by_runtime_system()` runtime check,
#      verified directly in fastgltf.cpp's `initialiseCrc()`) gives every
#      `target("sse4.2")`-attributed function crc32 too, resolving the
#      mismatch with no source patch.
# Both flags apply to every file the fastgltf sub-build compiles
# (simdjson.cpp + fastgltf's own .cpp files); harmless everywhere the
# specific macro/feature does not apply.
set(RX_FASTGLTF_TAG "v0.9.0")
rx_add_cached_dependency(
  NAME fastgltf
  REPO https://github.com/spnda/fastgltf.git
  TAG ${RX_FASTGLTF_TAG}
  CMAKE_ARGS -DFASTGLTF_COMPILE_AS_CPP20=ON -DFASTGLTF_ENABLE_TESTS=OFF -DFASTGLTF_ENABLE_EXAMPLES=OFF
             -DFASTGLTF_ENABLE_DOCS=OFF -DFASTGLTF_ENABLE_DEPRECATED_EXT=OFF
             "-DCMAKE_CXX_FLAGS=-mcrc32 -DSIMDJSON_IMPLEMENTATION_ICELAKE=0"
             "-DCMAKE_C_FLAGS=-mcrc32 -DSIMDJSON_IMPLEMENTATION_ICELAKE=0"
)
find_package(fastgltf CONFIG REQUIRED PATHS "${fastgltf_CACHE_DIR}" NO_DEFAULT_PATH)

# Same CMake 3.22 limitation as every other cached dependency above:
# promote to global scope for src/rx_asset (a sibling directory) to link.
set_target_properties(fastgltf::fastgltf PROPERTIES IMPORTED_GLOBAL TRUE)

# meshoptimizer: mesh optimization (remap/cache/overdraw/fetch, spec D7)
# AND the decode side of EXT_meshopt_compression (gate matrix §2C) -- ONE
# dependency serves both. MIT license (LICENSE.md, verified directly).
# Pinned v1.2 (2026-06-30 per the GitHub API, verified directly -- gate
# matrix Conflicts C1/C7: the plan's "already vendored" claim was false,
# and an in-repo research file's stale June-2024 date must not be
# propagated). NOT header-only (gate matrix row 7): `src/meshoptimizer.h` +
# 20 `.cpp` files. Exports a real CMake package
# (`install(EXPORT meshoptimizerTargets ... NAMESPACE meshoptimizer::)`,
# verified directly) -- routed through the dep-cache like fastgltf above.
#
# MESHOPT_BUILD_GLTFPACK=OFF: gltfpack (meshoptimizer's own CLI, gate
# matrix's cited fixture-generation precedent) additionally pulls in the
# Binomial Basis Universal texture encoder as a source-only dependency
# (`gltf/README.md`'s own build instructions; `gltf/CMakeLists.txt`'s
# `BASISU_PATH` handling) purely to encode KHR_texture_basisu textures --
# irrelevant to this task (Task 14 owns textures) and an avoidable extra
# transitive dependency to cross-compile through zig for zero decode-path
# benefit. This task's fixtures that need meshoptimizer-encoded buffers
# (EXT_meshopt_compression) are instead generated by
# `tools/gen_meshopt_fixture.cpp`, a small host tool linked directly
# against this SAME `meshoptimizer::meshoptimizer` target and calling its
# real `meshopt_encodeVertexBuffer`/`meshopt_encodeIndexBuffer`/
# `meshopt_encodeFilterOct` functions -- i.e. the identical codec gltfpack
# itself calls, without gltfpack's own CLI or its Basis Universal
# dependency. See that tool's own header comment and
# `tools/fetch_assets.sh` for the documented invocation this substitutes
# for a gltfpack call. MESHOPT_BUILD_DEMO=OFF: this project never builds
# meshoptimizer's own SDL2-based demo viewer.
set(RX_MESHOPTIMIZER_TAG "v1.2")
rx_add_cached_dependency(
  NAME meshoptimizer
  REPO https://github.com/zeux/meshoptimizer.git
  TAG ${RX_MESHOPTIMIZER_TAG}
  CMAKE_ARGS -DMESHOPT_BUILD_DEMO=OFF -DMESHOPT_BUILD_GLTFPACK=OFF -DMESHOPT_BUILD_SHARED_LIBS=OFF
)
find_package(meshoptimizer CONFIG REQUIRED PATHS "${meshoptimizer_CACHE_DIR}" NO_DEFAULT_PATH)
set_target_properties(meshoptimizer::meshoptimizer PROPERTIES IMPORTED_GLOBAL TRUE)

# MikkTSpace: the reference tangent-space generator [spec D7/G2 -- "the
# industry-standard reference implementation"]. NO TAGS OR RELEASES EXIST
# in github.com/mmikk/MikkTSpace (verified directly: `git ls-remote --tags`
# returns nothing; last commit 2020-03-25) and there is no LICENSE file --
# the license is a zlib-style grant embedded ONLY in the header's own
# comment block (mikktspace.h, verified directly against the pinned
# commit):
#
#   "Copyright (C) 2011 by Morten S. Mikkelsen
#
#    This software is provided 'as-is', without any express or implied
#    warranty. In no event will the authors be held liable for any damages
#    arising from the use of this software.
#
#    Permission is granted to anyone to use this software for any purpose,
#    including commercial applications, and to alter it and redistribute it
#    freely, subject to the following restrictions:
#
#    1. The origin of this software must not be misrepresented; you must
#       not claim that you wrote the original software. If you use this
#       software in a product, an acknowledgment in the product
#       documentation would be appreciated but is not required.
#    2. Altered source versions must be plainly marked as such, and must
#       not be misrepresented as being the original software.
#    3. This notice may not be removed or altered from any source
#       distribution."
#
# Pinned at commit `3e895b49d05ea07e4c2133156cfa94369e19e409` (`master`
# HEAD as of this task's vendoring, verified directly via
# `git ls-remote https://github.com/mmikk/MikkTSpace.git HEAD`) -- a
# commit hash, not a tag, since none exists (gate matrix row 5). No
# CMakeLists.txt exists in this repo either (it is two loose files,
# mikktspace.h/mikktspace.c, "designed to be stand-alone... copied and
# used as is into any tool" per the header's own doc comment) -- fetched
# source-only via FetchContent_Populate, exactly like volk/VMA/stb above,
# and compiled directly into src/rx_asset (see src/rx_asset/CMakeLists.txt)
# rather than through the dep-cache, matching this project's own
# established "no CMakeLists -> compile straight into the consumer"
# convention.
set(RX_MIKKTSPACE_COMMIT "3e895b49d05ea07e4c2133156cfa94369e19e409")
FetchContent_Declare(mikktspace
  GIT_REPOSITORY https://github.com/mmikk/MikkTSpace.git
  GIT_TAG ${RX_MIKKTSPACE_COMMIT})
FetchContent_GetProperties(mikktspace)
if(NOT mikktspace_POPULATED)
  FetchContent_Populate(mikktspace)
endif()
# Same CMake 3.22 sibling-scope reason as volk/VMA/stb above: propagate to
# root scope for src/rx_asset (a sibling directory) to locate
# mikktspace.c/mikktspace.h.
set(mikktspace_SOURCE_DIR "${mikktspace_SOURCE_DIR}" PARENT_SCOPE)

# Draco: KHR_draco_mesh_compression decode [gate matrix §2B -- fastgltf
# parses only the extension's metadata, "does NOT decode... zero draco::
# calls in fastgltf.cpp"]. Apache License 2.0 (LICENSE, verified
# directly). Pinned v1.5.7 (github.com/google/draco's newest tag as of
# this task, verified via `git ls-remote --tags`). Exports a real CMake
# package (`draco-config.cmake` via `cmake/draco_install.cmake`'s
# `configure_package_config_file`/`install(EXPORT dracoExport ...
# NAMESPACE draco::)`, verified directly) -- routed through the dep-cache
# like fastgltf/meshoptimizer above. The internal library target Draco's
# own CMakeLists.txt builds on a non-MSVC compiler (both this project's
# presets: native GCC on linux-native, zig's bundled Clang/LLD on
# windows-cross-zig) is named `draco_static`, but the INSTALLED/EXPORTED
# imported target is `draco::draco` regardless -- verified directly
# against this task's own build output (`draco-targets.cmake`'s
# `add_library(draco::draco STATIC IMPORTED)`): Draco's export machinery
# renames the target via its `EXPORT_NAME` property at install time, so
# `draco::draco` is the correct (and only) name to link against on every
# toolchain here, not `draco::draco_static`.
#
# DRACO_GLTF_BITSTREAM=ON [gate matrix row 9, BINDING]: "restricts feature
# set to the glTF Draco profile" -- verified directly against
# cmake/draco_options.cmake's `draco_set_optional_features()`: this flag
# enables ONLY DRACO_MESH_COMPRESSION_SUPPORTED/
# DRACO_NORMAL_ENCODING_SUPPORTED/DRACO_STANDARD_EDGEBREAKER_SUPPORTED --
# no point-cloud compression, no predictive edgebreaker, no attribute
# deduplication -- exactly glTF's own Draco profile, nothing broader.
# DRACO_TESTS=OFF (already Draco's own default; set explicitly for
# documentation) disables Draco's gtest suite. DRACO_TRANSCODER_SUPPORTED=OFF
# (already default) keeps the eigen/tinygltf transcoder dependency chain
# out entirely -- verified directly: `Decoder::DecodeMeshFromBuffer` (the
# only Draco API this task calls, decode.h) needs neither.
# DRACO_UNITY_PLUGIN/DRACO_MAYA_PLUGIN/DRACO_JS_GLUE/DRACO_WASM=OFF: this
# project links none of Draco's engine-integration plugin targets.
#
# NO DECODE-ONLY BUILD OPTION EXISTS [gate matrix row 9, verified directly
# against the full `draco_options.cmake` option list above]: Draco's
# `draco_static` library target unconditionally contains BOTH encoder and
# decoder object code, and its `draco_decoder`/`draco_encoder` CLI
# executables (cmake/../CMakeLists.txt's `draco_add_executable(NAME
# draco_decoder ...)` / `draco_encoder`) build with no guarding option at
# all -- they exist inside Draco's OWN dep-cache subbuild only (never
# installed into anything this project's own targets reference or ship;
# `draco_install.cmake` installs them as loose binaries this project's
# `find_package(draco ...)` never looks at). The accepted mitigation (gate
# ruling #2) is dead-stripping the unused ENCODER half out of this
# project's OWN linked binaries: `-ffunction-sections -fdata-sections`
# passed to Draco's own compile (via CMAKE_CXX_FLAGS/CMAKE_C_FLAGS below,
# since Draco's own cmake/draco_flags.cmake sets neither, verified
# directly) gives every function/global its own linker section, and
# src/rx_asset/CMakeLists.txt applies `-Wl,--gc-sections` PUBLICLY (so it
# reaches every executable that transitively links rx_asset -- gc-sections
# only prunes anything at a real executable/shared-library link step, not
# a static-library archive step) -- the measured binary-size delta this
# produces is recorded in the task report.
set(RX_DRACO_TAG "1.5.7")
rx_add_cached_dependency(
  NAME draco
  REPO https://github.com/google/draco.git
  TAG ${RX_DRACO_TAG}
  CMAKE_ARGS -DDRACO_GLTF_BITSTREAM=ON -DDRACO_TESTS=OFF -DDRACO_TRANSCODER_SUPPORTED=OFF
             -DDRACO_UNITY_PLUGIN=OFF -DDRACO_MAYA_PLUGIN=OFF -DDRACO_JS_GLUE=OFF -DDRACO_WASM=OFF
             -DBUILD_SHARED_LIBS=OFF
             "-DCMAKE_CXX_FLAGS=-ffunction-sections -fdata-sections"
             "-DCMAKE_C_FLAGS=-ffunction-sections -fdata-sections"
)
find_package(draco CONFIG REQUIRED PATHS "${draco_CACHE_DIR}" NO_DEFAULT_PATH)
set_target_properties(draco::draco PROPERTIES IMPORTED_GLOBAL TRUE)

# ---------------------------------------------------------------------------
# Phase 4 Stage 1 Task 14 -- KTX2/Basis compressed texture pipeline (spec
# D10, gate matrix-issue03 as amended by gate/rulings-2026-08-18.md #3).
#
# libktx (KTX-Software): CPU-side KTX2 container parse + Basis Universal
# (ETC1S/UASTC) transcode to block-compressed GPU formats -- upload itself
# stays this engine's own (rx::rhi::Uploader), per D10. Pinned **v4.4.2**
# (stable; gate ruling C2, BINDING: v5.0.0 exists only as a release
# candidate -- v5.0.0-rc2, 2026-08-17 -- a major rework, KTX-Spec-Rev-5,
# CMake-only; pinning a prerelease would contradict this project's own
# vendoring discipline. Registry watch item for the v5/Spec-Rev-5
# transition -- see task-14-report.md).
#
# LICENSE [gate ruling #3, BINDING: "Apache-2.0 PLUS the bundled-component
# licenses actually compiled"] -- verified directly against the pinned
# tag's own LICENSE.md/LICENSES/ dir and REUSE.toml:
#   - KTX-Software's own code: Apache License 2.0 (LICENSE.md: "Files
#     unique to this repository generally fall under the Apache 2.0
#     license"; NOASSERTION is what GitHub's license-detector reports for
#     this repo, precisely BECAUSE of the additional bundled licenses
#     below -- not a licensing gap).
#   - Basis Universal transcoder AND encoder (external/basisu/LICENSE):
#     Apache License 2.0 -- both halves are compiled into the exported
#     `ktx` target regardless (see the "NO DECODE/READ-ONLY BUILD OPTION"
#     note below), so both are covered here even though this project only
#     ever CALLS the transcoder half.
#   - astc-encoder (external/astc-encoder/LICENSE.txt): Apache License
#     2.0 -- pulled in transitively by KTX_FEATURE_WRITE (the astcenc
#     static library is unconditionally part of KTX_INSTALL_TARGETS for a
#     static build, CMakeLists.txt's own `if(NOT BUILD_SHARED_LIBS AND NOT
#     APPLE) list(APPEND KTX_INSTALL_TARGETS ${ASTCENC_LIB_TARGET})`) even
#     though this project never selects an ASTC transcode target.
#   - zstd single-file decoder (external/basisu/zstd/zstd.c, BSD License
#     per that file's own LICENSE, Copyright Facebook, Inc.): compiled
#     UNCONDITIONALLY into KTX_MAIN_SRC (CMakeLists.txt:392, not gated by
#     KTX_FEATURE_WRITE) -- this is the supercompression auto-inflation
#     path the "Supercompression (zstd/zlib)" matrix row depends on (no
#     explicit zstd call anywhere in this project's own texture_cache.cpp).
#   - Ericsson etcdec.cxx (LicenseRef-ETCSLA.txt, a separate non-OSI
#     Ericsson Software License Agreement) is DELIBERATELY EXCLUDED from
#     this build via KTX_FEATURE_ETC_UNPACK=OFF below (see that flag's own
#     comment) -- this project's target formats are BC7/BC5 only (D10;
#     desktop + Steam Deck both support S3TC/RGTC/BPTC, verified in-task,
#     never ETC), so the Ericsson license does NOT apply to this vendored
#     build and is intentionally not carried forward.
#
# BUILD-SIZE EVALUATION [gate matrix "parse/transcode-only... encoder OFF",
# scope summary -- evaluated in-task, both findings below verified directly
# against the pinned tag's own CMakeLists.txt, mirroring the Draco
# "NO DECODE-ONLY BUILD OPTION EXISTS" precedent above]:
#   1. A read/transcode-ONLY target (`ktx_read`, `common_libktx_settings(
#      ktx_read 0 ${LIB_TYPE})` -- the "0" is enable_write=false, meaning
#      NO basis_encode.cpp/BASISU_ENCODER_CXX_SRC/writer1.c/writer2.c) DOES
#      exist inside libktx's own build -- but it is used ONLY by libktx's
#      own Emscripten/WebGL sub-targets (ktx_js_read,
#      msc_basis_transcoder_js) and is NEVER added to `KTX_INSTALL_TARGETS`
#      (`set(KTX_INSTALL_TARGETS ktx)`, CMakeLists.txt, unconditional --
#      `ktx_read` never appears in that list on any platform). No
#      `install(EXPORT ...)`/CMake package is ever generated for it, so
#      `find_package(ktx ...)` (this project's own consumption path, same
#      as every other dep-cache library) cannot reach it without patching
#      libktx's own CMakeLists.txt -- and this project's dep-cache has no
#      patch step (same "no patch step" posture as the Draco encoder
#      finding above). CONCLUSION: the smaller, read/transcode-only build
#      is NOT reachable through this project's standard vendoring path;
#      this is an evaluated-and-verified negative finding, not an omitted
#      attempt.
#   2. The exported `ktx` target's own `enable_write` flag is hardcoded
#      `1` at its `common_libktx_settings(ktx 1 ${LIB_TYPE})` call site --
#      no CMake option threads through to disable it on the INSTALLED
#      target. The mitigations actually available and applied instead:
#      KTX_FEATURE_TOOLS/_TESTS/_DOC/_JNI/_PY=OFF (no CLI/GUI tools, no
#      test suite, no docs, no language bindings built at all -- toktx
#      itself is never compiled by this project; committed fixtures are
#      generated by the official prebuilt toktx release binary per
#      assets/test/ktx2/generate_fixtures.sh, matching D17's regeneration-
#      script discipline without paying toktx's own build cost here),
#      KTX_FEATURE_GL_UPLOAD/_VK_UPLOAD=OFF (D10: "upload stays ours" --
#      neither this project's own GL-free renderer nor its own Uploader
#      needs libktx's bundled GL/Vk upload helpers), KTX_FEATURE_ETC_UNPACK=OFF
#      (see its own comment below).
#
# KTX_FEATURE_KTX1 LEFT ON (default) -- EVALUATED AND REVERTED, this task:
# turning it OFF (this project's texture_cache.cpp calls only
# `ktxTexture2_*` entry points, D10 is KTX2-first, so KTX 1.0 container
# support -- lib/texture1.c/h -- looked like pure dead weight) produces an
# internally INCONSISTENT static `ktx` library at this pinned tag,
# reproduced directly this task: `lib/texture.c`'s generic
# `ktxTexture_CreateFromStream`/`ktxTexture_CreateFromMemory` dispatcher
# (compiled UNCONDITIONALLY, regardless of KTX_FEATURE_KTX1 -- it is the
# base-class auto-detect entry point, not a KTX1-only one) references
# `ktxTexture1_constructFromStreamAndHeader` unconditionally in its own
# object code with NO `#ifdef KTX_FEATURE_KTX1` guard around that call
# site -- verified directly against the pinned tag's lib/texture.c:336.
# With KTX_FEATURE_KTX1=OFF, texture1.c (which defines that symbol) is
# excluded from the source list entirely, leaving a real dangling
# undefined-symbol reference baked into texture.c.o inside the installed
# libktx.a -- invisible at libktx's OWN build/install step (nothing there
# tries to link the archive into an executable), surfacing only once a
# consumer's own link step pulls texture.c.o in transitively (this
# project's own rx_asset_gltf_gpu_tests build hit
# `ld.lld: error: undefined symbol: ktxTexture1_constructFromStreamAndHeader`
# the first time any target actually linked libktx.a, even though this
# project's own code never calls the generic dispatcher at all -- static
# archives link at whole-object-file granularity, and something this
# project DOES need pulls texture.c.o in alongside it). This project
# never calls the generic ktxTexture_CreateFromStream/CreateFromMemory
# dispatcher itself (texture_decode.cpp calls ktxTexture2_CreateFromMemory
# directly), so the broken code path is never actually EXECUTED -- but an
# undefined symbol at link time blocks the build regardless of whether the
# function is ever called at runtime. Reverting KTX_FEATURE_KTX1 to ON
# (its own documented default) is the minimal fix: texture1.c/h compile
# back in (real but small dead weight -- nothing in this project's own
# code references a ktxTexture1 symbol), the dangling reference resolves,
# and the library links cleanly on every target that needs any part of
# libktx.a. Recorded here so a future attempt to re-disable this flag
# does not have to rediscover the same failure from scratch.
#
# `ktx::ktx` is the correct namespaced target regardless of platform --
# `install(EXPORT KTXTargets FILE KtxTargets.cmake NAMESPACE KTX:: ...)`,
# verified directly against the pinned tag's own CMakeLists.txt (note the
# namespace is `KTX::`, capitalized, NOT `ktx::` -- verified exactly, a
# real pitfall this project's own find_package() call below must match).
set(RX_KTX_TAG "v4.4.2")
rx_add_cached_dependency(
  NAME ktx
  REPO https://github.com/KhronosGroup/KTX-Software.git
  TAG ${RX_KTX_TAG}
  CMAKE_ARGS -DKTX_FEATURE_TESTS=OFF -DKTX_FEATURE_TOOLS=OFF -DKTX_FEATURE_DOC=OFF
             -DKTX_FEATURE_JNI=OFF -DKTX_FEATURE_PY=OFF -DKTX_FEATURE_TOOLS_CTS=OFF
             -DKTX_FEATURE_GL_UPLOAD=OFF -DKTX_FEATURE_VK_UPLOAD=OFF
             # Explicit ON (not merely "omitted -> default"): this
             # dep-cache's own sub-build directory is reused by DEP_NAME
             # alone (cmake/DepCache.cmake's `_build_dir`, NOT keyed by
             # the content hash), so a value once cached there from an
             # EARLIER CMAKE_ARGS combination (this task tried
             # KTX_FEATURE_KTX1=OFF first, see this file's own comment
             # above) survives in that reused CMakeCache.txt unless a
             # later configure passes an explicit -D to override it --
             # relying on the option()'s own "default ON" is not
             # sufficient once that cache entry already exists. Passing
             # -DKTX_FEATURE_KTX1=ON explicitly here is what actually
             # forces it, regardless of sub-build-directory history.
             -DKTX_FEATURE_KTX1=ON
             # Ericsson-licensed lib/etcdec.cxx (LicenseRef-ETCSLA, a
             # separate non-OSI license) EXCLUDED: this project's transcode
             # targets are BC7/BC5 only (D10), never ETC -- see this file's
             # own LICENSE comment above.
             -DKTX_FEATURE_ETC_UNPACK=OFF
             -DBUILD_SHARED_LIBS=OFF
)
# NOTE the capitalization mismatch [verified directly against this task's
# own build output]: the dep-cache identity above is lowercase `ktx`
# (DEP_NAME, matching this project's own all-lowercase third-party naming
# convention), but the installed CMake package config file is
# `KtxConfig.cmake` (capital K, verified directly -- KTX-Software's own
# `write_basic_package_version_file("KtxConfigVersion.cmake" ...)` /
# `install(FILES ... KtxConfig.cmake ...)` calls use that exact
# capitalization), so `find_package()` must be called with `Ktx` (matching
# CMake's own <PackageName>Config.cmake resolution rule) or it silently
# fails to locate a real, successfully-installed package.
find_package(Ktx CONFIG REQUIRED PATHS "${ktx_CACHE_DIR}" NO_DEFAULT_PATH)
set_target_properties(KTX::ktx PROPERTIES IMPORTED_GLOBAL TRUE)
