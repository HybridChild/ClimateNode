#!/usr/bin/env bash
# Work out which ST-LINK probe -- and which serial port -- belongs to which app.
#
# With one Nucleo attached, none of this is needed: there is one probe and one
# /dev/cu.usbmodem*, and console.sh and flash.sh just use them. With both
# attached, every tool that has to pick one is guessing, and the two failure
# modes are unpleasant in different ways: console.sh opens the wrong board and
# looks dead (the peer node has no shell), while flash.sh writes the wrong image
# to the wrong part and succeeds.
#
# So this script answers the question once, from the hardware, and console.sh
# and flash.sh call it rather than each solving it badly.
#
# It keys on the ST-LINK **serial number**, which is burned into the probe and
# permanent. The obvious alternative -- the /dev/cu.usbmodemNNNNNN name -- is
# derived from the USB topology, so it changes when you move the board to a
# different port or hub, which makes it exactly the wrong thing to write down.
# scripts/probes.conf maps app -> serial; this script maps serial -> port.
#
# Usage:
#   scripts/probe.sh                        # table of every attached probe
#   scripts/probe.sh -a peer-node         # that app's serial and port
#   scripts/probe.sh -a peer-node --port  # just the port, for $(...)
#   scripts/probe.sh -a gateway --serial   # just the serial, for $(...)
#
# Exit status: 0 if the requested app resolved, 1 if it did not (with the reason
# on stderr). macOS only -- it reads the IORegistry, which is where the USB
# serial number and the tty device that hangs off it can both be seen.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

APP_NAME=""
FIELD="both"

while [[ $# -gt 0 ]]; do
	case "$1" in
	-a)
		if [[ $# -lt 2 ]]; then
			echo "-a needs an app name." >&2
			exit 2
		fi
		APP_NAME="$2"
		shift 2
		;;
	--port)
		FIELD="port"
		shift
		;;
	--serial)
		FIELD="serial"
		shift
		;;
	*)
		echo "Unknown argument: $1" >&2
		exit 2
		;;
	esac
done

# The work is in Python because it has to parse a plist and walk a tree, which
# is not a thing to do in bash 3.2. Same arrangement as ide-index.sh.
exec python3 - "$SCRIPT_DIR/probes.conf" "$APP_NAME" "$FIELD" <<'PY'
import plistlib
import subprocess
import sys

conf_path, want_app, field = sys.argv[1], sys.argv[2], sys.argv[3]


def attached_probes():
    """Every ST-LINK on the bus, as (product, serial, tty or None).

    ioreg -a gives the IORegistry as a plist. The USB serial number lives on the
    USB device node; the /dev/cu.* name lives on an IOSerialBSDClient several
    levels below it, under the CDC-ACM interface. Neither knows about the other,
    so the mapping is "walk down from each ST-LINK and find the tty".
    """
    raw = subprocess.run(
        ["ioreg", "-a", "-r", "-c", "IOUSBHostDevice", "-l", "-w0"],
        capture_output=True,
    ).stdout
    if not raw:
        return []

    def walk(node):
        yield node
        for child in node.get("IORegistryEntryChildren") or []:
            yield from walk(child)

    found = {}
    for root in plistlib.loads(raw):
        for node in walk(root):
            product = str(node.get("USB Product Name", ""))
            serial = node.get("USB Serial Number")
            # ST-LINK/V2-1 reports "STM32 STLink"; V3 reports "STLINK-V3" or
            # "STLINK_V3". Match loosely rather than listing every spelling ST
            # has shipped -- a future probe we do not recognise should still be
            # listed, so the reader can see it and add it to probes.conf.
            if not serial or "stlink" not in product.lower().replace("-", "").replace("_", ""):
                continue
            ttys = [n["IOCalloutDevice"] for n in walk(node) if n.get("IOCalloutDevice")]
            # The same probe appears at several levels of the tree (device,
            # configuration, interface), so key on the serial and keep the entry
            # that actually found a tty.
            if serial not in found or (ttys and not found[serial][1]):
                found[serial] = (product, ttys[0] if ttys else None)

    return sorted((serial, product, tty) for serial, (product, tty) in found.items())


def read_conf(path):
    """app -> serial. Comments start with #, including at end of line."""
    mapping = {}
    try:
        text = open(path).read()
    except OSError:
        return mapping
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) >= 2:
            mapping[parts[0]] = parts[1]
    return mapping


probes = attached_probes()
conf = read_conf(conf_path)
by_serial = {serial: (product, tty) for serial, product, tty in probes}
app_of = {serial: app for app, serial in conf.items()}

if not want_app:
    if not probes:
        print("No ST-LINK found — is a Nucleo plugged in and powered?", file=sys.stderr)
        raise SystemExit(1)
    print(f"{'APP':<14}{'PROBE':<16}{'SERIAL':<26}PORT")
    for serial, product, tty in probes:
        app = app_of.get(serial, "?  (add to probes.conf)")
        print(f"{app:<14}{product:<16}{serial:<26}{tty or '(no serial port)'}")
    # Flag configured boards that are not here, since "it did not appear" is the
    # most likely reason a later command fails.
    for app, serial in sorted(conf.items()):
        if serial not in by_serial:
            print(f"{app:<14}{'—':<16}{serial:<26}not attached")
    raise SystemExit(0)

serial = conf.get(want_app)
if serial is None:
    known = ", ".join(sorted(conf)) or "(none)"
    print(
        f"probes.conf has no entry for '{want_app}'. Known apps: {known}.\n"
        f"Run scripts/probe.sh with the board attached to see its serial.",
        file=sys.stderr,
    )
    raise SystemExit(1)

if serial not in by_serial:
    print(
        f"The probe for '{want_app}' (serial {serial}) is not attached.\n"
        f"Run scripts/probe.sh to see what is.",
        file=sys.stderr,
    )
    raise SystemExit(1)

product, tty = by_serial[serial]

if field == "serial":
    print(serial)
elif field == "port":
    if tty is None:
        print(
            f"The probe for '{want_app}' is attached but exposes no serial port.\n"
            f"On a Nucleo that usually means the ST-LINK's VCP is disabled or the "
            f"board is powered from the wrong connector.",
            file=sys.stderr,
        )
        raise SystemExit(1)
    print(tty)
else:
    print(f"{want_app}  {product}  {serial}  {tty or '(no serial port)'}")
PY
