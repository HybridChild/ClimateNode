# MQTT design — project reference

Decisions and verified setup for the Nucleo ↔ Pi MQTT link. Terse by intent — for the
concepts behind any of it, see the companion teaching guide,
[`communication-guide.md`](communication-guide.md).

Resolves the README's open decisions **"decide QoS per topic"** and **"the topic
hierarchy"**. Decided 2026-07-17.

## Transport

| | |
|---|---|
| Protocol | **MQTT 3.1.1** over TCP (matches the product; confirmed via the outgoing consultant) |
| Broker | **Mosquitto 2.0.11** on the Pi, `192.168.10.1:1883` |
| Client | Nucleo at `192.168.10.2`, Zephyr `CONFIG_MQTT_LIB` |
| Auth | **none** (bench only) — the real product uses credentials + TLS |

## Topic hierarchy

`node/<id>/<kind>` — general → specific, so a harness can select one node
(`node/1/telemetry`), one kind across nodes (`node/+/telemetry`), or one node entirely
(`node/1/#`) without firmware changes.

| Topic | Direction | QoS | Retain | Payload |
|---|---|---|---|---|
| `node/<id>/telemetry` | node → host | **0** | no | `Telemetry` protobuf |
| `node/<id>/command` | host → node | **1** | no | `Command` protobuf |
| `node/<id>/ack` | node → host | **1** | no | `Ack` protobuf |
| `node/<id>/status` | node → host | **1** | **yes** | ASCII `online` / `offline` (see LWT) |

`<id>` is `1` for the single bench node. Keep the level even with one node — retrofitting a
level into a topic scheme later breaks every subscriber.

`status` stays plain ASCII deliberately: it is the one topic the broker itself writes (as
the will), so it cannot be protobuf-encoded by firmware.

## QoS rationale

**Telemetry = QoS 0.** Not a cost decision — a correctness one. A fresh sample lands every
~5 s, so a lost one self-heals almost immediately. QoS 1 + a persistent session would
**queue telemetry during a disconnect and flood stale readings on reconnect**, which is
worse than losing them: two-minute-old "live" data is misleading. QoS 0 also spares the
target from tracking in-flight packet ids and retransmits.

**Command / Ack = QoS 1.** A dropped "set interval" has no self-healing path — there is no
next one coming. Consequence: QoS 1 is *at least* once, so **duplicates are possible** (the
`DUP` flag on redelivery after a lost `PUBACK`). Commands must therefore be **idempotent or
carry a `sequence` the node dedupes on** — this is what forces the `sequence` field in the
README's message set, and `Ack` echoes it back.

**QoS 2 unused.** Its exactly-once four-packet handshake buys nothing that a `sequence` +
dedupe doesn't already give us, at higher cost and complexity.

MQTT QoS is **not** TCP reliability. TCP guarantees bytes reached the broker's *TCP stack*;
QoS 1 guarantees the broker *application* took ownership. QoS 0's real exposure is the
reconnect gap, not wire corruption.

## Session, keepalive, will

- **Clean session** — the node keeps no server-side state. Follows directly from the QoS 0
  telemetry decision: there is no queue we want replayed.
- **Keepalive 60 s.** The node publishes every ~5 s so `PINGREQ` will rarely fire; the value
  sets how fast the broker declares us dead (1.5× keepalive) and fires the will. Needed
  because TCP notices a dead peer far too slowly and an idle connection is silent.
- **Last Will**: topic `node/<id>/status`, payload `offline`, **retained**, QoS 1,
  registered at `CONNECT`. On connect the node publishes `online` (retained) to the same
  topic. Net effect: `status` is always correct for any subscriber, including after a crash
  or cable pull, with no firmware handling the failure path.
- **Reconnect**: on drop, retry the TCP connect + MQTT `CONNECT` with backoff. This is the
  README's headline learning goal — treat it as real work, not error handling.

## Broker config (verified)

Mosquitto 2.0 binds to loopback and denies anonymous **by default**, so an untouched
install is unreachable from the Nucleo (connection *refused*). Drop-in at
`/etc/mosquitto/conf.d/bench.conf` (read via `include_dir` from the shipped
`mosquitto.conf`, which stays untouched):

```
listener 1883 192.168.10.1
allow_anonymous true
```

- Binding to `192.168.10.1` (not `0.0.0.0`) keeps the unauthenticated broker on the bench
  cable and **off the home LAN**, where the Pi is `192.168.1.105` over WiFi.
- Defining any listener **replaces** the implicit localhost one — verified: `ss -tlnp` shows
  only `192.168.10.1:1883`, loopback gone. So even on the Pi, use `-h 192.168.10.1`, never
  `-h localhost`.

### Boot ordering — required, or the broker dies on reboot

Binding to a *specific* IP means mosquitto cannot start until that address exists. On boot it
loses the race against NetworkManager: `bind()` fails with `Cannot assign requested address`,
and systemd's default limiter (5 starts / 10 s) gives up **within one second** —
`Start request repeated too quickly` — long before `eth0` is configured. Observed after the
first reboot, 2026-07-18; the service was dead until started by hand.

Fix, `/etc/systemd/system/mosquitto.service.d/override.conf` (a drop-in, so package upgrades
don't clobber it) — then `sudo systemctl daemon-reload`:

```ini
[Unit]
Wants=network-online.target
After=network-online.target
StartLimitIntervalSec=0

[Service]
Restart=on-failure
RestartSec=5
```

- `Wants=` **and** `After=network-online.target` — `After=` alone only orders *if* the target
  is already being started; `Wants=` pulls it in. Not `network.target`, which only means
  "networking is being configured" and still races.
- `StartLimitIntervalSec=0` disables the rate limiter that caused the give-up.
- `Restart=on-failure` + `RestartSec=5` retry indefinitely. This covers the case ordering
  *cannot* fix: with the **Nucleo powered off** there is no carrier, so NetworkManager never
  applies the profile and `192.168.10.1` genuinely does not exist. The broker then starts by
  itself once the board is plugged in.

Requires `NetworkManager-wait-online.service` to be **enabled** — it is what actually
satisfies `network-online.target`; if disabled, the target is inert and the ordering silently
does nothing. Check with `systemctl is-enabled NetworkManager-wait-online.service`, and
inspect the merged unit with `systemctl cat mosquitto`.

Verified surviving a reboot, 2026-07-18.

Verify / exercise:

```sh
ss -tlnp | grep 1883                                            # bound address
mosquitto_sub -h 192.168.10.1 -t 'node/#' -v                    # watch everything
mosquitto_pub -h 192.168.10.1 -t 'node/1/command' -m x -q 1 -d  # -d shows the packets
```

## Still open

- **Telemetry trigger** — 5 s poll vs. the SCD-40 data-ready signal (README open decision).
- **Retain on telemetry** — off for now. Turning it on gives a late-starting harness the
  last reading instantly; revisit if that friction shows up.
- **`<id>` source** — hardcoded `1`, or derived from the STM32 unique ID (the same source
  the Ethernet MAC `02:80:E1:9C:A7:DE` is hashed from). Only matters with a second node.
- **Payload schema** — `proto/` does not exist yet; `Telemetry` / `Command` / `Ack` are
  specified only in the README. That file becomes the contract once written.
