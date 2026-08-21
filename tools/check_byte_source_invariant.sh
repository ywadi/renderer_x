#!/usr/bin/env bash
# tools/check_byte_source_invariant.sh -- [Phase 5 Task 6, ticket #42,
# gate rulings-2026-08-20.md RC7e, gate matrix-p5t06-ktx2-cubemap-hdr row
# 14] Makes the "byte-source abstraction is grep-enforced" wording
# (texture_decode.cpp's own comment, this ticket's acceptance text, and
# matrix-issue03's Phase 4 gate) actually TRUE for the first time: this
# script IS the grep. Before this task, no CI workflow and no committed
# script performed it -- the phrase described a manual-code-review
# criterion, not an automated one (verified directly this task).
#
# Invariant: rx_asset's KTX2/stb decode layer (texture_decode.cpp) and its
# GPU-facing caller (texture_cache.cpp) must never touch the filesystem
# directly -- every byte comes in through the ByteSource abstraction
# (byte_source.h/.cpp, a DIFFERENT translation unit), so decode/transcode
# logic stays pure, device-free, and worker-thread-safe with no hidden I/O
# dependency (docs/threading.md's "Worker-allowed" list; texture_decode.h's
# own top-of-file comment).
#
# Two checks, over both files:
#   1. Zero occurrences of a direct file-I/O call (fopen(/std::ifstream/
#      std::ofstream) -- no exceptions; neither file needs to call these
#      at all.
#   2. Zero occurrences of `std::filesystem::<member>` where <member> is
#      anything OTHER than `path` -- the one documented exception is
#      TextureCache::load(const std::filesystem::path&)'s own path-typed
#      parameter/local (texture_cache.cpp): it constructs a
#      FilesystemByteSource and delegates immediately, never itself
#      touching the filesystem. Any OTHER std::filesystem member
#      (exists/directory_iterator/file_size/read_symlink/...) would be
#      real filesystem I/O creeping into this invariant-protected pair of
#      files.
#
# Usage: tools/check_byte_source_invariant.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FILES=(
  "$REPO_ROOT/src/rx_asset/texture_decode.cpp"
  "$REPO_ROOT/src/rx_asset/texture_cache.cpp"
)

for f in "${FILES[@]}"; do
  if [[ ! -f "$f" ]]; then
    echo "check_byte_source_invariant: file does not exist: $f" >&2
    exit 1
  fi
done

fail=0

for f in "${FILES[@]}"; do
  # Direct file-I/O calls -- real call syntax only (an open-paren after
  # fopen, the qualified std::ifstream/std::ofstream type names), so a
  # prose comment merely NAMING these (e.g. texture_decode.cpp's own
  # "zero std::filesystem/fopen in this whole translation unit" comment,
  # which has no trailing "(") never false-positives here.
  hits="$(grep -nE '\bfopen[[:space:]]*\(|\bstd::ifstream\b|\bstd::ofstream\b' "$f" || true)"
  if [[ -n "$hits" ]]; then
    echo "check_byte_source_invariant: FAIL - direct file-I/O call found in $f:" >&2
    echo "$hits" >&2
    fail=1
  fi
done

for f in "${FILES[@]}"; do
  # Every std::filesystem::<member> usage, filtered down to anything
  # other than the one allowed `path` member.
  hits="$(grep -noE 'std::filesystem::[A-Za-z_]+' "$f" | grep -vE '::path$' || true)"
  if [[ -n "$hits" ]]; then
    echo "check_byte_source_invariant: FAIL - std::filesystem member other than 'path' found in $f:" >&2
    echo "$hits" >&2
    fail=1
  fi
done

if [[ "$fail" -ne 0 ]]; then
  echo "check_byte_source_invariant: FAIL - byte-source abstraction invariant violated (see above)" >&2
  exit 1
fi

echo "check_byte_source_invariant: OK - no direct filesystem I/O found in ${FILES[*]}"
