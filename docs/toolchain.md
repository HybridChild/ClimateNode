# Toolchain & build setup

This project builds as a **freestanding Zephyr application** against a **shared global west
workspace** — it does *not* carry its own copy of the Zephyr tree. There are two independent
toolchains: one on the Mac that builds and flashes firmware, one on the Pi that runs the host
harness. They share only `proto/node.proto`.

## Firmware toolchain (Mac)

```
~/zephyr-workspace/        <- shared global west workspace (topdir, not a git repo)
├── .venv/                 python venv holding `west` (v1.5.0)
├── .west/config           manifest = zephyr, base = zephyr
├── zephyr/                Zephyr RTOS source, v4.4.1
├── modules/               HALs (hal_stm32 …) + libs (nanopb …)
├── bootloader/mcuboot     v2.4.0
└── tools/                 edtt, net-tools
~/zephyr-sdk-1.0.1/        Zephyr SDK, ARM-only (arm-zephyr-eabi), CMake-registered
EthernetProtobufZephyr/    this repo — pure source (proto/ firmware/ host/ docs/), builds vs. above
```

The workspace is **shared with `../ImpulseZephyr`** (originally a self-contained T2 workspace; its
Zephyr tree was promoted here so both projects reuse one install). Consequence: **both projects are
pinned to Zephyr v4.4.1.** If this project ever needs a different version, give it its own workspace
(escape hatch — not needed today; v4.4.1 already ships everything below).

### Everything this project needs is in v4.4.1

- **nanopb** — `modules/lib/nanopb` + `zephyr/modules/nanopb/nanopb.cmake` → use **in-tree nanopb**.
- **SCD4x sensor driver** — `zephyr/drivers/sensor/sensirion/scd4x/` + `sensirion,scd40` DT binding
  → use the **upstream in-tree driver** (no community module needed).
- **Board** — `nucleo_h753zi`.

### Build & flash

Use the wrappers; they source the workspace venv, export `ZEPHYR_BASE`, and pass the right
source/build directories:

```sh
./scripts/build.sh        # incremental; -p forces pristine (required after devicetree/Kconfig edits)
./scripts/flash.sh        # forces the openocd runner
./scripts/console.sh      # serial console @115200; quit with Ctrl-A then K
```

What they wrap, and why each detail matters:

```sh
source ~/zephyr-workspace/.venv/bin/activate
export ZEPHYR_BASE=~/zephyr-workspace/zephyr
cd ~/zephyr-workspace                       # west must run from INSIDE the workspace
west build -p auto -b nucleo_h753zi -s <repo>/firmware -d <repo>/firmware/build
west flash -r openocd --build-dir <repo>/firmware/build
```

- **`cd` into the workspace** — west enumerates modules relative to the workspace topdir; run it
  from the repo and nanopb and the HALs are invisible.
- **`-s`/`-d`** keep the app source in this repo and the build output beside it.
- **`-r openocd` is required for flashing.** `nucleo_h753zi` defaults to the `stm32cubeprogrammer`
  runner, which is not installed. OpenOCD (bundled in the SDK hosttools; ST-LINK over SWD) works and
  matches how ImpulseZephyr flashes. Forced by `flash.sh` rather than baked into the app as a
  default runner — overriding `board.cmake` for an in-tree board would mean carrying a board
  fragment in this repo purely to change one default, and a one-line `-r` in the wrapper is the
  smaller cost.
- **`-p always`** (the wrapper's `-p`) forces a pristine build — use it after devicetree or Kconfig
  changes.

### Hardware / connection facts

- Board: ST **Nucleo-H753ZI** (STM32H753ZI, Cortex-M7). ST-LINK **V3**.
- Serial console (VCP): `/dev/cu.usbmodem*` @ 115200 8N1 — `scripts/console.sh` auto-detects the
  port. Quit with **Ctrl-A then K**; Ctrl-A D merely detaches and leaves the port busy, which is
  why the next run then fails with `Resource busy`.
- Host tools (`cmake`, `dtc`, `openocd`, `st-flash`, `ninja`, `ccache`) come from Homebrew / the SDK.

## Host toolchain (Pi)

The harness in `host/` **must run on the Pi**. Mosquitto binds `192.168.10.1`, which exists only on
the direct cable to the Nucleo — deliberately, so an `allow_anonymous` broker stays off the home
LAN where the Pi is `192.168.1.105` over WiFi. Nothing on the WiFi side can reach it, including
your Mac.

```sh
python3 -m venv host/.venv                        # Raspberry Pi OS is PEP 668; a venv is mandatory
host/.venv/bin/pip install -r host/requirements.txt
./host/generate.sh                                # -> host/node_pb2.py

host/.venv/bin/python host/monitor.py             # decode and log every node topic
host/.venv/bin/python host/command.py info        # send a Command, await the Ack
```

Two constraints worth knowing before changing anything here:

- **`grpcio-tools` is pinned deliberately, and `generate.sh` goes through
  `python -m grpc_tools.protoc` rather than a system `protoc`.** Python's generated modules embed
  the compiler version and validate it at import: a newer runtime reads older gencode, never the
  reverse. A system `protoc` a generation ahead of the installed `protobuf` runtime fails with
  `Runtime version cannot be older than the linked gencode version`. Bundling both halves in one
  package keeps them in lockstep.
- **Never install protobuf into `~/zephyr-workspace/.venv`.** That venv drives the nanopb generator
  and is shared with `../ImpulseZephyr`; upgrading it to satisfy the host would risk the firmware
  build of two projects. The separation is the point — see `host/requirements.txt`.

The firmware side is immune to that whole class of problem, which is worth understanding: nanopb
has `protoc` emit a *descriptor set* and generates C from that with its own Python generator. It
never imports `protoc`'s generated Python, so the version check never runs. More in
[`protobuf-guide.md`](../notes/protobuf-guide.md) §9.

Generated artifacts — `node.pb.c/.h` on the firmware side, `node_pb2.py` on the host — are
gitignored and never hand-edited. Both regenerate from `proto/node.proto`, which is what makes
"single source of truth" mechanical rather than aspirational.

## Verifying the chain

- **Firmware toolchain**: `./scripts/build.sh` from a clean checkout, then `./scripts/flash.sh`.
  The console should show the node booting and reporting `SCD-40 online`.
- **Host toolchain**: `./host/generate.sh` prints the path it wrote; `host/.venv/bin/python -c
  "import node_pb2, paho.mqtt.client"` exits silently if both halves are installed.
- **The two together**: `host/.venv/bin/python host/monitor.py` on the Pi decodes telemetry the Mac
  just flashed — which is the only test that proves both generators agree on the schema.
