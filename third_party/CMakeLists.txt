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
