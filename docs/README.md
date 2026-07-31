# The references

Terse, project-specific documents recording what *this* build decided and why — the CO₂ node on a Nucleo-H753ZI talking to a Raspberry Pi. They are consulted, not read start to finish: jump in to check *which QoS did telemetry get*, *how does the static IP get set with no app code*, *why is the sensor deferred-init* — and leave.

If you want the **concept** rather than the decision, you are in the wrong folder. Each reference here pairs with a from-first-principles teaching guide in [`../notes/`](../notes/); start there if MQTT or devicetree or the sensor API is new to you, and come here for the facts once it isn't. [`../notes/README.md`](../notes/README.md) is the map of the guides, including the order to read them in.

## The pairings

Each row is a reference here and the guide it points back to. The guide teaches the concept; the reference records what we built.

| Reference (here) | Records | Teaching guide (`../notes/`) |
|---|---|---|
| [`build-system-overview.md`](build-system-overview.md) | Which build input produces which generated file, consumed by what — checked against a real `gateway/build/` | [`zephyr-build-system-guide.md`](../notes/zephyr-build-system-guide.md) |
| [`sensor-bringup.md`](sensor-bringup.md) | How the SCD-40 is wired, described, initialised and read on this bench; the deferred-init and poll-cadence decisions | [`sensor-api-guide.md`](../notes/sensor-api-guide.md) |
| [`mqtt-design.md`](mqtt-design.md) | The topic hierarchy, per-topic QoS, and retained-will status — the Nucleo ↔ Pi link, starting at the socket | [`communication-guide.md`](../notes/communication-guide.md) |
| [`network-bringup.md`](network-bringup.md) | How Ethernet comes up with no app code: the LAN8742 PHY, the STM32H7 MAC, the static IPv4 address | [`network-stack-guide.md`](../notes/network-stack-guide.md) |
| [`can-bringup.md`](can-bringup.md) | How the FDCAN controller is clocked and configured, why the bitrate must be stated explicitly, and the filter behaviour that makes `can dump` lie | [`can-guide.md`](../notes/can-guide.md) |
| [`test-strategy.md`](test-strategy.md) | What this repo tests, where, and what it deliberately leaves to the bench | [`testing-guide.md`](../notes/testing-guide.md) |

Two guides pair with a **source file** instead of a page here, because there the decision belongs next to the thing it constrains: `protobuf-guide.md` with [`../proto/node.proto`](../proto/node.proto) (the field-numbering rules), and `zbus-guide.md` with the [`app_channels.h`](../shared/app_channels.h) header comment (the observer-kind choice).

## The two without a guide

- [`toolchain.md`](toolchain.md) — the two toolchains (Mac firmware, Pi host) and the build/flash/console workflow. Pure setup; there is no concept to teach, so it has no guide half.
- [`firmware-mqtt-walkthrough.md`](firmware-mqtt-walkthrough.md) — a guided reading of *this repo's* `gateway/src/main.cpp`. It teaches, so by kind it looks like a `../notes/` guide — but it tracks one file's code rather than a portable concept, so it lives here with the references. It is the one page that connects most of these concepts in a single source file; a good place to land once the guides have done their work.
