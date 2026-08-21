#!/usr/bin/env bash
set -euo pipefail

# tools/fetch_gltf_sample_renderer.sh -- Phase 5 Stage 1 Task 11 (#47),
# gate ruling T11: "conformance ground truth via headless-browser
# automation of the Khronos Sample Viewer's GltfView/GltfState API".
#
# WHAT THIS FETCHES: NOT the glTF-Sample-Viewer Vue.js UI application
# (its own package.json has zero CLI/headless/screenshot capability --
# `"test": "echo \"Error: no test specified\" && exit 1"`, verified
# directly this task) -- the SEPARATE `glTF-Sample-Renderer` repository,
# the actual WebGL2 rendering LIBRARY the Viewer's UI wraps
# (`GltfView`/`GltfState`/`ResourceLoader`, `source/gltf-sample-
# renderer.js`'s own public export list). tools/gltf_conformance/harness.html
# drives this library directly, bypassing the Vue UI entirely -- the
# matrix's own recommended approach (matrix-p5t11-conformance-harness.md's
# Open Questions section), since the UI layer has nothing this task needs
# and driving it via DOM automation would be strictly more fragile than
# calling the library's own clean, documented JS API.
#
# PIN: commit 863b981fb755359063e370ff7b6e956bda0716e2 (2026-08-06,
# "Merge pull request #49 from KhronosGroup/fix/tanget-hashing") -- the
# exact commit glTF-Sample-Viewer's own git submodule pointer resolved to
# when this task fetched it (2026-08-21/22), i.e. "whatever the Viewer
# project itself currently treats as its own renderer" at pin time, not
# an arbitrarily chosen commit. Apache-2.0 (github.com/KhronosGroup/
# glTF-Sample-Renderer, verified via `gh api repos/...` this task).
#
# NOT committed to this repository -- a toolchain fetch, mirroring
# tools/fetch_slang.cmake / toolchain/zig's own on-demand pattern
# (.gitignore already excludes toolchain/ entirely). Only
# tools/gltf_conformance/generate_reference.mjs (the driver script) and
# its own committed output (tests/conformance/references/**) are checked
# in; this script's own output (the built ESM renderer bundle) is
# reproducible from this pin at any time and never needs to be.
#
# USAGE: tools/fetch_gltf_sample_renderer.sh
#   Idempotent: a versioned marker file (mirroring tools/fetch_assets.sh's
#   own convention) makes a re-run with an unchanged pin a fast no-op.
#   Requires `git`, `node`/`npm` (any reasonably current LTS -- built and
#   verified against Node 24 this task), and network access.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_DIR="${REPO_ROOT}/toolchain/gltf-sample-renderer"
PIN_COMMIT="863b981fb755359063e370ff7b6e956bda0716e2"
MARKER="${DEST_DIR}/.rx-fetched-${PIN_COMMIT}"

if [[ -f "${MARKER}" ]]; then
  echo "[fetch_gltf_sample_renderer] already fetched+built at ${PIN_COMMIT} (marker ${MARKER}) -- skipping"
  exit 0
fi

echo "[fetch_gltf_sample_renderer] glTF-Sample-Renderer @ ${PIN_COMMIT} -- Apache-2.0, Khronos Group Inc."
echo "[fetch_gltf_sample_renderer]   Source: https://github.com/KhronosGroup/glTF-Sample-Renderer"

rm -rf "${DEST_DIR}"
mkdir -p "${DEST_DIR}"
git init -q "${DEST_DIR}"
git -C "${DEST_DIR}" remote add origin https://github.com/KhronosGroup/glTF-Sample-Renderer.git
git -C "${DEST_DIR}" fetch -q --depth 1 origin "${PIN_COMMIT}"
git -C "${DEST_DIR}" checkout -q FETCH_HEAD

echo "[fetch_gltf_sample_renderer] npm ci"
( cd "${DEST_DIR}" && npm ci --no-audit --no-fund )

echo "[fetch_gltf_sample_renderer] npm run build (rollup -> dist/gltf-viewer.module.js)"
( cd "${DEST_DIR}" && npm run build )

if [[ ! -f "${DEST_DIR}/dist/gltf-viewer.module.js" ]]; then
  echo "[fetch_gltf_sample_renderer] build did not produce dist/gltf-viewer.module.js" >&2
  exit 1
fi

echo "${PIN_COMMIT}" >"${MARKER}"
echo "[fetch_gltf_sample_renderer] built -> ${DEST_DIR}/dist/gltf-viewer.module.js"
