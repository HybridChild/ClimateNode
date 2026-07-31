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
# With BOTH Nucleos plugged in, openocd picks whichever ST-LINK it finds first,
# which is a coin flip -- and losing it writes the wrong image to the wrong part
# and reports success. So when more than one probe is attached, this script
# resolves the app's own probe through scripts/probe.sh (which reads
# scripts/probes.conf) and passes its serial explicitly. If it cannot, it stops
# rather than guessing: a refused flash costs a second, a silently wrong one
# costs however long you spend debugging the board you did not flash.
#
# With a single probe attached, none of that happens -- no serial is passed and
# openocd takes the only one there, which is also what makes this work on a
# bench that has never heard of probes.conf.
#
# STLINK_SERIAL= overrides all of it:
#
#   STLINK_SERIAL=0670FF... scripts/flash.sh -a sensor-node
#
# List the attached probes and their serials with `scripts/probe.sh`. (The
# openocd runner spells this --serial; the generic --dev-id flag that other
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

# Decide which probe to use, unless the caller already has. Counting the ports
# is a cheap proxy for counting the boards and needs no tooling; only when there
# is more than one do we go and ask which is which.
if [[ -z "${STLINK_SERIAL:-}" ]]; then
	PORTS=(/dev/cu.usbmodem*)
	# An unmatched glob stays literal, so test the first entry for existence.
	if [[ -e "${PORTS[0]}" ]] && (( ${#PORTS[@]} > 1 )); then
		if ! STLINK_SERIAL="$("$SCRIPT_DIR/probe.sh" -a "$APP_NAME" --serial)"; then
			echo >&2
			echo "More than one board is attached and '$APP_NAME' could not be" >&2
			echo "resolved to a probe, so this would flash a board at random." >&2
			echo "Add it to scripts/probes.conf, or pass STLINK_SERIAL=." >&2
			exit 1
		fi
		echo "Flashing $APP_NAME on ST-LINK $STLINK_SERIAL"
	fi
fi

# Bring west + the cross toolchain into scope from the workspace venv.
# shellcheck source=/dev/null
source "$WORKSPACE/.venv/bin/activate"
export ZEPHYR_BASE="$WORKSPACE/zephyr"

# Two spelled-out exec lines rather than building up an argument array: macOS
# ships bash 3.2, where expanding an empty array ("${arr[@]}") under `set -u` is
# an "unbound variable" error rather than nothing. ("$@" is special-cased and
# safe when empty; a normal array is not.) Only pass --serial when we have one --
# openocd's default is the empty string, meaning "any probe", which is the right
# behaviour with a single board attached.
if [[ -n "${STLINK_SERIAL:-}" ]]; then
	exec west flash -r "$RUNNER" --build-dir "$BUILD_DIR" --serial "$STLINK_SERIAL" "$@"
fi

exec west flash -r "$RUNNER" --build-dir "$BUILD_DIR" "$@"
