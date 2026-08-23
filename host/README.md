# The host harness

Two small MQTT clients and the code generator that feeds them. This is the *other side* of the contract the firmware implements: `monitor.py` decodes everything the nodes say, `command.py` sends a `Command` and waits for its `Ack`. Neither is a product — they exist so the wire format has a second, independent implementation, and a payload that decodes here is a payload firmware and host genuinely agree on.

Both derive from [`../proto/node.proto`](../proto/node.proto), the single source of truth, via `generate.sh`. The firmware derives from the same file through nanopb. That is what makes "shared schema" mechanical rather than aspirational.

## This runs on the Pi

Mosquitto binds `192.168.10.1`, an address that exists only on the direct cable to the Nucleo. That is deliberate: an `allow_anonymous` broker stays on the bench cable and off the home LAN, where the Pi is `192.168.1.105` over WiFi. Nothing on the WiFi side can reach the broker, **including your Mac** — so these tools are run over SSH (`ssh pi@pi5.local`), never locally. Rationale and the broker config are in [`../docs/mqtt-design.md`](../docs/mqtt-design.md).

Before anything else, check the broker is actually up — it can lose a boot race with NetworkManager, which `mqtt-design.md` covers:

```sh
systemctl is-active mosquitto        # "active"
ss -tlnp | grep 1883                 # listener bound to 192.168.10.1:1883
```

## Setup, once

Every command here is run from the repo root, on the Pi. Raspberry Pi OS is PEP 668, so the venv is mandatory rather than a nicety. It is also deliberately **separate from the Zephyr workspace venv**, which drives the nanopb generator — see the comment at the top of [`requirements.txt`](requirements.txt).

```sh
python3 -m venv host/.venv
host/.venv/bin/pip install -r host/requirements.txt
./host/generate.sh                   # -> host/node_pb2.py
```

Both halves installed correctly if this exits silently:

```sh
( cd host && .venv/bin/python -c "import node_pb2, paho.mqtt.client" )
```

**The `cd` is load-bearing.** `node_pb2.py` sits in `host/` and nothing installs it into the venv, so it is importable only when `host/` is on `sys.path`. Running `host/.venv/bin/python host/monitor.py` puts it there automatically — Python prepends the *script's* directory — which is why the tools work from the repo root. A bare `python -c` gets the current directory instead, so the same import fails from anywhere but `host/`.

`generate.sh` goes through `python -m grpc_tools.protoc` rather than a system `protoc`, because Python's generated modules embed the compiler version and validate it at import — a newer runtime reads older gencode, never the reverse. Bundling both halves in one pinned package keeps them in lockstep. Its output, `node_pb2.py`, is generated code: gitignored, never hand-edited, and regenerated after **every** change to `proto/node.proto`.

**Regenerate from a `node.proto` that is actually current.** A schema change is edited and built on the Mac; nothing pushes it here, and `generate.sh` reads whatever this checkout happens to hold. Against a stale copy it succeeds quite happily and emits bindings for the previous schema, which then fail on every payload the freshly-flashed node sends — `DECODE FAILED` on all topics at once, looking exactly like a firmware bug. Update this checkout first, then generate, then compare against the board.

## `monitor.py` — read everything the nodes say

```sh
host/.venv/bin/python host/monitor.py          # Ctrl-C to quit
```

Subscribes to `node/#` at QoS 1 and pretty-prints each payload, so every node and every topic land in one window:

```
14:02:11  node/1/status        [retained] online
14:02:16  node/1/telemetry     seq=1042  co2=  812 ppm  temp=22.41 C  rh=41.3 %  p=    -- Pa  up=  5210.4s  SENSOR_STATUS_OK  (schema v1)
```

Four things to know when reading that output:

- **`[retained]`** means the broker replayed a stored message rather than one just published — you see it on `status` the instant you subscribe, whenever the node connected.
- **`status` is plain ASCII**, alone among the topics, because the *broker* writes it as the node's Last Will. Firmware is not running at that moment, so it cannot be Protobuf-encoded.
- **`--` in a measurement column means the field is absent, not zero.** The `Telemetry` measurements are proto3 `optional`, so a node without that sensor omits it; printing an unconditional `0` would turn "no CO₂ sensor here" into a confident "0 ppm". The gateway reports `co2/temp/rh`, the peer node `temp/rh/p`.
- **`DECODE FAILED (...) raw=<hex>`** is information, not a crash. It means the two sides have drifted on the schema, or something else is publishing to these topics. Rerun `generate.sh` first, against an up-to-date `node.proto` — a stale checkout here is the usual cause.

Commands are echoed too, so a monitor left running shows both halves of an exchange — the `Command` going down and the `Ack` coming back.

`--host` and `--port` override the defaults (`192.168.10.1`, `1883`).

## `command.py` — drive the request/response half

```sh
host/.venv/bin/python host/command.py info            # GetDeviceInfo: firmware, board, client id
host/.venv/bin/python host/command.py trigger         # force a single measurement now
host/.venv/bin/python host/command.py interval 2000   # retune the telemetry period, in ms
```

It publishes at QoS 1 and correlates the reply by `sequence` — subscribing to the ack topic *before* publishing, because a fast node can otherwise answer before anyone is listening. The sequence number is also what lets the node discard a duplicate when QoS 1 redelivers.

| Flag | Effect |
|---|---|
| `--node <id>` | which node to address (default `1`); `--node 2` is the peer node, whose traffic the gateway relays |
| `--sequence <n>` | force a sequence number — **reuse one to test duplicate suppression** |
| `--timeout <s>` | how long to await the `Ack` (default 5) |
| `--host` / `--port` | broker address (defaults `192.168.10.1:1883`) |

Exit codes make it usable from a script: **0** the node acked `ACK_STATUS_OK`, **1** no `Ack` arrived before the timeout, **2** an `Ack` arrived with any other status (`ACK_STATUS_INVALID_ARGUMENT` for an out-of-range interval, and the rest of the enum in `node.proto`).

Two useful things it can do that the happy path does not show: `--sequence` twice in a row exercises the node's duplicate suppression, and an out-of-range `interval` exercises its validator — both should be answered, not ignored.

## Two terminals is the normal setup

`command.py` prints its own `Ack` and exits, which is enough to know a command was accepted and not enough to see what it *did*. Open a second SSH session and leave `monitor.py` running in it; the effect of a command shows up there:

```
ssh pi@pi5.local        # terminal A: host/.venv/bin/python host/monitor.py
ssh pi@pi5.local        # terminal B: host/.venv/bin/python host/command.py ...
```

`interval 2000` is the clearest demonstration — the `Ack` in B says `ACK_STATUS_OK`, and A is where you watch the telemetry cadence actually change. `trigger` is similar: B confirms it was accepted, A shows the extra reading arrive out of schedule.

## What is in here

| File | |
|---|---|
| `monitor.py` | subscribe to `node/#`, decode and print |
| `command.py` | publish one `Command`, await the matching `Ack` |
| `generate.sh` | `proto/node.proto` → `node_pb2.py` |
| `requirements.txt` | pinned deps, and comments explaining *why* pinned |
| `node_pb2.py` | generated, gitignored, never hand-edited |

## Where to read more

- [`../notes/communication-guide.md`](../notes/communication-guide.md) §9 — the MQTT lab these tools belong to, including what `mosquitto_sub` can and cannot show you once payloads are binary.
- [`../docs/mqtt-design.md`](../docs/mqtt-design.md) — the topic hierarchy, the QoS decision per topic, the broker config, and the Last Will procedure.
- [`../notes/protobuf-guide.md`](../notes/protobuf-guide.md) — the layer these tools add on top of MQTT, and §11 on the version check that dictates how `generate.sh` is written.
- [`../docs/toolchain.md`](../docs/toolchain.md) — *Host toolchain (Pi)*: the venv rules, and why the firmware side is immune to the gencode-version problem.
