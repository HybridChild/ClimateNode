# Testing firmware without the firmware

*A from-first-principles guide to running embedded code on a machine that is not the target, and to writing code that will let you.*

A teaching document. It explains why testing firmware is awkward, what makes a piece of code testable at all, and how Zephyr's Ztest and Twister actually work — using this repository's own suites in [`tests/`](../tests/) as the running example. For what *this* project decided to test and what it deliberately does not, see its companion, [`test-strategy.md`](../docs/test-strategy.md).

**The shape of this document:**

- **§1–§2** — why the target is the wrong place to test, and the one property that decides whether code can be tested at all.
- **§3–§5** — the machinery: Ztest, the four places a test can run, and Twister.
- **§6** — three of this repo's five suites, read as worked examples: pure logic, a test-owned channel, and a driver under emulation.
- **§7–§8** — what to test and what not to, and why regulated industries make this mandatory rather than optional.
- **§9** — the whole picture, and what host tests structurally cannot catch.
- **§10–§12** — a lab, the model in one paragraph, and where to go next.

---

## 1. The problem: the target is a terrible place to test

On a desktop, running a test is free. You have a filesystem, a shell, seconds of patience, and a process you can start and kill a thousand times an hour.

An embedded target has none of that. To run one assertion on the Nucleo you must build a whole image, flash it over a debug probe, watch a serial port for output, and reset the board to try again. Each cycle is tens of seconds at best. Worse, the board is a *shared, physical* resource: it can be unplugged, it can be somewhere else, and there is exactly one of it.

That cost shapes everything. If tests are slow and awkward, they get run rarely; run rarely, they stop being a safety net and become a chore that fails for reasons nobody investigates. The only way out is to notice that **most firmware logic does not actually need the hardware.**

Consider what this node does with an incoming command. It:

1. reads bytes off a socket,
2. decides whether those bytes are a valid `Command`,
3. decides what that command *means* and what to do about it,
4. publishes a message on an internal bus,
5. encodes a reply and writes it back to the socket.

Steps 1 and 5 genuinely need a network. Steps 2, 3 and 4 are arithmetic, branching and memory — they would behave identically on a laptop. That split is not special to this project; it is the normal shape of firmware. The hardware-dependent part is usually a thin shell around a much larger core of decisions.

**The goal, then, is not "test the firmware on a PC". It is to arrange the code so the decisions are separable from the I/O, and then test the decisions.**

---

## 2. What makes code testable — and what does not

Here is the trap, and it is worth being blunt about because it produces worse code than having no tests at all.

The obvious move, when you find you cannot test something, is to carve a hole in it: split a function so a test can observe halfway through, add a flag that suppresses the side effect, inject a parameter that only the test ever supplies. Each of these makes the test possible and the *design* worse — an abstraction with one implementation and one caller, existing purely so something can be watched. That is **test-induced design damage**, and a reviewer who does not know about the test cannot tell why the code is shaped that way.

The honest version of the same instinct asks a different question: **why is this hard to test?** Usually the answer is not the function's shape but its *dependencies*.

### The untestable shape

Picture the natural first draft of this firmware: one `main.cpp` of several hundred lines carrying five unrelated jobs — the MQTT session lifecycle, MQTT event handling, protobuf encoding, command semantics, and zbus glue. The protobuf encoder in it is pure arithmetic, and it is nonetheless unreachable, because it lives in a translation unit that cannot be compiled without the socket layer and the whole network stack behind it. Nothing in that file can be linked into a test, so nothing in it can be tested.

The problem there is not `encode_telemetry()`'s signature. It is that five things share one file.

### The shape this repo has

The gateway is five translation units instead, each with one responsibility:

| File | Owns | Depends on |
|---|---|---|
| `sensor.cpp` | acquisition: the SCD-40, the sample period, the bounds | the device, zbus |
| `protocol.cpp` | the wire format: internal types ↔ protobuf | nanopb |
| `commands.cpp` | command semantics: dispatch, dedupe, node identity | zbus, protocol |
| `relay.cpp` | the CAN side: heartbeat liveness, ISO-TP both directions | the CAN device, zbus |
| `main.cpp` | the MQTT sessions: connect, poll, publish, reconnect | everything |

Nothing here is a seam carved for a test — no flag that suppresses a side effect, no parameter only a test supplies. The whole difference is *which file each thing lives in*, and it is what makes `protocol.cpp` and `commands.cpp` compile with no hardware dependency at all, so they can be tested. (`protocol.cpp` and `commands.cpp` live in [`shared/`](../shared/) rather than under `gateway/`, because the peer node links the very same two files — the same argument arriving a second time.)

The check on whether a split like this is design or damage: **each file is justifiable without mentioning tests.** "The wire format is a separate concern from the MQTT session" is an argument you would make in a review anyway. That is the difference between design improved under pressure from testing, and design damaged by it.

> **The rule of thumb:** if a change makes the code easier to explain, testing was the occasion for it, not the cause. If you find yourself explaining a construct by saying "it's like that so we can test it", look for a different change.

---

## 3. Ztest: the framework

Zephyr's test framework is deliberately small. Three things to know.

**A test is a function in a suite.**

```c
ZTEST_SUITE(protocol, NULL, NULL, NULL, NULL, NULL);

ZTEST(protocol, test_telemetry_round_trip)
{
        /* ... */
}
```

`ZTEST_SUITE`'s five `NULL`s are optional hooks: a *predicate* deciding whether the suite runs at all, then `setup`, `before`, `after` and `teardown`. `setup` runs once for the suite; `before`/`after` run around **each** test. Use them when tests share expensive state. Most suites here need none of it and pass all five as `NULL`; `tests/isotp_loopback/` is the exception and shows what the hook is actually for — it has a CAN controller to put into loopback mode and start, which is once-per-suite work that every test depends on and none of them owns.

**Assertions are macros that record a failure and return.**

| Macro | Checks |
|---|---|
| `zassert_true(cond, msg, ...)` / `zassert_false` | a condition |
| `zassert_equal(a, b, ...)` / `zassert_not_equal` | scalar equality |
| `zassert_ok(rc, ...)` | an integer return is 0 — the Zephyr idiom for success |
| `zassert_str_equal(a, b, ...)` | string contents |
| `zassert_is_null` / `zassert_not_null` | pointers |
| `zassert_within(a, b, delta, ...)` | floating point with tolerance |

The trailing message is a `printf` format, and it is the part people skip. `zassert_equal(x, 5)` tells you a number was wrong; `zassert_equal(x, 5, "interval %u should have been rejected", interval)` tells you *which case* was wrong without reading the source. Write the message as the sentence you would want to read at 5 pm.

There is also `zassume_*`, which *skips* rather than fails — for preconditions that mean "this test does not apply here", not "something is broken".

**Tests do not run in source order.** Ztest is free to order them as it likes, so a test that depends on a previous one having run is a bug that will surface later and confusingly. If state must be shared, put it in `before`; if module-level state persists, make each test independent of the order (§6 shows how this repo handles exactly that).

---

## 4. Where a test runs

This is the decision that surprises people, because Zephyr offers four answers and they differ enormously in speed and fidelity.

| Platform | How it runs | Speed | Fidelity |
|---|---|---|---|
| **Real board** (`nucleo_h753zi`) | flashed, output over serial | seconds per cycle | total — the actual chip |
| **QEMU** (`qemu_cortex_m3`) | emulated ARM, real Zephyr kernel, cross-compiled | ~1 s | high: real kernel, real toolchain, no real peripherals |
| **`native_sim`** | Zephyr compiled as a native host binary | milliseconds | medium: host compiler, host libc, POSIX underneath |
| **`unit_testing`** | host compiler, *no kernel at all* | milliseconds | low: no Zephyr APIs, pure C/C++ only |

The trade runs top to bottom: the further down, the faster the loop and the further you are from the thing that ships.

**Why this repo uses `qemu_cortex_m3`.** `native_sim` is the usual answer and it is much faster — but it **only supports Linux hosts**, and this bench develops on macOS. QEMU is the next rung up and costs nothing extra: the Zephyr SDK bundles `qemu-system-arm`, so `scripts/test.sh` works on a clean checkout.

Two things fall out of that choice, both worth understanding rather than working around:

- **The tests cross-compile with the same `arm-zephyr-eabi-g++` the firmware uses**, so they are subject to the same `-fno-exceptions`, the same freestanding runtime, and the same warnings-as-errors. Code that compiles for the test genuinely compiles for the target. `native_sim` would use the host compiler and lose that.
- **`float` arithmetic runs in software on both platforms**, so assertions on encoded floats hold identically. A Cortex-M3 has no FPU at all; the STM32H753's Cortex-M7 *does* (FPv5-D16), but this app never enables it — `CONFIG_FPU` is unset, so the target links the same `__aeabi_fadd`/`__aeabi_fmul` helpers QEMU does. Two layers of safety net, then: today it is literally the same code, and if you ever set `CONFIG_FPU=y` the results would still match because IEEE-754 pins the bit patterns. Worth knowing which one you are relying on.

`testcase.yaml` lists both platforms, so a Linux CI runner can use the faster one later without touching the tests.

---

## 5. Twister: the runner

Ztest is what a test *is*; **Twister** is what finds, builds and runs them. It walks a directory for `testcase.yaml` files, builds each one as a full Zephyr application, runs it on the platform you asked for, parses the output, and reports per test case.

A `testcase.yaml` is small:

```yaml
common:
  tags:
    - unit
    - protocol
tests:
  node.protocol:
    platform_allow:
      - qemu_cortex_m3
      - native_sim
```

- **`node.protocol`** is the test identifier — what `-s` selects and what the report names.
- **`platform_allow`** restricts where it may run. There is also `platform_exclude`, `filter` (a Kconfig expression), and `integration_platforms` for a reduced CI set.
- **`tags`** let you select across suites: `--tag unit`.

The important mental shift: **each suite is a complete, independent Zephyr application**, with its own `prj.conf`, its own devicetree, and its own `main`. It is not "your app plus tests" — it is a different program that happens to compile some of your app's source. That is what lets it leave out the network stack entirely.

**"Its own devicetree" is the part with the most left in it**, and it cuts against §4's "no real peripherals" more than you would expect. A suite can drop a `boards/<platform>.overlay` beside its `prj.conf` and *add hardware the platform does not have* — Zephyr ships emulated drivers for several subsystems, and a devicetree node is the whole of enabling one. `tests/isotp_loopback/` does exactly that: five lines declaring a `zephyr,can-loopback` node and choosing it as `zephyr,canbus`, and a board with no CAN controller acquires one that genuinely queues frames, evaluates receive filters and delivers to callbacks. Everything above the wire then runs for real — in that suite's case the whole of ISO-TP's segmentation and flow control.

Two lessons generalise from it. First, **the emulated driver is often already in the tree and merely unreferenced**: `zephyr,can-loopback` is architecture-independent and appears in exactly one board file upstream, which is the only reason a CAN suite does not build on `qemu_cortex_m3` out of the box. Second, **the code under test must not name its device**. `DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus))` is what lets one body of firmware run against three different controllers without an `#ifdef`; a `DT_NODELABEL(fdcan1)` would have made the emulation impossible and the design worse for the same reason.

`scripts/test.sh` wraps the invocation the same way `build.sh` wraps `west build`:

```sh
./scripts/test.sh                    # every suite
./scripts/test.sh -T tests/commands  # one suite
./scripts/test.sh -s node.protocol   # one identifier
```

---

## 6. Three of the five suites, read closely

### `tests/protocol/` — pure logic

The simplest possible case. `protocol.cpp` depends on nanopb and nothing else, so the test compiles it directly:

```cmake
set(SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../../shared)

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
zephyr_nanopb_sources(app ${CMAKE_CURRENT_SOURCE_DIR}/../../proto/node.proto)

target_sources(app PRIVATE src/main.cpp ${SHARED}/protocol.cpp)
target_include_directories(app PRIVATE ${SHARED})
```

Note what it does **not** do: copy `protocol.cpp`, or re-declare its functions. It compiles the shipped source itself — the same file both applications link, which is why it lives in `shared/` rather than inside either app — and it regenerates the schema from `proto/node.proto` with the same `zephyr_nanopb_sources()` line the firmware uses. A test that drifts from the code it tests is worse than no test, and the cheapest way to prevent drift is to have only one copy of everything.

### `tests/commands/` — using a seam the design already had

`commands.cpp` publishes to `chan_sensor_cmd`, which lives in `sensor.cpp` — a file that cannot compile without the SCD-40. That looks like a blocker, and it is where the temptation to add a mock or an injection point appears.

It is not a blocker, because of a linkage detail: **`ZBUS_CHAN_DECLARE` expands to `extern` declarations.** `app_channels.h` only *declares* the channel; whoever links it supplies the *definition*. In the firmware that is `sensor.cpp`. In the test it is the test:

```c
static bool test_cmd_valid(const void *msg, size_t msg_size)
{
        ARG_UNUSED(msg_size);
        return sensor_cmd_in_range(static_cast<const struct sensor_cmd *>(msg));
}

ZBUS_CHAN_DEFINE(chan_sensor_cmd, struct sensor_cmd, test_cmd_valid, nullptr,
                 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(SENSOR_CMD_TRIGGER, 0));
```

Three things about that are worth dwelling on.

**It is not a mock.** The validator calls `sensor_cmd_in_range()` — the production rule, from `app_channels.h`, next to the bounds it compares against. So when a test asserts that an out-of-range interval is rejected, it is exercising the real predicate through the real `zbus_chan_pub()`, getting a real `-ENOMSG`. A mock would have proved that the test's idea of the rule matches the test's idea of the rule.

**The channel is the seam, and it already existed.** The architecture's boundary between "decide what a command means" and "the sensor thread acts on it" is that channel. Testing across it does not require inventing anything.

**The definition sits at global scope**, outside any anonymous namespace, for the reason [`language-cpp.md`](language-cpp.md) §6 gives: `ZBUS_CHAN_DEFINE` emits symbols another translation unit names, and internal linkage would stop them meeting.

### Order independence in practice

`commands.cpp` remembers the last command's sequence number — module state that outlives an individual test, and Ztest does not run tests in source order. The wrong fix is a `commands_reset_for_test()` in the production header. The right one is to make each test independent:

```c
static uint32_t next_seq(void)
{
        static uint32_t seq = 1000;

        return ++seq;
}
```

Every test takes a fresh sequence; the duplicate-suppression test reuses one deliberately, which is the whole point of that test. No production API exists that only tests call.

### `tests/isotp_loopback/` — testing a path rather than a function

The two suites above compile a `.cpp` of ours and call into it. This one compiles none, and that is what makes it a different *kind* of test worth reading.

What it exercises is not a function but a **path**: this project's CAN framing — an identifier from `can_link.h`, a message-type byte, then an opaque payload — carried over Zephyr's ISO-TP through an emulated controller. Almost every line executing is Zephyr's. The suite's claim is not "our code is correct" but "our code works over the thing it runs on", and those fail in different ways.

That distinction matters because it is where the temptation to over-test lives. §7's rule — assert the promise, not the mechanism — could be read as "so don't test Zephyr's ISO-TP; it has its own conformance suite upstream." True, and beside the point. What is untested is not ISO-TP; it is the **conjunction**: our buffer size against ISO-TP's receive pool, our identifiers against a receive filter, our type byte against where reassembly places the first byte of a First Frame. Each half is fine alone. Nobody had ever run them together.

The concrete example is the buffer bound. The gateway reads into `1 + kRelayUpMax` = 161 bytes; `CONFIG_ISOTP_RX_BUF_COUNT` defaults to 4 buffers of 56, which is 224 and fits; the peer node trims that count to 2 to fit 16 KB of RAM, giving 112, which does not. Both settings are correct because each node reassembles a different direction — and no unit test of any function could have said so, because it is not a property of a function. It is a property of two configuration files and a protocol, and the only way to check it is to run all three.

One hazard in it generalises well beyond CAN. **Two of its tests are negative** — a receiver bound to the wrong identifier hears nothing, a heartbeat filter ignores the ISO-TP identifiers — and a negative test passes for free in an environment where nothing is delivered at all. This suite has a live trap of exactly that kind: `CAN_MODE_LOOPBACK` must be set explicitly before `can_start()`, and without it `can_loopback.c` drops every frame while still returning success, so the two negative tests are the only ones that would still pass. The defence is not vigilance, it is **mutation**: break the thing on purpose and confirm the test notices. Widening the heartbeat filter's mask to `0` must fail the filter test; binding the "wrong" context to the right address must fail the addressing test. A negative test you have never seen fail is not evidence of anything.

---

## 7. What to test, and what not to

The suites here are small — 51 cases across five suites — and chosen on one principle: **test behaviour that something outside this code depends on.**

What that selected:

- **The contract with the host.** Every field of a `Telemetry` survives a round trip; the internal status enum maps to the right wire enum; an `Ack`'s detail truncates at its bound rather than overflowing. A host written against `node.proto` depends on all of it.
- **The decisions that are hard to reason about.** That a warming-up reading is *shorter* on the wire than a full one, because an unset field is not transmitted. That `garbage` is rejected but `hi` is accepted as a structurally legal `Command` with no arm set — and is then caught by dispatch rather than by decoding. Those are the claims in [`protobuf-guide.md`](protobuf-guide.md) §4–§5, turned into assertions so the guide and the firmware cannot drift apart quietly.
- **The behaviour that only shows up under failure.** A rejected command leaves the channel *unchanged* — the atomicity claim in [`zbus-guide.md`](zbus-guide.md) §6. A duplicate is acked but not executed. Both are invisible in normal operation and both are the reason the code is shaped as it is.

What it deliberately left alone:

- **Getters, constructors, and code with no decisions in it.** A test that asserts `x = 5; assert(x == 5)` costs maintenance and buys nothing.
- **Implementation details.** No test here asserts *how many bytes* a specific message encodes to. That number is real and knowable — §3 of the protobuf guide works it out — but pinning it would mean every legitimate schema change breaks a test for no reason. The tests pin that a message *round-trips* and that it *fits its generated bound*, which are the properties anything actually depends on.
- **Anything needing the hardware.** The sensor driver, the MQTT session, the network. §9.

The general form: **assert the promise, not the mechanism.** A test that fails when you refactor, without anything having broken, will eventually be deleted or ignored — and it teaches the team that failing tests are noise, which is a far more expensive problem than the bug it might have caught.

---

## 8. Why this stops being optional

Everything above is an engineering argument: tests are worth it if they pay for themselves. In safety-regulated development the calculation is different, and it is worth knowing why because it changes what the tests are *for*.

A medical device running Class IIa software falls under **IEC 62304**, the software lifecycle standard. It does not merely encourage testing; it requires that software units be verified, that the verification be documented, and — the part people underestimate — that there is **traceability**: a demonstrable chain from a requirement, to the design that implements it, to the code, to the test that shows the code does it. An auditor asks "how do you know this requirement is met?" and expects a specific test to be the answer.

Two consequences follow for how you write the tests:

- **A test's name and message are documentation.** `test_out_of_range_interval_is_rejected_atomically` states a requirement. `test_case_3` states nothing, and cannot be traced to anything.
- **The suite's value is mostly regression, not discovery.** Its job is to make change safe years later, when the person changing the code is not the person who wrote it — which is exactly the situation when inheriting a codebase from someone who has left. A regression suite is the cheapest way to find out whether you have understood something.

None of this makes the technical guidance different. It raises the stakes on §7: under a regulated lifecycle, a test suite that cries wolf is not merely annoying, it is a process that stops being followed.

---

## 9. The whole picture, and its edges

```
   ┌──────────────────────────────────────────────────────────────┐
   │  no hardware — scripts/test.sh, ~31 s                         │
   │                                                              │
   │   tests/protocol/  ──▶ protocol.cpp  the wire format          │
   │   tests/commands/  ──▶ commands.cpp  dispatch, dedupe, bounds │
   │                        + a test-owned chan_sensor_cmd         │
   │   tests/heartbeat/ ──▶ can_link.h    the hand-packed frame    │
   │   tests/relay/     ──▶ relay.h       the liveness machine     │
   │   tests/isotp_loopback/              the CAN transport,       │
   │                        ──▶ an emulated controller in the      │
   │                            suite's own devicetree overlay     │
   └──────────────────────────────────────────────────────────────┘
                                │
                   everything below needs the bench
                                │
   ┌──────────────────────────────────────────────────────────────┐
   │  the board  ──▶  sensor.cpp   I²C, timing, the driver         │
   │             ──▶  main.cpp     sockets, keepalive, reconnect   │
   │  the broker ──▶  the wire     QoS, retain, the Last Will      │
   └──────────────────────────────────────────────────────────────┘
```

Host tests cannot catch, and never will:

- **Timing and concurrency.** That the sensor thread keeps sampling through a 30 s reconnect backoff. That a zbus listener running in the publisher's context does not block it.
- **The hardware itself.** That the SCD-40 is wired to the right pins, that its 30 ms power-up is respected, that `sensor_sample_fetch()` returns stale data at a fast poll.
- **Anything above the socket.** That a QoS 1 command is redelivered after a lost PUBACK. That the broker publishes the Last Will on an unclean disconnect.
- **Anything electrical.** An emulated CAN controller runs the whole protocol and none of the physics: differential levels, 120 Ω termination, a common ground between separately-powered boards, arbitration between two real transmitters, and the in-frame acknowledgement that makes a node alone on a bus unable to transmit at all. This is the sharpest example of the edge, because the emulation is convincing enough to hide it.

Those are exactly what the labs at the end of the other guides cover — the hardware half of the same job, run by hand today. The natural next step is to automate them from the Pi, which already has the broker, the harness and a cable to the board: a **hardware-in-the-loop** suite driving `command.py` and asserting on `monitor.py` output. The split in §2 is what makes that tractable — the HIL suite only has to cover what host tests structurally cannot, which is a much smaller set than "everything".

---

## 10. Exercising the tests

The suite runs on the Mac with nothing attached.

### Exercise 1 — Run it

```sh
./scripts/test.sh
```

```
INFO    - 5 of 5 executed test configurations passed (100.00%), 0 built (not run), 0 failed
INFO    - 51 of 51 executed test cases passed (100.00%)
```

About thirty seconds, most of it building five full Zephyr images. **Proves:** the whole loop works with no board, no broker and no sensor — which is the entire point of §1.

### Exercise 2 — Make it fail

A suite you have never seen fail is a suite you have no reason to trust. Break one assertion on purpose — in `tests/commands/src/main.cpp`, change the atomicity check to expect a value the code will never produce:

```c
zassert_equal(sc.interval_ms, 999, "a rejected publish must leave the channel unchanged");
```

```
    Assertion failed at CMAKE_SOURCE_DIR/src/main.cpp:179:
    commands_test_out_of_range_interval_is_rejected_atomically: (sc.interval_ms not equal to 999)
a rejected publish must leave the channel unchanged
 FAIL - test_out_of_range_interval_is_rejected_atomically in 0.002 seconds
```

Revert it afterwards. **Proves:** failures are reported with the file, the line, the values and your message — and that the assertion was genuinely running rather than silently skipped. Note the log line printed just before it: that context is why §5's `prj.conf` leaves logging on.

### Exercise 3 — Watch a real bug get caught

Break the *code* instead of the test. In `shared/protocol.cpp`, map the warming-up status to the wrong wire enum:

```c
case SENSOR_READING_WARMING_UP:
        msg.sensor_status = node_SensorStatus_SENSOR_STATUS_ERROR;   /* was WARMING_UP */
```

Re-run. `test_telemetry_status_mapping` fails and names the case. Revert.

**Proves:** the test is anchored to the firmware, not to a copy of it — the CMakeLists compiles `shared/protocol.cpp` itself. It also shows the class of bug these tests exist for: nothing crashes, nothing looks wrong on the node, and the host quietly displays a sensor fault that never happened.

### Exercise 4 — Prove a negative test can fail

A test that asserts something *does not* happen also passes when nothing happens at all, which is why §6 insists on mutating them. `tests/isotp_loopback/` has two, and both are one edit away from being checked.

In `tests/isotp_loopback/src/main.cpp`, widen the heartbeat filter in `test_the_heartbeat_filter_ignores_the_isotp_identifiers` so it matches everything:

```c
.mask = 0,   /* was CAN_STD_ID_MASK */
```

and in `test_a_receiver_bound_elsewhere_hears_nothing`, bind the "wrong" context to the address the transfer is actually sent to:

```c
int rc = isotp_bind(&wrong_ctx, can_dev, &id_to_gateway, &id_to_peer, &fc_opts, K_MSEC(200));
```

```
 FAIL - test_a_receiver_bound_elsewhere_hears_nothing
 FAIL - test_the_heartbeat_filter_ignores_the_isotp_identifiers
```

Revert both. **Proves:** each test fails for the reason it claims to check, rather than because the environment delivers nothing. Try it once with the `can_set_mode(can_dev, CAN_MODE_LOOPBACK)` line in `setup()` removed as well, and watch both mutations pass: with the mode bit clear the driver delivers nothing, so a negative test cannot fail no matter what you break. That is the failure this exercise exists to rule out.

### Exercise 5 — Add one

Pick a promise nothing currently asserts and write it. A good candidate: `Ack.detail` is bounded at 48 bytes, and `commands.cpp` writes `"interval %u outside [%u,%u]"` into it with `snprintf`. What happens at `UINT32_MAX` for all three numbers — does the message truncate cleanly, and is it still a valid string?

```c
ZTEST(commands, test_rejection_detail_survives_extreme_values)
{
        /* ... */
}
```

**Proves:** more than the assertion does. Writing a test forces you to say precisely what the code promises, and the useful outcome is often discovering that you cannot state it.

---

## 11. The model in one paragraph

Firmware is awkward to test because the target is slow, physical and singular — but most firmware logic never needed the target, and the job is to arrange the code so the decisions are separable from the I/O. The wrong way is to carve seams into functions so tests can peer inside; the right way is to notice that untestable code is usually code whose *dependencies* are tangled, and separate by responsibility instead — a change that makes the code easier to explain, with testability as a side effect. **Ztest** provides suites, tests and assertions whose messages are the report you will actually read; **Twister** finds each suite, builds it as a complete standalone Zephyr application, and runs it. Where it runs is a real trade: a real board is total fidelity at seconds per cycle, `native_sim` is milliseconds but Linux-only, and **QEMU sits in between** — same cross-compiler as the firmware, real kernel, and no peripherals except the emulated ones a suite adds to its own devicetree — which is why this repo uses it. Test the promises other code depends on, not the mechanism, because a test that breaks on every refactor teaches people to ignore failures. And accept the edge: timing, hardware and the network are structurally out of reach here, which is what a hardware-in-the-loop stage is for.

## 12. Where to go next

- **[`test-strategy.md`](../docs/test-strategy.md)** — the reference half: what this project tests, what it deliberately does not, and why `qemu_cortex_m3`.
- **The labs in the other guides** — [`communication-guide.md`](communication-guide.md) §9, [`can-guide.md`](can-guide.md) §10, [`zbus-guide.md`](zbus-guide.md) §10, [`sensor-api-guide.md`](sensor-api-guide.md) §11, [`protobuf-guide.md`](protobuf-guide.md) §12, [`zephyr-build-system-guide.md`](zephyr-build-system-guide.md) §12 and [`language-cpp.md`](language-cpp.md) §12 are the hardware half of this job, run by hand. They are the specification for a future HIL suite.
- **Hardware-in-the-loop from the Pi** — it already has the broker, `command.py`, `monitor.py` and a cable to the board. The gap between §9's two boxes is the work.
- **CI** — the suites are the hard part and they exist now; a GitHub Actions workflow that builds the firmware and runs `scripts/test.sh` on every push is mostly plumbing. Note that a Linux runner can use `native_sim` and finish in a fraction of the time.
- **Zephyr's own tests** — `~/zephyr-workspace/zephyr/tests/` is thousands of worked examples. `tests/subsys/zbus/` is the closest to this repo's concerns.
- **`ztress`** — Zephyr's concurrency stress helper, for the day a test needs to provoke races rather than check logic.
