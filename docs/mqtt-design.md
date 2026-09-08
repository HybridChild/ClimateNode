# MQTT design — project reference

Decisions and verified setup for the Nucleo ↔ Pi MQTT link. Terse by intent — for the concepts behind any of it, see the companion teaching guide, [`communication-guide.md`](../notes/communication-guide.md). For how these decisions are expressed in code, see [`firmware-mqtt-walkthrough.md`](firmware-mqtt-walkthrough.md). This page starts at the socket; for the layer *below* it — the PHY/MAC, the interface, and how the static address gets there with no app code — see [`network-bringup.md`](network-bringup.md).

## Transport

| | |
| --- | --- |
| Protocol | **MQTT 3.1.1** over TCP (fixed up front — see the README) |
| Broker | **Mosquitto 2.0.11** on the Pi, `192.168.10.1:1883` |
| Client | Nucleo at `192.168.10.2`, Zephyr `CONFIG_MQTT_LIB` |
| Auth | **none** (bench only) — production deployments would use credentials + TLS |

## Topic hierarchy

`node/<id>/<kind>` — general → specific, so a harness can select one node (`node/1/telemetry`), one kind across nodes (`node/+/telemetry`), or one node entirely (`node/1/#`) without firmware changes.

| Topic | Direction | QoS | Retain | Payload |
| --- | --- | --- | --- | --- |
| `node/<id>/telemetry` | node → host | **0** | no | `Telemetry` protobuf |
| `node/<id>/command` | host → node | **1** | no | `Command` protobuf |
| `node/<id>/ack` | node → host | **1** | no | `Ack` protobuf |
| `node/<id>/status` | node → host | **1** | **yes** | ASCII `online` / `offline` (see LWT) |

`<id>` is `1` for the gateway and `2` for the F072RB peer node reached over CAN. The level is in the scheme whether or not a second node exists, which is what makes adding one a configuration change rather than a migration — retrofitting a level into a topic scheme breaks every subscriber instead.

`status` stays plain ASCII deliberately: it is the one topic the broker itself writes (as the will), so it cannot be protobuf-encoded by firmware.

**Who publishes the `node/2` topics.** The gateway, over **a second MQTT connection presenting client id `nucleo-2`**. Nothing in MQTT requires the publisher of a topic to *be* the thing the topic names, and the peer node has no IP stack at all — it speaks only CAN. `relay.cpp` moves its already-encoded payloads across and `main.cpp` publishes them; see [`can-bringup.md`](can-bringup.md).

**Why two connections rather than one.** Everything above works on a single connection except one thing, and that one thing is the entire justification: **MQTT 3.1.1 permits exactly one Last Will per connection.** A will is registered in the CONNECT packet, so a connection can cover one status topic and no more.

| Failure | How `node/2/status` becomes `offline` |
| --- | --- |
| peer node dies, gateway alive | the relay's heartbeat timeout notices and publishes — firmware, either design |
| **gateway dies, peer alive** | **the `nucleo-2` session's will fires.** With one connection nothing published it and the topic stayed retained-`online`, stale |
| both die | both wills fire, one per status topic |

The two mechanisms cover different failures and neither substitutes for the other — which is the point. The broker can never observe the peer's own liveness, because there is no TCP connection between them to notice; and firmware can never publish anything about a gateway that has stopped running. Note also the asymmetry in *what the connection is*: the `nucleo-2` session is a real MQTT client with its own id, session state, keepalive and packet-id space, and none of that makes it any less true that the device behind it is a Cortex-M0 with no network stack.

**One consequence in the code worth knowing.** The gateway's session announces `online` on connect unconditionally — it speaks for itself, and it is evidently up. The peer's session must not: it speaks for a node whose liveness the relay tracks separately and which may be dead right now, so it publishes the relay's *current* belief instead, and publishes nothing at all while that belief is `PEER_UNKNOWN`. Announcing `online` there would overwrite a correct `offline` with a false one, on a retained topic, every time the gateway reconnected to the broker.

## QoS rationale

**Telemetry = QoS 0.** Not a cost decision — a correctness one. A fresh sample lands every ~5 s, so a lost one self-heals almost immediately. QoS 1 + a persistent session would **queue telemetry during a disconnect and flood stale readings on reconnect**, which is worse than losing them: two-minute-old "live" data is misleading. QoS 0 also spares the target from tracking in-flight packet ids and retransmits.

**Command / Ack = QoS 1.** A dropped "set interval" has no self-healing path — there is no next one coming. Consequence: QoS 1 is *at least* once, so **duplicates are possible** (the `DUP` flag on redelivery after a lost `PUBACK`). Commands must therefore be **idempotent or carry a `sequence` the node dedupes on** — this is what forces the `sequence` field in the README's message set, and `Ack` echoes it back.

**QoS 2 unused.** Its exactly-once four-packet handshake buys nothing that a `sequence` + dedupe doesn't already give us, at higher cost and complexity — and being hop-by-hop (below), it would not reach the CAN leg that `sequence` does.

MQTT QoS is **not** TCP reliability. TCP guarantees bytes reached the broker's *TCP stack*; QoS 1 guarantees the broker *application* took ownership. QoS 0's real exposure is the reconnect gap, not wire corruption.

**QoS is hop-by-hop, and the relay is what makes that visible.** A `PUBACK` for `node/2/command` means the *gateway* took ownership of those bytes. It says nothing about whether the CAN link delivered them, whether the peer node decoded them, or whether the peer acted. The end-to-end statement is the `Ack` — which is precisely why an `Ack` exists as an application message rather than being folded into the transport's acknowledgement.

The distinction only becomes observable once the hop that acknowledges and the node that acts are different devices, which is exactly what the relay makes them. The gap has two names. If `isotp_send()` fails outright — the usual case when the bus is unplugged — the gateway answers `ACK_STATUS_FAILED` with detail `"no route to node over CAN"`, typically within a second. If the transfer went out but no `Ack` came back inside `kAckWaitMs`, the detail is `"no response over CAN"` instead. Either way the host sees a QoS 1 delivery that succeeded and an application-level failure, which is the honest report of what happened. *Bring-up checks* step 8 in [`can-bringup.md`](can-bringup.md) exercises it against a real outage.

**Nothing is queued for later delivery, deliberately.** The relay has no store-and-forward, so a command refused during an outage is simply refused; reconnecting the bus does not deliver it. The reasoning is that a command's value usually decays faster than the outage lasts — `trigger_measurement` is meaningless a minute later, and `set_interval` applied after the operator has moved on is worse than a clean failure. Only the host knows whether it still wants the thing, so the host owns the retry, and that is safe precisely because `Command.sequence` plus the node's duplicate suppression makes a repeat idempotent. A firmware-update channel would answer this differently and would need a queue with an explicit expiry.

**One deliberate exception to the gateway's opacity, priced.** To correlate that synthesized `Ack` the gateway needs the command's `sequence`, so `main.cpp` decodes the `Command` **envelope** — one header field — while never interpreting the payload arm. The alternative was to synthesize nothing and let the host time out; that is simpler and strictly worse, because a host that times out cannot distinguish "the peer is gone" from "the gateway dropped it", and would have to invent its own deadline to say anything at all.

## Session, keepalive, will

- **Clean session** — the node keeps no server-side state. Follows directly from the QoS 0 telemetry decision: there is no queue we want replayed.
- **Keepalive 60 s.** The node publishes every ~5 s so `PINGREQ` will rarely fire; the value sets how fast the broker declares us dead (1.5× keepalive) and fires the will. Needed because TCP notices a dead peer far too slowly and an idle connection is silent.
- **Last Will**: topic `node/<id>/status`, payload `offline`, **retained**, QoS 1, registered at `CONNECT`. On connect the node publishes `online` (retained) to the same topic. Net effect: `status` is correct for any subscriber, including after a crash or cable pull, with no firmware handling the failure path — bounded by detection, not by firmware: an unclean death leaves the retained `online` standing until the will fires 1.5× keepalive later.
- **Reconnect**: on drop, retry the TCP connect + MQTT `CONNECT` with backoff. This is the README's headline learning goal — treat it as real work, not error handling.

### Reconnect latency vs. backoff

The backoff starts at 1 s, doubles to a 30 s cap, and resets to the minimum only after a session that actually reached CONNACK. Consequence: **a broker that comes back early still waits out the current delay.** Restarting Mosquitto takes seconds; the node can reconnect 30 s later.

That is the intended trade — patience over hammering a dead endpoint — but it means "broker downtime" and "node downtime" are not the same number, and a 5 s telemetry cadence can lose several samples to a 1 s outage. The sensor keeps sampling throughout (that is what the zbus split is for), so the loss is a `sequence` gap, not missing time.

## Broker config (verified)

Mosquitto 2.0 binds to loopback and denies anonymous **by default**, so an untouched install is unreachable from the Nucleo (connection *refused*). Drop-in at `/etc/mosquitto/conf.d/bench.conf` (read via `include_dir` from the shipped `mosquitto.conf`, which stays untouched):

```conf
listener 1883 192.168.10.1
allow_anonymous true
```

- Binding to `192.168.10.1` (not `0.0.0.0`) keeps the unauthenticated broker on the bench cable and **off the home LAN**, where the Pi is `192.168.1.105` over WiFi.
- Defining any listener **replaces** the implicit localhost one — verified: `ss -tlnp` shows only `192.168.10.1:1883`, loopback gone. So even on the Pi, use `-h 192.168.10.1`, never `-h localhost`.

### Boot ordering — required, or the broker dies on reboot

Binding to a *specific* IP means mosquitto cannot start until that address exists. On boot it loses the race against NetworkManager: `bind()` fails with `Cannot assign requested address`, and systemd's default limiter (5 starts / 10 s) gives up **within one second** — `Start request repeated too quickly` — long before `eth0` is configured. This shows up the first time the Pi reboots after the listener is bound: the service is simply dead until started by hand.

Fix, `/etc/systemd/system/mosquitto.service.d/override.conf` (a drop-in, so package upgrades don't clobber it) — then `sudo systemctl daemon-reload`:

```ini
[Unit]
Wants=network-online.target
After=network-online.target
StartLimitIntervalSec=0

[Service]
Restart=on-failure
RestartSec=5
```

- `Wants=` **and** `After=network-online.target` — `After=` alone only orders *if* the target is already being started; `Wants=` pulls it in. Not `network.target`, which only means "networking is being configured" and still races.
- `StartLimitIntervalSec=0` disables the rate limiter that caused the give-up.
- `Restart=on-failure` + `RestartSec=5` retry indefinitely. This covers the case ordering *cannot* fix: with the **Nucleo powered off** there is no carrier, so NetworkManager never applies the profile and `192.168.10.1` genuinely does not exist. The broker then starts by itself once the board is plugged in.

Requires `NetworkManager-wait-online.service` to be **enabled** — it is what actually satisfies `network-online.target`; if disabled, the target is inert and the ordering silently does nothing. Check with `systemctl is-enabled NetworkManager-wait-online.service`, and inspect the merged unit with `systemctl cat mosquitto`.

Check it holds: `sudo reboot`, then once the Pi is back, `systemctl is-active mosquitto` should say `active` with no manual start — and `ss -tlnp | grep 1883` should show the listener bound to `192.168.10.1`.

### Two different things write `node/2/status offline`

Worth separating before testing either, because they look identical on the topic and share nothing else:

| | Published by | Fires when | Latency |
| --- | --- | --- | --- |
| Relay liveness timeout | the **gateway's firmware**, via `chan_relay_status` | the peer stops beating | `kHeartbeatTimeoutMs`, 3.5 s |
| **Last Will** | the **broker**, unprompted | the gateway's MQTT connection dies uncleanly | 1.5 × `kKeepaliveSec` |

Unplugging the CAN bus exercises the first, and is *Bring-up checks* step 8 in [`can-bringup.md`](can-bringup.md). It says nothing about the Last Will.

The Last Will exists for the case the first mechanism structurally cannot cover: if the gateway dies, no firmware runs to publish anything, and `node/2/status` stays retained as `online` forever. That is the entire justification for the second MQTT connection, since MQTT 3.1.1 allows one will per connection — so testing it means killing the gateway's *connection* while leaving CAN alone. Both wills die with the same board, so `node/1/status` and `node/2/status` both go offline; seeing both flip while the gateway is unreachable is the proof, because an unreachable client cannot have published either. They are two separate TCP connections with two independent keepalive timers, though, so they do not fire simultaneously — see the transcript below.

### Testing the Last Will — two obvious methods silently cannot work

The will fires when the broker stops hearing from the node for 1.5× keepalive. Observing that requires the node to look dead **while the Pi's own networking stays intact** — because `192.168.10.1` is both where Mosquitto listens *and* where a local `mosquitto_sub` connects. Anything that drops carrier takes the observer down with the node, so nothing can watch.

| Method | Works? | Why |
| --- | --- | --- |
| Pull the Ethernet cable | ❌ | Carrier loss makes NetworkManager deactivate `eth0`, removing `192.168.10.1`. The broker's listener and the local subscriber both die with it. Verified: `ip -br addr show eth0` → `DOWN`. |
| Hold the board's reset button | ❌ | The LAN8742's nRST is tied to the board NRST, so reset kills the **PHY** too — link drops, same as above. (The PHY runs fine without firmware *configuring* it, but not while held in reset.) |
| **Drop the node's packets at the Pi** | ✅ | Operates at the IP layer; physical link and `eth0` address are untouched, so the observer stays connected. |

Bookworm has no `iptables`; use nftables in a dedicated table so nothing else is disturbed:

```sh
sudo nft add table inet bench
sudo nft add chain inet bench input '{ type filter hook input priority 0; }'
sudo nft add rule inet bench input ip saddr 192.168.10.2 drop
#   ... wait 1.5x keepalive; `node/1/status offline` appears, published by the broker
sudo nft delete table inet bench          # node reconnects and republishes "online"
```

Lower `kKeepaliveSec` to ~10 s and reflash first, or the wait is 90 s. Watch `node/1/status` from a second terminal on the Pi (`mosquitto_sub -h 192.168.10.1 -t 'node/1/status' -v`) — `offline` appears without any client having published it.

This exercises both directions of the failure at once: the broker detects a dead node and fires the will, while the node detects a dead broker (unacked `PINGREQ`) and enters its reconnect backoff, which you can watch on the console.

**Both wills, and what the stagger between them proves.** Run with `mosquitto_sub -h 192.168.10.1 -t 'node/#' -v` and leave CAN connected, so the peer stays alive and only the gateway's *connection* dies:

```text
18:59:18  node/1/telemetry  seq=348  ...        <- last packet of the nucleo-1 session
18:59:22  node/2/telemetry  seq=165  ...        <- last packet of the nucleo-2 session
          # nft rule applied here
19:00:50  node/1/status     offline             <- broker, 90 s after 18:59:18
19:00:56  node/2/status     offline             <- broker, 90 s after 18:59:22
          # nft delete table
19:02:26  node/1/status     online
19:02:26  node/1/telemetry  seq=367  ...
19:02:28  node/2/status     online
19:02:28  node/2/telemetry  seq=183  ...
```

Three things are worth reading out of that, and none of them is visible with one connection:

- **The six-second stagger is the point.** Each will fired 1.5 × `kKeepaliveSec` after *its own* session's last packet, and the two sessions last published four seconds apart. Two independent keepalive timers, which is the observable difference between "two MQTT clients" and "one connection with two topic names". At the default keepalive that is a 90 s wait; lower `kKeepaliveSec` to ~10 and reflash to make it 15.
- **Nothing was queued.** `node/1` went 348 → 367 and `node/2` 165 → 183 across the outage: every reading was taken on cadence, all of them discarded, and exactly one fresh sample published per node on reconnect. That is the QoS 0 + clean-session decision above, and it is precisely the stale-telemetry flood that QoS 1 with a persistent session would have produced instead.
- **`node/2/status` came back `online` for the right reason.** The peer's session does not announce `online` on connect; it publishes the relay's current belief. CAN was untouched here so that belief was `online` — but had the peer been dead, this reconnect would correctly have republished `offline` rather than overwriting a true `offline` with an assumed `online`. That is the asymmetry described above, observed.

Verify / exercise:

```sh
ss -tlnp | grep 1883                                            # bound address
mosquitto_sub -h 192.168.10.1 -t 'node/#' -v                    # watch everything
mosquitto_pub -h 192.168.10.1 -t 'node/1/command' -m x -q 1 -d  # -d shows the packets
```

## Payload

[`proto/node.proto`](../proto/node.proto) is the contract, and carries its own field-level rationale inline. Concepts in [`protobuf-guide.md`](../notes/protobuf-guide.md).

One consequence belongs here rather than there: **the topic is what says which message type a payload is.** Protobuf puts no type identity on the wire, so `node/<id>/command` carrying a `Command` is not a convention — it is the half of the wire format the `.proto` file does not contain. Never mix message types on one topic.

## Three smaller decisions

**Telemetry is not retained.** Retain on `status` and not on telemetry, which looks inconsistent until you ask what each topic is *for*: `status` answers "is this node alive?", a question whose answer is still true an hour later, while a retained reading hands a late-joining subscriber a measurement of air that no longer exists and no way to tell how stale it is. A harness that wants the current value waits one sample period for it. The same argument decides the observer kinds inside the firmware — [`zbus-guide.md`](../notes/zbus-guide.md) §5.

**The `<id>` is hardcoded per application, not derived from the STM32 unique ID.** `kNodeClientId` is declared in `commands.h` and defined by each app's `main.cpp` (and by the test), so identity is a link-time fact rather than a runtime one. A derived id would have to be discovered before anything could subscribe to it, which buys a bootstrapping problem in exchange for nothing on a bench of two known boards.

**Telemetry is a timed poll, not a data-ready trigger.** Not a preference: the in-tree SCD-40 driver never exposes data-ready through the sensor API, so there is no signal to trigger on. What that costs, and why the cadence floor exists, is *Accepted limitation* in [`sensor-bringup.md`](sensor-bringup.md).
