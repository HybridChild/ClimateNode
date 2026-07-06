#!/usr/bin/env bash
# Flash the built firmware to the connected Nucleo-H753ZI over ST-LINK/SWD.
#
# We force the openocd runner: nucleo_h753zi defaults to stm32cubeprogrammer
# (STM32_Programmer_CLI), which isn't installed — the SDK-bundled openocd is.
# Run scripts/build.sh first; this flashes whatever is in firmware/build.
#
# Usage:
#   scripts/flash.sh              # flash firmware/build via openocd
#   scripts/flash.sh <extra args> # anything else is passed through to `west flash`
set -euo pipefail

WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/zephyr-workspace}"
RUNNER="${RUNNER:-openocd}"

# Repo root = parent of this script's dir, resolved regardless of where it's called from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO/firmware/build"

if [[ ! -d "$BUILD_DIR" ]]; then
	echo "No build found at $BUILD_DIR — run scripts/build.sh first." >&2
	exit 1
fi

# Bring west + the cross toolchain into scope from the workspace venv.
# shellcheck source=/dev/null
source "$WORKSPACE/.venv/bin/activate"
export ZEPHYR_BASE="$WORKSPACE/zephyr"

exec west flash -r "$RUNNER" --build-dir "$BUILD_DIR" "$@"
