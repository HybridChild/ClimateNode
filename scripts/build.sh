#!/usr/bin/env bash
# Build one of this repo's Zephyr apps against the shared global Zephyr
# workspace (~/zephyr-workspace). This repo is a *freestanding* Zephyr app: west
# runs from inside the workspace (so it can enumerate modules) with -s pointed at
# the app dir and -d putting the build output back here.
#
# There are two apps, and each has its own board:
#
#   firmware/      the H753ZI gateway  (nucleo_h753zi)
#   sensor-node/   the F072RB peer node (nucleo_f072rb)
#
# The app defaults to firmware/, so every invocation that worked before this
# script grew a second app still works unchanged.
#
# Usage:
#   scripts/build.sh                    # incremental build of firmware/ (-p auto)
#   scripts/build.sh -p                 # pristine/clean build (after DT/Kconfig edits)
#   scripts/build.sh -a sensor-node     # build the other app
#   APP=sensor-node scripts/build.sh    # same thing via the environment
#   BOARD=... scripts/build.sh          # override the app's default board
#   scripts/build.sh <extra args>       # anything else is passed through to `west build`
set -euo pipefail

WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/zephyr-workspace}"
APP_NAME="${APP:-firmware}"

# Repo root = parent of this script's dir, resolved regardless of where it's called from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

# Default to an incremental build; -p (our shorthand) forces a pristine rebuild.
# Both flags are accepted in any order, and everything after them goes to west.
PRISTINE="auto"
while [[ $# -gt 0 ]]; do
	case "$1" in
	-p)
		PRISTINE="always"
		shift
		;;
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

APP="$REPO/$APP_NAME"
BUILD_DIR="$APP/build"

if [[ ! -f "$APP/CMakeLists.txt" ]]; then
	echo "No Zephyr app at $APP (expected a CMakeLists.txt there)." >&2
	exit 2
fi

# Each app targets exactly one board, so the board follows from the app rather
# than being something to remember. BOARD= still wins, for a one-off.
if [[ -z "${BOARD:-}" ]]; then
	case "$APP_NAME" in
	firmware) BOARD="nucleo_h753zi" ;;
	sensor-node) BOARD="nucleo_f072rb" ;;
	*)
		echo "No default board known for '$APP_NAME' — set BOARD=." >&2
		exit 2
		;;
	esac
fi

# Bring west + the cross toolchain into scope from the workspace venv.
# shellcheck source=/dev/null
source "$WORKSPACE/.venv/bin/activate"
export ZEPHYR_BASE="$WORKSPACE/zephyr"

cd "$WORKSPACE"
exec west build -p "$PRISTINE" -b "$BOARD" -s "$APP" -d "$BUILD_DIR" "$@"
