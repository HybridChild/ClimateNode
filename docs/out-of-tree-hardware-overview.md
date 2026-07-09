# Zephyr out-of-tree hardware — project reference

Written 2026-07-09. The companion to [`build-system-overview.md`](build-system-overview.md).
That doc traces the *easy* hardware case — an in-tree board (`nucleo_h753zi`) plus a
four-line overlay grafting one sensor onto an existing bus. This doc traces the *hard* case:
a board Zephyr has never heard of, for an MCU it does not ship, where **you** author the
entire hardware definition. The reference project is the sibling repo
[`../ImpulseZephyr`](../../ImpulseZephyr) — a TC Electronic guitar-pedal firmware whose board
`impulse_pedal` lives entirely inside the app repo.

Verified against `../ImpulseZephyr` source and the Zephyr v4.4.1 CMake modules in
`~/zephyr-workspace/zephyr/cmake/modules`. Unlike the sibling doc there is no live
`build/` to cite line numbers from — evidence here is source-level (the board files and the
CMake that consumes them). The pipeline itself is identical; only the *inputs* move from the
tree into your repo.

## The spectrum: where does the hardware definition live?

The build pipeline (cpp → `edt.pickle` → `devicetree_generated.h`; Kconfig → `.config` →
`autoconf.h`) is **the same** in both projects. What differs is *who authors the inputs* and
*where they live*.

| | SCD40 project (this repo) | Impulse pedal (`../ImpulseZephyr`) |
|---|---|---|
| Board | `nucleo_h753zi` — **in the tree** | `impulse_pedal` — **in the app repo** |
| SoC | `stm32h753` — in the tree, exact match | `stm32f732` **does not exist**; aliased to in-tree `stm32f722` |
| Your DT input | a small **overlay** patching `&i2c1` | a full **board `.dts`**: SoC include, clocks, memory, every peripheral |
| Kconfig you own | `prj.conf` only | `prj.conf` **+ board defconfig + `Kconfig.<board>`** |
| Bindings | all in-tree | in-tree **+ a custom `impulse,potentiometer.yaml`** |
| Runner | inherited from the in-tree board | **you write** `board.cmake` + `support/openocd.cfg` |
| Discovery plumbing | none (board found in the tree) | `BOARD_ROOT` + `DTS_ROOT` in `CMakeLists.txt` |

The lesson the pedal exists to teach: **Zephyr makes you describe, as data, any hardware it
does not already ship support for** — before a single line of C runs.

## Two roots make your hardware discoverable

An overlay needs no plumbing: `west build -b nucleo_h753zi` finds the board in the tree, and
Zephyr auto-applies `boards/<board>.overlay` from the app. An *out-of-tree* board and its
*custom bindings* are invisible until you point Zephyr at them. That is the only extra seam,
and it lives in `CMakeLists.txt` **before `find_package(Zephyr)`** (same rule as the seam
itself — these vars must exist before the modules run):

```cmake
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})   # dir that CONTAINS boards/  (not boards/ itself)
list(APPEND DTS_ROOT   ${CMAKE_CURRENT_SOURCE_DIR})   # dir that CONTAINS dts/bindings/
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(impulse_zephyr)
```

Mechanism (verified in `cmake/modules/`):

- `root.cmake` collects `BOARD_ROOT`/`DTS_ROOT` via `zephyr_get(... MERGE)` and normalizes
  any relative paths to absolute (`zephyr_file(APPLICATION_ROOT …)`).
- `boards.cmake` appends `ZEPHYR_BASE` to `BOARD_ROOT` (so the tree is always searched too),
  **requires each root to have a `boards/` subdirectory**, then runs `scripts/list_boards.py`
  with one `--board-root=<root>` per entry. That script scans `<root>/boards/**/board.yml`
  for the one whose `name:` matches `-b <BOARD>`, and sets `BOARD_DIR` to it.
- `dts.cmake`/`pre_dt.cmake` add `DTS_ROOT/dts/bindings` to the binding search path, so your
  YAML sits alongside the tree's bindings when `gen_edt.py` resolves `compatible`s.

`board.yml` is therefore the file that *makes a directory a board*: no `board.yml`, no board.

## Anatomy of an out-of-tree board

`../ImpulseZephyr/boards/st/impulse_pedal/` — seven files, each feeding a different stage.
(`st/` is just vendor grouping; `list_boards.py` recurses, so the nesting is cosmetic.)

| File | Role | Consumed by / stage |
|---|---|---|
| `board.yml` | **Identity**: `name`, `full_name`, `vendor`, `socs:`. Makes the dir a board. | `list_boards.py` during `boards.cmake` |
| `impulse_pedal.dts` | **The hardware, as a full tree root** — SoC `.dtsi` include, `chosen`, clock tree, every peripheral. Not a patch; the base tree itself. | `cpp` → `gen_edt.py` (DTS stage) |
| `Kconfig.impulse_pedal` | Ties the board symbol to its SoC: `config BOARD_IMPULSE_PEDAL` `select SOC_STM32F722XX`. | Kconfig parser |
| `impulse_pedal_defconfig` | Board-level Kconfig **defaults** always applied for this board (`ARM_MPU`, `HW_STACK_PROTECTION`, `GPIO`). | Kconfig (merged under `prj.conf`) |
| `Kconfig.defconfig` | Conditional board defaults (`if BOARD_IMPULSE_PEDAL … endif`) — e.g. flip a driver default when this board is selected. | Kconfig parser |
| `board.cmake` | **The runner**: which flash/debug backend. Here `include(common/openocd-stm32.board.cmake)`. | `boards.cmake` (defines `west flash`/`debug`) |
| `support/openocd.cfg` | Runner payload: ST-Link → STM32F7x over SWD, gdb attach/detach hooks. | OpenOCD at flash/debug time |
| `impulse_pedal.yaml` | **Twister** metadata (`ram`, `flash`, `supported:`) — test-orchestrator only, not part of the firmware build. | `west twister` |

Contrast: the SCD40 project owns **none** of these — it inherits all of them from
`nucleo_h753zi` in the tree and contributes only an overlay + `prj.conf`.

## SoC aliasing — building for a chip Zephyr doesn't ship

The pedal's real MCU is an **STM32F732RE**, which has no SoC in Zephyr v4.4.1. The F732 is an
F722 plus an unused AES block, so the board builds against the in-tree **F722**. This is done
in two independent places — and both are needed:

- **Kconfig** — `Kconfig.impulse_pedal`: `select SOC_STM32F722XX`. Picks the SoC's *code*
  (startup, clock driver, linker memory sizes → the 512K/256K F722Xe variant).
- **Devicetree** — `impulse_pedal.dts`: `#include <st/f7/stm32f722Xe.dtsi>` plus the
  package pinctrl `#include <st/f7/stm32f722r(c-e)tx-pinctrl.dtsi>`. Picks the SoC's
  *hardware description* (peripheral base addresses, the LQFP64 pin map).

Kconfig selects the silicon's behaviour; devicetree selects its shape. They are separate
systems (see the sibling doc's bridge section) and an alias must satisfy both.

## A board `.dts` is a root, not an overlay

This is the single biggest jump from the SCD40 case. An overlay is a *patch* — it opens
`&i2c1 { … }` and assumes a tree already exists. A board `.dts` **is** that tree. Only the
board author writes these top-level constructs (from `impulse_pedal.dts`):

- `/dts-v1/;` and the SoC `#include` — establishes the whole node hierarchy.
- `chosen { zephyr,sram/flash/dtcm/display = … }` — the system-level bindings the kernel and
  subsystems read (which RAM to link into, which node is *the* display).
- The **clock tree** — `&clk_hse`, `&pll` (`div-m=6 mul-n=216 div-p=2` → 216 MHz SYSCLK),
  `&rcc` prescalers. On an in-tree board this is pre-solved for you; here it is arithmetic you
  own, and getting it wrong bricks the boot or overclocks a bus.
- Peripheral nodes with full electrical intent: `&adc1` channel config
  (gain/reference/acquisition-time), the `zephyr,mipi-dbi-spi` → `sitronix,st7735r` display
  stack, `pwm-leds` backlight on `&timers3`.

Everything an overlay *relies on* being already present, a board `.dts` has to *supply*.

## Custom bindings via `DTS_ROOT`

`dts/bindings/impulse,potentiometer.yaml` exists because of a subtle rule: **a node with no
matching binding gets almost no generated macros.** The potentiometer has no driver — the app
just reads an ADC channel via `ADC_DT_SPEC_GET` — but without a binding, `gen_edt.py` won't
recognise its `io-channels` as a phandle-array and won't emit the accessor macros. So even a
driver-less node needs a binding to become a first-class devicetree citizen:

```yaml
compatible: "impulse,potentiometer"
include: [base.yaml]
properties:
  io-channels:
    type: phandle-array
    required: true
```

`DTS_ROOT` is what lets `gen_edt.py` find this file next to the tree's own bindings. Same
lesson as the board: describe what the tree doesn't already know.

## Where board inputs enter the master pipeline

The pipeline is the one from [`build-system-overview.md`](build-system-overview.md); the
board simply supplies inputs the SCD40 project inherited from the tree. Annotated:

```
 STEP 1 — DISCOVERY  (the CMakeLists roots, resolved before find_package):
   BOARD_ROOT ─► boards.cmake ─► list_boards.py ─► BOARD_DIR   (the dir whose board.yml `name:` == -b <BOARD>)
   DTS_ROOT   ─────────────────────────────────► adds dts/bindings/ to the binding search path

 STEP 2 — BOARD_DIR's files then fan out to the three usual destinations:

   ┌─ DTS stage ──────────────────────────────────────────────────────────────────┐
   │   impulse_pedal.dts   (base tree)  ┐                                         │
   │   dts/bindings/*.yaml (via DTS_ROOT)┴► cpp ─► gen_edt.py ─► devicetree_generated.h ─► DT_* macros
   └──────────────────────────────────────────────────────────────────────────────┘

   ┌─ Kconfig ─────────────────────────────────────────────────────────────────────┐
   │   *_defconfig + Kconfig.<board> / Kconfig.defconfig   (board defaults) ┐      │
   │   prj.conf                        (app — layered on top, overrides)    ┴► Kconfig ─► .config ─► autoconf.h ─► CONFIG_* macros
   └───────────────────────────────────────────────────────────────────────────────┘

   ┌─ Runner ──────────────────────────────────────────────────────────────────────┐
   │   board.cmake ─► support/openocd.cfg ─────────────────► west flash / debug    │
   └───────────────────────────────────────────────────────────────────────────────┘
```

Two facts worth internalising:

- The board `.dts` enters at the **base** of the DTS stage (where the SCD40 overlay entered as
  a *late patch*). Module order still holds: `boards` runs before `dts` before `kconfig`, so
  `BOARD_DIR` is known before either generator runs.
- Board Kconfig is layered **under** `prj.conf`: `*_defconfig` sets board defaults, then the
  app's `prj.conf` overrides. Same precedence model, one more layer.

## Worked example: `-b impulse_pedal` → every file it touches

| Stage | File | What it contributes |
|---|---|---|
| discover | `CMakeLists.txt:8,11` | `list(APPEND BOARD_ROOT/DTS_ROOT …)` before `find_package` |
| discover | `boards.cmake` → `list_boards.py` | scans `BOARD_ROOT/boards/**/board.yml`, matches `name: impulse_pedal` → `BOARD_DIR` |
| identity | `boards/st/impulse_pedal/board.yml` | `socs: [stm32f722xx]` |
| kconfig | `Kconfig.impulse_pedal` | `config BOARD_IMPULSE_PEDAL select SOC_STM32F722XX` |
| kconfig | `impulse_pedal_defconfig` | `CONFIG_ARM_MPU=y`, `CONFIG_GPIO=y` (board defaults) |
| kconfig | `prj.conf` | `CONFIG_SPI/DISPLAY/PWM/LED/ADC/CPP=y` (app, overrides on top) |
| dts | `impulse_pedal.dts` | `#include <st/f7/stm32f722Xe.dtsi>` + clock tree + peripherals → base tree |
| dts | `dts/bindings/impulse,potentiometer.yaml` (via `DTS_ROOT`) | makes `pot { io-channels }` emit macros |
| dts→C | `devicetree_generated.h` (in `build/`) | `DEVICE_DT_GET`/`ADC_DT_SPEC_GET`/`DT_ALIAS` for the app's C++ platform layer |
| runner | `board.cmake` → `support/openocd.cfg` | `west flash` = ST-Link/SWD via OpenOCD |
| twister | `impulse_pedal.yaml` | lets `west twister -b impulse_pedal` build the `tests/control` suite |

Everything above resolves at **configure/compile time**, exactly as in the SCD40 chain — the
board just relocates the inputs from `~/zephyr-workspace/zephyr` into the app repo. Master the
overlay case first (sibling doc), then this is the same machine with more of its inputs in
your hands.
