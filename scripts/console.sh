#!/usr/bin/env bash
# Open a Nucleo's serial console (ST-LINK VCP) with screen.
#
# The VCP enumerates as /dev/cu.usbmodem* on macOS; the trailing digits come from
# the USB topology and change if the board moves to a different port or hub, so
# we never hardcode one. There are three ways to say which board you mean, in
# descending order of how much you have to know:
#
#   -a <app>      by app name, resolved through scripts/probe.sh and probes.conf
#   <port>        an explicit /dev/cu.* path
#   (nothing)     auto-detect, which works only when exactly one board is attached
#
# Zephyr's console runs at 115200 8N1 on both boards. What is on the far end
# differs, and it is worth knowing before concluding a board is dead:
#
#   firmware (H753ZI)     a shell. Prompts `uart:~$`, takes input, has the `net`
#                         and `can` command sets.
#   sensor-node (F072RB)  OUTPUT ONLY. Log lines come out; nothing you type goes
#                         anywhere, because the default image has no shell -- a
#                         stock one needs 95% of this part's 16 KB of RAM. It
#                         logs at boot and on state changes, and is otherwise
#                         silent. A quiet console there is the node working.
#
# For an interactive shell on the peer, build the bench variant:
#
#   ./scripts/build.sh -a sensor-node --debug -p
#
# which merges sensor-node/debug.conf and gets you a trimmed shell with the `can`
# commands at 83% RAM. No history and no tab completion -- type it correctly.
#
# Quitting screen matters here: Ctrl-A then K (then y) terminates the session and
# frees the port. Ctrl-A then D only *detaches* — the port stays busy and the next
# run fails with "Resource busy"; reattach with `screen -r` (or `screen -X -S <id>
# quit` to kill it).
#
# Usage:
#   scripts/console.sh -a sensor-node  # the peer node, whichever port it is on
#   scripts/console.sh -a firmware     # the gateway
#   scripts/console.sh                 # auto-detect; refuses if two are attached
#   scripts/console.sh /dev/cu.usbXYZ  # explicit port
#   BAUD=9600 scripts/console.sh       # override the baud rate
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BAUD="${BAUD:-115200}"
APP_NAME="${APP:-}"

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

PORT="${1:-${PORT:-}}"

# -a resolves a port, but an explicit path still wins, so a one-off
# `scripts/console.sh /dev/cu.usbmodemXXX` does exactly what it says.
if [[ -z "$PORT" && -n "$APP_NAME" ]]; then
	PORT="$("$SCRIPT_DIR/probe.sh" -a "$APP_NAME" --port)"
fi

if [[ -z "$PORT" ]]; then
	PORTS=(/dev/cu.usbmodem*)
	# An unmatched glob stays literal, so test the first entry for existence.
	if [[ ! -e "${PORTS[0]}" ]]; then
		echo "No /dev/cu.usbmodem* found — is the Nucleo plugged in and powered?" >&2
		exit 1
	fi
	if (( ${#PORTS[@]} > 1 )); then
		# Two boards attached and nothing said which. Rather than print bare
		# device paths, which mean nothing on their own, show the probe
		# table -- it names the app beside each port.
		echo "More than one board attached — say which with -a, or pass a port:" >&2
		echo >&2
		"$SCRIPT_DIR/probe.sh" >&2 || printf '  %s\n' "${PORTS[@]}" >&2
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
