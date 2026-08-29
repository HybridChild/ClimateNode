# Toolchain & build setup

This project builds as a **freestanding Zephyr application** against a **shared global west workspace** — it does *not* carry its own copy of the Zephyr tree. There are two independent toolchains: one on the Mac that builds and flashes firmware, one on the Pi that runs the host harness. They share only `proto/node.proto`.

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
EthernetProtobufZephyr/    this repo — pure source (proto/ shared/ gateway/ peer-node/ host/), builds vs. above
```

The workspace is a shared global install, **pinned to Zephyr v4.4.1.** If this project ever needs a different version, give it its own workspace (escape hatch — not needed today; v4.4.1 already ships everything below).

### Everything this project needs is in v4.4.1

- **nanopb** — `modules/lib/nanopb` + `zephyr/modules/nanopb/nanopb.cmake` → use **in-tree nanopb**.
- **SCD4x sensor driver** — `zephyr/drivers/sensor/sensirion/scd4x/` + `sensirion,scd40` DT binding → use the **upstream in-tree driver** (no community module needed).
- **Board** — `nucleo_h753zi`.

### Build & flash

Use the wrappers; they source the workspace venv, export `ZEPHYR_BASE`, and pass the right source/build directories:

```sh
./scripts/build.sh        # incremental; -p forces pristine (required after devicetree/Kconfig edits)
./scripts/flash.sh        # forces the openocd runner
./scripts/console.sh      # serial console @115200; quit with Ctrl-A then K
```

**There are two apps**, and all three scripts take `-a <app>` (or `APP=`), defaulting to `gateway`:

```sh
./scripts/build.sh   -a peer-node -p    # the F072RB peer node
./scripts/flash.sh   -a peer-node
./scripts/console.sh -a peer-node
```

The board follows from the app rather than being something to remember — `gateway` → `nucleo_h753zi`, `peer-node` → `nucleo_f072rb` — so `-b` is never needed. `BOARD=` still wins for a one-off. `./scripts/cleanup.sh` takes the same flag and removes every app's build dir without it; `--all` widens that to everything generated in the repo — twister's output, the merged compile database and the clangd caches, `host/`'s generated protobuf code — and `-n` prints what would go without removing it. `host/.venv` survives either way, since refilling it needs the network.

`build.sh` also takes **`--debug`**, which merges `<app>/debug.conf` over `prj.conf` via `-DEXTRA_CONF_FILE`. Only `peer-node` has one today, and it exists because that node's shipped image has no shell — a stock Zephyr shell needs 95 % of its 16 KB of RAM, so an interactive console is a bench variant rather than a default. Kconfig fragments merge in order, so `debug.conf` adds rather than replaces. Toggling it changes Kconfig, so pair it with `-p`:

```sh
./scripts/build.sh -a peer-node --debug -p    # trimmed shell + the `can` commands, 83 % RAM
```

**Two boards attached at once is the normal case**, and it breaks the two scripts that have to pick one. `./scripts/probe.sh` is what they ask:

```
$ ./scripts/probe.sh
APP           PROBE           SERIAL                    PORT
gateway       STLINK_V3       0052003D3335510235383531  /dev/cu.usbmodem202144403
peer-node     STM32 STLink    066DFF363732594D43162633  /dev/cu.usbmodem202144103
```

It reads the IORegistry for every attached ST-LINK and pairs each with the `/dev/cu.*` that hangs off it, then names it using `scripts/probes.conf`. **The mapping is keyed on the ST-LINK serial**, which is burned into the probe and permanent; the `usbmodemNNNNNN` name comes from the USB topology and changes when the board moves to a different port or hub, which is exactly why it is not what gets written down. `probes.conf` is committed for the same reason the static IPs in `gateway/prj.conf` are — this repo describes one bench, and the real numbers are more use than a placeholder. On a different bench, run `probe.sh` and paste in what it prints.

What the two consumers do with it:

- **`console.sh -a <app>`** resolves the port. Without `-a` and with two boards attached it prints that same table and refuses, rather than opening one at random.
- **`flash.sh -a <app>`** passes `--serial` when more than one probe is present, and **stops** if it cannot resolve one. Losing that coin flip writes the wrong image to the wrong part and reports success, which is the one failure here worth an extra second to avoid. With a single probe attached nothing is passed and nothing is consulted, so a fresh bench works before `probes.conf` means anything. `STLINK_SERIAL=` overrides all of it.

What they wrap, and why each detail matters:

```sh
source ~/zephyr-workspace/.venv/bin/activate
export ZEPHYR_BASE=~/zephyr-workspace/zephyr
cd ~/zephyr-workspace                       # west must run from INSIDE the workspace
west build -p auto -b nucleo_h753zi -s <repo>/gateway -d <repo>/gateway/build
west flash -r openocd --build-dir <repo>/gateway/build
```

- **`cd` into the workspace** — west enumerates modules relative to the workspace topdir; run it from the repo and nanopb and the HALs are invisible.
- **`-s`/`-d`** keep the app source in this repo and the build output beside it.
- **`-r openocd` is required for flashing.** `nucleo_h753zi` defaults to the `stm32cubeprogrammer` runner, which is not installed. OpenOCD (bundled in the SDK hosttools; ST-LINK over SWD) works. Forced by `flash.sh` rather than baked into the app as a default runner — overriding `board.cmake` for an in-tree board would mean carrying a board fragment in this repo purely to change one default, and a one-line `-r` in the wrapper is the smaller cost.
- **`-p always`** (the wrapper's `-p`) forces a pristine build — use it after devicetree or Kconfig changes.

### Editor index (clangd)

`.vscode/settings.json` points clangd at **`compile_commands.json` at the repo root**, which `./scripts/ide-index.sh` writes by merging the three build trees that exist here: `gateway/build/` (the gateway), `peer-node/build/` (the peer node) and `twister-out/**` (the test suites, from `test.sh`). Run it by hand — once after a first build, then only when the editor reports missing headers in a file that compiles fine.

- **Why merged.** A test TU's compile command exists *only* in twister's database. Point clangd at `gateway/build` alone and everything under `tests/` fails to resolve `<zephyr/ztest.h>`, which cascades into an error on nearly every line — real-looking diagnostics with no build problem behind them.
- **Why not automatic.** `build.sh` and `test.sh` stay thin `exec` wrappers. A compile database goes stale only when the set of files or the flags changes — a new source file, a new suite, a Kconfig or devicetree edit — so refreshing on every build would tax every build for a result that changes a few times a month.
- **First build required.** Neither database exists on a fresh clone, so clangd has nothing until the first `build.sh` or `test.sh`, and then the merge. The merged file is generated, machine-specific (absolute paths) and `.gitignore`d.
- **Duplicates resolve to the gateway.** All three trees compile much of Zephyr itself under different Kconfig, so shared kernel sources appear several times; the merge keeps the first entry, and `gateway/` is listed first because it is the widest configuration — networking, MQTT, four threads — and so the most informative one to read shared Zephyr code under. Files unique to `peer-node/` still get their own app's flags, and the two files in `shared/` get the gateway's, which is fine because neither app's Kconfig changes how they parse.
- **clangd must be restarted to notice a changed `--compile-commands-dir`** — *clangd: Restart language server* in the command palette. It picks up *content* changes to the database on its own.
- Flag/diagnostic tweaks (query driver, GCC-only flags, `-Wvla`, `-Wsection`) live in `.clangd`, commented there and in [`../notes/language-cpp.md`](../notes/language-cpp.md) §11.

### Hardware / connection facts

- Board: ST **Nucleo-H753ZI** (STM32H753ZI, Cortex-M7). ST-LINK **V3**.
- Serial console (VCP): `/dev/cu.usbmodem*` @ 115200 8N1 — `scripts/console.sh` auto-detects the port when only one board is attached, and takes `-a <app>` when both are. Quit with **Ctrl-A then K**; Ctrl-A D merely detaches and leaves the port busy, which is why the next run then fails with `Resource busy`.
- Peer board: ST **Nucleo-F072RB** (STM32F072RB, Cortex-M0). ST-LINK **V2-1**, which is how `probe.sh` shows up as `STM32 STLink` against the H753ZI's `STLINK_V3`. Console on `usart2`, same 115200 8N1, but **no shell** — it logs at boot and is then silent by design, so a quiet console there is the node working.
- Host tools (`cmake`, `dtc`, `openocd`, `st-flash`, `ninja`, `ccache`) come from Homebrew / the SDK.

## Host toolchain (Pi)

The harness in `host/` **must run on the Pi**. Mosquitto binds `192.168.10.1`, which exists only on the direct cable to the Nucleo — deliberately, so an `allow_anonymous` broker stays off the home LAN where the Pi is `192.168.1.105` over WiFi. Nothing on the WiFi side can reach it, including your Mac.

```sh
python3 -m venv host/.venv                        # Raspberry Pi OS is PEP 668; a venv is mandatory
host/.venv/bin/pip install -r host/requirements.txt
./host/generate.sh                                # -> host/node_pb2.py

host/.venv/bin/python host/monitor.py             # decode and log every node topic
host/.venv/bin/python host/command.py info        # send a Command, await the Ack
```

Two constraints worth knowing before changing anything here:

- **`grpcio-tools` is pinned deliberately, and `generate.sh` goes through `python -m grpc_tools.protoc` rather than a system `protoc`.** Python's generated modules embed the compiler version and validate it at import: a newer runtime reads older gencode, never the reverse. A system `protoc` a generation ahead of the installed `protobuf` runtime fails with `Runtime version cannot be older than the linked gencode version`. Bundling both halves in one package keeps them in lockstep.
- **Never install protobuf into `~/zephyr-workspace/.venv`.** That venv drives the nanopb generator; upgrading it to satisfy the host would risk the firmware build. The separation is the point — see `host/requirements.txt`.

The firmware side is immune to that whole class of problem, which is worth understanding: nanopb has `protoc` emit a *descriptor set* and generates C from that with its own Python generator. It never imports `protoc`'s generated Python, so the version check never runs. More in [`protobuf-guide.md`](../notes/protobuf-guide.md) §11.

Generated artifacts — `node.pb.c/.h` on the firmware side, `node_pb2.py` on the host — are gitignored and never hand-edited. Both regenerate from `proto/node.proto`, which is what makes "single source of truth" mechanical rather than aspirational.

## Verifying the chain

- **Firmware toolchain**: `./scripts/build.sh` from a clean checkout, then `./scripts/flash.sh`. The console should show the node booting and reporting `SCD-40 online`.
- **Host toolchain**: `./host/generate.sh` prints the path it wrote; `host/.venv/bin/python -c "import node_pb2, paho.mqtt.client"` exits silently if both halves are installed.
- **The two together**: `host/.venv/bin/python host/monitor.py` on the Pi decodes telemetry the Mac just flashed — which is the only test that proves both generators agree on the schema.
