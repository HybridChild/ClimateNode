# Toolchain & build setup

Set up 2026-07-02. This project builds as a **freestanding Zephyr application** against a
**shared global west workspace** — it does *not* carry its own copy of the Zephyr tree.

## Layout

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

## Everything this project needs is in v4.4.1 (verified)

- **nanopb** — `modules/lib/nanopb` + `zephyr/modules/nanopb/nanopb.cmake` → use **in-tree nanopb**.
- **SCD4x sensor driver** — `zephyr/drivers/sensor/sensirion/scd4x/` + `sensirion,scd40` DT binding
  → use the **upstream in-tree driver** (no community module needed).
- **Board** — `nucleo_h753zi`.

(These resolve two of the README's "open decisions".)

## Build & flash workflow

`west build` must run from **inside** the workspace (so west can enumerate modules), with the app
source pointed at this repo via `-s`:

```sh
source ~/zephyr-workspace/.venv/bin/activate
export ZEPHYR_BASE=~/zephyr-workspace/zephyr
cd ~/zephyr-workspace
west build -p always -b nucleo_h753zi -s <path-to-app> -d <build-dir>
west flash -r openocd --build-dir <build-dir>
```

- `-s <path-to-app>` — e.g. `~/Engineering/Projects/EthernetProtobufZephyr/firmware` (once created).
- **`-r openocd` is required for flashing.** `nucleo_h753zi` defaults to the `stm32cubeprogrammer`
  runner, which is not installed. OpenOCD (bundled in the SDK hosttools; ST-LINK over SWD) works and
  matches how ImpulseZephyr flashes. *TODO once `firmware/` exists: bake `openocd` in as the app's
  default runner so `-r` isn't needed.*
- `-p always` forces a pristine build — use it after devicetree/Kconfig changes.

## Hardware / connection facts

- Board: ST **Nucleo-H753ZI** (STM32H753ZI, Cortex-M7). ST-LINK **V3**.
- Serial console (VCP): `/dev/cu.usbmodem202144403` @ 115200 8N1.
- Host tools (`cmake`, `dtc`, `openocd`, `st-flash`, `ninja`, `ccache`) come from Homebrew / the SDK.

## Verified working

`samples/basic/blinky` built for `nucleo_h753zi` and flashed via `-r openocd`; LD1 blinks. Full
chain (compiler → OpenOCD → ST-LINK → board) confirmed 2026-07-02.
