# Fetches the prebuilt Slang shader compiler (slangc) from its GitHub
# release archive. Slang is NEVER compiled from source in this project --
# only pinned prebuilt release binaries are used.
#
# slangc is host-side tooling: it runs on the machine doing the build, not
# on whatever the build's *target* is. That means this must always fetch the
# HOST's archive, even when the active CMake toolchain targets a different
# platform (e.g. the windows-cross-zig preset still runs slangc here on
# Linux to produce SPIR-V, then just copies the resulting .spv bytes into
# whichever target binary needs them). Guard on CMAKE_HOST_SYSTEM_NAME (the
# machine actually invoking cmake), never on CMAKE_SYSTEM_NAME/
# CMAKE_SYSTEM_PROCESSOR (those describe the *target*, and would say
# "Windows" under windows-cross-zig despite slangc still running here).
#
# Produces: RX_SLANGC (cache variable, absolute path to the extracted
# slangc executable).

set(RX_SLANG_VERSION "2026.14.1")

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  # Verified directly against the pinned release: shader-slang publishes a
  # single glibc-2.27-baseline linux-x86_64 archive, no per-distro variants.
  set(RX_SLANG_PLATFORM "linux-x86_64")
  set(RX_SLANG_ARCHIVE_NAME "slang-${RX_SLANG_VERSION}-linux-x86_64-glibc-2.27.tar.gz")
else()
  message(FATAL_ERROR
    "[fetch_slang] No prebuilt Slang archive mapping for host system "
    "'${CMAKE_HOST_SYSTEM_NAME}'. slangc runs on the host, not the cross-"
    "compilation target, so a host-specific archive must be added to "
    "tools/fetch_slang.cmake before building on this host.")
endif()

set(RX_SLANG_URL
  "https://github.com/shader-slang/slang/releases/download/v${RX_SLANG_VERSION}/${RX_SLANG_ARCHIVE_NAME}")

# third_party/slang-prebuilt/ is git-ignored (see .gitignore) -- this is
# fetched binary tooling, not source under version control.
set(RX_SLANG_ROOT "${CMAKE_SOURCE_DIR}/third_party/slang-prebuilt/${RX_SLANG_PLATFORM}")
set(RX_SLANG_MARKER "${RX_SLANG_ROOT}/.rx-fetched")

if(NOT EXISTS "${RX_SLANG_MARKER}")
  message(STATUS "[fetch_slang] Fetching Slang ${RX_SLANG_VERSION} prebuilt (${RX_SLANG_PLATFORM}) from ${RX_SLANG_URL} ...")

  file(MAKE_DIRECTORY "${RX_SLANG_ROOT}")
  set(_rx_slang_archive_path "${CMAKE_BINARY_DIR}/_slang-fetch/${RX_SLANG_ARCHIVE_NAME}")

  file(DOWNLOAD "${RX_SLANG_URL}" "${_rx_slang_archive_path}"
       STATUS _rx_slang_dl_status)
  list(GET _rx_slang_dl_status 0 _rx_slang_dl_code)
  list(GET _rx_slang_dl_status 1 _rx_slang_dl_message)
  if(NOT _rx_slang_dl_code EQUAL 0)
    file(REMOVE "${_rx_slang_archive_path}")
    message(FATAL_ERROR
      "[fetch_slang] Download failed (${_rx_slang_dl_message}) for URL: ${RX_SLANG_URL}")
  endif()

  file(ARCHIVE_EXTRACT INPUT "${_rx_slang_archive_path}" DESTINATION "${RX_SLANG_ROOT}")
  file(REMOVE "${_rx_slang_archive_path}")

  # Verified directly against the real 2026.14.1 linux-x86_64-glibc-2.27
  # archive (extracted and inspected its tree): it has NO top-level wrapper
  # directory -- bin/, include/, lib/, share/ sit right at the archive
  # root, so slangc lands at "${RX_SLANG_ROOT}/bin/slangc". Do not assume
  # this blindly for other platforms/versions if this script is ever
  # extended -- re-verify against the actual extracted tree first.
  #
  # Also verified: slangc's ELF RUNPATH is "$ORIGIN/../lib:$ORIGIN", so it
  # resolves its own shared libraries (libslang.so etc.) relative to its own
  # location without needing LD_LIBRARY_PATH set by the caller.
  if(NOT EXISTS "${RX_SLANG_ROOT}/bin/slangc")
    message(FATAL_ERROR
      "[fetch_slang] Extracted archive has no 'bin/slangc' under "
      "'${RX_SLANG_ROOT}'. The upstream archive layout may have changed -- "
      "inspect that directory and update this script's assumed layout.")
  endif()

  file(WRITE "${RX_SLANG_MARKER}" "${RX_SLANG_URL}\n")
else()
  message(STATUS "[fetch_slang] Slang ${RX_SLANG_VERSION} prebuilt already present at ${RX_SLANG_ROOT} (marker found) - skipping download")
endif()

set(RX_SLANGC "${RX_SLANG_ROOT}/bin/slangc" CACHE FILEPATH "Path to the fetched slangc compiler executable")

if(NOT EXISTS "${RX_SLANGC}")
  message(FATAL_ERROR "[fetch_slang] RX_SLANGC ('${RX_SLANGC}') does not exist after fetch/marker check.")
endif()
