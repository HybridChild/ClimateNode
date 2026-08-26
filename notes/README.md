# The guides

Ten teaching documents, each building one concept from first principles and using this repo — a Nucleo-H753ZI sensor node and CAN gateway, a Nucleo-F072RB peer node behind it, and a Raspberry Pi host — as the running example. They are meant to be readable by someone who has never seen this project.

Most guides have a **reference half** in [`../docs/`](../docs/) recording what *this* project decided and why. The guides explain concepts; the references record facts. Each links to the other, and the pairing is listed in the table below. One guide pairs with a source file instead, and two (`language-cpp.md`, `shell-guide.md`) have no reference half at all — there was no project *decision* to record — as the table's last column notes.

## Where to start

Reading them in file order is the wrong order, and you rarely need all ten. Two things below help: **a recommended order for reading them coherently, end to end**, and **shortcuts** for when you only want a slice.

### The coherent order

Read them along the path one reading travels — from the sensor that produces it to the host that decodes it — with the foundation first and the cross-cutting concerns last. Each step is placed so the ones before it have already supplied what it assumes:

1. **[`zephyr-build-system-guide.md`](zephyr-build-system-guide.md)** — the foundation. A Zephyr build is a *configuration system* (devicetree + Kconfig) before it is a compiler, and almost every guide below assumes you know what an overlay and a `prj.conf` option are.
2. **[`language-cpp.md`](language-cpp.md)** — *orthogonal; read here or skip.* What actually changes because the app is C++ rather than C. It stands alone; take it now if you intend to read the source, later or never if not.
3. **[`sensor-api-guide.md`](sensor-api-guide.md)** — where a reading begins: the SCD-40 over I²C, and why a measurement arrives as two integers instead of a float.
4. **[`communication-guide.md`](communication-guide.md)** — the reading's path to the host: MQTT, the broker, and why publish/subscribe rather than a direct socket. The spine of the whole comms story.
5. **[`protobuf-guide.md`](protobuf-guide.md)** — what the bytes on that path *mean*. It goes after #4 because its central argument — the type is not on the wire, so the *topic* asserts it — needs MQTT topics to already mean something.
6. **[`network-stack-guide.md`](network-stack-guide.md)** — the floor beneath that path: how `prj.conf` and one devicetree node become the socket MQTT rides, with no app code, and how portable that is to USB or Wi-Fi. It lifts exactly the black box #4 sets down ("everything below MQTT, Zephyr implements for you"), so it wants a little of #4 first.
7. **[`can-guide.md`](can-guide.md)** — the *other* transport, and the one that takes back what MQTT gave. A bus with no addresses, no connection and eight payload bytes, so framing, liveness and acknowledgement all have to be re-earned. It goes after #4–#6 because almost every section argues against the IP/MQTT side, and reads best once that side is familiar.
8. **[`zbus-guide.md`](zbus-guide.md)** — the one piece *inside* the firmware: the in-process bus where the sensor thread hands each reading to the MQTT thread, and the relay hands over a peer's. It lands best now, once you have seen the threads it joins.
9. **[`shell-guide.md`](shell-guide.md)** — *off to the side.* The `uart:~$` console used to poke at everything above (`sensor get`, `net iface`); `sensor-api-guide.md` §7 and the `net` shell in #4 both rest on it. Read it whenever the prompt itself is the mystery.
10. **[`testing-guide.md`](testing-guide.md)** — deliberately last. It is the only guide that argues how the firmware must be *arranged* to be tested off-target, and that argument only lands once you have seen what the pieces do.

### The dependencies, at a glance

The order above is a single thread through this lattice. Arrows mean "assumes"; the two independent roots are `zephyr-build-system-guide` (the Zephyr/sensor side) and `communication-guide` (the networking side), and they converge at `zbus-guide`, where the two threads meet.

```
                       ┌─▶ sensor-api-guide ──────────────────────────┐
zephyr-build-system ───┤     the SCD-40                               │
  devicetree, Kconfig  ├─▶ language-cpp   (orthogonal)                ├─▶ zbus-guide ─▶ testing-guide
  the foundation       └─▶ shell-guide    (off to the side)           │     the two        (last: how it's tested)
                                                                      │     threads meet    
         communication-guide ─┬─▶ protobuf-guide ─────────────────────┘
           MQTT, the broker   │     the wire format
                              ├─▶ network-stack-guide
                              │     the socket beneath MQTT
                              │     (also needs the build-system guide)
                              └─▶ can-guide
                                    the other transport, argued
                                    against this one throughout
```

### Shortcuts

- **New to Zephyr?** Start at #1 and go down. `zephyr-build-system-guide.md` is the one whose absence is felt most: `sensor-api-guide.md` §2.2 assumes you know what a devicetree overlay does, and `language-cpp.md` assumes you know what `prj.conf` is for.
- **Came only for the networking?** Read #4 → #5 as a self-contained pair — they never touch devicetree — then drop to #6 for the plumbing beneath them. That is the quickest route to the MQTT, QoS, and schema material.
- **Chasing a bug?** Go straight to the lab at the end of the relevant guide. Each is a numbered set of exercises with expected output and a **Proves:** line, and `sensor-api-guide.md` §11 Exercise 1 is written as a pipeline bisect. If the wire format, command handling or the CAN link contract is the suspect, `./scripts/test.sh` answers in about half a minute with nothing plugged in.

## The ten

The rows are in the reading order above (#1–#10), so the column you scan for "can I start here?" is **Assumes you know**. It digests each guide's own `Prerequisites:` line, which is fuller — it also says what you can safely skip.

| # | Guide | Answers | Assumes you know | Reference half |
|---|---|---|---|---|
| 1 | [`zephyr-build-system-guide.md`](zephyr-build-system-guide.md) | Why is a Zephyr build a configuration system rather than a compiler? | C, and what a compiler and linker do | [`build-system-overview.md`](../docs/build-system-overview.md) |
| 2 | [`language-cpp.md`](language-cpp.md) | What actually changes when a Zephyr app is C++? | C++ basics; the freestanding/boundary consequences are the subject | *(none — the source files are the reference)* |
| 3 | [`sensor-api-guide.md`](sensor-api-guide.md) | How does `sensor_sample_fetch()` become I²C traffic, and why two integers instead of a float? | devicetree basics (the build-system guide), roughly what I²C is | [`sensor-bringup.md`](../docs/sensor-bringup.md) |
| 4 | [`communication-guide.md`](communication-guide.md) | How does one reading get from the sensor to the host PC, and why a broker? | TCP/IP exists; no MQTT knowledge needed | [`mqtt-design.md`](../docs/mqtt-design.md) |
| 5 | [`protobuf-guide.md`](protobuf-guide.md) | How does a reading become bytes, and what does a schema actually buy? | binary/hex, and MQTT topics (the guide above) | [`node.proto`](../proto/node.proto), decisions inline |
| 6 | [`network-stack-guide.md`](network-stack-guide.md) | How does `prj.conf` plus one devicetree node become a live socket with no app code — and how portable is that to USB/Wi-Fi? | devicetree & Kconfig (the build-system guide); enough of the communication guide that MQTT rides on a socket | [`network-bringup.md`](../docs/network-bringup.md) |
| 7 | [`can-guide.md`](can-guide.md) | What is a bus with no addresses, no connection and eight payload bytes — and what must a protocol do about that? | roughly what a bit on a wire is; the MQTT comparisons in §8–§9 read better after #4 | [`can-bringup.md`](../docs/can-bringup.md) |
| 8 | [`zbus-guide.md`](zbus-guide.md) | What is an in-process message bus, and why does a multi-threaded firmware want one? | threads and blocking calls; `poll()` and mutexes are explained in §1–§2 | [`zbus-design.md`](../docs/zbus-design.md), plus the header comments |
| 9 | [`shell-guide.md`](shell-guide.md) | What is the `uart:~$` prompt, and how does typing `net iface` call a function inside the firmware? | what a Kconfig option is (the build-system guide); the rest is built up | *(none — a stock subsystem, no project decision to record)* |
| 10 | [`testing-guide.md`](testing-guide.md) | How do you test firmware on a machine that is not the target — and what has to be true of the code first? | roughly what a unit test is; the rest is built up | [`test-strategy.md`](../docs/test-strategy.md) |

One of the ten pairs with a **source file** rather than a `docs/` page, because there the decisions belong next to the thing they constrain: the field-numbering rules live in the schema. `zbus-guide.md` has both — a page for what spans the whole bus, and header comments for what belongs beside one channel.

## How each guide is built

They share a skeleton, so you can navigate any of them the same way:

- a **`Prerequisites:`** line — what you need before starting, and as often what you explicitly do *not*;
- an opening **roadmap** of the sections, so you can skip to what you need;
- numbered concept sections, each building on the one before;
- a **synthesis** — an end-to-end trace or cheat sheet — where the guide has one;
- a **lab**: numbered exercises with commands, expected output, and what each proves;
- **the model in one paragraph**, and **where to go next**.

Cross-references are by section number (`§7`) within a guide and by name across guides. Code in this repo is cited by function name because it moves; Zephyr's own code is cited by `file:line` against v4.4.1, which is pinned in `~/zephyr-workspace`.

## What is not here

`../docs/` holds the project references — the terse, decision-recording other half of each guide here; [`../docs/README.md`](../docs/README.md) is their map, with the pairings pointing back to these guides. Two references have no guide: [`toolchain.md`](../docs/toolchain.md) (both toolchains and the build/flash workflow) and [`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md) — a guided reading of `gateway/src/main.cpp` that connects most of these concepts in one file. It teaches, but it tracks this repo's code rather than a general concept, so it lives with the references.
