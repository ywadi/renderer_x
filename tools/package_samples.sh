#!/usr/bin/env bash
# Packages this project's nine sample binaries into a single per-platform
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
#   06_materials      binary + materials/checker.slang + materials/rim.slang
#                     (this sample's OWN two materials) + material_shaders/
#                     material.slang + material_shaders/forward_entry.slang
#                     (rx_material's two SHARED shader files, deployed
#                     alongside this sample rather than referenced via
#                     RX_MATERIAL_SHADER_DIR's build-tree-only compile-time
#                     path [Task 8 ledger item carried from Task 5] -- see
#                     src/rx_material/include/rx_material/material_system.h's
#                     MaterialSystem::create() `sharedShaderDir` parameter
#                     and samples/06_materials/main.cpp's
#                     basePathDirectory()) + Slang runtime libs + LICENSE
#   07_stress         binary + shaders/stress/*.slang (4 files:
#                     instanced.vert.slang/instanced.frag.slang/
#                     tonemap.vert.slang/tonemap.frag.slang -- see
#                     samples/07_stress/CMakeLists.txt's own POST_BUILD
#                     deploy step) + Slang runtime libs + LICENSE (no other
#                     external asset -- every mesh is procedural, every
#                     instance's transform/color lives in a bindless
#                     storage buffer this sample uploads itself)
#   08_gltf_viewer    binary + material_shaders/ (material.slang/
#                     forward_entry.slang [rx_material's two shared files,
#                     same convention as 06] + standard_pbr.slang/
#                     unlit.slang [D22's shipped material library]) +
#                     tonemap.vert.slang/tonemap.frag.slang (shared
#                     shaders/multipass/ shaders, deployed flat, same
#                     convention as 05/07's own copies -- see
#                     samples/08_gltf_viewer/CMakeLists.txt's own POST_BUILD
#                     deploy step) + references/ (loading_state.png/
#                     loaded_scene.png, D17's committed lavapipe reference
#                     PNGs -- shipped so a redistributed binary's own
#                     `--validate` headless gate is self-contained too) +
#                     Slang runtime libs + LICENSE + a PRE-STAGED copy of
#                     the DamagedHelmet glTF asset (this sample's own
#                     default `--scene`, resolveDefaultScenePath()'s
#                     packaged-first lookup: `assets/DamagedHelmet/glTF/`
#                     next to the binary) plus that asset's own dual
#                     CC-BY-4.0/CC-BY-NC-4.0 attribution text -- UNLIKE
#                     every other sample here, this one genuinely
#                     redistributes third-party content, so the license
#                     text travels with it (tools/fetch_assets.sh's own
#                     header comment covers the exact same dual-license
#                     finding; this script prints/writes the identical
#                     attribution, not a fresh derivation of it).
#   09_scene          binary + material_shaders/ (material.slang/
#                     forward_entry.slang/standard_pbr.slang/unlit.slang,
#                     same convention as 08) + shadow_shaders/
#                     (shaders/shadow/shadow_caster.vert.slang -- this
#                     sample's own first runtime-deployed consumer of
#                     rx_shadow's shader) + tonemap.vert.slang/
#                     tonemap.frag.slang (flat, same convention as 05/07/08)
#                     + references/ (grid_scene.png, this sample's own D17
#                     committed lavapipe reference) + Slang runtime libs +
#                     LICENSE + a PRE-STAGED copy of the DamagedHelmet glTF
#                     asset (this sample's own default scene, same
#                     packaged-first lookup convention as 08's
#                     resolveHelmetScenePath()) plus its own dual
#                     CC-BY-4.0/CC-BY-NC-4.0 attribution text -- the SAME
#                     asset 08 already stages, staged again here since each
#                     sample's own packaged subdirectory is self-contained
#                     (no cross-sample sharing in the redistribution
#                     layout).
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

# --- 02_hotreload / 03_bindless_mesh / 04_streaming / 05_multipass /
# 06_materials / 07_stress: real in-process Slang compilation -- each needs
# the Slang runtime libs + LICENSE deployed next to it. Globbed rather than
# hardcoded to the pinned version string, same posture as
# rx_shader_deploy_runtime_libs() in src/rx_shader/CMakeLists.txt: Linux
# filenames embed the Slang version, Windows filenames don't [R:A1/A6/D2].
for RX_SAMPLE in 02_hotreload 03_bindless_mesh 04_streaming 05_multipass 06_materials 07_stress 08_gltf_viewer 09_scene; do
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
# to the binary exactly like the other five. 06 ships its OWN materials/
# subdirectory (checker.slang/rim.slang) PLUS a sibling material_shaders/
# subdirectory carrying rx_material's two shared files (material.slang/
# forward_entry.slang) -- see samples/06_materials/CMakeLists.txt's own
# POST_BUILD deploy steps for why two separate subdirectories, not a flat
# layout like 05's. 07 ships its own four on-disk shader sources flat (same
# convention as 05 -- see samples/07_stress/CMakeLists.txt's own POST_BUILD
# deploy step): instanced.vert.slang/instanced.frag.slang (the chunked
# forward pass) and tonemap.vert.slang/tonemap.frag.slang (its own copy,
# not shared with 05's -- see shaders/stress/tonemap.*.slang's own header
# comment for why this codebase does not share per-pass shader files
# across samples).
copy_required "$STAGE_DIR/02_hotreload" "$SAMPLES_BUILD_DIR/02_hotreload/hotreload.slang"
copy_required "$STAGE_DIR/03_bindless_mesh" "$SAMPLES_BUILD_DIR/03_bindless_mesh/texture.png"
copy_required "$STAGE_DIR/05_multipass" \
  "$SAMPLES_BUILD_DIR/05_multipass/scene_types.slang" \
  "$SAMPLES_BUILD_DIR/05_multipass/shadow.vert.slang" \
  "$SAMPLES_BUILD_DIR/05_multipass/lit.vert.slang" \
  "$SAMPLES_BUILD_DIR/05_multipass/lit.frag.slang" \
  "$SAMPLES_BUILD_DIR/05_multipass/tonemap.vert.slang" \
  "$SAMPLES_BUILD_DIR/05_multipass/tonemap.frag.slang"
mkdir -p "$STAGE_DIR/06_materials/materials" "$STAGE_DIR/06_materials/material_shaders"
copy_required "$STAGE_DIR/06_materials/materials" \
  "$SAMPLES_BUILD_DIR/06_materials/materials/checker.slang" \
  "$SAMPLES_BUILD_DIR/06_materials/materials/rim.slang"
copy_required "$STAGE_DIR/06_materials/material_shaders" \
  "$SAMPLES_BUILD_DIR/06_materials/material_shaders/material.slang" \
  "$SAMPLES_BUILD_DIR/06_materials/material_shaders/forward_entry.slang"
copy_required "$STAGE_DIR/07_stress" \
  "$SAMPLES_BUILD_DIR/07_stress/instanced.vert.slang" \
  "$SAMPLES_BUILD_DIR/07_stress/instanced.frag.slang" \
  "$SAMPLES_BUILD_DIR/07_stress/tonemap.vert.slang" \
  "$SAMPLES_BUILD_DIR/07_stress/tonemap.frag.slang"

# 08_gltf_viewer: material_shaders/ (rx_material's two shared files plus
# D22's StandardPBR/Unlit pair), the shared tonemap shaders (flat, same
# convention as 05/07), and D17's committed reference PNGs -- see this
# script's own header comment.
mkdir -p "$STAGE_DIR/08_gltf_viewer/material_shaders" "$STAGE_DIR/08_gltf_viewer/references"
copy_required "$STAGE_DIR/08_gltf_viewer/material_shaders" \
  "$SAMPLES_BUILD_DIR/08_gltf_viewer/material_shaders/material.slang" \
  "$SAMPLES_BUILD_DIR/08_gltf_viewer/material_shaders/forward_entry.slang" \
  "$SAMPLES_BUILD_DIR/08_gltf_viewer/material_shaders/standard_pbr.slang" \
  "$SAMPLES_BUILD_DIR/08_gltf_viewer/material_shaders/unlit.slang"
copy_required "$STAGE_DIR/08_gltf_viewer" \
  "$SAMPLES_BUILD_DIR/08_gltf_viewer/tonemap.vert.slang" \
  "$SAMPLES_BUILD_DIR/08_gltf_viewer/tonemap.frag.slang"
copy_required "$STAGE_DIR/08_gltf_viewer/references" \
  "$SAMPLES_BUILD_DIR/08_gltf_viewer/references/loading_state.png" \
  "$SAMPLES_BUILD_DIR/08_gltf_viewer/references/loaded_scene.png"

# 08_gltf_viewer's own default `--scene` asset, PRE-STAGED (UNLIKE every
# other sample here, which ships zero third-party content) so a
# redistributed copy runs out of the box with no separate fetch step --
# resolveDefaultScenePath()'s own packaged-first lookup (samples/
# 08_gltf_viewer/main.cpp) expects exactly `assets/DamagedHelmet/glTF/`
# next to the binary. Requires `tools/fetch_assets.sh` to have already
# populated `assets/fetched/DamagedHelmet/glTF/` (this script does not fetch
# it itself -- same "this script does not build/fetch anything" posture as
# its own header comment already states for the Slang runtime libs).
DAMAGED_HELMET_SRC="$REPO_ROOT/assets/fetched/DamagedHelmet/glTF"
if [[ ! -d "$DAMAGED_HELMET_SRC" ]]; then
  echo "package_samples: '$DAMAGED_HELMET_SRC' does not exist -- run tools/fetch_assets.sh first" >&2
  exit 1
fi
mkdir -p "$STAGE_DIR/08_gltf_viewer/assets/DamagedHelmet/glTF"
copy_required "$STAGE_DIR/08_gltf_viewer/assets/DamagedHelmet/glTF" \
  "$DAMAGED_HELMET_SRC/DamagedHelmet.gltf" \
  "$DAMAGED_HELMET_SRC/DamagedHelmet.bin" \
  "$DAMAGED_HELMET_SRC/Default_albedo.jpg" \
  "$DAMAGED_HELMET_SRC/Default_AO.jpg" \
  "$DAMAGED_HELMET_SRC/Default_emissive.jpg" \
  "$DAMAGED_HELMET_SRC/Default_metalRoughness.jpg" \
  "$DAMAGED_HELMET_SRC/Default_normal.jpg"

# DamagedHelmet's own dual-license attribution -- verified text, identical
# to tools/fetch_assets.sh's own printed lines (that script's own header
# comment has the full LICENSE.md-sourced finding: BOTH CC-BY-4.0 (the
# glTF rebuild/conversion) AND CC-BY-NC-4.0 (the earlier model this rebuild
# incorporates) apply, not plain CC BY as the original planning documents
# assumed) -- written into the packaged asset's own directory so it travels
# with the redistributed files rather than living only in a build script.
#
# [Fix round, item 4 -- coordinator ruling] This notice is the human-
# readable SUMMARY only, not the whole legal grant -- a public release zip
# ships the FULL legal texts too (fetched once from creativecommons.org and
# vendored, committed, into this sample's own licenses/ directory --
# licenses/CC-BY-4.0.txt / CC-BY-NC-4.0.txt -- rather than fetched at
# package time, so packaging never gains a network dependency it did not
# have before; a small, static legal document is exactly the kind of
# third-party content this project already commits directly, unlike
# assets/fetched/'s own large, versioned binary content).
cat >"$STAGE_DIR/08_gltf_viewer/assets/DamagedHelmet/LICENSE.txt" <<'RX_DAMAGED_HELMET_LICENSE'
DamagedHelmet -- CC-BY-4.0 AND CC-BY-NC-4.0

  (c) 2018 ctxwing (rebuild and glTF conversion, CC-BY-4.0)
  (c) 2016 theblueturtle_ (earlier model, CC-BY-NC-4.0)

  Source: https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/DamagedHelmet

The combination of these two licenses means the whole asset carries the
Non-Commercial restriction forward (CC-BY-NC-4.0), not a plain CC BY grant.
The full legal text of both licenses is bundled alongside this file:
CC-BY-4.0.txt and CC-BY-NC-4.0.txt (fetched verbatim from
creativecommons.org/licenses/{by,by-nc}/4.0/legalcode.txt).
RX_DAMAGED_HELMET_LICENSE
copy_required "$STAGE_DIR/08_gltf_viewer/assets/DamagedHelmet" \
  "$REPO_ROOT/samples/08_gltf_viewer/licenses/CC-BY-4.0.txt" \
  "$REPO_ROOT/samples/08_gltf_viewer/licenses/CC-BY-NC-4.0.txt"

# 09_scene: material_shaders/ (rx_material's shared files + D22's
# StandardPBR/Unlit pair, same as 08) + shadow_shaders/ (rx_shadow's own
# shadow_caster.vert.slang) + the shared tonemap shaders (flat) + this
# sample's own D17 reference PNG -- see this script's own header comment.
mkdir -p "$STAGE_DIR/09_scene/material_shaders" "$STAGE_DIR/09_scene/shadow_shaders" "$STAGE_DIR/09_scene/references"
copy_required "$STAGE_DIR/09_scene/material_shaders" \
  "$SAMPLES_BUILD_DIR/09_scene/material_shaders/material.slang" \
  "$SAMPLES_BUILD_DIR/09_scene/material_shaders/forward_entry.slang" \
  "$SAMPLES_BUILD_DIR/09_scene/material_shaders/standard_pbr.slang" \
  "$SAMPLES_BUILD_DIR/09_scene/material_shaders/unlit.slang"
copy_required "$STAGE_DIR/09_scene/shadow_shaders" \
  "$SAMPLES_BUILD_DIR/09_scene/shadow_shaders/shadow_caster.vert.slang"
copy_required "$STAGE_DIR/09_scene" \
  "$SAMPLES_BUILD_DIR/09_scene/tonemap.vert.slang" \
  "$SAMPLES_BUILD_DIR/09_scene/tonemap.frag.slang"
copy_required "$STAGE_DIR/09_scene/references" \
  "$SAMPLES_BUILD_DIR/09_scene/references/grid_scene.png"

# 09_scene's own default scene asset -- the SAME pre-staged DamagedHelmet
# copy 08 ships, staged again here (each packaged sample subdirectory is
# self-contained -- see this script's own header comment).
mkdir -p "$STAGE_DIR/09_scene/assets/DamagedHelmet/glTF"
copy_required "$STAGE_DIR/09_scene/assets/DamagedHelmet/glTF" \
  "$DAMAGED_HELMET_SRC/DamagedHelmet.gltf" \
  "$DAMAGED_HELMET_SRC/DamagedHelmet.bin" \
  "$DAMAGED_HELMET_SRC/Default_albedo.jpg" \
  "$DAMAGED_HELMET_SRC/Default_AO.jpg" \
  "$DAMAGED_HELMET_SRC/Default_emissive.jpg" \
  "$DAMAGED_HELMET_SRC/Default_metalRoughness.jpg" \
  "$DAMAGED_HELMET_SRC/Default_normal.jpg"
cat >"$STAGE_DIR/09_scene/assets/DamagedHelmet/LICENSE.txt" <<'RX_DAMAGED_HELMET_LICENSE'
DamagedHelmet -- CC-BY-4.0 AND CC-BY-NC-4.0

  (c) 2018 ctxwing (rebuild and glTF conversion, CC-BY-4.0)
  (c) 2016 theblueturtle_ (earlier model, CC-BY-NC-4.0)

  Source: https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/DamagedHelmet

The combination of these two licenses means the whole asset carries the
Non-Commercial restriction forward (CC-BY-NC-4.0), not a plain CC BY grant.
The full legal text of both licenses is bundled alongside this file:
CC-BY-4.0.txt and CC-BY-NC-4.0.txt (fetched verbatim from
creativecommons.org/licenses/{by,by-nc}/4.0/legalcode.txt).
RX_DAMAGED_HELMET_LICENSE
copy_required "$STAGE_DIR/09_scene/assets/DamagedHelmet" \
  "$REPO_ROOT/samples/09_scene/licenses/CC-BY-4.0.txt" \
  "$REPO_ROOT/samples/09_scene/licenses/CC-BY-NC-4.0.txt"

mkdir -p "$(dirname "$RX_OUT_ZIP")"
RX_OUT_ZIP_ABS="$(cd "$(dirname "$RX_OUT_ZIP")" && pwd)/$(basename "$RX_OUT_ZIP")"
rm -f "$RX_OUT_ZIP_ABS"

echo "package_samples: zipping into '$RX_OUT_ZIP_ABS' ..."
(cd "$STAGE_DIR" && zip -r -X -q "$RX_OUT_ZIP_ABS" 01_triangle 02_hotreload 03_bindless_mesh 04_streaming 05_multipass 06_materials 07_stress 08_gltf_viewer 09_scene)

echo "package_samples: done. Contents:"
unzip -l "$RX_OUT_ZIP_ABS"
