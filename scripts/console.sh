#!/usr/bin/env bash
# Open a Nucleo's serial console (ST-LINK VCP) with screen.
#
# The VCP enumerates as /dev/cu.usbmodem* on macOS; the trailing digits come from
# the ST-LINK serial number and change if the board or USB port changes, so we
# glob for it rather than hardcode. Zephyr's console + shell run at 115200 8N1.
#
# With both Nucleos attached there are two matching ports -- one per ST-LINK --
# and auto-detection deliberately refuses to guess. It prints both, and you pick:
# the H753ZI gateway is the one running the shell with `net` and `can` commands,
# the F072RB peer node logs but has no shell (it has no RAM to spare for one).
#
# Quitting screen matters here: Ctrl-A then K (then y) terminates the session and
# frees the port. Ctrl-A then D only *detaches* — the port stays busy and the next
# run fails with "Resource busy"; reattach with `screen -r` (or `screen -X -S <id>
# quit` to kill it).
#
# Usage:
#   scripts/console.sh                 # auto-detect the port, 115200
#   scripts/console.sh /dev/cu.usbXYZ  # explicit port
#   BAUD=9600 scripts/console.sh       # override the baud rate
set -euo pipefail

BAUD="${BAUD:-115200}"
PORT="${1:-${PORT:-}}"

if [[ -z "$PORT" ]]; then
	PORTS=(/dev/cu.usbmodem*)
	# An unmatched glob stays literal, so test the first entry for existence.
	if [[ ! -e "${PORTS[0]}" ]]; then
		echo "No /dev/cu.usbmodem* found — is the Nucleo plugged in and powered?" >&2
		exit 1
	fi
	if (( ${#PORTS[@]} > 1 )); then
		echo "Multiple serial ports found — pass one explicitly:" >&2
		printf '  %s\n' "${PORTS[@]}" >&2
		exit 1
	fi
	PORT="${PORTS[0]}"
fi

if [[ ! -e "$PORT" ]]; then
	echo "Serial port not found: $PORT" >&2
	exit 1
fi

echo "Connecting to $PORT @ $BAUD — quit with Ctrl-A then K"
exec screen "$PORT" "$BAUD"
