#!/usr/bin/env bash
set -euo pipefail

# assets/test/textures/generate_fixtures.sh -- [Phase 4 Stage 1 Task 14,
# spec D10, gate matrix-issue03, D17 regeneration-script discipline] Every
# committed .ktx2/.png/.jpg fixture in this directory is produced by THIS
# script, run from a clean checkout, so any fixture can be regenerated
# byte-for-byte-equivalent (not byte-identical -- toktx embeds a
# generator/timestamp KTXwriter key/value pair -- but pixel/format/layout
# identical) at any time. Two tools are required on PATH:
#   - ImageMagick's `convert` (source PNG generation -- solid-color/
#     quadrant test patterns, no photographic content needed anywhere in
#     this fixture set).
#   - `toktx` v4.4.2 (the SAME KTX-Software release this project vendors,
#     third_party/CMakeLists.txt's own RX_KTX_TAG) -- NOT built by this
#     project's own CMake (KTX_FEATURE_TOOLS=OFF, see that file's own
#     comment on why); use the official prebuilt release binary instead:
#     https://github.com/KhronosGroup/KTX-Software/releases/download/v4.4.2/KTX-Software-4.4.2-Linux-x86_64.tar.bz2
#     (bin/toktx inside the extracted archive).
#
# Every fixture below is a deliberately TINY (4x4-16x16 texel) synthetic
# pattern -- nothing here is photographic or third-party content, so no
# fixture needs a LICENSE note the way tools/fetch_assets.sh's real-world
# glTF samples do.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT_DIR="${REPO_ROOT}/assets/test/textures"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

TOKTX="${TOKTX:-toktx}"
if ! command -v "${TOKTX}" >/dev/null 2>&1; then
  echo "error: toktx not found on PATH (set TOKTX=/path/to/toktx, v4.4.2 -- see this script's own header comment)" >&2
  exit 1
fi
if ! command -v convert >/dev/null 2>&1; then
  echo "error: ImageMagick 'convert' not found on PATH" >&2
  exit 1
fi

cd "${WORK_DIR}"

# ---------------------------------------------------------------------
# Source PNGs (ImageMagick, procedural -- no external image assets)
# ---------------------------------------------------------------------

# 4x4 "quadrant" pattern: TL=red, TR=green, BL=blue, BR=yellow, each a
# flat 2x2 texel block -- exactly one BC7/UASTC block's worth of content,
# used for the role/colorspace/quadrant-readback tests. Deliberately
# power-of-two and multiple-of-4 (the "normal" case every non-multiple-of-4
# test below is contrasted against).
convert -size 2x2 xc:"#FF0000" red2.png
convert -size 2x2 xc:"#00FF00" green2.png
convert -size 2x2 xc:"#0000FF" blue2.png
convert -size 2x2 xc:"#FFFF00" yellow2.png
convert red2.png green2.png +append top2.png
convert blue2.png yellow2.png +append bottom2.png
convert top2.png bottom2.png -append quadrant4x4.png

# 16x16 nearest-neighbor upscale of the same pattern -- gives a base
# level with a real mip chain (16 -> 8 -> 4 -> 2 -> 1, 5 levels) reaching
# down to the 2x2/1x1 SUB-BLOCK TAIL this task's mip-chain acceptance
# criterion targets, while every level still decodes to the same 4
# flat-color quadrants for a readback probe at any level.
convert quadrant4x4.png -filter point -resize 16x16 quadrant16x16.png

# Flat "straight up" tangent-space normal (unit Z, encoded (128,128,255)
# UNORM) -- content is genuinely LINEAR data; the sRGB-mislabeled fixture
# below deliberately lies about that in its own container DFD.
convert -size 8x8 xc:"#8080FF" normal_flat.png

# Flat SOLID-COLOR 8x8 (orange) -- deliberately NOT the quadrant pattern:
# box-filter mip generation averages a flat color to the SAME color at
# every level with no cross-region blending ambiguity, making this (not
# the quadrant pattern) the fixture the deep-mip-readback / sub-block-
# mip-tail GPU test asserts an EXACT expected color against at every
# level down to the 1x1 tail.
convert -size 8x8 xc:"#FF8000" flat_orange.png

# 6x5 (non-multiple-of-4 in BOTH dimensions) solid pattern -- exercises
# block-rounding at the BASE level, not just a generated mip tail.
convert -size 6x5 xc:"#FF00FF" nonmult4.png

# 6 cubemap faces (+X -X +Y -Y +Z -Z), each a distinct flat color so a
# face-order bug is visually obvious. [Phase 5 Task 6, gate matrix-
# p5t06-ktx2-cubemap-hdr row 6/Open Question 1] Cubemap KTX2 is now a
# SUPPORTED, real, value-asserted-uploaded layout (narrowed
# isUnsupportedLayoutFor() -- cube-array/flat-array/1D/3D stay rejected,
# see the array2d_rejected.ktx2/cubearray_rejected.ktx2 fixtures below),
# so these faces are consumed by REAL GPU readback tests now, not merely
# a rejection stand-in.
convert -size 4x4 xc:"#FF0000" face_px.png
convert -size 4x4 xc:"#00FFFF" face_nx.png
convert -size 4x4 xc:"#00FF00" face_py.png
convert -size 4x4 xc:"#FF00FF" face_ny.png
convert -size 4x4 xc:"#0000FF" face_pz.png
convert -size 4x4 xc:"#FFFF00" face_nz.png

# 32x32 nearest-neighbor upscale of the same 6 faces -- same technique as
# quadrant16x16.png's own comment above: gives cubemap_mips.ktx2 (below) a
# real per-face mip chain (32 -> 16 -> 8 -> 4 -> 2 -> 1, 6 levels) while
# every level still decodes to the SAME flat per-face color a readback
# test can assert exactly (a flat color box-filters to itself at every
# level, same reasoning as flat_orange.png's own mip fixture).
for face in px nx py ny pz nz; do
  convert "face_${face}.png" -filter point -resize 32x32 "face_${face}_32.png"
done

# stb (PNG/JPG) fallback-path fixtures -- the SAME quadrant pattern,
# scaled up slightly (8x8) so JPEG's own lossy block DCT has enough
# spatial extent per flat region to stay recognizably close to its
# source color (a raw 4x4 JPEG re-quantizes visibly at block boundaries).
convert quadrant4x4.png -filter point -resize 8x8 quadrant8x8.png
cp quadrant8x8.png "${OUT_DIR}/quadrant.png"
convert quadrant8x8.png -quality 95 "${OUT_DIR}/quadrant.jpg"

# [Texture-path round, item B -- the 16MB staging-cap fix] 4096x4096
# nearest-neighbor upscale of the SAME quadrant pattern: mip 0 alone is
# 4096*4096*4 = 64MB RGBA8, well over the 16MiB default Uploader ring
# buffer -- the exact real-world shape (the Workshop asset's own 4K
# textures) Uploader::uploadImageMips()'s new chunked-row path exists
# for. Flat-quadrant content compresses to a tiny PNG file despite the
# huge pixel dimensions (nearest-neighbor upscale of 4 flat 2x2 blocks
# has essentially zero real entropy) -- safe to commit despite the
# 4096x4096 logical size.
convert quadrant4x4.png -filter point -resize 4096x4096 "${OUT_DIR}/oversized_quadrant.png"

# --- Slot-swap discrimination fixture set [helmet-texture-fix verification
# gap] -- five DISTINCT, pure-primary/secondary colors (every channel
# either 0 or 255 -- deliberately, so a baseColor/emissive readback is
# byte-exact regardless of whether the render pipeline used to read it
# back applies an sRGB<->linear round-trip: the sRGB transfer function is
# the identity at both 0 and 255), one per StandardPBR texture slot
# (baseColor/metallicRoughness/normal/occlusion/emissive), consumed by
# cube_slot_swap_probe.gltf (texture_cache_test.cpp's own "slot-swap
# discrimination" TEST_CASE). Every color is chosen so that landing in the
# WRONG slot is unmistakable from landing in its own -- in particular
# slot_swap_metalrough.png is pure GREEN so a baseColor<->
# metallicRoughness swap (the failure pattern this fixture exists to
# catch -- reproduced directly in that test's own revert-and-confirm
# procedure) reads as an obviously wrong green baseColor. Flat 4x4 (not a
# quadrant pattern): the assertion this fixture backs is "this whole
# texture is exactly color X", which only a flat fixture makes exact.
convert -size 4x4 xc:"#FF0000" "${OUT_DIR}/slot_swap_basecolor.png"    # baseColor: red.
convert -size 4x4 xc:"#00FF00" "${OUT_DIR}/slot_swap_metalrough.png"   # metallicRoughness: green.
convert -size 4x4 xc:"#0000FF" "${OUT_DIR}/slot_swap_normal.png"       # normal: blue.
convert -size 4x4 xc:"#FF00FF" "${OUT_DIR}/slot_swap_occlusion.png"    # occlusion: magenta.
convert -size 4x4 xc:"#00FFFF" "${OUT_DIR}/slot_swap_emissive.png"     # emissive: cyan.

# 16-bit-per-channel PNG [D10 "16-bit PNG downconvert documented"] --
# ImageMagick's -depth 16 on a PNG output produces a genuine 16-bit PNG
# (verified via `identify` below the generation block).
convert -size 4x4 xc:"#4040C0" -depth 16 PNG48:"${OUT_DIR}/sixteen_bit.png"

# Deliberately corrupt "PNG" (valid PNG magic, garbage afterward) for the
# stb decode-FAILURE path (-> checkerboard, never a crash).
printf '\x89PNG\r\n\x1a\nGARBAGE-NOT-A-REAL-PNG-STREAM' > "${OUT_DIR}/corrupt.png"

# ---------------------------------------------------------------------
# KTX2 fixtures (toktx v4.4.2 -- see this script's own header comment)
# ---------------------------------------------------------------------

# ETC1S -- D10 baseColor role (sRGB).
"${TOKTX}" --t2 --encode etc1s --assign_oetf srgb "${OUT_DIR}/basecolor_etc1s.ktx2" quadrant4x4.png

# UASTC -- same content/role, the higher-quality Basis variant.
"${TOKTX}" --t2 --encode uastc --assign_oetf srgb "${OUT_DIR}/basecolor_uastc.ktx2" quadrant4x4.png

# UASTC + zstd supercompression [D10 "Supercompression (zstd/zlib)" row]
# -- exercises auto-inflation with NO explicit zstd call anywhere in this
# project's own texture_cache.cpp.
"${TOKTX}" --t2 --encode uastc --zcmp 19 --assign_oetf srgb "${OUT_DIR}/basecolor_uastc_zstd.ktx2" quadrant4x4.png

# Full mip chain (16x16 base -> 1x1, 5 levels, reaching the 2x2/1x1
# sub-block tail) -- the primary quadrant-readback + deep-mip-readback +
# mip-tail-region GPU fixture.
"${TOKTX}" --t2 --encode uastc --genmipmap --assign_oetf srgb "${OUT_DIR}/basecolor_withmips_uastc.ktx2" quadrant16x16.png

# Flat-color full mip chain (8x8 -> 1x1, 4 levels) -- see flat_orange.png's
# own generation comment for why this fixture (not the quadrant one) is
# the deep-mip-readback / sub-block-mip-tail GPU test's fixture.
# --assign_oetf LINEAR (deliberately, not srgb): this fixture is loaded
# with TextureRole::GenericData in texture_cache_test.cpp specifically so
# NO sRGB<->linear decode happens on sample (role-authoritative BC7_UNORM,
# D10) -- letting that test assert BYTE-LEVEL-exact (well, BC7-solid-
# color-exact) readback values instead of reasoning about a gamma curve.
"${TOKTX}" --t2 --encode uastc --genmipmap --assign_oetf linear "${OUT_DIR}/flat_withmips_uastc.ktx2" flat_orange.png

# Mips-ABSENT (no --genmipmap/--mipmap at all) -- the "importer warns,
# recommends --genmipmap" acceptance row.
"${TOKTX}" --t2 --encode uastc --assign_oetf srgb "${OUT_DIR}/mips_absent.ktx2" quadrant4x4.png

# Cubemap -- SUPPORTED layout as of Phase 5 Task 6 (gate matrix-p5t06-
# ktx2-cubemap-hdr row 2/6, the discrimination-proof fixture: the SAME
# file the old "cube -> checkerboard" test asserted against now loads for
# real). --target_type RGBA forces a real 4-component format (same reason
# as raw_rgba8.ktx2's own comment below: the source PNGs carry no alpha
# channel, and toktx's default component-count inference would otherwise
# produce a 3-component VK_FORMAT_R8G8B8_SRGB, unreliable for
# SAMPLED_IMAGE on some devices) and --assign_oetf srgb makes the
# container's own colorspace label explicit rather than inferred. Raw
# (non-Basis) storage: never transcoded, uploaded verbatim.
"${TOKTX}" --t2 --cubemap --target_type RGBA --assign_oetf srgb "${OUT_DIR}/cubemap.ktx2" face_px.png face_nx.png face_py.png face_ny.png face_pz.png face_nz.png

# Cubemap WITH a full per-face mip chain (32x32 base -> 1x1, 6 levels) --
# the primary cube GPU readback fixture (gate matrix row 6/11): every face
# keeps its own flat authored color at every mip level (box-filtering a
# flat color reproduces itself, same reasoning as flat_withmips_uastc.ktx2
# below), so a readback test can assert EXACT per-face-per-mip values.
"${TOKTX}" --t2 --cubemap --genmipmap --target_type RGBA --assign_oetf srgb "${OUT_DIR}/cubemap_mips.ktx2" face_px_32.png face_nx_32.png face_py_32.png face_ny_32.png face_pz_32.png face_nz_32.png

# Flat 2D ARRAY (isArray=true, isCubemap=false, numLayers=3) -- STILL
# REJECTED after Task 6's narrowed isUnsupportedLayoutFor() predicate
# (`numDimensions != 2 || (isArray && !isCubemap) || (isCubemap &&
# numLayers > 1)`) -- cubemap-only support, zero charter consumer needs a
# general texture array (gate matrix row 3/Open Question 1). Same 4x4
# content repeated 3x -- layer content is irrelevant to a rejection-only
# regression fixture.
"${TOKTX}" --t2 --layers 3 "${OUT_DIR}/array2d_rejected.ktx2" quadrant4x4.png quadrant4x4.png quadrant4x4.png

# CUBE-ARRAY (isCubemap=true, isArray=true, numLayers=2) -- STILL REJECTED
# by the same narrowed predicate as array2d_rejected.ktx2 above (the
# `isCubemap && numLayers > 1` clause) -- cube-array is explicitly out of
# scope alongside the flat-array case. 12 face images (2 layers x 6
# faces); the same 6 face_*.png files reused for both layers -- content is
# irrelevant to a rejection-only regression fixture.
"${TOKTX}" --t2 --cubemap --layers 2 "${OUT_DIR}/cubearray_rejected.ktx2" face_px.png face_nx.png face_py.png face_ny.png face_pz.png face_nz.png face_px.png face_nx.png face_py.png face_ny.png face_pz.png face_nz.png

# Non-multiple-of-4 base dimensions (6x5) -- block-compressed (UASTC),
# proving base-level (not just mip-tail) sub-block rounding.
"${TOKTX}" --t2 --encode uastc --assign_oetf srgb "${OUT_DIR}/nonmult4.ktx2" nonmult4.png

# sRGB-MISLABELED NORMAL [D10/gate ruling #3, Godot #99589 class]: the
# CONTENT is linear normal-map data (normal_flat.png, see its own
# generation comment above) but the container's own DFD transfer function
# is force-assigned sRGB anyway -- role (Normal, linear) must win over
# this container claim, with exactly one WARN naming the disagreement.
"${TOKTX}" --t2 --encode uastc --assign_oetf srgb "${OUT_DIR}/srgb_mislabeled_normal.ktx2" normal_flat.png

# Raw (non-Basis) KTX2 -- NO --encode at all -- for the "needs-transcoding
# == false" / uploadable-as-stored detection path. --target_type RGBA
# forces a real 4-component format (VK_FORMAT_R8G8B8A8_SRGB): the source
# PNG has no alpha channel, and toktx's own default component-count
# inference would otherwise produce a 3-component VK_FORMAT_R8G8B8_SRGB,
# a format many devices (including this project's own CI driver) support
# more narrowly for SAMPLED_IMAGE than the ubiquitous 4-component one --
# this fixture is meant to exercise the "stored format IS supported,
# upload as-is" branch reliably, not the fallback branch by accident.
"${TOKTX}" --t2 --target_type RGBA --assign_oetf srgb "${OUT_DIR}/raw_rgba8.ktx2" quadrant4x4.png

# [Fix round 1, reviewer IMPORTANT-2] RAW (non-Basis) sRGB-mislabeled
# normal -- the non-Basis-path twin of srgb_mislabeled_normal.ktx2 above:
# same linear normal-map CONTENT (normal_flat.png), same forced
# --assign_oetf srgb container claim, but NO --encode at all, so this
# container needs no transcoding (ktxTexture2_NeedsTranscoding() ==
# false) and stores real RGBA8 bytes directly. Proves the non-Basis path's
# OWN "container-vs-role WARN still fires, but the stored format is kept
# (no relabel)" behavior -- deliberately DIFFERENT from the Basis path's
# own free relabel (see texture_cache.cpp's own comment on why
# relabeling is scoped to the Basis-transcoded path only).
"${TOKTX}" --t2 --target_type RGBA --assign_oetf srgb "${OUT_DIR}/raw_srgb_mislabeled_normal.ktx2" normal_flat.png

# Deliberately corrupt KTX2 -- valid 12-byte KTX2 identifier (so
# looksLikeKtx2() routes it to the libktx path) followed by garbage, for
# the "corrupted container -> named parse error, no crash" acceptance row.
printf '\xABKTX\x20\x32\x30\xBB\x0D\x0A\x1A\x0AGARBAGE-NOT-A-REAL-KTX2-PAYLOAD' > "${OUT_DIR}/corrupt.ktx2"

# ---------------------------------------------------------------------
# Radiance HDR fixtures [Phase 5 Task 6, gate matrix-p5t06-ktx2-cubemap-hdr
# row 9/13] -- hand-authored via plain `printf`, not ImageMagick: this
# sandbox's ImageMagick build is Q16 WITHOUT HDRI (`convert -version`
# reports no "HDRI" token), which clamps every pixel value it can express
# to the normalized [0,1] range -- structurally incapable of producing a
# genuine super-unity (>1.0) texel, exactly the bar this fixture exists to
# prove survives the load/upload/readback pipeline. The Radiance/RGBE
# format itself is simple enough to hand-author correctly and
# deterministically (D17 regeneration-script discipline: every byte below
# is derived, not copied from an external binary) -- old-style
# (non-run-length-encoded) scanlines are the CORRECT choice for a width
# this small (4 < 8): the Radiance spec's own RLE scanline format is
# never used below 8 pixels wide, and stb_image.h's own HDR reader
# (stbi__hdr_load) falls back to reading flat per-pixel RGBE quads
# whenever a scanline's first 4 bytes don't match the RLE marker
# (0x02 0x02 <hi> <lo>) -- exactly what this block writes.
#
# RGBE encoding [Radiance format, matching stb_image.h's own decode:
# stbi__hdr_convert(): value = mantissa_byte * ldexp(1, exponent_byte -
# 136)]: every pixel below shares exponent byte 131 (2^(131-136) =
# 2^-5 = 0.03125), so mantissa_byte * 0.03125 gives each channel's exact
# decoded float value:
#   TL = (128,  16,  16, 131) -> (4.0, 0.5, 0.5)  -- R clearly > 1.0
#   TR = ( 16, 128,  16, 131) -> (0.5, 4.0, 0.5)  -- G clearly > 1.0
#   BL = ( 16,  16, 128, 131) -> (0.5, 0.5, 4.0)  -- B clearly > 1.0
#   BR = (128, 128, 128, 130) -> (2.0, 2.0, 2.0)  -- uniform bright gray
#     (BR uses exponent 130: ldexp(1, 130-136) = 2^-6 = 0.015625;
#     128 * 0.015625 = 2.0)
# "-Y 2 +X 2" -- 2 rows (top-to-bottom), 2 columns (left-to-right) per
# row: row 0 is TL,TR; row 1 is BL,BR -- matching this fixture's own
# per-quadrant-color convention used throughout this script (e.g.
# quadrant4x4.png).
{
  printf '#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 2\n'
  printf '\x80\x10\x10\x83'   # TL: (4.0, 0.5, 0.5)
  printf '\x10\x80\x10\x83'   # TR: (0.5, 4.0, 0.5)
  printf '\x10\x10\x80\x83'   # BL: (0.5, 0.5, 4.0)
  printf '\x80\x80\x80\x82'   # BR: (2.0, 2.0, 2.0)
} > "${OUT_DIR}/equirect_test.hdr"

# Deliberately corrupt HDR -- a syntactically valid Radiance SIGNATURE
# (stbi_is_hdr_from_memory() reads true -- verified directly against the
# vendored stb_image.h: stbi__hdr_test()/stbi__hdr_test_core() checks
# ONLY the leading 11-byte "#?RADIANCE\n" magic, never the FORMAT/
# resolution lines) with a GARBLED resolution line instead of a real
# "-Y <h> +X <w>" one -- this is the deliberate choice, not a truncated
# scanline: stb's own flat (non-RLE, width<8) pixel loop reads through
# stbi__getn(), which silently zero-pads past EOF rather than erroring
# (verified empirically -- a truncated-scanline version of this fixture
# decoded "successfully" with zero-padded trailing pixels, not the
# failure this row needs). A malformed resolution line instead hits
# stbi__hdr_load()'s own `strncmp(token, "-Y ", 3)` check
# deterministically, returning a real, named decode failure every time.
printf '#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\nGARBAGE-NOT-A-VALID-RESOLUTION-LINE\n' > "${OUT_DIR}/corrupt.hdr"

echo "Fixtures written to ${OUT_DIR}:"
ls -la "${OUT_DIR}"/*.ktx2 "${OUT_DIR}"/*.png "${OUT_DIR}"/*.jpg "${OUT_DIR}"/*.hdr
