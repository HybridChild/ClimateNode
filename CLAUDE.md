# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**`README.md` is the complete spec** — the what, why, hardware, message set, intended layout, and references. Read it before any implementation work. This file does not restate the spec; it only covers how to *operate* in the repo.

## Status & operating constraints

- **Implementation has begun.** `firmware/` is a working Zephyr **C++** app (see `docs/language-cpp.md`): it reads the SCD-40 over I²C via the sensor API and brings up an IPv4 stack on the on-board Ethernet. `proto/` and `host/` do **not** exist yet — create them when that work starts, not up front.
- **Build / flash / console via the wrapper scripts**, not raw `west` — they source the workspace venv and pass `-s`/`-d` correctly:
  ```sh
  ./scripts/build.sh      # incremental; -p forces pristine (required after devicetree/Kconfig edits)
  ./scripts/flash.sh      # forces the openocd runner; the board's default runner isn't installed
  ./scripts/console.sh    # serial console @115200 (quit with Ctrl-A then K, not Ctrl-A D)
  ```
  The app is **freestanding**: it builds against a shared global west workspace at `~/zephyr-workspace` (**Zephyr v4.4.1**, shared with `../ImpulseZephyr`, so both are pinned to that version). Details in `docs/toolchain.md`.
- **No test/lint tooling exists.** Verification is build → flash → observe on hardware (console, `net` shell commands, or the Pi).
- **Networking is up:** static IPv4 `192.168.10.2/24`, no gateway, on a direct cable to the Pi at `192.168.10.1`. It is configured *entirely* in `firmware/prj.conf` via `CONFIG_NET_CONFIG_SETTINGS` — `net_config` applies it at boot, so no app code touches interface bring-up. Ping is verified both ways.
- **Three of the README's open decisions are resolved.** In `docs/toolchain.md`: nanopb integration = the **in-tree module**; sensor driver = the **upstream in-tree `sensirion,scd40`**. In `docs/mqtt-design.md`: **QoS per topic + the topic hierarchy** (`node/<id>/{telemetry,command,ack,status}`; telemetry QoS 0, command/ack QoS 1, retained-will status). Still open: **telemetry trigger** (5 s poll vs. data-ready). Record new decisions and their rationale in `docs/`.
- `notes/learning-roadmap.md` sequences the concepts as Phases 1–5. Phases 1 (Ethernet/IP) and 2 (TCP) are **done**; **Phase 3 (MQTT) is next** — no MQTT/TCP/socket configs are in `prj.conf` yet. (`notes/` is personal learning material; `docs/` is project documentation.)

## Working principles

- The deliverable is fluency in the patterns the README calls out — MQTT client lifecycle (connect/keepalive/reconnect, QoS, topics), zbus as the internal bus, schema versioning, and nanopb on a constrained target — not a polished product. Treat those as the real work.
- When a choice trades simplicity for fidelity to the real firmware↔SW-team contract, prefer fidelity. That is the point of the exercise.
- `proto/` is the single source of truth for the wire format; firmware and host both derive from it. Change the schema there and regenerate — never hand-edit generated code.
- **`docs/` pairs a from-first-principles teaching guide with a terse project reference** — `zephyr-build-system-guide.md` + `build-system-overview.md`, `communication-guide.md` + `mqtt-design.md`. Keep new topics to that shape and cross-link the pair: concepts explained in the guide, decisions and verified facts in the reference. `firmware-mqtt-walkthrough.md` is a third kind — a guided reading of `firmware/src/main.cpp` that connects the two; keep it in sync when the MQTT client changes. The user is learning these concepts, so **explain rather than assume** — don't take familiarity with a CLI, tool, or protocol for granted.
- Sensor access goes through Zephyr's sensor API, not raw I²C command codes. `../../shared_refs/sensor/SCD4x.yaml` documents what the driver does underneath (command codes, conversion formulas, CRC-8 params) — reach for it when debugging the sensor path.
