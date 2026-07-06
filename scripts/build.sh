#!/usr/bin/env bash
# Build the firmware app for the Nucleo-H753ZI against the shared global Zephyr
# workspace (~/zephyr-workspace). This repo is a *freestanding* Zephyr app: west
# runs from inside the workspace (so it can enumerate modules) with -s pointed at
# our firmware/ dir and -d putting the build output back here.
#
# Usage:
#   scripts/build.sh              # incremental build (-p auto)
#   scripts/build.sh -p           # pristine/clean build (-p always; after DT/Kconfig edits)
#   scripts/build.sh <extra args> # anything else is passed through to `west build`
set -euo pipefail

WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/zephyr-workspace}"
BOARD="${BOARD:-nucleo_h753zi}"

# Repo root = parent of this script's dir, resolved regardless of where it's called from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
APP="$REPO/firmware"
BUILD_DIR="$APP/build"

# Default to an incremental build; -p (our shorthand) forces a pristine rebuild.
PRISTINE="auto"
if [[ "${1:-}" == "-p" ]]; then
	PRISTINE="always"
	shift
fi

# Bring west + the cross toolchain into scope from the workspace venv.
# shellcheck source=/dev/null
source "$WORKSPACE/.venv/bin/activate"
export ZEPHYR_BASE="$WORKSPACE/zephyr"

cd "$WORKSPACE"
exec west build -p "$PRISTINE" -b "$BOARD" -s "$APP" -d "$BUILD_DIR" "$@"
