# SCD-40 sensor bring-up

First firmware milestone, done 2026-07-02. A minimal Zephyr app on the Nucleo-H753ZI reads the
Adafruit **SCD-40** (CO₂ / temperature / humidity) over I²C using Zephyr's **sensor API** and prints
each reading to the serial console every ~5 s. This is the *sensor path only* — no networking or
Protobuf yet; it's the live data source the later telemetry milestones will marshal onto the wire.

Builds against the shared global Zephyr workspace — see [`toolchain.md`](toolchain.md).

## What was built

```
firmware/
├── CMakeLists.txt              freestanding app (find_package(Zephyr HINTS $ENV{ZEPHYR_BASE}))
├── prj.conf                    Kconfig: C++17 + sensor + I2C + float printf
├── boards/nucleo_h753zi.overlay   the SCD-40 device on I2C1
└── src/main.cpp                fetch/get loop, prints every 5 s
scripts/
├── build.sh                    wraps `west build` (venv + ZEPHYR_BASE + -s/-d)
└── flash.sh                    wraps `west flash -r openocd`
```

## How it fits together (the four moving parts)

**1. Devicetree overlay — `firmware/boards/nucleo_h753zi.overlay`.** Declares the sensor as a child
of the I²C bus:

```dts
&i2c1 {
	scd40: scd40@62 {
		compatible = "sensirion,scd40";
		reg = <0x62>;
		status = "okay";
	};
};
```

- The board's own `nucleo_h753zi.dts` **already** enables `&i2c1` with the right pins
  (`pinctrl-0 = <&i2c1_scl_pb8 &i2c1_sda_pb9>`, PB8/PB9 = AF4, Arduino D15/D14) at 400 kHz — so the
  overlay adds *only* the device, not the bus config.
- `compatible = "sensirion,scd40"` is the match key: it binds this node to the in-tree scd4x driver
  at build time. `reg = <0x62>` is the SCD4x's fixed I²C address.
- Zephyr auto-applies any `boards/<board>.overlay` under the app root when building `-b <board>`, so
  no `DTC_OVERLAY_FILE` wiring is needed.

**2. Kconfig — `firmware/prj.conf`.**

```
CONFIG_CPP=y               # build the app as C++ (see language-cpp.md)
CONFIG_STD_CPP17=y         # …at C++17 (default would be C++11)
CONFIG_SENSOR=y            # the sensor subsystem (sample_fetch / channel_get API)
CONFIG_I2C=y               # I2C bus driver
CONFIG_STDOUT_CONSOLE=y    # route printf() to the console UART
CONFIG_CBPRINTF_FP_SUPPORT=y   # %f support (Zephyr strips float printf by default)
```

Note what's *absent*: we never set `CONFIG_SCD4X`. The driver's Kconfig is `default y` gated on
`DT_HAS_SENSIRION_SCD40_ENABLED`, so **adding the overlay node auto-selects the driver** (and it
pulls in `I2C` + `CRC`). Verified in the build: `CONFIG_SCD4X=y`, `CONFIG_CRC=y` appear in
`build/zephyr/.config` without us asking.

**3. Application — `firmware/src/main.cpp`.** The classic synchronous sensor loop:

- `DEVICE_DT_GET(DT_NODELABEL(scd40))` resolves the overlay's `scd40:` label to a device pointer *at
  compile time* — if the overlay didn't apply, the build fails rather than crashing at runtime.
- `device_is_ready()` guards against a driver that failed to init.
- Per iteration: `sensor_sample_fetch()` reads all three values in one I²C transaction;
  `sensor_channel_get()` copies each into a `struct sensor_value` (integer `val1` + micro-fraction
  `val2` — no floats on the wire); `sensor_value_to_double()` collapses that to a printable double.
- `k_sleep(K_SECONDS(5))` matches the sensor's own sample cadence.

**4. Console.** The board's chosen `zephyr,console = &usart3` (PD8/PD9) is the ST-LINK Virtual COM
Port. `printf` reaches it at **115200 8N1** on `/dev/cu.usbmodem202144403`.

## Driver behaviour worth knowing

- **Auto-start.** `scd4x_init` issues `START_PERIODIC_MEASUREMENT`. The app does *not* have to
  trigger measurement — the sensor free-runs at ~5 s.
- **Init is deferred to the app** (decided 2026-07-21). The node carries `zephyr,deferred-init`, so
  the boot sweep skips it (`kernel/init.c`) and `src/sensor.cpp` calls `device_init()` itself after
  a 100 ms wait. Reason: the chip needs up to **30 ms** after VDD to answer on I²C at all
  (`power_up_time`), it shares a rail with the board, and a failed init is **permanent** —
  `do_device_init()` sets `initialized` even on error, so `device_is_ready()` latches false;
  `device_init()` then returns `-EALREADY` and `device_deinit()` `-ENOTSUP` (the driver registers no
  deinit op). Only the *timing* of the single attempt is controllable, so we control it.
  Side effect: `scd4x_init` takes ~530 ms (datasheet waits after `stop_periodic_measurement` and
  `reinit`), now spent off the boot path — but `sensor get scd40@62` fails for the first ~600 ms.
- **Data-ready / no-block.** In periodic mode `sensor_sample_fetch()` is non-blocking and **returns 0
  without updating the values when no fresh sample is ready yet** (`scd4x.c`: `if (!is_data_ready)
  return 0;`, skipping `scd4x_read_sample()`). So the first read(s) after boot may show stale/zero
  data until the sensor's first ~5 s sample lands. See *Accepted limitation* below — `SetInterval`
  has since made this reachable at runtime, not just at boot.
- **Mode.** The `sensirion,scd40` binding has no `mode` property → always normal periodic (~5 s).
  Low-power (30 s) or single-shot would require the register-compatible `sensirion,scd41` compatible
  with a `mode = <...>` property.
- **Runtime attributes** (via `sensor_attr_set`, `#include <zephyr/drivers/sensor/scd4x.h>`):
  temperature offset, sensor altitude, ambient pressure, automatic self-calibration. Not used yet.

## Build & flash

```sh
scripts/build.sh -p     # pristine (after devicetree/Kconfig edits); plain form = incremental
scripts/flash.sh        # forces -r openocd (nucleo_h753zi defaults to the uninstalled cube runner)
screen /dev/cu.usbmodem202144403 115200   # watch output; exit: Ctrl-A K Y
```

Both scripts activate the workspace venv and export `ZEPHYR_BASE` themselves, so no manual `source`
is needed. `build.sh` runs `west build` from inside `~/zephyr-workspace` (so west can enumerate
modules) with `-s firmware -d firmware/build`.

Build footprint: **37832 B FLASH (1.80 %)**, ~5 KB RAM.

## Verified on hardware (2026-07-02)

Wiring: SCD-40 STEMMA QT → Nucleo — SCL→**PB8** (D15), SDA→**PB9** (D14), plus 3V3 and GND (breakout
has its own pull-ups). Serial output:

```
CO2: 412 ppm  |  T: 25.77 C  |  RH: 52.9 %
CO2: 411 ppm  |  T: 25.69 C  |  RH: 53.4 %
...
```

Sanity of the numbers: **CO₂ ~410 ppm** is the fresh-air baseline (the SCD40 assumes its first
reading is fresh air, ~400 ppm). Temperature and RH track inversely as expected. The slow downward
temperature drift right after start is the sensor's **self-heating** settling — it reads slightly
high initially; the temperature-offset attribute exists to correct this if accuracy matters later.

## Accepted limitation: a fast poll republishes the last conversion

**Decided 2026-07-21: known, documented, not fixed.**

The sensor free-runs at one conversion per ~5 s, and `sensor_sample_fetch()` returns **0** when no
new one is ready, leaving the driver's cached values in place. `sensor_channel_get()` then hands
back the *previous* reading with no indication that it is old.

At bring-up this only affected the first few seconds, so "poll at 5 s to match" was enough. The
`SetInterval` command changed that: `SAMPLE_PERIOD_MIN_MS` is **1000 ms**, so
`command.py interval 1000` makes roughly four in five publishes silent repeats of the last
conversion — a fresh `sequence` and `uptime_ms` wrapped around stale measurements, reported as
`SENSOR_STATUS_OK`. Even at exactly 5000 ms the two clocks are independent and will drift into each
other periodically. `TriggerMeasurement` has the same shape: it republishes the newest conversion
rather than forcing a new one.

Why it stands:

- **Single-shot is not available on this part.** `SCD4x.yaml` lists `single_shot: unavailable` for
  the SCD40 (SCD41/SCD43 only), which is why the `sensirion,scd40` binding has no `mode` property.
  There is no way to command a conversion on demand.
- **The driver reports no staleness**, and the sensor API exposes no data-ready channel. The only
  in-API detection is comparing the three raw `sensor_value`s against the previous fetch and
  treating an identical triple as "no new data" — a heuristic, since a genuinely repeated reading
  is possible.
- **It is a device quirk, not one of this repo's learning objectives** (MQTT lifecycle, schema
  evolution, zbus). Carrying a heuristic through the sensor path would obscure those without
  teaching anything transferable.

If it ever matters: raise `SAMPLE_PERIOD_MIN_MS` to 5000 to shrink the window, or move to an
SCD41/SCD43 and set `mode = <2>` for genuine single-shot.
