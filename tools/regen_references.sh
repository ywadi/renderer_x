#!/usr/bin/env bash
set -euo pipefail

# tools/regen_references.sh -- regenerates sample 08_gltf_viewer's own D17
# committed reference PNGs (samples/08_gltf_viewer/references/
# loading_state.png / loaded_scene.png) [Phase 4 Stage 1 Task 16, spec D17].
#
# NEVER AUTO-RUN: no CI workflow, build target, or test invokes this script.
# It exists purely as a documented, deliberate, human-invoked maintenance
# step for the one situation that legitimately needs a new reference pair --
# a real, intentional rendering change (StandardPBR/Unlit shader edits, the
# default light/ambient tuning in samples/08_gltf_viewer/main.cpp's own
# updateDrawDataPerPassFields(), a DamagedHelmet asset version bump, ...).
# Running it and committing its output IS the mechanism by which such a
# change updates the gate's own expectation -- there is no other way to
# regenerate these two files, and nothing regenerates them silently.
#
# WHY LAVAPIPE SPECIFICALLY [D17]: the committed references are the ONE
# thing sample 08_gltf_viewer's own headless `--validate` gate enforces a
# hard PASS/FAIL verdict against (main.cpp's own isLavapipeDevice() check) --
# every other driver's own divergence from them is logged as INFO only.
# Regenerating against any OTHER driver would silently change what "correct"
# means for every future lavapipe run (including CI, which only ever has
# lavapipe available) -- this script FORCES VK_ICD_FILENAMES to the system's
# real lavapipe ICD (mesa-vulkan-drivers' own lvp_icd.json, the same package
# CI installs) specifically so a maintainer cannot accidentally regenerate
# against whatever GPU their own dev machine happens to have instead.
#
# USAGE: tools/regen_references.sh [preset]
#   preset  linux-native (default) -- the build preset to build/run.
#           windows-cross-zig is NOT supported: lavapipe (a Linux Vulkan
#           ICD) cannot run under Wine, and this script's whole point is a
#           lavapipe-rendered reference.
#
# PREREQUISITES:
#   - mesa-vulkan-drivers installed (provides /usr/share/vulkan/icd.d/
#     lvp_icd.json + libvulkan_lvp.so) -- the same package CI's own
#     "Test (xvfb + lavapipe)" step relies on being the ONLY Vulkan ICD
#     present there; this script does not install it for you.
#   - assets/fetched/DamagedHelmet/glTF/ already populated (tools/
#     fetch_assets.sh) -- this sample's own default `--scene`.
#   - xvfb-run available (SDL3 needs a video driver backend even for a
#     hidden window).
#
# AFTER RUNNING: `git diff --stat samples/08_gltf_viewer/references/` and
# actually LOOK at the two PNGs before committing -- a regenerated reference
# that "looks wrong" is a real regression this script cannot itself detect
# (it has no opinion on whether the new render is CORRECT, only that it is
# what this exact binary, on lavapipe, right now, produces).

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RX_PRESET="${1:-linux-native}"

if [[ "${RX_PRESET}" != "linux-native" ]]; then
  echo "regen_references: unsupported preset '${RX_PRESET}' -- only linux-native can run lavapipe (see this" >&2
  echo "  script's own header comment for why windows-cross-zig is not supported)" >&2
  exit 1
fi

LAVAPIPE_ICD="/usr/share/vulkan/icd.d/lvp_icd.json"
if [[ ! -f "${LAVAPIPE_ICD}" ]]; then
  echo "regen_references: '${LAVAPIPE_ICD}' not found -- install mesa-vulkan-drivers first (the same package" >&2
  echo "  CI's own lavapipe test step depends on)" >&2
  exit 1
fi

SCENE_PATH="${REPO_ROOT}/assets/fetched/DamagedHelmet/glTF/DamagedHelmet.gltf"
if [[ ! -f "${SCENE_PATH}" ]]; then
  echo "regen_references: '${SCENE_PATH}' not found -- run tools/fetch_assets.sh first" >&2
  exit 1
fi

BINARY="${REPO_ROOT}/build/${RX_PRESET}/samples/08_gltf_viewer/sample_08_gltf_viewer"
if [[ ! -x "${BINARY}" ]]; then
  echo "regen_references: '${BINARY}' does not exist or is not executable -- build the '${RX_PRESET}' preset" >&2
  echo "  first (cmake --build --preset ${RX_PRESET} --target sample_08_gltf_viewer)" >&2
  exit 1
fi

REFERENCES_DIR="${REPO_ROOT}/samples/08_gltf_viewer/references"
OUT_DIR="$(mktemp -d)"
trap 'rm -rf "${OUT_DIR}"' EXIT

echo "regen_references: rendering loading_state.png / loaded_scene.png on lavapipe ..."
VK_ICD_FILENAMES="${LAVAPIPE_ICD}" xvfb-run -a "${BINARY}" --write-references "${OUT_DIR}"

for name in loading_state.png loaded_scene.png; do
  if [[ ! -f "${OUT_DIR}/${name}" ]]; then
    echo "regen_references: expected output '${OUT_DIR}/${name}' was not produced -- see the run's own log above" >&2
    exit 1
  fi
  cp -a "${OUT_DIR}/${name}" "${REFERENCES_DIR}/${name}"
done

echo "regen_references: updated ${REFERENCES_DIR}/loading_state.png and loaded_scene.png."
echo "regen_references: rebuild sample_08_gltf_viewer (its own CMakeLists.txt tracks these two files as real"
echo "  build inputs, so an incremental build picks up the change) and re-run its headless gate before committing:"
echo "    cmake --build --preset ${RX_PRESET} --target sample_08_gltf_viewer"
echo "    VK_ICD_FILENAMES=${LAVAPIPE_ICD} xvfb-run -a ${BINARY} --validate"
echo "regen_references: then LOOK at the two PNGs and git diff before committing -- see this script's own header"
echo "  comment."
