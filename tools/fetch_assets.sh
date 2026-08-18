#!/usr/bin/env bash
set -euo pipefail

# tools/fetch_assets.sh -- fetches Khronos glTF-Sample-Assets test content
# [Phase 4 Stage 1 Task 13, spec D16]: DamagedHelmet is MANDATORY (small,
# the standard PBR-correctness asset, gate-tested by
# import_gltf_damagedhelmet_test.cpp) and fetched by default; Sponza is
# OPTIONAL (large, local/present-mode "wow" content only -- CI NEVER
# downloads it) and fetched only when --sponza is passed. Checksummed,
# cached with a versioned marker file exactly like
# tools/fetch_slang.cmake's own pattern (a version/checksum-set bump
# self-invalidates a stale cache; re-running this script when everything
# is already fetched and verified is a fast no-op).
#
# LICENSE CORRECTION (this task, verified directly against
# glTF-Sample-Assets' own per-model LICENSE.md files -- flagged
# prominently, since the binding matrix/plan text this task's brief
# points to states both assets are plain "CC BY 4.0", which is WRONG for
# both, not just imprecise):
#   - DamagedHelmet: LICENSE.md lists it under BOTH CC-BY-4.0 AND
#     CC-BY-NC-4.0 (the earlier draft this model incorporates,
#     theblueturtle_'s work, was itself only ever released
#     Non-Commercial) -- the combination means the WHOLE asset carries
#     the NC restriction forward, not plain CC BY.
#   - Sponza: LICENSE.md lists it under the CRYENGINE Limited License
#     Agreement (a Crytek EULA-style license referenced at
#     https://www.cryengine.com/ce-terms), NOT any Creative Commons
#     license at all.
# Neither asset is committed to this repository -- both are fetched here,
# on demand, for local/CI TESTING of this importer against real content,
# never redistributed as part of this project's own shipped output. This
# script prints the accurate attribution below rather than the
# unqualified "CC BY 4.0" the planning documents assumed.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_ROOT="${REPO_ROOT}/assets/fetched"
DAMAGED_HELMET_DIR="${DEST_ROOT}/DamagedHelmet/glTF"
SPONZA_DIR="${DEST_ROOT}/Sponza/glTF"

FETCH_SPONZA=0
for arg in "$@"; do
  case "$arg" in
    --sponza) FETCH_SPONZA=1 ;;
    *)
      echo "usage: $0 [--sponza]" >&2
      exit 1
      ;;
  esac
done

# ---------------------------------------------------------------------
# DamagedHelmet (mandatory) -- pinned sha256 checksums, computed directly
# against github.com/KhronosGroup/glTF-Sample-Assets main branch this
# task (2026-08-18), the same "hardcode + verify" pattern this project's
# own CI already uses for the zig toolchain download
# (.github/workflows/ci.yml's RX_ZIG_SHA256).
# ---------------------------------------------------------------------
DAMAGED_HELMET_BASE_URL="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/DamagedHelmet/glTF"
DAMAGED_HELMET_VERSION="2026-08-18"
DAMAGED_HELMET_MARKER="${DEST_ROOT}/.rx-fetched-damagedhelmet-${DAMAGED_HELMET_VERSION}"

# "<file> <sha256>" pairs.
DAMAGED_HELMET_FILES='
DamagedHelmet.gltf efe99dfac198094a30c71dc02a4d3421f0eef6bf335aeb695daa4d62134cd93f
DamagedHelmet.bin 61b33ead6aa3de23f39f02aaf0965bc3988575716a768f45b15d5ea3fb783fdf
Default_albedo.jpg 2dc95e87aeb0cd7c8a65ef0eb8b23212388da0534ca379811427a9a8780511f5
Default_AO.jpg 7024051b65ed2ead8d24ebc9bd5ea61d4c21639a57b15dab29870668bcc6b90f
Default_emissive.jpg dd0057989f22f93a4ab796d06ebf85a7c12fbfae1d59a86ae8b0c54770dc1159
Default_metalRoughness.jpg 0f05e7ffbeaa974f7d2c83436b04109969966d64ab188cbc3a19a265a0a69ae0
Default_normal.jpg f253ba09a90a86ffd8a807dc45d7953111f8b3421e15d7a21d1e915b01dd33c1
'

fetch_damaged_helmet() {
  if [[ -f "${DAMAGED_HELMET_MARKER}" ]]; then
    echo "[fetch_assets] DamagedHelmet already fetched and verified (marker ${DAMAGED_HELMET_MARKER}) -- skipping"
    return
  fi

  echo "[fetch_assets] DamagedHelmet -- CC-BY-4.0 AND CC-BY-NC-4.0 (see this script's own header comment)."
  echo "[fetch_assets]   (c) 2018 ctxwing (rebuild and glTF conversion, CC-BY-4.0)"
  echo "[fetch_assets]   (c) 2016 theblueturtle_ (earlier model, CC-BY-NC-4.0)"
  echo "[fetch_assets]   Source: https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/DamagedHelmet"

  mkdir -p "${DAMAGED_HELMET_DIR}"
  echo "${DAMAGED_HELMET_FILES}" | while read -r name checksum; do
    [[ -z "${name}" ]] && continue
    local_path="${DAMAGED_HELMET_DIR}/${name}"
    echo "[fetch_assets] downloading ${name}"
    curl -fsSL -o "${local_path}" "${DAMAGED_HELMET_BASE_URL}/${name}"
    echo "${checksum}  ${local_path}" | sha256sum -c -
  done

  mkdir -p "${DEST_ROOT}"
  echo "${DAMAGED_HELMET_VERSION}" >"${DAMAGED_HELMET_MARKER}"
  echo "[fetch_assets] DamagedHelmet fetched and verified -> ${DAMAGED_HELMET_DIR}"
}

# ---------------------------------------------------------------------
# Sponza (optional, --sponza only; CI never passes this flag). 71 files
# (~53 MB) -- impractical to hand-pin one checksum per file the way
# DamagedHelmet's 7 are above, so integrity is verified against the git
# BLOB sha1 the GitHub Contents API itself reports for each file (the
# same value `git hash-object` would compute locally) rather than a
# checksum hardcoded in this script's text. This is a real, computed
# verification against a value fetched from a SEPARATE API call than the
# download itself, not merely trusting HTTPS -- it just is not a
# manually-pinned constant, which the file count makes impractical to
# maintain by hand.
# ---------------------------------------------------------------------
SPONZA_API_URL="https://api.github.com/repos/KhronosGroup/glTF-Sample-Assets/contents/Models/Sponza/glTF"
SPONZA_VERSION="2026-08-18"
SPONZA_MARKER="${DEST_ROOT}/.rx-fetched-sponza-${SPONZA_VERSION}"

fetch_sponza() {
  if [[ -f "${SPONZA_MARKER}" ]]; then
    echo "[fetch_assets] Sponza already fetched and verified (marker ${SPONZA_MARKER}) -- skipping"
    return
  fi

  echo "[fetch_assets] Sponza -- CRYENGINE Limited License Agreement (NOT Creative Commons; see"
  echo "[fetch_assets]   https://www.cryengine.com/ce-terms and this script's own header comment)."
  echo "[fetch_assets]   Source: https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Sponza"

  mkdir -p "${SPONZA_DIR}"
  python3 "${REPO_ROOT}/tools/fetch_sponza_helper.py" "${SPONZA_API_URL}" "${SPONZA_DIR}"

  mkdir -p "${DEST_ROOT}"
  echo "${SPONZA_VERSION}" >"${SPONZA_MARKER}"
  echo "[fetch_assets] Sponza fetched and verified -> ${SPONZA_DIR}"
}

fetch_damaged_helmet
if [[ "${FETCH_SPONZA}" -eq 1 ]]; then
  fetch_sponza
else
  echo "[fetch_assets] Sponza skipped (pass --sponza to fetch; CI never does)"
fi
