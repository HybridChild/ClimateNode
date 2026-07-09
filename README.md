# Ethernet + Protobuf on Zephyr — practice project

**Goal:** rehearse the *core daily interface* of the Vitrolife role — Protobuf messages published over **MQTT 3.1.1 / TCP** between a Zephyr firmware node and a host PC — and get fluent in the parts that bite in production (MQTT client lifecycle, schema versioning, nanopb on a constrained target, and zbus as the internal bus feeding the publisher). The transport was confirmed pre-start via the outgoing consultant (July 2026): **MQTT 3.1.1 over TCP, nanopb on firmware, zbus internally.** See `../../Vitrolife/role.md` and `../../Vitrolife/CLAUDE.md` for why this is the Tier-1 prep focus.

This is a **prep/practice project**, not the real product. The aim is learning the patterns, not shipping.

To make the telemetry *real* (rather than a hard-coded counter), the node reads a live sensor — an Adafruit SCD-40 CO₂/temperature/humidity breakout — over I²C and publishes those readings over MQTT. This mirrors the real product shape: a sensor-bearing node marshalling readings into Protobuf and publishing them to the host PC.

## Hardware
- **Board:** ST Nucleo-H753ZI (STM32H753ZI, Cortex-M7). Same H7 family as the product's STM32H735G.
- **Ethernet:** on-board RJ45 + LAN8742 PHY (Zephyr board target `nucleo_h753zi`, net-enabled).
- **Sensor (data source):** Adafruit SCD-40 True CO₂ / Temperature / Humidity breakout ([product 5187](https://www.adafruit.com/product/5187)).
  - Sensirion **SCD40** photoacoustic NDIR sensor on **I²C, address `0x62`**, on a STEMMA QT / Qwiic board.
  - Runs at **3.3 V** (on-board regulator + level shifters; I²C logic works directly with the Nucleo's 3.3 V).
  - CO₂ **400–2000 ppm**, accuracy **±(50 ppm + 5 % of reading)**, plus on-die temperature and humidity.
  - **Periodic measurement mode** produces a fresh sample roughly **every 5 s** — a natural, self-clocking telemetry cadence.
- **Wiring:** SCD-40 → Nucleo **I2C1** via a STEMMA QT-to-jumper cable: `SCL` → **PB8**, `SDA` → **PB9**, plus `3V3` and `GND` (4 wires). PB8/PB9 are AF4 `I2C1_SCL`/`I2C1_SDA`, broken out on the Arduino header as D15/D14. No pull-up resistors needed — the breakout has them.
- **Host:** Raspberry Pi 5 (Linux) — native Gigabit Ethernet, wired **direct-cable** to the Nucleo (no switch). Static IPs on both ends in one subnet, e.g. Pi `192.168.10.1` / Nucleo `192.168.10.2`, mask `255.255.255.0`, no gateway. The Nucleo's LAN8742 PHY has Auto-MDIX, so a normal straight-through cable works. Runs a **Mosquitto MQTT broker** plus the Python test harness (a paho-mqtt client that subscribes to telemetry and publishes commands); can stay permanently wired as a dedicated bench host.

## What "done" looks like (scope)
A Zephyr app on the Nucleo that:
1. Brings up the network interface and connects as an **MQTT client** to the broker on the Pi (keepalive/ping, reconnect on drop).
2. Reads the SCD-40 over I²C via Zephyr's **sensor API** (upstream `sensirion,scd40` / `scd4x` driver): `SENSOR_CHAN_CO2`, `SENSOR_CHAN_AMBIENT_TEMP`, `SENSOR_CHAN_HUMIDITY`.
3. **Publishes** those readings as a **Protobuf telemetry message** to a telemetry topic (~every 5 s), and **subscribes** to a command topic, answering with a **Protobuf ack** — encoding/decoding with **nanopb**. MQTT carries each message as one complete payload, so there is no app-level framing / stream reassembly.
4. Talks to a **host-side test harness** on the Raspberry Pi (Python, a paho-mqtt client) that decodes and logs the telemetry stream and can publish commands. *(Optional higher-fidelity pass: re-run the harness in C#/.NET on a Windows box later to mirror the real Windows app side.)*

## The things to actually learn (don't skip these)
1. **MQTT client on Zephyr** — connect/keepalive, QoS levels, topic design (telemetry vs. command topics), and especially **reconnect handling** when the link drops. Uses Zephyr's `CONFIG_MQTT_LIB`. (MQTT frames and delimits messages itself, so the length-prefix / partial-read problem of raw TCP goes away — each payload arrives whole.)
2. **zbus as the internal bus** — mirror the real firmware's "zbus inden MQTT": the sensor thread publishes readings to a **zbus channel**; a separate MQTT-publisher observer subscribes to that channel and marshals to nanopb → MQTT publish. Decouples sensing from transport, exactly like the product.
3. **Schema versioning & backward-compat** — exercise adding a field and talking old↔new: field numbers, `optional`, unknown-field handling. This is the firmware↔SW-team contract in miniature.
4. **nanopb on a constrained target** — `.proto` → generated C, `.options` files, fixed-size vs callback fields, no-malloc/static allocation.

**Bonus learning from the sensor:** the SCD-40 adds a clean rehearsal of the **Zephyr sensor subsystem + a devicetree I²C overlay** — wiring a real driver instance in a board overlay, reading channels with `sensor_sample_fetch` / `sensor_channel_get`, and turning `struct sensor_value` into wire fields. That's a common shape in the real firmware.

## Message set (starting point)
Small but realistic — enough to feel like the real node↔PC protocol:
- **`Telemetry`** (node → host, periodic): `co2_ppm`, `temperature_c`, `humidity_rh`, plus a `sequence`/`uptime_ms` and a `sensor_status`/validity flag. Fixed-size fields — no dynamic allocation.
- **`Command`** (host → node) + **`Ack`** (node → host): e.g. set measurement interval, trigger a single-shot measurement, or request device/firmware info. Exercises the request/response half and gives something concrete to version.

Suggested topics: `node/<id>/telemetry`, `node/<id>/command`, `node/<id>/ack`. The SCD-40's ~5 s sample rate defines the natural telemetry period; the command path lets the host change or force it.

## Proposed structure (to be filled in later)
- `proto/` — the `.proto` schema (shared contract) + nanopb `.options`.
- `firmware/` — Zephyr application (`prj.conf`, `CMakeLists.txt`, `src/`, and a **board overlay** defining the I²C bus + `sensirion,scd40` node).
- `host/` — host test harness: a **Mosquitto** broker config + a Python **paho-mqtt** client that encodes/decodes Protobuf, subscribes to telemetry, and publishes commands.
- `docs/` — notes: MQTT topic/QoS decisions, versioning experiments, sensor/overlay setup, gotchas.

## Open decisions (resolve before coding)
- Transport: **MQTT 3.1.1 over TCP** (confirmed to match the product). Broker = **Mosquitto on the Pi**. Decide QoS per topic (0 vs. 1 for telemetry vs. commands), the topic hierarchy, and MQTT keepalive / reconnect strategy. (Raw-TCP framing and gRPC are both out — the real product uses MQTT.)
- nanopb integration path on Zephyr (module vs. vendored generator step).
- Sensor driver source: upstream Zephyr `sensirion,scd4x` driver vs. a community module — confirm which the in-tree board/Zephyr version ships.
- Telemetry trigger: poll on a ~5 s timer vs. the SCD-40 data-ready signal.

## Status
Spec only — **no code yet.** This README is the spec; `CLAUDE.md` covers how to work in the repo once implementation begins.

## References
Local PDFs live in [`../../Datasheets/sensor/Adafruit_SCD40/`](../../Datasheets/sensor/Adafruit_SCD40/).

**Board (Adafruit SCD-40 breakout, product 5187):**
- [SCD-40 / SCD-41 learn guide (PDF)](../../Datasheets/sensor/Adafruit_SCD40/adafruit-scd-40-and-scd-41.pdf) — pinouts, wiring, and board schematic (at the end).
- [Product page](https://www.adafruit.com/product/5187) — specs, pricing, overview.
- [Downloads page](https://learn.adafruit.com/adafruit-scd-40-and-scd-41/downloads) — schematic + Fab print (labeled SCD-41; the SCD-40 board is identical).

**Sensor (Sensirion SCD4x) — the reference for firmware/driver work:**
- [`SCD4x.yaml`](../../shared_refs/sensor/SCD4x.yaml) — **structured digest of the datasheet (v1.7)**, and the quickest thing to reach for while coding: I²C command codes (`start_periodic_measurement` `0x21b1`, `read_measurement` `0xec05`, …), the raw-word → CO₂/T/RH conversion formulas, CRC-8 params (poly `0x31`, init `0xFF`, no reflection), execution times, and the SCD40/41/43 variant/accuracy tables. Mostly what the Zephyr driver does under the hood, but essential for understanding and debugging it.
- [SCD4x datasheet (PDF)](../../Datasheets/sensor/Adafruit_SCD40/Sensirion_SCD4x_Datasheet.pdf) — the full source document behind the YAML (v1.7, April 2025).

**Zephyr:**
- [MQTT client library](https://docs.zephyrproject.org/latest/connectivity/networking/api/mqtt.html) — `CONFIG_MQTT_LIB`; connect / publish / subscribe / keepalive API. The core of the transport rehearsal.
- [zbus](https://docs.zephyrproject.org/latest/services/zbus/index.html) — in-process message bus (channels / observers) for the internal sensor → publisher path.
- [`sensirion,scd40` devicetree binding](https://docs.zephyrproject.org/latest/build/dts/api/bindings/sensor/sensirion,scd40.html) — I²C node properties for the overlay.
- [`sensirion,scd41` devicetree binding](https://docs.zephyrproject.org/latest/build/dts/api/bindings/sensor/sensirion,scd41.html) — sibling variant, for reference.
- [nobodyguy/sensirion_zephyr_drivers](https://github.com/nobodyguy/sensirion_zephyr_drivers) — community SCD4x/SCD30 driver module, fallback if the in-tree driver isn't available.
