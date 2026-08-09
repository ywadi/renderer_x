#!/usr/bin/env bash
# Measures a real incremental rebuild and fails if it exceeds a time budget.
#
# Usage: tools/check_build_budget.sh <preset> [budget_seconds=60]
#
# This must measure an actual incremental build, not a no-op: touching a
# leaf .cpp that other targets depend on (rx_core's log.cpp, linked into
# rx_core -> rx_platform -> rx_rhi_vk -> samples/tests) forces a real
# recompile of that translation unit plus a relink of everything downstream
# of it, immediately before starting the timer. A preset that has never been
# configured/built (no binary dir yet) is configured+built once, untimed,
# first -- so the touch below always has a real object file to invalidate.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <preset> [budget_seconds=60]" >&2
  exit 1
fi

RX_PRESET="$1"
RX_BUDGET_SECONDS="${2:-60}"

RX_BINARY_DIR="$REPO_ROOT/build/$RX_PRESET"
RX_LEAF_SOURCE="$REPO_ROOT/src/rx_core/src/log.cpp"

if [[ ! -f "$RX_LEAF_SOURCE" ]]; then
  echo "check_build_budget: leaf source '$RX_LEAF_SOURCE' does not exist." >&2
  exit 1
fi

if [[ ! -d "$RX_BINARY_DIR" ]]; then
  echo "check_build_budget: no existing build directory at '$RX_BINARY_DIR' - configuring and building once (untimed) first"
  cmake --preset "$RX_PRESET"
  cmake --build --preset "$RX_PRESET"
fi

# Force recompile+relink of the dependent chain: touch the leaf .cpp
# directly (not a header), so only the one translation unit that already
# depends on it needs recompiling, plus a relink of every target that links
# rx_core transitively -- a realistic single-file-edit incremental build,
# not a full rebuild and not a no-op.
touch "$RX_LEAF_SOURCE"

echo "check_build_budget: timing incremental build of preset '$RX_PRESET' (budget ${RX_BUDGET_SECONDS}s) ..."

RX_START_SECONDS=$(date +%s)
cmake --build --preset "$RX_PRESET"
RX_END_SECONDS=$(date +%s)

RX_ELAPSED_SECONDS=$((RX_END_SECONDS - RX_START_SECONDS))

echo "check_build_budget: incremental build of '$RX_PRESET' took ${RX_ELAPSED_SECONDS}s"

if (( RX_ELAPSED_SECONDS > RX_BUDGET_SECONDS )); then
  echo "check_build_budget: FAIL - ${RX_ELAPSED_SECONDS}s exceeds the ${RX_BUDGET_SECONDS}s budget for preset '$RX_PRESET'" >&2
  exit 1
fi

echo "check_build_budget: OK - ${RX_ELAPSED_SECONDS}s is within the ${RX_BUDGET_SECONDS}s budget for preset '$RX_PRESET'"
