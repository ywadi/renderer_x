#!/usr/bin/env bash
# Verifies the prebuilt slangc fetched by tools/fetch_slang.cmake is the
# pinned version. Run a cmake configure first (e.g. `cmake --preset
# linux-native`) so the fetch has actually happened.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Must stay in sync with RX_SLANG_VERSION / RX_SLANG_PLATFORM in
# tools/fetch_slang.cmake.
RX_SLANG_VERSION="2026.14.1"
RX_SLANG_PLATFORM="linux-x86_64"
RX_SLANGC="$REPO_ROOT/third_party/slang-prebuilt/$RX_SLANG_PLATFORM/bin/slangc"

if [[ ! -x "$RX_SLANGC" ]]; then
  echo "fetch_slang_test: slangc not found (or not executable) at '$RX_SLANGC'." >&2
  echo "fetch_slang_test: run a cmake configure first, e.g.: cmake --preset linux-native" >&2
  exit 1
fi

# slangc -v writes the version to stderr, not stdout (verified directly
# against the pinned 2026.14.1 binary) -- capture both streams.
actual_version="$("$RX_SLANGC" -v 2>&1)"

if [[ "$actual_version" != "$RX_SLANG_VERSION" ]]; then
  echo "fetch_slang_test: FAIL - expected slangc -v to report '$RX_SLANG_VERSION', got '$actual_version'" >&2
  exit 1
fi

echo "fetch_slang_test: OK - slangc -v reports $actual_version ($RX_SLANGC)"
