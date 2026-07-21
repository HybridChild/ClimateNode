# The guides

Six teaching documents, each building one concept from first principles and using this
repo — a CO₂ node on a Nucleo-H753ZI talking to a Raspberry Pi — as the running example.
They are meant to be readable by someone who has never seen this project.

Every guide has a **reference half** in [`../docs/`](../docs/) recording what *this* project
decided and why. The guides explain concepts; the references record facts. Each links to
the other, and the pairing is listed in the table below.

## Where to start

There is no need to read all six, and reading them in file order is the wrong order. The
dependencies are real but shallow:

```
zephyr-build-system-guide  ─┬─▶  sensor-api-guide  ─┐
   devicetree, Kconfig      │      the SCD-40       │
                            │                       ├─▶  zbus-guide
                            └─▶  language-cpp       │     the internal bus
                                  orthogonal        │
                                                    │
        communication-guide  ──▶  protobuf-guide  ──┘
          MQTT, the broker         the wire format
```

**If you are new to Zephyr, start at the top left.** `zephyr-build-system-guide.md` is the
one whose absence is most felt elsewhere: `sensor-api-guide.md` §2.2 assumes you know what
a devicetree overlay does, and `language-cpp.md` assumes you know what `prj.conf` is for.

**If you already know Zephyr and came for the networking**, start at the bottom left
instead. `communication-guide.md` and `protobuf-guide.md` are a self-contained pair that
never touch devicetree — read them in that order, because Protobuf §4's argument (the type
is not on the wire, so the *topic* asserts it) needs MQTT topics to already mean something.

**If you are chasing a bug**, go straight to the lab at the end of the relevant guide.
Each is a numbered set of exercises with expected output and a **Proves:** line, and
`sensor-api-guide.md` §11 Exercise 1 in particular is written as a pipeline bisect.

## The six

| Guide | Answers | Assumes you know | Reference half |
|---|---|---|---|
| [`zephyr-build-system-guide.md`](zephyr-build-system-guide.md) | Why is a Zephyr build a configuration system rather than a compiler? | C, and what a compiler and linker do | [`build-system-overview.md`](../docs/build-system-overview.md) |
| [`sensor-api-guide.md`](sensor-api-guide.md) | How does `sensor_sample_fetch()` become I²C traffic, and why two integers instead of a float? | devicetree basics (the guide above), roughly what I²C is | [`sensor-bringup.md`](../docs/sensor-bringup.md) |
| [`zbus-guide.md`](zbus-guide.md) | What is an in-process message bus, and why does a two-thread firmware want one? | threads and blocking calls; `poll()` and mutexes are explained in §1–§2 | [`app_channels.h`](../firmware/src/app_channels.h) header comment |
| [`communication-guide.md`](communication-guide.md) | How does one reading get from the sensor to the host PC, and why a broker? | TCP/IP exists; no MQTT knowledge needed | [`mqtt-design.md`](../docs/mqtt-design.md) |
| [`protobuf-guide.md`](protobuf-guide.md) | How does a reading become bytes, and what does a schema actually buy? | binary/hex, and MQTT topics (the guide above) | [`node.proto`](../proto/node.proto), decisions inline |
| [`language-cpp.md`](language-cpp.md) | What actually changes when a Zephyr app is C++? | C++ basics; the freestanding/boundary consequences are the subject | *(none — the two source files are the reference)* |

Two of the six pair with a **source file** rather than a `docs/` page, because in those
cases the decisions belong next to the thing they constrain: the field-numbering rules live
in the schema, and the observer-kind choice lives in the header both threads include.

## How each guide is built

They share a skeleton, so you can navigate any of them the same way:

- an opening **roadmap** of the sections, so you can skip to what you need;
- numbered concept sections, each building on the one before;
- a **synthesis** — an end-to-end trace or cheat sheet — where the guide has one;
- a **lab**: numbered exercises with commands, expected output, and what each proves;
- **the model in one paragraph**, and **where to go next**.

Cross-references are by section number (`§7`) within a guide and by name across guides.
Code in this repo is cited by function name because it moves; Zephyr's own code is cited by
`file:line` against v4.4.1, which is pinned in `~/zephyr-workspace`.

## What is not here

`../docs/` holds the project references, plus [`toolchain.md`](../docs/toolchain.md) (both
toolchains and the build/flash workflow) and
[`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md) — a guided reading of
`firmware/src/main.cpp` that connects most of these concepts in one file. It teaches, but it
tracks this repo's code rather than a general concept, so it lives with the references.
