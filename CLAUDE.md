# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**`README.md` is the complete spec** — the what, why, hardware, message set, intended layout, and references. Read it before any implementation work. This file does not restate the spec; it only covers how to *operate* in the repo.

## Status & operating constraints

- **The core path works end to end, verified on hardware 2026-07-20.** `firmware/` is a working Zephyr **C++** app (see `docs/language-cpp.md`): it reads the SCD-40 over I²C via the sensor API, brings up IPv4 on the on-board Ethernet, and runs an MQTT client that publishes nanopb-encoded `Telemetry` and answers `Command`s with `Ack`s. `proto/` holds the schema; `host/` holds the Python paho harness (`monitor.py`, `command.py`) — run it **on the Pi**, which is the only machine that can reach the broker. Verified against the bench: telemetry decode, all three commands, out-of-range rejection, duplicate suppression, and a malformed payload that did not desync the stream. What remains is **zbus** (splitting the sensor read from the publish, which today share one loop) and the **schema-versioning exercise**.
- **Build / flash / console via the wrapper scripts**, not raw `west` — they source the workspace venv and pass `-s`/`-d` correctly:
  ```sh
  ./scripts/build.sh      # incremental; -p forces pristine (required after devicetree/Kconfig edits)
  ./scripts/flash.sh      # forces the openocd runner; the board's default runner isn't installed
  ./scripts/console.sh    # serial console @115200 (quit with Ctrl-A then K, not Ctrl-A D)
  ```
  The app is **freestanding**: it builds against a shared global west workspace at `~/zephyr-workspace` (**Zephyr v4.4.1**, shared with `../ImpulseZephyr`, so both are pinned to that version). Details in `docs/toolchain.md`.
- **No test/lint tooling exists.** Verification is build → flash → observe on hardware (console, `net` shell commands, or the Pi).
- **Networking is up:** static IPv4 `192.168.10.2/24`, no gateway, on a direct cable to the Pi at `192.168.10.1`. It is configured *entirely* in `firmware/prj.conf` via `CONFIG_NET_CONFIG_SETTINGS` — `net_config` applies it at boot, so no app code touches interface bring-up. Ping is verified both ways.
- **Decisions already made** (don't reopen without reason). In `docs/toolchain.md`: nanopb integration = the **in-tree module**; sensor driver = the **upstream in-tree `sensirion,scd40`**. In `docs/mqtt-design.md`: **QoS per topic + the topic hierarchy** (`node/<id>/{telemetry,command,ack,status}`; telemetry QoS 0, command/ack QoS 1, retained-will status). Still open: **telemetry trigger** — currently a timed poll (default 5 s, retunable via `SetInterval`); the SCD-40 data-ready signal is the alternative. Record new decisions and their rationale in `docs/`.
- `notes/learning-roadmap.md` sequences the concepts as Phases 1–5. Phases 1–4 (Ethernet/IP, TCP, MQTT, Protobuf/nanopb) are **done and verified on hardware**; **Phase 5 (zbus) is next**. (`notes/` is personal learning material; `docs/` is project documentation.)

## Working principles

- The deliverable is fluency in the patterns the README calls out — MQTT client lifecycle (connect/keepalive/reconnect, QoS, topics), zbus as the internal bus, schema versioning, and nanopb on a constrained target — not a polished product. Treat those as the real work.
- When a choice trades simplicity for fidelity to the real firmware↔SW-team contract, prefer fidelity. That is the point of the exercise.
- `proto/` is the single source of truth for the wire format; firmware and host both derive from it. Change the schema there and regenerate — never hand-edit generated code.
- **`docs/` pairs a from-first-principles teaching guide with a terse project reference** — `zephyr-build-system-guide.md` + `build-system-overview.md`, `communication-guide.md` + `mqtt-design.md`. Keep new topics to that shape and cross-link the pair: concepts explained in the guide, decisions and verified facts in the reference. `firmware-mqtt-walkthrough.md` is a third kind — a guided reading of `firmware/src/main.cpp` that connects the two; keep it in sync when the MQTT client changes. The user is learning these concepts, so **explain rather than assume** — don't take familiarity with a CLI, tool, or protocol for granted.
- Sensor access goes through Zephyr's sensor API, not raw I²C command codes. `../../shared_refs/sensor/SCD4x.yaml` documents what the driver does underneath (command codes, conversion formulas, CRC-8 params) — reach for it when debugging the sensor path.
