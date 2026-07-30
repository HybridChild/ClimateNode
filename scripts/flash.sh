#!/usr/bin/env bash
# Flash one of this repo's built apps to its board over ST-LINK/SWD.
#
# We force the openocd runner: both boards default to stm32cubeprogrammer
# (STM32_Programmer_CLI), which isn't installed, while the SDK-bundled openocd
# is. Both nucleo_h753zi and nucleo_f072rb include the common openocd runner
# file, so the same override works for either -- the board-specific openocd
# arguments come from the board's own board.cmake.
#
# Run scripts/build.sh first; this flashes whatever is in that app's build dir.
#
# With BOTH Nucleos plugged in, openocd will pick whichever ST-LINK it finds
# first, which is a coin flip. Pass the probe's serial to pin it down:
#
#   STLINK_SERIAL=0670FF... scripts/flash.sh -a sensor-node
#
# List the attached probes' serials with:
#
#   system_profiler SPUSBDataType | grep -A4 -i st-link      # macOS
#
# (The openocd runner spells this --serial; the generic --dev-id flag that other
# runners take is not one of its capabilities.)
#
# Usage:
#   scripts/flash.sh                     # flash firmware/build via openocd
#   scripts/flash.sh -a sensor-node      # flash the other app
#   APP=sensor-node scripts/flash.sh     # same thing via the environment
#   scripts/flash.sh <extra args>        # anything else is passed through to `west flash`
set -euo pipefail

WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/zephyr-workspace}"
RUNNER="${RUNNER:-openocd}"
APP_NAME="${APP:-firmware}"

# Repo root = parent of this script's dir, resolved regardless of where it's called from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

while [[ $# -gt 0 ]]; do
	case "$1" in
	-a)
		if [[ $# -lt 2 ]]; then
			echo "-a needs an app name (firmware or sensor-node)." >&2
			exit 2
		fi
		APP_NAME="$2"
		shift 2
		;;
	*)
		break
		;;
	esac
done

BUILD_DIR="$REPO/$APP_NAME/build"

if [[ ! -d "$BUILD_DIR" ]]; then
	echo "No build found at $BUILD_DIR — run scripts/build.sh -a $APP_NAME first." >&2
	exit 1
fi

# Bring west + the cross toolchain into scope from the workspace venv.
# shellcheck source=/dev/null
source "$WORKSPACE/.venv/bin/activate"
export ZEPHYR_BASE="$WORKSPACE/zephyr"

# Two spelled-out exec lines rather than building up an argument array: macOS
# ships bash 3.2, where expanding an empty array ("${arr[@]}") under `set -u` is
# an "unbound variable" error rather than nothing. ("$@" is special-cased and
# safe when empty; a normal array is not.) Only pass --serial when asked --
# openocd's default is the empty string, meaning "any probe", which is the right
# behaviour with a single board attached.
if [[ -n "${STLINK_SERIAL:-}" ]]; then
	exec west flash -r "$RUNNER" --build-dir "$BUILD_DIR" --serial "$STLINK_SERIAL" "$@"
fi

exec west flash -r "$RUNNER" --build-dir "$BUILD_DIR" "$@"
