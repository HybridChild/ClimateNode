# Test strategy — project reference

What this repo tests, where, and what it deliberately leaves to the bench. Terse by intent:
decisions and their rationale. For the concepts underneath — what Ztest and Twister are,
what makes code testable, where a test can run — see the companion teaching guide,
[`testing-guide.md`](../notes/testing-guide.md).

## Running them

```sh
./scripts/test.sh                    # every suite, ~18 s
./scripts/test.sh -T tests/commands  # one suite
./scripts/test.sh -s node.protocol   # one identifier
```

No board, no broker, no sensor. The script sources the workspace venv and exports
`ZEPHYR_BASE` itself, like `build.sh`. Output goes to `twister-out/` (gitignored).

## What is covered

| Suite | Code under test | Cases |
|---|---|---|
| `tests/protocol/` | `firmware/src/protocol.cpp` — the wire format | 11 |
| `tests/commands/` | `firmware/src/commands.cpp` — command semantics | 10 |

**`tests/protocol/`** — telemetry round-trip; the internal→wire status enum mapping; that a
warming-up reading is shorter on the wire because proto3 omits defaults; that the worst case
fits `node_Telemetry_size`; that an undersized buffer fails rather than overflows. For
commands: `garbage` rejected as structurally invalid, `hi` *accepted* as a legal `Command`
with no arm set, oversize refused outright, empty payload decoding to all defaults. For
acks: round-trip, `detail` truncating at its 47-character bound, and a worst-case Ack with
`DeviceInfo` fitting `node_Ack_size`.

**`tests/commands/`** — that `set_interval` and `trigger` reach the bus carrying the right
values; that `get_device_info` reports the identity constants; that an unknown `oneof` arm
and a `which_payload == 0` both answer `UNSUPPORTED`; that an out-of-range interval is
rejected **and leaves the channel unchanged**; that the bounds are inclusive at both ends;
and that a duplicate sequence is acked without re-running the side effect, while a fresh one
afterwards still acts.

Several of these are claims from the guides turned into assertions — `protobuf-guide.md`
§4–§5 and `zbus-guide.md` §6 — so prose and firmware cannot drift apart quietly.

## Decisions

**Platform: `qemu_cortex_m3`.** `native_sim` is the usual choice and is far faster, but it
**only supports Linux hosts** (`boards/native/native_sim/doc/index.rst`) and this bench is
macOS. QEMU ships inside the Zephyr SDK (`~/zephyr-sdk-1.0.1/hosttools/opt/qemu/bin/`), so
nothing extra is installed. `testcase.yaml` lists `native_sim` as well, so a Linux CI runner
can use it without touching the tests.

Two consequences, both accepted:

- Tests cross-compile with the same `arm-zephyr-eabi-g++`, `-fno-exceptions`, freestanding
  runtime and warnings-as-errors as the firmware. Higher fidelity than a host build.
- The M3 has no FPU, so `float` is soft-float here and hardware float on the H7. IEEE-754
  makes the encoded bytes identical, which is what the float assertions rely on.

**The four-way source split.** `main.cpp` was 713 lines carrying the MQTT session, event
handling, wire encoding, command semantics and zbus glue; nothing in it could be compiled
without the network stack. It is now `main.cpp` (MQTT session), `protocol.cpp` (wire
format), `commands.cpp` (command semantics) and `sensor.cpp` (acquisition, unchanged).
Every function moved essentially verbatim — no seam was carved and no test-only parameter
added. Each file is justifiable without mentioning tests, which is the test of whether the
split was design or damage. Cost: FLASH +72 B, RAM unchanged.

**The test owns `chan_sensor_cmd`.** `ZBUS_CHAN_DECLARE` expands to `extern`
(`include/zephyr/zbus/zbus.h:362`), so `app_channels.h` declares the channel and whoever
links supplies the definition — `sensor.cpp` in the firmware, the test in
`tests/commands/`. Its validator calls the production `sensor_cmd_in_range()`, so rejection
tests exercise the real predicate through a real `zbus_chan_pub()` and a real `-ENOMSG`.
**Not a mock**, and not a seam invented for testing: the channel is the boundary the design
already had.

**`sensor_cmd_in_range()` lives in `app_channels.h`.** The `SAMPLE_PERIOD_*` bounds were
already there; the comparison now sits beside them. `sensor_cmd_valid()` in `sensor.cpp` is
the zbus adapter around it (the bus requires a `const void *`). One rule, one definition,
reachable from both the firmware and the tests.

**Logging stays on in the suites.** `CONFIG_TEST_LOGGING_DEFAULTS` (default `y` under
`CONFIG_TEST`) does `select LOG`, and a `select` is a forced enable that `CONFIG_LOG=n`
cannot override. It earns its place anyway — the log line before a failed assertion is
useful context. Expect `W:`/`E:` lines from the deliberate-rejection tests; those paths are
working correctly. `CONFIG_TEST_LOGGING_DEFAULTS=n` is the switch if silence is wanted.

**No test-only production API.** `commands.cpp` keeps module-level dedupe state that
outlives a single test, and Ztest does not run tests in source order. Rather than exporting
a reset function, each test takes a fresh sequence number from a local counter; the
duplicate test reuses one deliberately.

## What is deliberately not tested here

Not gaps to be filled by more host tests — these are structurally out of reach:

- **`sensor.cpp`** — needs the SCD-40. Timing, the deferred init, the fast-poll republish.
  Covered by *Bring-up checks* in [`sensor-bringup.md`](sensor-bringup.md) and
  `sensor-api-guide.md` §11.
- **`main.cpp`'s MQTT session** — connect, keepalive, reconnect backoff, the poll loop with
  its two deadlines. Needs a broker and a socket.
- **Anything on the wire** — QoS 1 redelivery, retained messages, the Last Will. Covered by
  `communication-guide.md` §9 and *Testing the Last Will* in [`mqtt-design.md`](mqtt-design.md).
- **Byte-exact encoded sizes.** The tests assert messages round-trip and fit their generated
  bounds, never that a specific message is N bytes. Pinning that would break on every
  legitimate schema change; the worked example in `protobuf-guide.md` §3 is where the exact
  arithmetic lives.

## Still open

- **Hardware-in-the-loop.** The Pi has the broker, `command.py`, `monitor.py` and a cable to
  the board — the ingredients for automating the labs that currently run by hand. The host
  suites bound the problem: HIL only has to cover what they structurally cannot.
- **CI.** `scripts/test.sh` plus `scripts/build.sh` on every push is mostly plumbing; the
  wrinkle is that this is a freestanding app, so CI must reconstruct a west workspace. A
  Linux runner should use `native_sim` and finish far faster.
- **Coverage measurement.** Twister supports `--coverage`; not wired up, and of limited
  value at this suite size.
