# SCD-40 sensor bring-up — project reference

How the Adafruit **SCD-40** (CO₂ / temperature / humidity) is wired, described, initialised and read on this bench. Terse by intent: decisions, rationale, and the facts you need when the sensor path misbehaves. For the concepts underneath — what a `struct device` is, why reading is `fetch` + `get`, what `struct sensor_value` is for — see the companion teaching guide, [`sensor-api-guide.md`](../notes/sensor-api-guide.md). The chip's own command codes and conversion formulas are noted inline in these two docs where they matter.

Builds against the shared global Zephyr workspace — see [`toolchain.md`](toolchain.md).

## The sensor path

Four files, each doing one thing:

```
firmware/boards/nucleo_h753zi.overlay   the SCD-40 device on I2C1, marked deferred-init
firmware/prj.conf                       CONFIG_SENSOR + CONFIG_I2C + the sensor shell
firmware/src/sensor.cpp                 owns the device: init, the sampling thread, both channels
firmware/src/app_channels.h             the struct the readings travel in
```

`sensor.cpp` is the only file that talks to the sensor. It publishes each reading to the `chan_telemetry` zbus channel and never mentions MQTT, protobuf or the network; the wire side picks readings up from that channel. That boundary is the point — see the header comment in `app_channels.h`.

## How it fits together

**1. Devicetree overlay — `firmware/boards/nucleo_h753zi.overlay`.** Declares the sensor as a child of the I²C bus:

```dts
&i2c1 {
	scd40: scd40@62 {
		compatible = "sensirion,scd40";
		reg = <0x62>;
		status = "okay";
		zephyr,deferred-init;
	};
};
```

- The board's own `nucleo_h753zi.dts` **already** enables `&i2c1` with the right pins (`pinctrl-0 = <&i2c1_scl_pb8 &i2c1_sda_pb9>`, PB8/PB9 = AF4, Arduino D15/D14) at 400 kHz — so the overlay adds *only* the device, not the bus config.
- `compatible = "sensirion,scd40"` is the match key: it binds this node to the in-tree scd4x driver at build time. `reg = <0x62>` is the SCD4x's fixed I²C address.
- `zephyr,deferred-init` hands the single init attempt to the application — see *Init is deferred to the app* below.
- Zephyr auto-applies any `boards/<board>.overlay` under the app root when building `-b <board>`, so no `DTC_OVERLAY_FILE` wiring is needed.

**2. Kconfig — `firmware/prj.conf`.** The sensor's share of it:

```
CONFIG_SENSOR=y            # the sensor subsystem (sample_fetch / channel_get API)
CONFIG_I2C=y               # I2C bus driver
CONFIG_SENSOR_SHELL=y      # `sensor get scd40@62` from the console — see Bring-up checks
CONFIG_CBPRINTF_FP_SUPPORT=y   # %f support (Zephyr strips float printf by default)
```

Note what's *absent*: we never set `CONFIG_SCD4X`. The driver's Kconfig is `default y` gated on `DT_HAS_SENSIRION_SCD40_ENABLED`, so **adding the overlay node auto-selects the driver** (and it pulls in `I2C` + `CRC`). Verified in the build: `CONFIG_SCD4X=y`, `CONFIG_CRC=y` appear in `build/zephyr/.config` without us asking. The mechanism is the devicetree→Kconfig bridge described in [`build-system-overview.md`](build-system-overview.md).

**3. Application — `firmware/src/sensor.cpp`.**

- `DEVICE_DT_GET(DT_NODELABEL(scd40))` resolves the overlay's `scd40:` label to a device pointer *at compile time* — if the overlay didn't apply, the build fails rather than crashing at runtime.
- `sensor_thread()` sleeps `kSensorPowerUpMs`, calls `device_init()`, then latches `device_ok` from `device_is_ready()` once and keeps it `const`. Re-checking per cycle would look like resilience and provide none — readiness cannot change after the first attempt (below).
- `read_scd40()` does the reading: `sensor_sample_fetch()` reads all three values in one I²C transaction, then three `sensor_channel_get()` calls copy each into a `struct sensor_value` (integer `val1` + micro-fraction `val2` — no floats in the driver), and `sensor_value_to_double()` collapses that to the `float`/`uint32_t` fields of `struct sensor_reading`.
- A failed read is reported, not hidden: the reading still goes out carrying `SENSOR_READING_ERROR`. The host cannot distinguish a broken sensor from a dead node if we simply go quiet, which is why the status field exists. It carries **no measurements** — `read_scd40()` sets the `has_*` flags in `struct sensor_reading` only on the success path, and a warming-up read returns before touching them, so the three zeros never reach the wire as values. They become absent `optional` fields instead; see [`protobuf-guide.md` §5](../notes/protobuf-guide.md).
- The thread then waits out the sample period in a loop that can also wake early on a `chan_sensor_cmd` message, so `SetInterval` and `TriggerMeasurement` take effect without a timer.

**4. Console.** The board's chosen `zephyr,console = &usart3` (PD8/PD9) is the ST-LINK Virtual COM Port, at **115200 8N1**. `scripts/console.sh` opens it.

## Driver behaviour worth knowing

- **Auto-start.** `scd4x_init` issues `START_PERIODIC_MEASUREMENT`. The app does *not* have to trigger measurement — the sensor free-runs at ~5 s.
- **Init is deferred to the app.** The node carries `zephyr,deferred-init`, so the boot sweep skips it (`kernel/init.c`) and `sensor.cpp` calls `device_init()` itself after a 100 ms wait. Reason: the chip needs up to **30 ms** after VDD to answer on I²C at all (`power_up_time`), it shares a rail with the board, and a failed init is **permanent** — `do_device_init()` sets `initialized` even on error, so `device_is_ready()` latches false; `device_init()` then returns `-EALREADY` and `device_deinit()` `-ENOTSUP` (the driver registers no deinit op). Only the *timing* of the single attempt is controllable, so we control it. Side effect: `scd4x_init` takes ~530 ms (datasheet waits after `stop_periodic_measurement` and `reinit`), now spent off the boot path — but `sensor get scd40@62` fails for the first ~600 ms.
- **Data-ready / no-block.** In periodic mode `sensor_sample_fetch()` is non-blocking and **returns 0 without updating the values when no fresh sample is ready yet** (`scd4x.c`: `if (!is_data_ready) return 0;`, skipping `scd4x_read_sample()`). So the first read(s) after boot may show stale/zero data until the sensor's first ~5 s sample lands. See *Accepted limitation* below — `SetInterval` makes this reachable at runtime, not just at boot.
- **Mode.** The `sensirion,scd40` binding has no `mode` property → always normal periodic (~5 s). Low-power (30 s) or single-shot would require the register-compatible `sensirion,scd41` compatible with a `mode = <...>` property.
- **Runtime attributes** (via `sensor_attr_set`, `#include <zephyr/drivers/sensor/scd4x.h>`): temperature offset, sensor altitude, ambient pressure, automatic self-calibration. Not used.

## Build & flash

```sh
scripts/build.sh -p     # pristine (after devicetree/Kconfig edits); plain form = incremental
scripts/flash.sh        # forces -r openocd (nucleo_h753zi defaults to the uninstalled cube runner)
scripts/console.sh      # serial console @115200; quit with Ctrl-A then K
```

Both scripts activate the workspace venv and export `ZEPHYR_BASE` themselves, so no manual `source` is needed. `build.sh` runs `west build` from inside `~/zephyr-workspace` (so west can enumerate modules) with `-s firmware -d firmware/build`.

Footprint of the whole app (sensor + networking + MQTT + protobuf + zbus + shell), as the build's own memory report gives it: **FLASH 219 252 B (10.45 % of 2 MB)**, **RAM 52 000 B (9.92 % of 512 KB)**, plus 16 KB in SRAM3. Read it off the end of a `scripts/build.sh` run rather than trusting this number — it moves with every Kconfig change.

## Bring-up checks

Wiring first: SCD-40 STEMMA QT → Nucleo — SCL→**PB8** (D15), SDA→**PB9** (D14), plus 3V3 and GND (the breakout carries its own pull-ups). Then flash and open the console.

**1. The device initialised.** Within ~2.5 s of reset the log should carry:

```
<inf> node_sensor: SCD-40 online
```

`SCD-40 not ready — publishing ERROR readings` instead means the single init attempt failed — check wiring and power before anything else, because there is no second attempt (above).

**2. The driver returns plausible numbers.** Wait ~5 s after the message above, then:

```
uart:~$ sensor get scd40@62 co2 ambient_temp humidity
```

Expect CO₂ near **400–450 ppm** in a ventilated room (the SCD-40 assumes its first reading is fresh air, ~400 ppm), a room temperature, and a plausible RH. Name the channels explicitly — a bare `sensor get` walks every channel in the enum and scrolls `-ENOTSUP` for the ones the SCD-40 lacks.

**Proves:** the overlay applied, the driver bound, I²C is wired correctly, and the CRCs pass — all below the application. This is the bisect point: a healthy reading here with absent or wrong telemetry on the Pi puts the fault at or above the zbus channel, not at the chip.

**3. Readings reach the wire.** With the harness running on the Pi (`host/.venv/bin/python host/monitor.py`), telemetry should appear every ~5 s with `SENSOR_STATUS_OK` and a `sequence` incrementing by exactly 1.

**4. Self-heating is visible.** Watch the temperature over the first few minutes: it starts slightly high and drifts down as the sensor's own heat settles. That is expected, and the temperature-offset attribute exists to correct it if accuracy ever matters here.

## Accepted limitation: a fast poll republishes the last conversion

**Known, documented, not fixed.**

The sensor free-runs at one conversion per ~5 s, and `sensor_sample_fetch()` returns **0** when no new one is ready, leaving the driver's cached values in place. `sensor_channel_get()` then hands back the *previous* reading with no indication that it is old.

`SAMPLE_PERIOD_MIN_MS` is **1000 ms**, so `command.py interval 1000` makes roughly four in five publishes silent repeats of the last conversion — a fresh `sequence` and `uptime_ms` wrapped around stale measurements, reported as `SENSOR_STATUS_OK`. Even at exactly 5000 ms the two clocks are independent and will drift into each other periodically. `TriggerMeasurement` has the same shape: it republishes the newest conversion rather than forcing a new one.

You can watch it happen: run `monitor.py`, send `command.py interval 1000`, and look for runs of consecutive readings with identical `co2`/`temp`/`rh` under advancing sequence numbers. Restore with `command.py interval 5000`.

Why it stands:

- **Single-shot is not available on this part.** Single-shot is an SCD41/SCD43 feature; the SCD40 lacks it, which is why the `sensirion,scd40` binding has no `mode` property. There is no way to command a conversion on demand.
- **The driver reports no staleness**, and the sensor API exposes no data-ready channel. The only in-API detection is comparing the three raw `sensor_value`s against the previous fetch and treating an identical triple as "no new data" — a heuristic, since a genuinely repeated reading is possible.
- **It is a device quirk, not one of this repo's learning objectives** (MQTT lifecycle, schema evolution, zbus). Carrying a heuristic through the sensor path would obscure those without teaching anything transferable.

If it ever matters: raise `SAMPLE_PERIOD_MIN_MS` to 5000 to shrink the window, or move to an SCD41/SCD43 and set `mode = <2>` for genuine single-shot.
