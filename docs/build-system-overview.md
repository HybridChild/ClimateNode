# Zephyr build system overview

Written 2026-07-06. A map of how West, CMake, devicetree, and Kconfig turn source
into a firmware image — organized around the question *"which input lines produce
which generated file, consumed by what?"*. Line numbers below were verified against a
real `firmware/build/` for `nucleo_h753zi` (Zephyr v4.4.1); they will drift on
regeneration but the structure holds.

## The big idea (read this first)

Two orthogonal questions have to be answered before any code compiles, and Zephyr
answers them with two *separate* systems. Keeping them straight is 80% of understanding
the build:

| Question | Answered by | Nature |
|---|---|---|
| **"What hardware exists, and how is it wired?"** | **devicetree** (`.dts` + overlays + bindings) | *facts*, fixed by the physical board |
| **"Which software features do I compile in?"** | **Kconfig** (`prj.conf` + defconfigs) | *policy*, your choice per application |

They are deliberately split so the *same* board files serve every app, and the *same*
app config can target multiple boards. The build's job is to **reconcile** them into two
generated headers your C code consumes.

The second big idea: **Zephyr resolves everything it possibly can at compile time.** A
sensor isn't "discovered" at runtime — by the time `main()` runs it's already a
ready-made `struct device` with its bus and address baked in by macros. Describe hardware
as data; resolve before boot. Almost every arrow in this document happens during the
build, not on the target.

## Three tools, three tiers

```
west   → manages the workspace, launches the build   (Python, multi-repo)
cmake  → CONFIGURES: resolves board/dts/kconfig, plans the build
ninja  → BUILDS: runs the planned compile/link commands  (fast, mechanical)
```

- **West does not build.** `west build` is a thin wrapper that invokes `cmake` then `ninja`.
  Its real job is multi-repo management (`west init` / `west update` clone Zephyr + modules).
- **`west zephyr-export`** wrote one file into CMake's user package registry
  (`~/.cmake/packages/Zephyr/<hash>`) pointing at
  `~/zephyr-workspace/zephyr/share/zephyr-package/cmake`. That is how
  `find_package(Zephyr)` locates the workspace with `ZEPHYR_BASE` unset.
- **CMake thinks, Ninja executes.** This split is why *what you edit* determines *how much
  rebuilds*: editing a `.c` only re-runs Ninja (fast). Editing your overlay, `prj.conf`, or
  a Kconfig fragment changes *configuration*, so CMake must regenerate the artifacts below.
  A plain `west build` usually detects this and reconfigures; `west build -p` (pristine)
  wipes `build/` for a clean slate when stale generated state misbehaves.

## Entry point: `firmware/CMakeLists.txt`

```cmake
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})   # line 3 — the seam
project(scd40_read)                                     # AFTER find_package (toolchain set first)
target_sources(app PRIVATE src/main.c)                  # `app` target created by Zephyr's kernel.cmake
```

Two ordering facts that look wrong to a normal-CMake eye but are load-bearing:

- **`find_package(Zephyr)` comes before `project()`.** Zephyr must select and configure the
  ARM cross-compiler *before* `project()` triggers CMake's compiler-detection probe —
  otherwise CMake would test your host `gcc`.
- **`target_sources(app …)` works even though you never defined `app`.** The `app` target
  is created for you deep inside the Zephyr modules (`kernel.cmake`); you're contributing
  your source into Zephyr's pre-existing application target.

`find_package(Zephyr)` loads `share/zephyr-package/cmake/ZephyrConfig.cmake`, which
prepends `zephyr/cmake/modules` to `CMAKE_MODULE_PATH` and `include(zephyr_default)`.

## `zephyr_default.cmake` — the ordered module list

`zephyr/cmake/modules/zephyr_default.cmake` builds a list via `list(APPEND zephyr_cmake_modules …)`
and `include()`s each in turn. **Order is a dependency chain**, and the key fact is:

```
… boards → (dts) → (kconfig) → arch → soc → [foreach ends] → kernel
```

**`dts` runs BEFORE `kconfig`** — because devicetree *feeds* Kconfig (see the ⭐ bridge
below), not the other way around. `include(kernel)` is last and unconditional; it defines
the `app` target.

## The four kinds of things

Every file in this system plays exactly one of four roles. Learn to classify a file on
sight and the mental clutter clears:

| Role | Examples | Rule |
|---|---|---|
| **Source inputs — your app** | overlay, `prj.conf`, `CMakeLists.txt`, `main.c` (in *this* repo) | you author these |
| **Source inputs — the tree** (read-only) | board `.dts`, SoC `.dtsi`, defconfig, bindings, in-tree driver `.c`/`Kconfig` | live in `~/zephyr-workspace/zephyr`; you **override** via overlay/`prj.conf`, not by editing them |
| **Generators** (run at CMake configure) | C preprocessor, the three DT Python scripts, Kconfig | run automatically |
| **Generated artifacts** | `zephyr.dts`, `devicetree_generated.h`, `Kconfig.dts`, `.config`, `autoconf.h` | live in `build/` only — **never edit** |
| **Consumers** | driver/app `DT_*` macros, the C compiler's `#ifdef CONFIG_*`, CMake's file-list logic | read the artifacts |

## The master pipeline

`edt.pickle` is the hub of the whole devicetree half — a single parsed model of the tree,
read by `gen_defines.py` (→ C macros) and again by Kconfig (→ symbol *values*). Note that
`dtc` (the classic devicetree compiler) is *not* the parser here; Zephyr parses with its
own Python `edtlib`, and only runs `dtc` as an optional linter if it's installed. Note too
that `Kconfig.dts` is generated from the **bindings**, not from your tree — it *declares*
the symbols; `edt.pickle` only supplies their values later (see the ⭐ bridge).

```
 SOURCE INPUTS                    DTS STAGE  (Python; dtc only lints)                   CONSUMED BY
 board .dts ┐
 SoC .dtsi  ├─► cpp ─► zephyr.dts.pre ─┐
 overlay    ┘                          ├─► gen_edt.py ─► edt.pickle ─► gen_defines.py ─► devicetree_generated.h ─► <devicetree.h> → DT_* macros
 bindings/*.yaml ─┬─────────────────────┘       (also dumps zephyr.dts for humans + dtc lint)
                  │
                  └─► gen_driver_kconfig_dts.py ─► build/Kconfig/Kconfig.dts ─► Kconfig parser ⭐

 prj.conf ─────┐
 defconfig     ├─► Kconfig ─────┬─► .config ──────────────► CMake: which sources to compile
 tree Kconfig ─┤ (+ Kconfig.dts ├─► autoconf.h ───────────► EVERY .c via -imacros → #ifdef CONFIG_*
 edt.pickle ⭐ ┘  evaluated via  └─► misc/generated/configs.c ─► debugger symbols
                  dt_compat_enabled)
```

### The ⭐ bridge, precisely

This is the mechanism by which *enabling a hardware node turns on its driver* — and it's
worth getting exactly right:

1. **Declaration (from bindings, board-independent).** `gen_driver_kconfig_dts.py` scans
   **all** bindings — not your tree — and writes `Kconfig.dts`, which *declares* one symbol
   per compatible. That's why `Kconfig.dts` lists thousands of symbols for hardware you
   don't have:
   ```
   config DT_HAS_SENSIRION_SCD40_ENABLED
       def_bool $(dt_compat_enabled,$(DT_COMPAT_SENSIRION_SCD40))
   ```
2. **Evaluation (from your tree).** `dt_compat_enabled` is a **Kconfig preprocessor
   function** (in `scripts/kconfig/kconfigfunctions.py`) that, during Kconfig parsing,
   reads **`edt.pickle`** and returns `y` iff some `status = "okay"` node has that compatible.

So the coupling is a division of labour: **bindings** decide which symbols *exist*;
**`edt.pickle`** (your actual tree) decides which are *`y`*. `edt.pickle` itself is read
twice — by `gen_defines.py` for the C macros, and by `dt_compat_enabled` here — and that is
the entire devicetree→Kconfig link.

## Artifact reference (verified paths + line numbers)

All under `firmware/build/zephyr/` unless noted.

| Artifact | Made by ← from | Consumed by | Verified evidence |
|---|---|---|---|
| `zephyr.dts.pre` | C preproc ← board `.dts` + SoC `.dtsi` + overlay | `gen_edt.py` (intermediate) | 202 KB |
| `edt.pickle` | `gen_edt.py` ← `.dts.pre` + bindings | `gen_defines.py`, Kconfig's `dt_compat_enabled` | 1.7 MB — **the DT hub** |
| `zephyr.dts` | `gen_edt.py` (debug dump) | **humans** (debug: "did my overlay merge?") + `dtc` lint | `scd40@62` at line 669, back-refs `overlay:5` |
| `include/generated/zephyr/devicetree_generated.h` | `gen_defines.py` ← `edt.pickle` | `#include <devicetree.h>` → all `DT_*` macros | `_scd40_62_BUS` at 25907; `_ADDRESS 0x62` at 25912 |
| `build/Kconfig/Kconfig.dts` ⭐ | `gen_driver_kconfig_dts.py` ← bindings | Kconfig parser (as input) | declares `DT_HAS_SENSIRION_SCD40_ENABLED` |
| `.config` | Kconfig ← `prj.conf`+defconfig+tree Kconfig+`Kconfig.dts` | **CMake** (which files to compile) | `CONFIG_SCD4X=y` at 1036; `CONFIG_DT_HAS_SENSIRION_SCD40_ENABLED=y` at 20 |
| `include/generated/zephyr/autoconf.h` | Kconfig ← `.config` | **every `.c`** via `-imacros` | `#define CONFIG_SCD4X 1` at 328 |
| `misc/generated/configs.c` | Kconfig | debugger symbol table | `GEN_ABSOLUTE_SYM_KCONFIG(CONFIG_DT_HAS_SENSIRION_SCD40_ENABLED, 1)` |

`autoconf.h` is force-included into every translation unit — confirmed in
`firmware/build/compile_commands.json`: `-imacros …/autoconf.h`. That is why any `.c` can
test `#ifdef CONFIG_SCD4X` with no `#include`.

## The linchpin: `compatible`

The string that ties the whole devicetree half together is a node's `compatible`. It does
double duty, both at compile time:

- **Validation** — it selects a *binding* (`dts/bindings/…/sensirion,scd40.yaml`) that
  defines which properties are legal. `gen_edt.py` checks the node against it.
- **Driver binding** — the driver declares `#define DT_DRV_COMPAT sensirion_scd40` (note the
  `,`→`_` transform) and `DT_INST_FOREACH_STATUS_OKAY(...)` instantiates itself **once per
  enabled node** with that compatible.

Same string, two independent lookups. Get it wrong (typo, or no matching binding) and the
build fails at parse time — never silently at runtime.

## Worked example: `&i2c1` → SCD40, end to end

| Stage | File : line | Content |
|---|---|---|
| input | `firmware/boards/nucleo_h753zi.overlay:4-7` | `scd40@62 { compatible="sensirion,scd40"; reg=<0x62>; status="okay" }` |
| ↓ gen_edt | `build/zephyr/zephyr.dts:669` | node merged under `/soc/i2c@40005400`; recorded in `edt.pickle` |
| ↓ gen_defines | `devicetree_generated.h:25907,25912` | `_BUS → i2c@40005400`, `_ADDRESS 0x62` |
| ↓ gen_driver_kconfig | `build/Kconfig/Kconfig.dts` | declares `DT_HAS_SENSIRION_SCD40_ENABLED` (value from `edt.pickle`) |
| ↓ kconfig | `zephyr/drivers/sensor/sensirion/scd4x/Kconfig` | `config SCD4X … depends on DT_HAS_SENSIRION_SCD40_ENABLED; select I2C,CRC` |
| ↓ kconfig | `build/zephyr/.config:1036` | `CONFIG_SCD4X=y` |
| ↓ kconfig | `autoconf.h:328` | `#define CONFIG_SCD4X 1` |
| ↓ CMake | `scd4x/CMakeLists.txt` | compiles `scd4x.c` **because** `CONFIG_SCD4X` |
| ↓ driver | `scd4x.c:902-903` | `DT_DRV_COMPAT sensirion_scd40` + `DT_INST_FOREACH_STATUS_OKAY` → 1 instance |
| ↓ driver | `scd4x.c:894` | `.bus = I2C_DT_SPEC_INST_GET(0)` ← reads the `_BUS`/`_ADDRESS` macros above |
| ↓ app | `firmware/src/main.c:16` | `DEVICE_DT_GET(DT_NODELABEL(scd40))` ← same node symbol |
| ↓ runtime | `firmware/src/main.c:26` | `sensor_sample_fetch()` → I²C bytes on the wire |

Everything above the `sample_fetch` call is resolved **at compile time** by devicetree
macros — the arrow from `&i2c1` to the SCD40 is literally drawn by `I2C_DT_SPEC_INST_GET`
reading the node's parent bus and `reg` address. Only the final byte-level I²C exchange is
runtime. The command codes, timings, and CRC-8 params underneath `sensor_sample_fetch` are
documented in `../../shared_refs/sensor/SCD4x.yaml`.

## Two-sentence mental model

1. **Devicetree** (`.dts`/overlay + bindings) → three Python scripts → `edt.pickle`, which
   becomes `devicetree_generated.h` (C macros for `DT_*` in code); separately, the bindings
   become `Kconfig.dts` (symbol *declarations* that feed the config stage).
2. **Kconfig** (`prj.conf` + defconfig + `Kconfig.dts`, its `DT_HAS_*` values read from
   `edt.pickle`) → `.config` (tells CMake *what to compile*) and `autoconf.h` (force-included
   into every `.c` for `#ifdef CONFIG_*`).

Everything else is detail hanging off those two generators — hardware facts vs. software
policy, both resolved before the target ever boots.
