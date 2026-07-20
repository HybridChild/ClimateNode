# ClimateNode

**A Zephyr CO₂/temperature/humidity sensor node that publishes Protobuf telemetry over MQTT to a host PC.**

**Goal:** rehearse the *core daily interface* of production sensor firmware — Protobuf messages published over **MQTT 3.1.1 / TCP** between a Zephyr firmware node and a host PC — and get fluent in the parts that bite in production (MQTT client lifecycle, schema versioning, nanopb on a constrained target, and zbus as the internal bus feeding the publisher). The stack is fixed up front: **MQTT 3.1.1 over TCP, nanopb on firmware, zbus internally.**

This is a **practice project**, not a shipping product. The aim is learning the patterns.

To make the telemetry *real* (rather than a hard-coded counter), the node reads a live sensor — an Adafruit SCD-40 CO₂/temperature/humidity breakout — over I²C and publishes those readings over MQTT. That mirrors the shape of a production node: a sensor-bearing device marshalling readings into Protobuf and publishing them to the host PC.

## Hardware
- **Board:** ST Nucleo-H753ZI (STM32H753ZI, Cortex-M7) — an H7-class part of the kind commonly used in production sensor nodes.
- **Ethernet:** on-board RJ45 + LAN8742 PHY (Zephyr board target `nucleo_h753zi`, net-enabled).
- **Sensor (data source):** Adafruit SCD-40 True CO₂ / Temperature / Humidity breakout ([product 5187](https://www.adafruit.com/product/5187)).
  - Sensirion **SCD40** photoacoustic NDIR sensor on **I²C, address `0x62`**, on a STEMMA QT / Qwiic board.
  - Runs at **3.3 V** (on-board regulator + level shifters; I²C logic works directly with the Nucleo's 3.3 V).
  - CO₂ **400–2000 ppm**, accuracy **±(50 ppm + 5 % of reading)**, plus on-die temperature and humidity.
  - **Periodic measurement mode** produces a fresh sample roughly **every 5 s** — a natural, self-clocking telemetry cadence.
- **Wiring:** SCD-40 → Nucleo **I2C1** via a STEMMA QT-to-jumper cable: `SCL` → **PB8**, `SDA` → **PB9**, plus `3V3` and `GND` (4 wires). PB8/PB9 are AF4 `I2C1_SCL`/`I2C1_SDA`, broken out on the Arduino header as D15/D14. No pull-up resistors needed — the breakout has them.
- **Host:** Raspberry Pi 5 (Linux) — native Gigabit Ethernet, wired **direct-cable** to the Nucleo (no switch). Static IPs on both ends in one subnet: Pi `192.168.10.1` / Nucleo `192.168.10.2`, mask `255.255.255.0`, no gateway. On the firmware side this is configured entirely in `firmware/prj.conf` via `CONFIG_NET_CONFIG_SETTINGS` — `net_config` applies it at boot, so **no application code touches interface bring-up**. The Nucleo's LAN8742 PHY has Auto-MDIX, so a normal straight-through cable works. Runs a **Mosquitto MQTT broker** plus the Python test harness (a paho-mqtt client that subscribes to telemetry and publishes commands); can stay permanently wired as a dedicated bench host.

## Scope and status
A Zephyr app on the Nucleo — written in **C++ (C++17)** to match how production firmware of this kind is written; see [`docs/language-cpp.md`](docs/language-cpp.md).

**Implemented** (`firmware/src/main.cpp`, walked through in [`docs/firmware-mqtt-walkthrough.md`](docs/firmware-mqtt-walkthrough.md)):
1. Brings up the network interface and connects as an **MQTT client** to the broker on the Pi. Reconnect is the shape of the program, not error handling bolted on: a forever loop of connect → serve until dropped → back off (1 s doubling to 30 s) → retry, so a cable pull or a downed broker is survivable.
2. Reads the SCD-40 over I²C via Zephyr's **sensor API** (upstream `sensirion,scd40` driver): `SENSOR_CHAN_CO2`, `SENSOR_CHAN_AMBIENT_TEMP`, `SENSOR_CHAN_HUMIDITY`. A failed read still publishes, carrying `sensor_status = ERROR` rather than silently going quiet.
3. **Publishes** those readings as **Protobuf `Telemetry`** (default every 5 s) and **subscribes** to the command topic, answering every `Command` with a **Protobuf `Ack`** — encoded/decoded with **nanopb**. `SetInterval` retunes the publish period at runtime (bounded to 1 s–300 s), `TriggerMeasurement` forces one, `GetDeviceInfo` returns firmware/board/client id. MQTT carries each message as one complete payload, so there is no app-level framing / stream reassembly.
4. Announces liveness on a **retained `status` topic** — `online` on connect, `offline` published by the broker via the Last Will if the node drops without a clean DISCONNECT.
5. Talks to a **host-side test harness** on the Raspberry Pi (Python, paho-mqtt) that decodes and logs the telemetry stream and can publish commands.

**Verification status.** All of the above is confirmed on the bench (2026-07-20): connect,
reconnect with backoff, QoS 0 telemetry decoded by the host, QoS 1 commands with every
branch exercised, an out-of-range `SetInterval` rejected rather than silently clamped, a
redelivered command acknowledged but not re-executed, and a malformed payload answered
with `MALFORMED` without desynchronising the MQTT stream. The node also ran ~14 hours
unattended without a gap in the sequence counter.

**Still to do:**
- **zbus** — the sensor read and the MQTT publish are still in one loop; the point is to split them across a zbus channel (see the learning goals below).
- **Schema-versioning exercise** — add a field and deliberately run old↔new against each other.
- *(Optional higher-fidelity pass: re-run the harness in C#/.NET on a Windows box to mirror a Windows-side desktop application.)*

## The things to actually learn (don't skip these)
1. **MQTT client on Zephyr** — connect/keepalive, QoS levels, topic design (telemetry vs. command topics), and especially **reconnect handling** when the link drops. Uses Zephyr's `CONFIG_MQTT_LIB`. (MQTT frames and delimits messages itself, so the length-prefix / partial-read problem of raw TCP goes away — each payload arrives whole.)
2. **zbus as the internal bus** — the sensor thread publishes readings to a **zbus channel**; a separate MQTT-publisher observer subscribes to that channel and marshals to nanopb → MQTT publish. Decouples sensing from transport, the way production firmware does.
3. **Schema versioning & backward-compat** — exercise adding a field and talking old↔new: field numbers, `optional`, unknown-field handling. This is the firmware↔SW-team contract in miniature.
4. **nanopb on a constrained target** — `.proto` → generated C, `.options` files, fixed-size vs callback fields, no-malloc/static allocation.

**Bonus learning from the sensor:** the SCD-40 adds a clean rehearsal of the **Zephyr sensor subsystem + a devicetree I²C overlay** — wiring a real driver instance in a board overlay, reading channels with `sensor_sample_fetch` / `sensor_channel_get`, and turning `struct sensor_value` into wire fields. That's a common shape in production firmware.

## Message set
Three messages, defined in **[`proto/node.proto`](proto/node.proto)** — read that file for the fields, the field-number budget, and the evolution rules; it is the contract, and this summary will drift if it tries to restate it.

- **`Telemetry`** (node → host, periodic) — the sensor readings, plus a `sequence` so the host can spot QoS 0 drops, an `uptime_ms`, and a `sensor_status` distinguishing a warming-up sensor from a failed read. Fixed-size fields only, no dynamic allocation.
- **`Command`** (host → node) — a `oneof` payload: set the measurement interval, trigger a single-shot measurement, or request device info. The `oneof` is the part worth studying; nanopb turns it into a tagged union with a `which_payload` discriminator.
- **`Ack`** (node → host) — echoes the command's `sequence` for correlation and reports an `AckStatus`. That enum carries the interesting cases: `UNSUPPORTED` (host newer than the node) and `MALFORMED` (didn't decode at all) are what make the versioning exercise concrete.

All three carry a `schema_version`, bumped only on a *breaking* change — additive changes don't touch it, because Protobuf already handles those.

Topics are `node/<id>/{telemetry,command,ack,status}` — telemetry at QoS 0, command/ack at QoS 1, and `status` a retained last-will carrying plain ASCII `online`/`offline`. The SCD-40's ~5 s sample rate defines the natural telemetry period; the command path lets the host change or force it. See [`docs/mqtt-design.md`](docs/mqtt-design.md) for the QoS rationale and the session/keepalive/will settings.

## Repository layout
- `proto/` — the `.proto` schema (shared contract) + nanopb `.options`. **The single source of truth for the wire format**; firmware and host both generate from it. Generated `*.pb.c/.h` are **C** and stay C even though the firmware is C++ (they're included across the C↔C++ boundary; see [`docs/language-cpp.md`](docs/language-cpp.md)).
- `firmware/` — Zephyr application in **C++17** (`prj.conf` with `CONFIG_CPP=y`, `CMakeLists.txt`, `src/*.cpp`, and a **board overlay** defining the I²C bus + `sensirion,scd40` node). Generates `node.pb.c/.h` at build time so it can't drift from the schema.
- `host/` — host test harness on the Pi: a Python **paho-mqtt** monitor and command client that encode/decode Protobuf, plus `generate.sh` for the Python bindings.
- `scripts/` — the build/flash/console wrappers. Use these rather than raw `west`; they source the workspace venv and pass the right source/build directories.
- `docs/` — project documentation: each topic pairs a from-first-principles guide with a terse reference (e.g. `communication-guide.md` + `mqtt-design.md`).
- `notes/` — personal learning material, including the phased roadmap.

## Getting started

**Firmware** — the app is *freestanding*: it builds against a shared global west workspace at `~/zephyr-workspace` (**Zephyr v4.4.1**). Use the wrappers rather than raw `west`; they source the workspace venv and pass the source/build directories correctly. See [`docs/toolchain.md`](docs/toolchain.md).

```sh
./scripts/build.sh        # incremental; -p forces a pristine build (required after devicetree/Kconfig edits)
./scripts/flash.sh        # forces the openocd runner; the board's default runner isn't installed
./scripts/console.sh      # serial console @115200 (quit with Ctrl-A then K)
```

**Host** — runs on the Pi, which also hosts the Mosquitto broker. The venv is deliberately separate from the Zephyr workspace venv (see `host/requirements.txt` for why):

```sh
python3 -m venv host/.venv
host/.venv/bin/pip install -r host/requirements.txt
host/generate.sh                                    # regenerate node_pb2.py after any proto/ change

host/.venv/bin/python host/monitor.py               # decode and log the telemetry stream
host/.venv/bin/python host/command.py info          # send GetDeviceInfo, await the Ack
host/.venv/bin/python host/command.py trigger       # force a single measurement
host/.venv/bin/python host/command.py interval 2000 # retune the publish period (ms)
```

There is no test or lint tooling: verification is build → flash → observe, via the console, Zephyr's `net` shell commands, or the host harness.

## Documentation
`docs/` pairs a from-first-principles **guide** with a terse **reference** per topic — concepts in the guide, decisions and verified facts in the reference.

| Topic | Guide | Reference |
|---|---|---|
| Communication (MQTT, QoS, topics) | [`communication-guide.md`](docs/communication-guide.md) | [`mqtt-design.md`](docs/mqtt-design.md) |
| Zephyr build system | [`zephyr-build-system-guide.md`](docs/zephyr-build-system-guide.md) | [`build-system-overview.md`](docs/build-system-overview.md) |

Standalone: [`firmware-mqtt-walkthrough.md`](docs/firmware-mqtt-walkthrough.md) — a guided reading of `firmware/src/main.cpp` connecting the two · [`language-cpp.md`](docs/language-cpp.md) — why C++17, and the C↔C++ boundary · [`toolchain.md`](docs/toolchain.md) — workspace layout, build/flash workflow, verified facts · [`sensor-bringup.md`](docs/sensor-bringup.md) — SCD-40 wiring and devicetree overlay · [`out-of-tree-hardware-overview.md`](docs/out-of-tree-hardware-overview.md).

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
- [nobodyguy/sensirion_zephyr_drivers](https://github.com/nobodyguy/sensirion_zephyr_drivers) — community SCD4x/SCD30 driver module. Not used; the in-tree driver ships with Zephyr v4.4.1. Kept as a road not taken.
