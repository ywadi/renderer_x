#!/usr/bin/env bash
set -euo pipefail

# tools/fetch_assets.sh -- fetches Khronos glTF-Sample-Assets test content
# [Phase 4 Stage 1 Task 13, spec D16; extended round-11/issue-31]:
# DamagedHelmet is MANDATORY (small, the standard PBR-correctness asset,
# gate-tested by damaged_helmet_test.cpp) and fetched by default; BoomBox
# (glTF-Draco variant) is also MANDATORY (small, the real-world
# KHR_draco_mesh_compression compatibility asset, gate-tested by
# draco_compression_test.cpp -- see that file's own header comment for
# why this specific model was chosen over an ad hoc Sketchfab download);
# Sponza is OPTIONAL (large, local/present-mode "wow" content only -- CI
# NEVER downloads it) and fetched only when --sponza is passed.
# [Phase 5 Stage 1 Task 11, ticket #47] Six glTF PBR conformance models
# (MetalRoughSpheres, MetalRoughSpheresNoTextures, EmissiveStrengthTest,
# CompareEmissiveStrength, TextureTransformTest, AlphaBlendModeTest) are
# MANDATORY (small, CC-BY-4.0/CC0-1.0, gated by tests/conformance/ against
# committed Khronos Sample Viewer references); EnvironmentTest is OPTIONAL
# (--environment-test only, proprietary Adobe Stock license -- see that
# section's own comment below).
# Checksummed, cached with a versioned marker file exactly like
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
BOOMBOX_DRACO_DIR="${DEST_ROOT}/BoomBox/glTF-Draco"

FETCH_SPONZA=0
FETCH_ENVIRONMENT_TEST=0
for arg in "$@"; do
  case "$arg" in
    --sponza) FETCH_SPONZA=1 ;;
    --environment-test) FETCH_ENVIRONMENT_TEST=1 ;;
    *)
      echo "usage: $0 [--sponza] [--environment-test]" >&2
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

# ---------------------------------------------------------------------
# BoomBox (glTF-Draco variant) -- MANDATORY, like DamagedHelmet above
# [issue #31 acceptance criterion: "a real-world Draco asset (Sketchfab-
# class) imports headless without errors"]. CC0-1.0 (Creative Commons
# Zero -- public-domain-equivalent; LICENSE.md verified directly against
# the pinned commit: "All files directly associated with the model...
# Creative Commons Zero v1.0 Universal"), by Microsoft (glTF-Sample-
# Assets' own attribution). Not literally sourced from Sketchfab.com --
# the ticket's own "(Sketchfab-class)" parenthetical reads as a
# description of TYPICAL characteristics (a small real-world-authored
# asset a converted Sketchfab download would also have: baked PBR
# textures, KHR_draco_mesh_compression, a single mesh/material), not a
# literal sourcing requirement -- this is glTF-Sample-Assets' own
# canonical Draco-compressed reference model, maintained by the Khronos
# Group specifically to exercise KHR_draco_mesh_compression decoders
# (verified directly: extensionsRequired=["KHR_draco_mesh_compression"]),
# which makes it a stronger real-world compatibility fixture than an
# arbitrary Sketchfab download would be. Per-file sha256 pinned exactly
# like DamagedHelmet above (computed directly against
# github.com/KhronosGroup/glTF-Sample-Assets main branch, this task,
# 2026-08-20).
# ---------------------------------------------------------------------
BOOMBOX_DRACO_BASE_URL="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/BoomBox/glTF-Draco"
BOOMBOX_DRACO_VERSION="2026-08-20"
BOOMBOX_DRACO_MARKER="${DEST_ROOT}/.rx-fetched-boombox-draco-${BOOMBOX_DRACO_VERSION}"

BOOMBOX_DRACO_FILES='
BoomBox.gltf aa0c0eaeeedd1ab0d30f89b19acd0b7ca5caf2c04f07e2041680e5ea6aca7b42
BoomBox.bin a619055105a510746d14a599074373ab38fb3c0a04486465f662ae79114f536e
BoomBox_baseColor.png 099816a7afc5f6690494313ac8039806fd6d5b84179126a808b2678aaab3563a
BoomBox_emissive.png e9970da7010591b73070151fe5039a158413499e38300d14106e367472c03b5b
BoomBox_normal.png c9a7904e7f25246ac47f86c337cfd4ec8e103fff83e07d3af472e5c620ec6f27
BoomBox_occlusionRoughnessMetallic.png 496704a4836ff364dc4441f41651edb25178498926c859933e33df42d6361412
'

fetch_boombox_draco() {
  if [[ -f "${BOOMBOX_DRACO_MARKER}" ]]; then
    echo "[fetch_assets] BoomBox (glTF-Draco) already fetched and verified (marker ${BOOMBOX_DRACO_MARKER}) -- skipping"
    return
  fi

  echo "[fetch_assets] BoomBox (glTF-Draco) -- CC0-1.0 (Creative Commons Zero / public domain)."
  echo "[fetch_assets]   Source: https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/BoomBox"

  mkdir -p "${BOOMBOX_DRACO_DIR}"
  echo "${BOOMBOX_DRACO_FILES}" | while read -r name checksum; do
    [[ -z "${name}" ]] && continue
    local_path="${BOOMBOX_DRACO_DIR}/${name}"
    echo "[fetch_assets] downloading ${name}"
    curl -fsSL -o "${local_path}" "${BOOMBOX_DRACO_BASE_URL}/${name}"
    echo "${checksum}  ${local_path}" | sha256sum -c -
  done

  mkdir -p "${DEST_ROOT}"
  echo "${BOOMBOX_DRACO_VERSION}" >"${BOOMBOX_DRACO_MARKER}"
  echo "[fetch_assets] BoomBox (glTF-Draco) fetched and verified -> ${BOOMBOX_DRACO_DIR}"
}

# ---------------------------------------------------------------------
# glTF PBR conformance models [Phase 5 Stage 1 Task 11, ticket #47, gate
# ruling T11] -- ≥6 conformance models gated against Khronos Sample
# Viewer references (tests/conformance/). Six MANDATORY, CC-BY-4.0/CC0-1.0
# models (safe to commit references for -- matrix-p5t11-conformance-
# harness.md's own per-model LICENSE.md verification, this task):
# MetalRoughSpheres/MetalRoughSpheresNoTextures (the metallic/roughness
# grid, ± textures -- the charter's own headline conformance model, plus
# an isolation case for a texture-sampling-vs-core-BRDF bug split),
# EmissiveStrengthTest/CompareEmissiveStrength (KHR_materials_
# emissive_strength -- consume-ready per gate ruling RC3), TextureTransformTest/
# TextureTransformMultiTest (KHR_texture_transform, already consumed per
# Phase 4 gate ruling C4). All seven files' sha256 pinned below, computed
# directly against glTF-Sample-Assets main branch this task (2026-08-22),
# same "hardcode + verify" pattern as DamagedHelmet/BoomBox above.
#
# EnvironmentTest is the SEVENTH, --environment-test-only model: its own
# model content is under a proprietary Adobe Stock commercial license
# (LICENSE.md's own "LicenseRef-Adobe-Stock" pointer, NOT Creative
# Commons -- verified this task, matrix-p5t11-conformance-harness.md),
# so it follows Sponza's own established disposition exactly: fetched on
# demand ONLY (never by CI, never committed to this public repository),
# for LOCAL conformance-harness testing only -- neither its glTF source
# NOR any reference render generated from it may ever be committed.
# ---------------------------------------------------------------------
CONFORMANCE_VERSION="2026-08-22b"
CONFORMANCE_MARKER="${DEST_ROOT}/.rx-fetched-conformance-${CONFORMANCE_VERSION}"

fetch_conformance_model() {
  local model_name="$1" base_url="$2" dest_subdir="$3" files="$4"
  local dest_dir="${DEST_ROOT}/${model_name}/${dest_subdir}"
  mkdir -p "${dest_dir}"
  echo "${files}" | while read -r name checksum; do
    [[ -z "${name}" ]] && continue
    local_path="${dest_dir}/${name}"
    echo "[fetch_assets] downloading ${model_name}/${name}"
    curl -fsSL -o "${local_path}" "${base_url}/${name}"
    echo "${checksum}  ${local_path}" | sha256sum -c -
  done
}

fetch_conformance_models() {
  if [[ -f "${CONFORMANCE_MARKER}" ]]; then
    echo "[fetch_assets] conformance models already fetched and verified (marker ${CONFORMANCE_MARKER}) -- skipping"
    return
  fi

  echo "[fetch_assets] glTF conformance models -- CC-BY-4.0 / CC0-1.0 (see this script's own header comment)."
  echo "[fetch_assets]   Source: https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models"

  fetch_conformance_model "MetalRoughSpheres" \
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/MetalRoughSpheres/glTF" \
    "glTF" '
MetalRoughSpheres.gltf a24fd6fd24b2baef80a5d56b465a55635f7ec79039414e25c341d1402cc28048
MetalRoughSpheres0.bin f35a1caf37837b2987e24859d812a23e40ca37e1f741782e2a754961250e8c36
Spheres_BaseColor.png 7327b8de2ce91d0ca4fdefcf28fdc58cb555e885a8e5f3b3cfab867c3f349efc
Spheres_MetalRough.png 7b352787106705789df0e025a8ea2209074569cd87029369bc66080dfca78e5a
'

  # CC0-1.0 (model content); the model's own metadata.json is separately
  # CC-BY-4.0 -- metadata.json itself is not fetched (not consumed by this
  # gate), so only the CC0-1.0 disposition applies to what this script
  # downloads.
  fetch_conformance_model "MetalRoughSpheresNoTextures" \
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/MetalRoughSpheresNoTextures/glTF" \
    "glTF" '
MetalRoughSpheresNoTextures.bin 1ce2d45aa5e99ec1a9d0018aa6cbd9cb3156f4fa9452c668525ddc63e9b31a66
MetalRoughSpheresNoTextures.gltf fc5d59f4dcd674ef3de98bbe8d3f8cad569394aa07481c803c4449f8834bcada
'

  fetch_conformance_model "EmissiveStrengthTest" \
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/EmissiveStrengthTest/glTF" \
    "glTF" '
EmissiveStrengthTest.bin 54ba759d776228b52a256bb7b739581ad3081ac77677ee905b4920e869ceccf5
EmissiveStrengthTest.gltf 84264f4300d2618aa2be39e90448f1716df8f1946d0d172476773bc9a4a99f65
PlainGrid.png c67b0411f4f8d2c940c7e4583ce6c7607cc6c45779c88c3c16f2d2892fb9c411
'

  # CC0-1.0, PLUS a Khronos trademark/logo reference in its own LICENSE.md
  # (../../LICENSES/LicenseRef-LegalMark-Khronos.txt) -- Compare_Emissive-
  # Strength_img0.jpg is an embedded comparison-chart texture that may
  # itself carry the Khronos logo; recorded here rather than silently
  # dropped, matching this script's own DamagedHelmet/Sponza precedent of
  # naming a non-obvious license nuance instead of rounding it off to a
  # plain "CC0".
  fetch_conformance_model "CompareEmissiveStrength" \
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/CompareEmissiveStrength/glTF" \
    "glTF" '
CompareEmissiveStrength.bin 44c23126516708d693283143a15adf32cfc7399eaf2766f477d3c27aaa152622
CompareEmissiveStrength.gltf c9d8f7b9cbaf6b54961b86b23c34a1dee23b99f22220507340e330db61287970
Compare_Emissive-Strength_img0.jpg 7900f41ce34ee5a0922b4b03983875154e13ec165b17df05e4559d843a184d87
'

  fetch_conformance_model "TextureTransformTest" \
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/TextureTransformTest/glTF" \
    "glTF" '
Arrow.png 5df5b251fed0ac306cf859a30b59a4953d152483a4bed2b84e5bba1c667a1e64
Correct.png 3a14b12635ebbcee3ea427fbdb4d20da73e4740ec7a51683d700a2d6b4b7861a
Error.png 8d1032ef535a5c379bf78f9f565d3180ee3fc2d1cfef6892dee6e567728b619c
NotSupported.png 1fbdeca7e5c105d677a39578e4bab82b89c8aa9f406ee681ede05635ea0f8c74
TextureTransformTest.bin d0cdd23f2fa0996a0db99d4932fb9912a51a34e27c8bf832d072dc856ee9a7af
TextureTransformTest.gltf c22c8c6c96c0ea4bcbb9b47ea245a093c5ef59acc5fd425effa4c00da4cdf164
UV.png ac37ff52fe06a4c8c35ad4e1e7e8da039dfa8afc9b629d40a44ca8b8cfe9d03d
'

  # [Stage-1-scope correction, this task] NOT TextureTransformMultiTest --
  # verified directly against its own glTF JSON
  # (extensionsUsed includes KHR_materials_clearcoat, 9 of its 29 materials
  # actually use it) that model ALSO exercises KHR_materials_clearcoat,
  # which RendererX does not implement until Stage 3 (Task 21) -- gating
  # the WHOLE image against it now would fail on clearcoat rows for a
  # reason that has nothing to do with this ticket's own texture-transform
  # scope, and "failures are findings to fix, never tolerance widenings"
  # (this ticket's own binding text) forbids papering over that with a
  # wider tolerance. AlphaBlendModeTest is the clean substitute: zero
  # extensionsUsed (verified), pure core-PBR alphaMode content (Opaque/
  # Mask/Blend), already consume-ready since Phase 4's D28.
  fetch_conformance_model "AlphaBlendModeTest" \
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/AlphaBlendModeTest/glTF" \
    "glTF" '
AlphaBlendLabels.png 19c705f7e207384ca1ab01e0665601c051ca33c5848c03ce4fffad625b416b53
AlphaBlendModeTest.bin bb89ab8d9ac0cfbc168e5d552e3ec2fe1285fd01bacb65c863d838b3db025c85
AlphaBlendModeTest.gltf 49e06672900df95593040d35bbc7a2ee5921ae8d46edc44b64ccf2df65e64849
MatBed_baseColor.jpg 71571cce0d86c7c5ed4ac73d852ec34e72e7234a2f905f2758e118519c950bcf
MatBed_normal.jpg d8891a9c08e375507ac8f1fb260577bda5e90506e79db92ebbfb5caa9216be49
MatBed_occlusionRoughnessMetallic.jpg e1472454ad6be574c3a418985f06a8ba36877108a94f61590f01e118804379b4
'

  mkdir -p "${DEST_ROOT}"
  echo "${CONFORMANCE_VERSION}" >"${CONFORMANCE_MARKER}"
  echo "[fetch_assets] conformance models fetched and verified -> ${DEST_ROOT}/{MetalRoughSpheres,MetalRoughSpheresNoTextures,EmissiveStrengthTest,CompareEmissiveStrength,TextureTransformTest,AlphaBlendModeTest}"
}

# EnvironmentTest -- OPTIONAL, --environment-test only; CI never passes
# this flag. Adobe Stock proprietary license (see this section's own
# header comment above) -- neither this glTF source nor any reference
# render generated from it is ever committed to this public repository.
ENVIRONMENT_TEST_VERSION="2026-08-22"
ENVIRONMENT_TEST_MARKER="${DEST_ROOT}/.rx-fetched-environmenttest-${ENVIRONMENT_TEST_VERSION}"

fetch_environment_test() {
  if [[ -f "${ENVIRONMENT_TEST_MARKER}" ]]; then
    echo "[fetch_assets] EnvironmentTest already fetched and verified (marker ${ENVIRONMENT_TEST_MARKER}) -- skipping"
    return
  fi

  echo "[fetch_assets] EnvironmentTest -- Adobe Stock License (proprietary; NOT Creative Commons -- see"
  echo "[fetch_assets]   ../../LICENSES/LicenseRef-Adobe-Stock.txt and this script's own header comment)."
  echo "[fetch_assets]   Source: https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/EnvironmentTest"
  echo "[fetch_assets]   LOCAL/CI-on-demand testing only -- never committed (glTF source or generated references)."

  fetch_conformance_model "EnvironmentTest" \
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/EnvironmentTest/glTF" \
    "glTF" '
EnvironmentTest.gltf c38d83e48ce1714e0dc9683ffbd8928da6bf2f11a7adeea50be5702bf055bfdb
EnvironmentTest_binary.bin bc6f6118c3052384623b59c74488c5786975a2723eae3ef7136dffd7b6c42cc4
'
  fetch_conformance_model "EnvironmentTest" \
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/EnvironmentTest/glTF/EnvironmentTest_images" \
    "glTF/EnvironmentTest_images" '
roughness_metallic_0.png 0e07400f4b9a0cb0cd625a65018fac96516a1e98be4a6c85fe618076a0c620df
roughness_metallic_1.png 2709360619580ae8aace9fc7afb94a80ba6b987297948829f4093026e80426d3
'

  mkdir -p "${DEST_ROOT}"
  echo "${ENVIRONMENT_TEST_VERSION}" >"${ENVIRONMENT_TEST_MARKER}"
  echo "[fetch_assets] EnvironmentTest fetched and verified -> ${DEST_ROOT}/EnvironmentTest/glTF"
}

fetch_damaged_helmet
fetch_boombox_draco
fetch_conformance_models
if [[ "${FETCH_SPONZA}" -eq 1 ]]; then
  fetch_sponza
else
  echo "[fetch_assets] Sponza skipped (pass --sponza to fetch; CI never does)"
fi
if [[ "${FETCH_ENVIRONMENT_TEST}" -eq 1 ]]; then
  fetch_environment_test
else
  echo "[fetch_assets] EnvironmentTest skipped (pass --environment-test to fetch; CI never does)"
fi
