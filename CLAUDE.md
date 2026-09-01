# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**`README.md` is the overview** — the what, why, the system diagram, the hardware table, the message set and the layout, each in brief. Read it first for orientation, then follow it down: the detail it delegates — wiring and bring-up, the QoS and schema rationale, the toolchain — lives in `docs/`, and [`docs/README.md`](docs/README.md) maps where. The `.proto` schema remains the single source of truth for the wire format, whatever any prose says. This file restates neither; it covers how to *operate* in the repo.

## What this is

Two Zephyr C++17 applications and a Python host harness, all working end to end on hardware.

- **`gateway/`** — a Nucleo-H753ZI that is both a sensor node and a CAN-to-MQTT relay. Four threads across five translation units, one responsibility each: `sensor.cpp` (the SCD-40 over I²C), `protocol.cpp` (the wire format — the only place internal types meet nanopb), `commands.cpp` (dispatch, duplicate suppression, node identity), `relay.cpp` (the CAN side, two threads) and `main.cpp` (two MQTT sessions as a state machine over one poll loop). It publishes its own telemetry to `node/1/…` and the peer's, relayed without decoding, to `node/2/…`.
- **`peer-node/`** — a Nucleo-F072RB with a BME280, reached only over CAN. 16 KB of RAM, so every Kconfig option in its `prj.conf` is justified in a comment. Its own files are just `main.cpp` (the CAN session) and `sensor.cpp`.
- **`shared/`** — the code both apps link: `protocol.{h,cpp}`, `commands.{h,cpp}`, `app_channels.h` (the zbus channels), `can_link.h` (the CAN address map and heartbeat frame). Three consumers — `gateway/`, `peer-node/`, `tests/` — each compiling these by relative path via a `SHARED` variable in its `CMakeLists.txt`.
- **`proto/`** the schema, **`host/`** the paho harness (`monitor.py`, `command.py`), which runs **on the Pi** — the only machine that can reach the broker.

**When you add a file, the question is how many apps link it.** Two means `shared/`; one means that app. That is why `relay.{h,cpp}` stays in `gateway/src/` (only the gateway relays) and why each app keeps its own `main.cpp` and `sensor.cpp`. Nothing in `shared/` may depend on an app, and no app may depend on another. **Deliberately not a `zephyr_library`:** the generated `node.pb.h` that `protocol.h` and `commands.h` include is attached to each app's own `zephyr_nanopb_sources(app …)` target, so a library would have to re-create that plumbing for no gain.

## Build, flash, test

Use the wrapper scripts, not raw `west` — they source the workspace venv and pass `-s`/`-d` correctly. **All take `-a <app>` (or `APP=`) and default to `gateway`**; the board follows from the app, so `-b` is never needed.

```sh
./scripts/build.sh                   # incremental; -p forces pristine (required after devicetree/Kconfig edits)
./scripts/build.sh -a peer-node -p   # the F072RB peer node
./scripts/flash.sh -a peer-node      # forces the openocd runner; the board's default runner isn't installed
./scripts/console.sh -a peer-node    # serial console @115200 (quit with Ctrl-A then K, not Ctrl-A D)
./scripts/build.sh -a peer-node --debug -p   # + a trimmed shell on the peer (bench only)
./scripts/probe.sh                   # which ST-LINK and which /dev/cu.* belongs to which app
./scripts/test.sh                    # 51 Ztest cases on qemu_cortex_m3, ~31 s, no board needed
./scripts/cleanup.sh                 # remove build dirs; -a for one app, --all for every generated file
```

The apps are **freestanding**: they build against a shared global west workspace at `~/zephyr-workspace` (**Zephyr v4.4.1**). Details in `docs/toolchain.md`.

**With both Nucleos attached, always say which one.** `console.sh` refuses to guess and prints `probe.sh`'s table; `flash.sh` resolves the app's probe through `scripts/probes.conf` and passes `--serial`, and **stops rather than flashing a board at random** if it cannot — losing that coin flip writes the wrong image to the wrong part and reports success. `STLINK_SERIAL=` overrides. The mapping is keyed on the ST-LINK serial, not the `usbmodemNNNNNN` name, which moves with the USB port. With one board attached none of this engages.

**The peer node's console is output-only by default** — no shell, because a stock one takes 95 % of its 16 KB. `--debug` merges `peer-node/debug.conf` for a trimmed shell with the `can` commands (83 % RAM); it changes Kconfig, so pair it with `-p`. `CONFIG_SENSOR_SHELL` will not fit at all.

`./scripts/test.sh` covers `tests/protocol/` (the wire format), `tests/commands/` (dispatch, validator rejection, duplicate suppression), `tests/heartbeat/` (the hand-packed CAN frame and address map), `tests/relay/` (the liveness state machine) and `tests/isotp_loopback/` (the CAN transport through an emulated controller). Run them before and after touching `protocol.cpp`, `commands.cpp`, `can_link.h` or `relay.h`. There is no lint tooling.

Everything below the socket needs the bench: build → flash → observe, via the console, `net` shell commands, or the Pi. The procedures are the **lab at the end of each guide** — `communication-guide.md` §9, `network-stack-guide.md` §8, `can-guide.md` §10, `zbus-guide.md` §10, `protobuf-guide.md` §12, `sensor-api-guide.md` §11, `shell-guide.md` §9, `zephyr-build-system-guide.md` §12, `language-cpp.md` §12 — plus *Testing the Last Will* in `mqtt-design.md` and *Bring-up checks* in both `sensor-bringup.md` and `can-bringup.md`.

## Sharp edges

Four things that are silent when violated. The mechanism behind each, what enforces it, and the page that treats it fully are in [`docs/invariants.md`](docs/invariants.md), which also carries the contracts these four are the operational tip of — read it before touching the CAN link, the zbus channels, or the schema.

- **`can_send()` with a NULL callback blocks with no bound**, and the timeout argument does not change that. Pass a callback in anything that must stay responsive, as `peer-node/src/main.cpp` does.
- **ISO-TP flow control needs identifiers of its own.** Two contexts on one node must never share an identifier; the loser is starved with no diagnostic.
- **The zbus pool is sized by every channel, not every message subscriber.** Adding a channel or growing a message means re-checking the `static_assert`s in `shared/app_channels.h` and `gateway/src/relay.h`.
- **The SCD-40 republishes on a fast poll.** Read *Accepted limitation* in `docs/sensor-bringup.md` before touching `SAMPLE_PERIOD_MIN_MS` or believing a sub-5 s cadence.

## Documentation

**Split by kind, not by topic: guides live in `notes/`, references in `docs/`.** A topic normally has one of each, cross-linked both ways.

- **`notes/` — from-first-principles teaching guides**, general concepts largely portable beyond this repo. `notes/README.md` is their index: reading order, per-guide prerequisites, and the skeleton every guide follows. Keep it current when adding or renaming a guide.
- **`docs/` — terse project references**: decisions, rationale, and verified facts. **`docs/README.md` is the map of every guide↔reference pairing and of the topics that deviate from the pattern** — consult it before assuming where something lives, and keep it current when adding a page.
- **`docs/firmware-mqtt-walkthrough.md` tracks `gateway/src/main.cpp` line by line** — keep it in sync when the client changes. Why a teaching document lives with the references is in `docs/README.md`.
- Relative links cross the directory boundary (`../docs/…` from a guide, `../notes/…` from a reference).

Three rules about what goes in it:

- **Documentation describes the project as it is, not how it got there.** No development narrative, no "this used to be X", no dated reports of past runs. Explaining *why* the code is shaped as it is, including reasons discovered the hard way, is right — state the constraint and the mechanism, not the story.
- **Write bench results as a procedure someone can re-run** — commands, expected output, and what it proves. Write one only for what has actually been run, and update the lab that covers a behaviour when the behaviour changes.
- **The reader is learning these concepts, so explain rather than assume** — don't take familiarity with a CLI, tool, or protocol for granted.

## Working principles

- The deliverable is fluency in the patterns the README calls out — MQTT client lifecycle, zbus as the internal bus, schema versioning, nanopb on a constrained target — not a polished product.
- When a choice trades simplicity for fidelity to the real firmware↔SW-team contract, prefer fidelity. That is the point of the exercise.
- `proto/` is the single source of truth for the wire format; firmware and host both derive from it. Change the schema there and regenerate — never hand-edit generated code.
- Sensor access goes through Zephyr's sensor API, not raw I²C command codes. What the driver does underneath is documented inline in `notes/sensor-api-guide.md` and `docs/sensor-bringup.md`.
- Decisions and their rationale are recorded in `docs/` — read the relevant reference before reopening one.
