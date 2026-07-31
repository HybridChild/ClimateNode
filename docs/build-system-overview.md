# Zephyr build system — project reference

A terse, project-specific map of *which input produces which generated file, consumed by what* — checked against a real `gateway/build/` for `nucleo_h753zi` (Zephyr v4.4.1). Line numbers into generated files drift whenever Kconfig gains or loses a symbol; the structure holds. Regenerate and re-grep rather than trusting a number that looks off.

**For the concepts** (the mental model, why it's designed this way, how devicetree and Kconfig fit together) see the companion guide, [`zephyr-build-system-guide.md`](../notes/zephyr-build-system-guide.md). This file is the lookup reference that guide points back to.

## Workspace & entry point

- Built as a freestanding app against the shared workspace at `~/zephyr-workspace` (see [`toolchain.md`](toolchain.md)); this repo carries no Zephyr copy.
- **`west zephyr-export`** registered the workspace by writing one file into CMake's user package registry — `~/.cmake/packages/Zephyr/<hash>` containing the single path `~/zephyr-workspace/zephyr/share/zephyr-package/cmake`. That is how `find_package(Zephyr)` locates Zephyr with `ZEPHYR_BASE` unset.

`gateway/CMakeLists.txt`:

```cmake
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})   # the seam; must precede project()
project(scd40_read)

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
zephyr_nanopb_sources(app ${CMAKE_CURRENT_SOURCE_DIR}/../proto/node.proto)

set(SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../shared)

target_sources(app PRIVATE src/main.cpp src/sensor.cpp   # `app` target is created by
               src/relay.cpp                             # Zephyr's kernel.cmake
               ${SHARED}/protocol.cpp                    # both these are linked by
               ${SHARED}/commands.cpp)                   # peer-node/ too
target_include_directories(app PRIVATE ${SHARED})
```

Note the two sources from outside the app directory. `target_sources` takes any path, so sharing a translation unit between two applications needs no library and no install step — but the *generated* `node.pb.h` those two files include comes from the `zephyr_nanopb_sources(app …)` line above, which is this app's alone. Each app therefore regenerates the schema for itself; `shared/` carries no build rules at all. Why that beats a `zephyr_library`, a Zephyr module or sysbuild is in [`zephyr-build-system-guide.md`](../notes/zephyr-build-system-guide.md) §9.

`find_package(Zephyr)` loads `share/zephyr-package/cmake/ZephyrConfig.cmake`, which prepends `zephyr/cmake/modules` to `CMAKE_MODULE_PATH` and `include(zephyr_default)`.

## Module order (`zephyr/cmake/modules/zephyr_default.cmake`)

`list(APPEND zephyr_cmake_modules …)` builds an ordered list, each `include()`d in turn:

```
… boards → (dts) → (kconfig) → arch → soc → [foreach ends] → include(kernel)
```

`dts` runs **before** `kconfig` (devicetree feeds Kconfig — see the ⭐ bridge). `include(kernel)` is last and unconditional; it defines the `app` target.

## The four kinds of files

| Role | Examples | Rule |
|---|---|---|
| **Source inputs — your app** | overlay, `prj.conf`, `CMakeLists.txt`, `main.cpp` (in *this* repo) | you author these |
| **Source inputs — the tree** (read-only) | board `.dts`, SoC `.dtsi`, defconfig, bindings, in-tree driver `.c`/`Kconfig` | live in `~/zephyr-workspace/zephyr`; **override** via overlay/`prj.conf`, don't edit |
| **Generators** (run at CMake configure) | C preprocessor (`cpp`), the three DT Python scripts, Kconfig | run automatically |
| **Generated artifacts** | `zephyr.dts`, `devicetree_generated.h`, `Kconfig.dts`, `.config`, `autoconf.h` | live in `build/` only — **never edit** |
| **Consumers** | driver/app `DT_*` macros, the C compiler's `#ifdef CONFIG_*`, CMake's file-list logic | read the artifacts |

## The master pipeline

`dtc` is **not** the parser — Zephyr parses with its own Python `edtlib` and only runs `dtc` as an optional linter. `edt.pickle` is the parsed-tree hub: read by `gen_defines.py` (→ C macros) and by Kconfig (→ `DT_HAS_*` values). `Kconfig.dts` is generated from the **bindings**, not the tree.

```
 SOURCE INPUTS                    DTS STAGE  (Python; dtc only lints)                   CONSUMED BY
 board .dts ┐
 SoC .dtsi  ├─► cpp ─► zephyr.dts.pre ─┐
 overlay    ┘                          ├─► gen_edt.py ─► edt.pickle ─► gen_defines.py ─► devicetree_generated.h ─► <devicetree.h> → DT_* macros
 bindings/*.yaml ─┬────────────────────┘       (also dumps zephyr.dts for humans + dtc lint)
                  │
                  └─► gen_driver_kconfig_dts.py ─► build/Kconfig/Kconfig.dts ─► Kconfig parser ⭐

 prj.conf ─────┐
 defconfig     ├─► Kconfig ─────┬─► .config ──────────────► CMake: which sources to compile
 tree Kconfig ─┤ (+ Kconfig.dts ├─► autoconf.h ───────────► EVERY .c via -imacros → #ifdef CONFIG_*
 edt.pickle ⭐ ┘  evaluated via └─► misc/generated/configs.c ─► debugger symbols
                  dt_compat_enabled)
```

### The ⭐ bridge (exact mechanism)

- **Declaration** — `gen_driver_kconfig_dts.py` scans *all* bindings (not your tree) and writes `Kconfig.dts`, declaring one symbol per compatible:
  ```
  config DT_HAS_SENSIRION_SCD40_ENABLED
      def_bool $(dt_compat_enabled,$(DT_COMPAT_SENSIRION_SCD40))
  ```
- **Evaluation** — `dt_compat_enabled` (a Kconfig function in `scripts/kconfig/kconfigfunctions.py`) reads `edt.pickle` during parsing and returns `y` if and only if some `status = "okay"` node has that compatible.

Bindings decide which symbols *exist*; `edt.pickle` decides which are `y`.

## Artifact reference (verified paths + line numbers)

All under `gateway/build/zephyr/` unless noted.

| Artifact | Made by ← from | Consumed by | Verified evidence |
|---|---|---|---|
| `zephyr.dts.pre` | `cpp` ← board `.dts` + SoC `.dtsi` + overlay | `gen_edt.py` (intermediate) | 202 KB |
| `edt.pickle` | `gen_edt.py` ← `.dts.pre` + bindings | `gen_defines.py`, Kconfig's `dt_compat_enabled` | 1.7 MB — **the DT hub** |
| `zephyr.dts` | `gen_edt.py` (debug dump) | **humans** (debug: "did my overlay merge?") + `dtc` lint | `scd40@62` at line 669, back-refs `overlay:5` |
| `include/generated/zephyr/devicetree_generated.h` | `gen_defines.py` ← `edt.pickle` | `#include <devicetree.h>` → all `DT_*` macros | `_scd40_62_BUS` at 25907; `_ADDRESS 0x62` at 25912 |
| `build/Kconfig/Kconfig.dts` ⭐ | `gen_driver_kconfig_dts.py` ← bindings | Kconfig parser (as input) | declares `DT_HAS_SENSIRION_SCD40_ENABLED` |
| `.config` | Kconfig ← `prj.conf`+defconfig+tree Kconfig+`Kconfig.dts` | **CMake** (which files to compile) | `CONFIG_SCD4X=y` at 1241; `CONFIG_DT_HAS_SENSIRION_SCD40_ENABLED=y` at 20 |
| `include/generated/zephyr/autoconf.h` | Kconfig ← `.config` | **every `.c`** via `-imacros` | `#define CONFIG_SCD4X 1` at 421 |
| `misc/generated/configs.c` | Kconfig | debugger symbol table | `GEN_ABSOLUTE_SYM_KCONFIG(CONFIG_DT_HAS_SENSIRION_SCD40_ENABLED, 1)` |
| `../node.pb.c` / `../node.pb.h` | nanopb generator ← `proto/node.proto` + `node.options` | `#include <node.pb.h>` in both `.cpp` files | `#define node_Telemetry_size 42` |

The last row is the one generator this *app* adds; everything above it is Zephyr's own. It follows the same rule as the rest — the input is `proto/node.proto`, the output lives in `build/` and is never checked in or hand-edited. Concepts in [`zephyr-build-system-guide.md`](../notes/zephyr-build-system-guide.md) §8.

`autoconf.h` is force-included into every translation unit — confirmed in `gateway/build/compile_commands.json`: `-imacros …/autoconf.h`. That is why any `.c` can test `#ifdef CONFIG_SCD4X` with no `#include`.

## Worked example: `&i2c1` → SCD40, end to end

| Stage | File : line | Content |
|---|---|---|
| input | `gateway/boards/nucleo_h753zi.overlay:4-17` | `scd40@62 { compatible="sensirion,scd40"; reg=<0x62>; status="okay"; zephyr,deferred-init }` |
| ↓ gen_edt | `build/zephyr/zephyr.dts:669` | node merged under `/soc/i2c@40005400`; recorded in `edt.pickle` |
| ↓ gen_defines | `devicetree_generated.h:25907,25912` | `_BUS → i2c@40005400`, `_ADDRESS 0x62` |
| ↓ gen_driver_kconfig | `build/Kconfig/Kconfig.dts` | declares `DT_HAS_SENSIRION_SCD40_ENABLED` (value from `edt.pickle`) |
| ↓ kconfig | `zephyr/drivers/sensor/sensirion/scd4x/Kconfig` | `config SCD4X … depends on DT_HAS_SENSIRION_SCD40_ENABLED; select I2C,CRC` |
| ↓ kconfig | `build/zephyr/.config:1036` | `CONFIG_SCD4X=y` |
| ↓ kconfig | `autoconf.h:328` | `#define CONFIG_SCD4X 1` |
| ↓ CMake | `scd4x/CMakeLists.txt` | compiles `scd4x.c` **because** `CONFIG_SCD4X` |
| ↓ driver | `scd4x.c:902-903` | `DT_DRV_COMPAT sensirion_scd40` + `DT_INST_FOREACH_STATUS_OKAY` → 1 instance |
| ↓ driver | `scd4x.c:894` | `.bus = I2C_DT_SPEC_INST_GET(0)` ← reads the `_BUS`/`_ADDRESS` macros above |
| ↓ app | `gateway/src/sensor.cpp` | `DEVICE_DT_GET(DT_NODELABEL(scd40))` ← same node symbol |
| ↓ runtime | `gateway/src/sensor.cpp`, `read_scd40()` | `sensor_sample_fetch()` → I²C bytes on the wire |

Everything above the `sample_fetch` row resolves **at compile time**; only the final I²C exchange is runtime. The command codes, timings, and CRC-8 params underneath `sensor_sample_fetch` live in the SCD4x datasheet, and the sensor API itself in [`sensor-api-guide.md`](../notes/sensor-api-guide.md).

One extra wrinkle this table does not show: the node carries `zephyr,deferred-init`, so the boot sweep skips it and `sensor.cpp` calls `device_init()` itself. That changes *when* the driver's init runs, not any of the compile-time resolution above — see [`sensor-bringup.md`](sensor-bringup.md).
