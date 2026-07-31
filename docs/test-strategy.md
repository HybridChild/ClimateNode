# Test strategy — project reference

What this repo tests, where, and what it deliberately leaves to the bench. Terse by intent: decisions and their rationale. For the concepts underneath — what Ztest and Twister are, what makes code testable, where a test can run — see the companion teaching guide, [`testing-guide.md`](../notes/testing-guide.md).

## Running them

```sh
./scripts/test.sh                    # every suite, ~18 s
./scripts/test.sh -T tests/commands  # one suite
./scripts/test.sh -s node.protocol   # one identifier
```

No board, no broker, no sensor. The script sources the workspace venv and exports `ZEPHYR_BASE` itself, like `build.sh`. Output goes to `twister-out/` (gitignored).

## What is covered

| Suite | Code under test | Cases |
|---|---|---|
| `tests/protocol/` | `firmware/src/protocol.cpp` — the wire format | 16 |
| `tests/commands/` | `firmware/src/commands.cpp` — command semantics | 9 |
| `tests/heartbeat/` | `firmware/src/can_link.h` — the CAN link contract | 8 |
| `tests/relay/` | `firmware/src/relay.h` — the gateway's liveness machine | 10 |

**`tests/protocol/`** — telemetry round-trip; the internal→wire status enum mapping; that a warming-up reading is shorter on the wire *and* decodes as absent rather than zero; that an explicitly-set zero costs its tag plus value and comes back present; that a pressure-only reading round-trips with the other three absent; that the worst case fits `node_Telemetry_size`; that an undersized buffer fails rather than overflows. For commands: `garbage` rejected as structurally invalid, `hi` *accepted* as a legal `Command` with no arm set, oversize refused outright, empty payload decoding to all defaults. For acks: round-trip, `detail` truncating at its 47-character bound, and a worst-case Ack with `DeviceInfo` fitting `node_Ack_size`.

**`tests/commands/`** — that `set_interval` and `trigger` reach the bus carrying the right values; that `get_device_info` reports the identity constants; that an unknown `oneof` arm and a `which_payload == 0` both answer `UNSUPPORTED`; that an out-of-range interval is rejected **and leaves the channel unchanged**; that the bounds are inclusive at both ends; and that a duplicate sequence is acked without re-running the side effect, while a fresh one afterwards still acts.

Two cases carry the schema-versioning exercise: `test_old_reader_decodes_new_telemetry` and `test_new_reader_decodes_old_telemetry` hand-build a nanopb descriptor for the *previous* Telemetry — no `optional`, no `pressure_pa` — with the `PB_BIND` X-macro, and run both directions against the current schema. The old reader skips the field it has never heard of; the new reader recovers everything the old writer sent, except a zero, which was never on the wire to recover. That asymmetry is the point, and it is why the old schema is an executable participant here rather than a comment.

**`tests/heartbeat/`** — the heartbeat frame's byte layout asserted position by position, not only round-tripped; the round trip across every state and the `uint32` uptime boundary; rejection of a wrong layout version, a short frame, and an unknown state; and the address map, including that heartbeat identifiers sort below the ISO-TP ranges.

Two decisions in that suite are worth stating. **The layout is asserted byte by byte and not only through a round trip**, because pack and unpack were written by the same author in the same sitting: a shift in the wrong direction in both would round-trip perfectly and mean something else on the other board. These bytes are the only wire format in the project that is *not* generated, so nothing else is checking them. And **an unknown state byte is rejected**, which is the opposite of the protobuf rule for enums (`../notes/protobuf-guide.md` §7) — a fixed byte layout has no way to describe what else may have changed alongside a value it does not know, so the conservative reading is the right one and the version byte is what should have carried the news.

The suite links **no subsystem at all** — no CAN driver, no nanopb, no zbus. That it can is the evidence that the link contract has no dependencies, which is what lets two applications and a test share one definition of the frame.

**`tests/relay/`** — the state machine that turns a heartbeat and a clock into the retained `online`/`offline` that MQTT would otherwise supply for free. The boot window (nothing is published until there is evidence either way); that silence from boot does eventually conclude `offline`; that sixty steady heartbeats produce exactly **one** publish and sustained silence produces exactly one; that the timeout is measured from the last beat rather than from boot or from the last step; that three lost beats are tolerated, which is what the 3500 ms / 1000 ms ratio in `can_link.h` buys; that recovery is announced; that a heartbeat wins over an expired clock when both arrive in the same step; and the `relay_up`/`relay_down` size asymmetry the message-subscriber pool depends on.

The first of those is the one that matters. `status` is a **retained** topic, so a gateway restart that published `offline` for a healthy peer would leave that lie on the broker until the next heartbeat corrected it, and every host connecting in between would be told the peer is dead. `PEER_UNKNOWN` exists as a state distinct from `PEER_OFFLINE` for exactly that reason, and this suite is what stops someone collapsing the two.

It runs with no CAN controller, no thread and no passage of time, because `liveness_step()` takes the clock as an argument. That is the same shape as `sensor_cmd_in_range()` — the *rule* is an ordinary function, and only its caller touches hardware — and fast-forwarding a minute of heartbeats in a nanosecond is the reward for it.

Several of these are claims from the guides turned into assertions — [`protobuf-guide.md` §4–§5](../notes/protobuf-guide.md) and [`zbus-guide.md` §6](../notes/zbus-guide.md) — so prose and firmware cannot drift apart quietly.

## Decisions

**Platform: `qemu_cortex_m3`.** `native_sim` is the usual choice and is far faster, but it **only supports Linux hosts** (`boards/native/native_sim/doc/index.rst`) and this bench is macOS. QEMU ships inside the Zephyr SDK (`~/zephyr-sdk-1.0.1/hosttools/opt/qemu/bin/`), so nothing extra is installed. `testcase.yaml` lists `native_sim` as well, so a Linux CI runner can use it without touching the tests.

Two consequences, both accepted:

- Tests cross-compile with the same `arm-zephyr-eabi-g++`, `-fno-exceptions`, freestanding runtime and warnings-as-errors as the firmware. Higher fidelity than a host build.
- `float` is soft-float in QEMU *and* on the H7. The M3 has no FPU; the H7's Cortex-M7 does (FPv5-D16) but `CONFIG_FPU` is unset here, so `zephyr.elf` links the same `__aeabi_*` helpers — verify with `nm firmware/build/zephyr/zephyr.elf | grep __aeabi_fadd`. On `native_sim` the host FPU runs them instead, and IEEE-754 is what keeps the encoded bytes identical; that same guarantee would cover the H7 if `CONFIG_FPU=y` were ever set.

**The four-way source split.** `main.cpp` was 713 lines carrying the MQTT session, event handling, wire encoding, command semantics and zbus glue; nothing in it could be compiled without the network stack. It is now `main.cpp` (MQTT session), `protocol.cpp` (wire format), `commands.cpp` (command semantics) and `sensor.cpp` (acquisition, unchanged). Every function moved essentially verbatim — no seam was carved and no test-only parameter added. Each file is justifiable without mentioning tests, which is the test of whether the split was design or damage. Cost: FLASH +72 B, RAM unchanged.

**The test owns `chan_sensor_cmd`.** `ZBUS_CHAN_DECLARE` expands to `extern` (`include/zephyr/zbus/zbus.h:362`), so `app_channels.h` declares the channel and whoever links supplies the definition — `sensor.cpp` in the firmware, the test in `tests/commands/`. Its validator calls the production `sensor_cmd_in_range()`, so rejection tests exercise the real predicate through a real `zbus_chan_pub()` and a real `-ENOMSG`. **Not a mock**, and not a seam invented for testing: the channel is the boundary the design already had.

**`sensor_cmd_in_range()` lives in `app_channels.h`.** The `SAMPLE_PERIOD_*` bounds were already there; the comparison now sits beside them. `sensor_cmd_valid()` in `sensor.cpp` is the zbus adapter around it (the bus requires a `const void *`). One rule, one definition, reachable from both the firmware and the tests.

**Logging stays on in the suites.** `CONFIG_TEST_LOGGING_DEFAULTS` (default `y` under `CONFIG_TEST`) does `select LOG`, and a `select` is a forced enable that `CONFIG_LOG=n` cannot override. It earns its place anyway — the log line before a failed assertion is useful context. Expect `W:`/`E:` lines from the deliberate-rejection tests; those paths are working correctly. `CONFIG_TEST_LOGGING_DEFAULTS=n` is the switch if silence is wanted.

**No test-only production API.** `commands.cpp` keeps module-level dedupe state that outlives a single test, and Ztest does not run tests in source order. Rather than exporting a reset function, each test takes a fresh sequence number from a local counter; the duplicate test reuses one deliberately.

## What is deliberately not tested here

Not gaps to be filled by more host tests — these are structurally out of reach:

- **`sensor.cpp`** — needs the SCD-40. Timing, the deferred init, the fast-poll republish. Covered by *Bring-up checks* in [`sensor-bringup.md`](sensor-bringup.md) and [`sensor-api-guide.md` §11](../notes/sensor-api-guide.md).
- **`main.cpp`'s MQTT session** — connect, keepalive, reconnect backoff, the poll loop with its two deadlines. Needs a broker and a socket.
- **Anything on the wire** — QoS 1 redelivery, retained messages, the Last Will. Covered by [`communication-guide.md` §9](../notes/communication-guide.md) and *Testing the Last Will* in [`mqtt-design.md`](mqtt-design.md).
- **`relay.cpp`'s routing and its two threads.** `handle_relayed()`'s three-way switch on the message-type byte, the synthesized Ack, and the RX/TX rendezvous are all reachable only by linking a CAN device. Factoring the switch clear of `zbus_chan_pub()` to test it would mean inventing a seam for the tests alone, which this repo does not do — see *No test-only production API* above. The switch is three arms and a default; what it decides is covered on the bench, and what it decides *with* (`liveness_step`, the message-type constants, the frame layout) is covered here and in `tests/heartbeat/`.
- **`sensor-node/src/main.cpp`'s CAN session** — the heartbeat cadence, ISO-TP segmentation and reassembly, and the command round trip. Needs a peer: a node alone on a bus cannot successfully transmit, because nothing drives the ACK slot ([`can-guide.md` §5](../notes/can-guide.md)). `tests/heartbeat/` covers the one part that is reachable off-target — the bytes — and stops exactly where the driver begins. Covered on the bench by *Bring-up checks* in [`can-bringup.md`](can-bringup.md), which today stop at loopback on one board.
- **Byte-exact encoded sizes.** The tests assert messages round-trip and fit their generated bounds, never that a specific message is N bytes. Pinning that would break on every legitimate schema change; the worked example in [`protobuf-guide.md` §3](../notes/protobuf-guide.md) is where the exact arithmetic lives.

## Still open

- **Hardware-in-the-loop.** The Pi has the broker, `command.py`, `monitor.py` and a cable to the board — the ingredients for automating the labs that currently run by hand. The host suites bound the problem: HIL only has to cover what they structurally cannot.
- **CI.** `scripts/test.sh` plus `scripts/build.sh` on every push is mostly plumbing; the wrinkle is that this is a freestanding app, so CI must reconstruct a west workspace. A Linux runner should use `native_sim` and finish far faster.
- **Coverage measurement.** Twister supports `--coverage`; not wired up, and of limited value at this suite size.
