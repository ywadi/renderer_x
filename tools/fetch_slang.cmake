# Fetches prebuilt Slang shader-compiler archives from GitHub releases.
# Slang is NEVER compiled from source in this project -- only pinned
# prebuilt release binaries are used.
#
# Two independent things get fetched, for two independent reasons:
#
# 1. slangc (host-side tooling): it runs on the machine doing the build,
#    not on whatever the build's *target* is. That means this always
#    fetches the HOST's archive, even when the active CMake toolchain
#    targets a different platform (e.g. the windows-cross-zig preset still
#    runs slangc here on Linux to produce SPIR-V, then just copies the
#    resulting .spv bytes into whichever target binary needs them). Guard
#    on CMAKE_HOST_SYSTEM_NAME (the machine actually invoking cmake), never
#    on CMAKE_SYSTEM_NAME/CMAKE_SYSTEM_PROCESSOR (those describe the
#    *target*, and would say "Windows" under windows-cross-zig despite
#    slangc still running here).
# 2. The Slang runtime library (`slang::slang`, i.e. libslang-compiler.so /
#    slang-compiler.dll + its import lib) that rx_shader links against and
#    ships next to any binary doing in-process Slang compilation. This one
#    IS target-side -- an import lib/DLL built for Windows is useless when
#    the target is Linux and vice versa -- so it's fetched per
#    CMAKE_SYSTEM_NAME instead. On a native build (linux-native), target ==
#    host, so this reuses the exact same extracted tree as slangc above;
#    no second download. On windows-cross-zig, this is a second archive
#    (a real MSVC-built Windows tree) fetched alongside the host's Linux
#    one.
#
# Produces:
#   RX_SLANGC            - absolute path to the extracted host-side slangc
#                           executable (cache variable).
#   RX_SLANG_ROOT         - root of the fetched host-side Slang prebuilt
#                           tree (cache variable).
#   RX_SLANG_TARGET_ROOT - root of the Slang prebuilt tree matching the
#                           current CMAKE_SYSTEM_NAME target -- what
#                           rx_shader's find_package(slang CONFIG) should
#                           be pointed at (cache variable).
#   slang_ROOT            - same value as RX_SLANG_TARGET_ROOT, exposed
#                           under the exact name find_package(slang ...)
#                           consults automatically (CMP0074) so callers can
#                           just say find_package(slang CONFIG REQUIRED)
#                           with no extra PATHS plumbing.

set(RX_SLANG_VERSION "2026.14.1")

# Fetches and extracts one platform's prebuilt Slang archive into
# third_party/slang-prebuilt/<platform_dir>, gated on a version-keyed
# marker file (.rx-fetched-<version>) rather than the old Phase 1
# unversioned ".rx-fetched" marker. That old marker is a real defect
# (tracked in the Phase 1 deferred findings): because it didn't encode
# which version was actually fetched, bumping RX_SLANG_VERSION left the
# marker in place from a stale prior fetch, and the fetch step silently
# skipped re-downloading -- the tree on disk stayed at the OLD version
# while every other part of the build assumed the newly-pinned one. A
# version-keyed marker makes a version bump self-invalidating: the marker
# this function looks for changes name the moment RX_SLANG_VERSION changes,
# so a mismatched/absent marker always forces a fresh fetch. Any leftover
# Phase-1-era unversioned marker is never consulted as evidence of
# "already fetched" -- it is simply irrelevant now, and gets removed along
# with the rest of the directory whenever a fetch actually runs (see
# file(REMOVE_RECURSE...) below).
function(_rx_fetch_slang_platform)
  set(oneValueArgs PLATFORM_DIR ARCHIVE_NAME SENTINEL_RELATIVE ROOT_OUT_VAR)
  cmake_parse_arguments(ARG "" "${oneValueArgs}" "" ${ARGN})

  set(_root "${CMAKE_SOURCE_DIR}/third_party/slang-prebuilt/${ARG_PLATFORM_DIR}")
  set(_marker "${_root}/.rx-fetched-${RX_SLANG_VERSION}")
  set(_url "https://github.com/shader-slang/slang/releases/download/v${RX_SLANG_VERSION}/${ARG_ARCHIVE_NAME}")

  if(NOT EXISTS "${_marker}")
    message(STATUS "[fetch_slang] Fetching Slang ${RX_SLANG_VERSION} prebuilt (${ARG_PLATFORM_DIR}) from ${_url} ...")

    # Wipe any previous extraction (a stale different-version tree, a
    # leftover Phase-1-era unversioned marker, or a partial extraction from
    # an interrupted fetch) rather than extracting on top of it -- Slang's
    # own archive layout has changed between releases before (see the
    # A6 threading/ABI research note), so old files from a different
    # version must never linger alongside the new ones.
    file(REMOVE_RECURSE "${_root}")
    file(MAKE_DIRECTORY "${_root}")

    set(_archive_path "${CMAKE_BINARY_DIR}/_slang-fetch/${ARG_ARCHIVE_NAME}")
    file(DOWNLOAD "${_url}" "${_archive_path}" STATUS _dl_status)
    list(GET _dl_status 0 _dl_code)
    list(GET _dl_status 1 _dl_message)
    if(NOT _dl_code EQUAL 0)
      file(REMOVE "${_archive_path}")
      message(FATAL_ERROR "[fetch_slang] Download failed (${_dl_message}) for URL: ${_url}")
    endif()

    file(ARCHIVE_EXTRACT INPUT "${_archive_path}" DESTINATION "${_root}")
    file(REMOVE "${_archive_path}")

    if(NOT EXISTS "${_root}/${ARG_SENTINEL_RELATIVE}")
      message(FATAL_ERROR
        "[fetch_slang] Extracted archive has no '${ARG_SENTINEL_RELATIVE}' under "
        "'${_root}'. The upstream archive layout may have changed -- inspect "
        "that directory and update this script's assumed layout.")
    endif()

    file(WRITE "${_marker}" "${_url}\n")
  else()
    message(STATUS "[fetch_slang] Slang ${RX_SLANG_VERSION} prebuilt already present at ${_root} (marker found) - skipping download")
  endif()

  set("${ARG_ROOT_OUT_VAR}" "${_root}" PARENT_SCOPE)
endfunction()

# --- 1. Host-side slangc -----------------------------------------------
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  # Verified directly against the pinned release: shader-slang publishes a
  # single glibc-2.27-baseline linux-x86_64 archive, no per-distro variants.
  set(RX_SLANG_HOST_PLATFORM "linux-x86_64")
  set(RX_SLANG_HOST_ARCHIVE "slang-${RX_SLANG_VERSION}-linux-x86_64-glibc-2.27.tar.gz")
else()
  message(FATAL_ERROR
    "[fetch_slang] No prebuilt Slang archive mapping for host system "
    "'${CMAKE_HOST_SYSTEM_NAME}'. slangc runs on the host, not the cross-"
    "compilation target, so a host-specific archive must be added to "
    "tools/fetch_slang.cmake before building on this host.")
endif()

# Verified directly against the real 2026.14.1 linux-x86_64-glibc-2.27
# archive (extracted and inspected its tree): it has NO top-level wrapper
# directory -- bin/, include/, lib/, share/ sit right at the archive root,
# so slangc lands at "<root>/bin/slangc" -- used below as both the fetch
# sentinel and RX_SLANGC's location.
_rx_fetch_slang_platform(
  PLATFORM_DIR "${RX_SLANG_HOST_PLATFORM}"
  ARCHIVE_NAME "${RX_SLANG_HOST_ARCHIVE}"
  SENTINEL_RELATIVE "bin/slangc"
  ROOT_OUT_VAR RX_SLANG_HOST_ROOT
)

set(RX_SLANG_ROOT "${RX_SLANG_HOST_ROOT}" CACHE PATH "Root of the fetched host-side Slang prebuilt tree" FORCE)

# Also verified: slangc's ELF RUNPATH is "$ORIGIN/../lib:$ORIGIN", so it
# resolves its own shared libraries (libslang.so etc.) relative to its own
# location without needing LD_LIBRARY_PATH set by the caller.
set(RX_SLANGC "${RX_SLANG_ROOT}/bin/slangc" CACHE FILEPATH "Path to the fetched slangc compiler executable" FORCE)

if(NOT EXISTS "${RX_SLANGC}")
  message(FATAL_ERROR "[fetch_slang] RX_SLANGC ('${RX_SLANGC}') does not exist after fetch/marker check.")
endif()

# --- 2. Target-side Slang runtime library (what rx_shader links) -------
#
# CMAKE_SYSTEM_NAME describes the build *target* here (unlike the host
# guard above, this is deliberately the target, not the host) -- exactly
# the distinction the windows-cross-zig toolchain file exists to make:
# CMAKE_SYSTEM_NAME is "Windows" there even though everything in section 1
# above still runs on this Linux host.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  # Verified directly against the real 2026.14.1 windows-x86_64 archive:
  # unlike the Linux layout, the CMake package sits at a top-level "cmake/"
  # (not "lib/cmake/slang/"), and all binaries/import libs live in
  # flat bin/ and lib/ dirs -- see the docstring at the top of this file
  # and [R:A1] for the full archive layout comparison.
  set(RX_SLANG_WINDOWS_ARCHIVE "slang-${RX_SLANG_VERSION}-windows-x86_64.tar.gz")
  _rx_fetch_slang_platform(
    PLATFORM_DIR "windows-x86_64"
    ARCHIVE_NAME "${RX_SLANG_WINDOWS_ARCHIVE}"
    SENTINEL_RELATIVE "cmake/slangConfig.cmake"
    ROOT_OUT_VAR RX_SLANG_WINDOWS_ROOT
  )
  set(RX_SLANG_TARGET_ROOT "${RX_SLANG_WINDOWS_ROOT}" CACHE PATH
    "Root of the Slang prebuilt tree matching the current CMAKE_SYSTEM_NAME target" FORCE)
else()
  # Native build (e.g. linux-native): target == host, reuse the same tree
  # already fetched above instead of fetching a second, identical copy.
  set(RX_SLANG_TARGET_ROOT "${RX_SLANG_ROOT}" CACHE PATH
    "Root of the Slang prebuilt tree matching the current CMAKE_SYSTEM_NAME target" FORCE)
endif()

# find_package(<PackageName> ...)'s Config-mode search consults the cache/
# environment variable "<PackageName>_ROOT" automatically (CMP0074, default
# NEW since this project's cmake_minimum_required is well above 3.12) as
# one of the installation-prefix candidates fed into its standard
# W/U-convention subdirectory search (see the CMake find_package docs:
# "<prefix>/(cmake|CMake)/" and "<prefix>/(lib/<arch>|lib*|share)/cmake/
# <name>*/" are BOTH always searched under every prefix, regardless of
# host OS) -- so exposing this one variable is sufficient for rx_shader's
# plain `find_package(slang CONFIG REQUIRED)` to find slangConfig.cmake
# whether it's under RX_SLANG_TARGET_ROOT/lib/cmake/slang/ (Linux layout)
# or RX_SLANG_TARGET_ROOT/cmake/ (Windows layout), with no per-platform
# branching needed at the call site.
set(slang_ROOT "${RX_SLANG_TARGET_ROOT}" CACHE PATH
  "Root of the Slang prebuilt tree find_package(slang CONFIG) should search (CMP0074)" FORCE)
