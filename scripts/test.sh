#!/usr/bin/env bash
# Run the host-side unit tests -- no board, no broker, no sensor.
#
# Twister builds each suite under tests/ and runs it under emulation, then
# reports pass/fail per test case.
#
#   scripts/test.sh                   # run every suite on qemu_cortex_m3
#   scripts/test.sh -T tests/protocol # one suite
#   scripts/test.sh <extra args>      # anything else is passed through to twister
#
# Why qemu_cortex_m3: native_sim (which compiles the test as a host binary and
# is much faster) only supports Linux hosts, and this bench is macOS. QEMU ships
# with the Zephyr SDK, so nothing extra is installed. See notes/testing-guide.md.
set -euo pipefail

WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/zephyr-workspace}"
PLATFORM="${TEST_PLATFORM:-qemu_cortex_m3}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

# Bring west + the cross toolchain into scope from the workspace venv.
# shellcheck source=/dev/null
source "$WORKSPACE/.venv/bin/activate"
export ZEPHYR_BASE="$WORKSPACE/zephyr"

# Default to every suite in the repo; an explicit -T overrides it.
#
# twister runs from inside the workspace, so a relative -T would resolve against
# the wrong directory. Rewrite one to an absolute path under the repo, so
# `scripts/test.sh -T tests/commands` means what it looks like it means.
ARGS=()
if [[ $# -eq 0 || " $* " != *" -T "* ]]; then
	ARGS+=(-T "$REPO/tests")
fi
while [[ $# -gt 0 ]]; do
	if [[ "$1" == "-T" && -n "${2:-}" && "$2" != /* ]]; then
		ARGS+=(-T "$REPO/$2")
		shift 2
		continue
	fi
	ARGS+=("$1")
	shift
done

cd "$WORKSPACE"
exec west twister \
	-p "$PLATFORM" \
	--outdir "$REPO/twister-out" \
	--clobber-output \
	--inline-logs \
	"${ARGS[@]}"

# --clobber-output: twister's default is to *rename* the previous output to
# twister-out.1, .2, ... rather than replace it, so a morning's runs leave a
# stack of ~30 MB build trees behind. Each suite is a complete Zephyr image, so
# they are large and worth nothing once the run is reported. Drop the flag if you
# ever need to compare a run against the one before it.
