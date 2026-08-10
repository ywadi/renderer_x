#!/usr/bin/env bash
# Packages this project's five sample binaries into a single per-platform
# .zip laid out exactly as a user would unzip-and-run it: one subdirectory
# per sample, containing that sample's binary plus everything IT needs to
# run standalone -- nothing more, nothing missing [R:D2].
#
# Usage: tools/package_samples.sh <preset> <slang-platform-dir> <output-zip>
#   <preset>             linux-native | windows-cross-zig
#                        (selects the build dir + the executable suffix)
#   <slang-platform-dir> linux-x86_64 | windows-x86_64
#                        (the third_party/slang-prebuilt/<this> subdirectory
#                        fetch_slang.cmake populated for this preset's
#                        *target* -- see that file's own header comment; used
#                        here only to locate the Slang LICENSE file to bundle
#                        alongside any sample that ships Slang runtime libs)
#   <output-zip>         path (relative or absolute) to write the .zip to;
#                        parent directories are created if needed.
#
# Per-sample contents [R:D2, spec Fixed decision #11]:
#   01_triangle       binary + triangle.vert.spv + triangle.frag.spv
#                     (precompiled offline by slangc at build time -- NO
#                     Slang runtime libraries needed; this sample never
#                     compiles a shader in-process. Keep this distinction
#                     sharp: it's the one sample that doesn't need Slang.)
#   02_hotreload      binary + hotreload.slang + Slang runtime libs + LICENSE
#   03_bindless_mesh  binary + texture.png + Slang runtime libs + LICENSE
#   04_streaming      binary + Slang runtime libs + LICENSE
#                     (no external asset -- every texture is procedural)
#   05_multipass      binary + shaders/multipass/*.slang (6 files, incl.
#                     scene_types.slang [fix round 1] -- the single source
#                     of truth for ObjectTransform, concatenated ahead of
#                     both shadow.vert.slang and lit.vert.slang at compile
#                     time, so it must ship too) + Slang runtime libs +
#                     LICENSE (no other external asset -- every texture
#                     this sample uses is a graph-pooled transient, never
#                     an on-disk file)
#
# This script does NOT build anything -- it assumes `cmake --build
# --preset <preset>` already ran and each sample's build-output directory
# already has its runtime libs/assets deployed next to its binary (that
# deployment is CMake's job: rx_shader_deploy_runtime_libs() in
# src/rx_shader/CMakeLists.txt for the Slang libs, and each sample's own
# POST_BUILD copy step for its on-disk asset). This script's only job is to
# select, per sample, the subset of that build-output directory a
# redistributed copy actually needs -- leaving out CMake/Ninja build
# bookkeeping (CMakeFiles/, cmake_install.cmake, CTestTestfile.cmake, *.pdb,
# etc.) that a real user unzipping this would never want -- and to add the
# Slang LICENSE alongside any sample that bundles Slang's shared libraries
# (Apache-2.0 attribution).
#
# Every expected file is verified to exist before being copied: a missing
# file fails this script loudly (non-zero exit, clear message) rather than
# silently shipping an incomplete redistribution layout -- same
# investigate-don't-skip discipline the rest of this project's CI uses.
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <preset> <slang-platform-dir> <output-zip>" >&2
  echo "  preset:             linux-native | windows-cross-zig" >&2
  echo "  slang-platform-dir: linux-x86_64 | windows-x86_64" >&2
  echo "  output-zip:         path to write the packaged .zip to" >&2
  exit 1
fi

RX_PRESET="$1"
RX_SLANG_PLATFORM_DIR="$2"
RX_OUT_ZIP="$3"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SAMPLES_BUILD_DIR="$REPO_ROOT/build/$RX_PRESET/samples"
SLANG_LICENSE="$REPO_ROOT/third_party/slang-prebuilt/$RX_SLANG_PLATFORM_DIR/LICENSE"

case "$RX_PRESET" in
  linux-native)
    RX_EXE_SUFFIX=""
    ;;
  windows-cross-zig)
    RX_EXE_SUFFIX=".exe"
    ;;
  *)
    echo "package_samples: unknown preset '$RX_PRESET' (expected linux-native or windows-cross-zig)" >&2
    exit 1
    ;;
esac

if [[ ! -d "$SAMPLES_BUILD_DIR" ]]; then
  echo "package_samples: '$SAMPLES_BUILD_DIR' does not exist -- build the '$RX_PRESET' preset first" >&2
  exit 1
fi

STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGE_DIR"' EXIT

# Copies every path given, failing loudly if any of them doesn't exist --
# used both for required files (binary, on-disk assets, LICENSE) and for
# the Slang runtime-lib globs below (an unmatched glob is passed through
# by bash as the literal, non-existent pattern string, so it fails this
# same existence check rather than silently copying zero files).
copy_required() {
  local dest_dir="$1"
  shift
  local src
  for src in "$@"; do
    if [[ ! -e "$src" ]]; then
      echo "package_samples: expected file '$src' does not exist -- redistribution layout is incomplete" >&2
      exit 1
    fi
    cp -a "$src" "$dest_dir/"
  done
}

echo "package_samples: staging '$RX_PRESET' samples in '$STAGE_DIR' ..."

# --- 01_triangle: precompiled SPIR-V only, no Slang runtime libs [D2] -----
SAMPLE_DIR="$STAGE_DIR/01_triangle"
mkdir -p "$SAMPLE_DIR"
copy_required "$SAMPLE_DIR" \
  "$SAMPLES_BUILD_DIR/01_triangle/sample_01_triangle${RX_EXE_SUFFIX}" \
  "$SAMPLES_BUILD_DIR/01_triangle/triangle.vert.spv" \
  "$SAMPLES_BUILD_DIR/01_triangle/triangle.frag.spv"

# --- 02_hotreload / 03_bindless_mesh / 04_streaming / 05_multipass: real
# in-process Slang compilation -- each needs the Slang runtime libs +
# LICENSE deployed next to it. Globbed rather than hardcoded to the pinned
# version string, same posture as rx_shader_deploy_runtime_libs() in
# src/rx_shader/CMakeLists.txt: Linux filenames embed the Slang version,
# Windows filenames don't [R:A1/A6/D2].
for RX_SAMPLE in 02_hotreload 03_bindless_mesh 04_streaming 05_multipass; do
  SAMPLE_DIR="$STAGE_DIR/$RX_SAMPLE"
  mkdir -p "$SAMPLE_DIR"
  copy_required "$SAMPLE_DIR" "$SAMPLES_BUILD_DIR/$RX_SAMPLE/sample_${RX_SAMPLE}${RX_EXE_SUFFIX}"
  copy_required "$SAMPLE_DIR" "$SLANG_LICENSE"

  if [[ "$RX_PRESET" == "windows-cross-zig" ]]; then
    copy_required "$SAMPLE_DIR" \
      "$SAMPLES_BUILD_DIR/$RX_SAMPLE/slang-compiler.dll" \
      "$SAMPLES_BUILD_DIR/$RX_SAMPLE/slang-glslang.dll" \
      "$SAMPLES_BUILD_DIR/$RX_SAMPLE/slang-glsl-module.dll" \
      "$SAMPLES_BUILD_DIR/$RX_SAMPLE/slang-rt.dll"
  else
    copy_required "$SAMPLE_DIR" \
      "$SAMPLES_BUILD_DIR/$RX_SAMPLE"/libslang-compiler.so* \
      "$SAMPLES_BUILD_DIR/$RX_SAMPLE"/libslang-glslang-*.so \
      "$SAMPLES_BUILD_DIR/$RX_SAMPLE"/libslang-glsl-module-*.so \
      "$SAMPLES_BUILD_DIR/$RX_SAMPLE"/libslang-rt.so*
  fi
done

# 02_hotreload additionally ships the live-reloadable shader source; 03
# additionally ships its one real-PNG texture. 04 has no external asset
# (every texture is procedurally generated) -- nothing extra for it. 05
# ships its own six on-disk shader sources (see samples/05_multipass/
# CMakeLists.txt's own POST_BUILD deploy step for why these six, flat, no
# "multipass/" subdirectory in the deployed layout) -- scene_types.slang
# [fix round 1] included: it is concatenated ahead of shadow.vert.slang/
# lit.vert.slang at compile time, so a packaged run needs it on disk next
# to the binary exactly like the other five.
copy_required "$STAGE_DIR/02_hotreload" "$SAMPLES_BUILD_DIR/02_hotreload/hotreload.slang"
copy_required "$STAGE_DIR/03_bindless_mesh" "$SAMPLES_BUILD_DIR/03_bindless_mesh/texture.png"
copy_required "$STAGE_DIR/05_multipass" \
  "$SAMPLES_BUILD_DIR/05_multipass/scene_types.slang" \
  "$SAMPLES_BUILD_DIR/05_multipass/shadow.vert.slang" \
  "$SAMPLES_BUILD_DIR/05_multipass/lit.vert.slang" \
  "$SAMPLES_BUILD_DIR/05_multipass/lit.frag.slang" \
  "$SAMPLES_BUILD_DIR/05_multipass/tonemap.vert.slang" \
  "$SAMPLES_BUILD_DIR/05_multipass/tonemap.frag.slang"

mkdir -p "$(dirname "$RX_OUT_ZIP")"
RX_OUT_ZIP_ABS="$(cd "$(dirname "$RX_OUT_ZIP")" && pwd)/$(basename "$RX_OUT_ZIP")"
rm -f "$RX_OUT_ZIP_ABS"

echo "package_samples: zipping into '$RX_OUT_ZIP_ABS' ..."
(cd "$STAGE_DIR" && zip -r -X -q "$RX_OUT_ZIP_ABS" 01_triangle 02_hotreload 03_bindless_mesh 04_streaming 05_multipass)

echo "package_samples: done. Contents:"
unzip -l "$RX_OUT_ZIP_ABS"
