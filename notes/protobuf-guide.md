# Understanding Protobuf and nanopb

*A from-first-principles guide to how a sensor reading becomes bytes on the wire, and back.*

A teaching document. It explains the concepts in the order that makes them easiest to learn, using this repository's contract — [`proto/node.proto`](../proto/node.proto) — as the running example.

Every byte sequence below is verifiable, and was re-checked with `protoc --decode_raw` while writing this. The `Command` byte strings are captures from packets this bench actually sent; the `Telemetry` breakdown is reconstructed field by field from a logged reading, and reproduces the exact length the node reported on the console.

**Prerequisites:** binary and hex, and MQTT topics already meaning something ([`communication-guide.md`](communication-guide.md) §4) — §4's central argument turns on the topic being what asserts a payload's type. No Protobuf knowledge: the encoding is taken apart byte by byte in §2–§3.

**The shape of this document:**

- **§1** — what a schema buys you that text and a hand-rolled struct do not.
- **§2–§3** — the encoding itself: tags, varints and wire types, then a real captured packet taken apart byte by byte.
- **§4** — the fact that surprises people most, and the one §8 turns into a real failure mode: *nothing on the wire says which message this is.*
- **§5–§8** — four more things that surprise people: absent defaults, `oneof`, evolution (the actual point of the format), and what it deliberately does *not* protect you from.
- **§9–§11** — nanopb, this firmware, and the two generators that share the contract.
- **§12** — exercises: most reproducible from a terminal alone, the rest on the node.
- **§13–§14** — the whole model in a paragraph, and where to go next.

For how those bytes get carried, see [`communication-guide.md`](communication-guide.md) (MQTT concepts) and [`mqtt-design.md`](../docs/mqtt-design.md) (this project's topic and QoS decisions). For the surrounding firmware, see [`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md).

---

## 1. Why a schema at all

The cheapest thing a node can publish is text:

```
seq=1 co2=812 temp=22.41 rh=41.3
```

That is readable, debuggable with `mosquitto_sub`, and needs no tooling at all. It is also a contract that exists only in the heads of the people who wrote both ends. Nothing stops the host from expecting `co2_ppm=` while the node sends `co2=`. Nothing records that `temp` is Celsius. Adding a field means every parser must be updated in lockstep, and a typo becomes a runtime surprise rather than a build error.

A schema moves that contract into a file both sides generate from. The trade is readability on the wire for **a single definition neither side can silently diverge from** — which is why `proto/` is described as the source of truth and why generated code is never hand-edited.

Protobuf adds three things beyond "an agreed format":

1. **Compactness.** A real reading — with a five-digit sequence number, an uptime, a schema version and a status, none of which the text line above carried — encodes to **26 bytes** (§5). That text line, with the same sequence number substituted in, is 36 characters, and it says less; the equivalent JSON object is 149 bytes. Useful, but the least interesting of the three.
2. **Typed fields.** `float` vs `uint32` is decided once, in one place, rather than re-parsed and re-guessed by every reader.
3. **Defined evolution rules.** The part that actually matters, covered in §7 — and the reason Protobuf exists at all rather than "a struct we both memcpy".

A useful way to hold the last point: text and JSON let *anything* change and hope both sides notice; a hand-rolled binary struct lets *nothing* change without breaking. Protobuf picks a precise middle — a specific list of changes that are guaranteed safe, and a specific list that are not.

---

## 2. The wire format

### A message is just fields, back to back

A Protobuf message is a flat sequence of **fields**, each preceded by a tag:

```
[tag][value][tag][value][tag][value]...
```

There is no message header, no field count, no terminator, and — importantly — **no magic number and no checksum**. A message is fields, one after another, until the buffer ends. Everything the decoder needs in order to walk it comes from the tags themselves.

That has a consequence worth stating early, because §4 turns on it: **the encoding cannot tell you whether it is looking at the right kind of message.** It can only tell you whether the bytes are structurally walkable.

Note that the shape is *tag-value*, not tag-length-value: only some field types carry an explicit length, and the rest are self-delimiting. Which ones, and why, is the next three subsections.

### Varints: the encoding everything else leans on

Integers — including the tags — are encoded as **varints**: seven bits at a time, lowest group first, with the top bit of each byte meaning "another byte follows".

Decoding `ac 06` by hand:

```
ac = 1010 1100   continuation bit set, payload 010 1100 = 44
06 = 0000 0110   continuation bit clear, payload 000 0110 = 6   (last byte)

value = 44 | (6 << 7) = 44 + 768 = 812
```

Small numbers are small, which is the whole point:

| Value | Encoding | Bytes |
|---|---|---|
| 1 | `01` | 1 |
| 42 | `2a` | 1 |
| 812 | `ac 06` | 2 |
| 26404 | `a4 ce 01` | 3 |
| 654128137 | `89 e8 f4 b7 02` | 5 |

A `uint32` therefore costs between 1 and 5 bytes depending on its magnitude, and a varint is **self-delimiting**: a decoder knows where it ends without being told a length.

Two gotchas that explain choices in `node.proto`:

- **Negative `int32` is the worst case in the format.** It is sign-extended to 64 bits before encoding, so `-1` takes **ten** bytes. If you need negatives, use `sint32`, which applies zigzag encoding (`0, -1, 1, -2, 2 → 0, 1, 2, 3, 4`) so small magnitudes stay small either side of zero.
- **This schema sidesteps it entirely.** Every integer here is `uint32`: counters, `co2_ppm`, `interval_ms`, `uptime_ms` — all naturally non-negative. Temperature, which genuinely goes below zero, is a `float` and so does not use varints at all.

### The tag: field number and wire type in one varint

Each field's tag is a single varint packing two things:

```
tag = (field_number << 3) | wire_type
```

The low three bits are the **wire type**, which tells a decoder *how to read the next bytes* even if it has never heard of the field number:

| Wire type | Meaning | Used by |
|---|---|---|
| 0 | varint | `uint32`, `int32`, `bool`, enums |
| 1 | 64-bit fixed | `double`, `fixed64` |
| 2 | length-delimited | `string`, `bytes`, nested messages, packed repeated |
| 3, 4 | start/end group | deprecated, never emitted by proto3 |
| 5 | 32-bit fixed | `float`, `fixed32` |
| 6, 7 | — | **invalid** — remember this, it explains §8 |

**This is why field numbers 1–15 matter.** The tag is itself a varint, and a one-byte varint holds values up to 127. Field 15 produces tags in the range `120`–`127`, whatever its wire type — still one byte. Field 16 with wire type 0 is already `128`, which needs two. So every field numbered 1–15 saves a byte on *every single message that carries it*.

`Telemetry` is the high-rate message here, so `node.proto` deliberately keeps all eight of its fields inside that range. `Command`'s `oneof` arms start at 10 and still fit; the numbering leaves 3–9 free for future fields common to every command (§6).

Reading the tags in the packet below:

```
0x08 >> 3 = 1   0x08 & 7 = 0     field 1, varint
0x62 >> 3 = 12  0x62 & 7 = 2     field 12, length-delimited
```

### How each wire type says where its value ends

The tag alone is enough to skip a field, because each wire type is self-describing about its own length:

| Wire type | Value layout | Length known from |
|---|---|---|
| 0 varint | `[varint]` | the continuation bits |
| 1 64-bit | `[8 bytes]` | fixed by the type |
| 2 length-delimited | `[length varint][that many bytes]` | the explicit length prefix |
| 5 32-bit | `[4 bytes]` | fixed by the type |

Fixed-width types are little-endian IEEE-754 for `float`/`double`. `22.41` encodes as `ae 47 b3 41` — that is `0x41b347ae` read back the other way.

Note the asymmetry: **a `float` never gets smaller.** A `uint32` of 812 costs two bytes, but a `float` of 22.41 costs four regardless of value. That is the price of not having to agree on a fixed-point scale factor, and it is why the choice between `uint32 co2_ppm` and `float co2_ppm` is a real one.

Wire type 2 is the interesting one. **A nested message is encoded exactly like a `bytes` field**: a length, then that many bytes, which happen to be a complete Protobuf message. Nothing marks it as a message rather than a string. This is what makes the whole format recursive with no extra machinery — and what lets a decoder skip an unknown nested message as easily as an unknown integer.

### Order, duplicates, and unknown fields

Three rules that follow from "just fields, back to back", and that matter when debugging:

- **Field order is not guaranteed.** Encoders normally emit fields in number order (nanopb and `protoc` both do), but a decoder must accept any order, and must not depend on it.
- **A repeated non-repeated field is legal.** If the same scalar field appears twice, the last occurrence wins. For a nested message, the occurrences are merged.
- **Unknown fields are skipped, not fatal.** The wire type in the tag is exactly enough information to step over a field the decoder has never heard of. This is the mechanism behind every evolution rule in §7.

---

## 3. Taking a real message apart

§2 gave you everything needed to decode Protobuf by hand: tags carry a field number and a wire type, and each wire type says where its value ends. That is genuinely the whole format — so here it is applied to real bytes this bench sent.

When the bench ran `command.py info`, the tool reported `info (10 bytes)`. Those bytes are:

```
08 01 10 89 e8 f4 b7 02 62 00
```

Taken apart:

| Bytes | Tag | Field | Wire type | Meaning |
|---|---|---|---|---|
| `08 01` | `0x08` | 1 | 0 (varint) | `schema_version = 1` |
| `10 89 e8 f4 b7 02` | `0x10` | 2 | 0 (varint) | `sequence = 654128137` |
| `62 00` | `0x62` | 12 | 2 (length-delimited) | `get_device_info`, length 0 |

The same command with `--sequence 42` was **6 bytes**:

```
08 01 10 2a 5a 00
```

Identical structure. The sequence collapsed from 5 bytes to 1 because varints are magnitude-sensitive, and the arm is field 11 (`trigger_measurement`) instead of 12, so `0x5a` rather than `0x62`.

Note what the empty message costs: **two bytes**, a tag and a zero length. Its *presence* is the whole instruction — exactly as `node.proto` claims when it says an empty message "is the instruction". Adding a parameter to it later is an additive change (§7), so the zero-byte body is not a corner cut, it is room left open.

You can take apart any captured payload the same way:

```sh
printf '\x08\x01\x10\x2a\x5a\x00' | protoc --decode_raw
```

```
1: 1
2: 42
11: ""
```

`--decode_raw` needs no schema — it recovers field numbers and wire types straight from the bytes, which is exactly what a decoder does before it knows what the fields *mean*. Note what it cannot recover: names, and the difference between an empty nested message and an empty string. Both are wire type 2 with length 0.

---

## 4. Nothing on the wire says *which* message this is

A natural question after that walkthrough: those bytes were a `Command` — but where does the encoding say so?

It does not. **A serialised message carries no type identity at all** — no name, no type ID, no header. `Telemetry`, `Command` and `Ack` are all just field lists, and this schema deliberately numbers `schema_version = 1` and `sequence = 2` the same way in all three, so their first bytes are literally identical.

The six-byte command above therefore decodes cleanly as any of them:

```
$ printf '\x08\x01\x10\x2a\x5a\x00' | protoc --decode=node.Command --proto_path=proto proto/node.proto
schema_version: 1
sequence: 42
trigger_measurement {
}

$ printf '\x08\x01\x10\x2a\x5a\x00' | protoc --decode=node.Telemetry --proto_path=proto proto/node.proto
schema_version: 1
sequence: 42
11: ""
```

Exit status 0 both times. As a `Telemetry`, field 11 is simply unknown and gets skipped (§7) — the very mechanism that makes evolution work also makes a wrong guess about the type look successful.

**The type is always supplied from outside the bytes.** `--decode=node.Command` is you asserting it. In the firmware the assertion is the descriptor table you pass in:

```c
pb_decode(&stream, node_Command_fields, &cmd);   /* "read these bytes as a Command" */
```

Nothing in the payload can contradict that argument. So every Protobuf system needs a type discriminator somewhere, and there are three usual places to put it:

| Where the type lives | Cost | Used by |
|---|---|---|
| Out-of-band context | zero bytes | **this project** — the MQTT topic; also gRPC's method name, or a dedicated socket/port |
| An envelope message with a `oneof` over every type | tag + length per message | most single-channel protocols |
| `google.protobuf.Any` | a type-URL *string* per message | dynamic/plugin systems |

This project uses the first. `node/<id>/command` carries a `Command`, `node/<id>/telemetry` a `Telemetry`, `node/<id>/ack` an `Ack` — one message type per topic, never mixed. That is why the topic hierarchy in [`mqtt-design.md`](../docs/mqtt-design.md) is part of the *contract* and not merely housekeeping: it is the half of the wire format that the `.proto` file does not contain.

The catch is that this makes routing load-bearing. Anything that publishes to `node/1/command` is asserting "this is a Command", and the node has no way to check the claim — it decodes whatever arrives as a `Command` and acts on it. That is the failure §8 comes back to.

---

## 5. What is not on the wire

This is the biggest conceptual trap in proto3.

**Fields equal to their default value are not transmitted at all.** A `uint32` that is 0, a `float` that is 0.0, a `string` that is empty, an enum that is 0 — all are simply absent from the encoded bytes. The decoder fills them in from the defaults afterwards.

So the 26-byte telemetry above is not a fixed layout at all. A reading with `co2_ppm = 0` would be 23 bytes, and a `Telemetry` with every field defaulted encodes to **zero bytes** — a perfectly valid message with no content whatsoever.

Two consequences worth internalising:

**Absence and zero are indistinguishable for scalars.** A decoder receiving no `co2_ppm` field yields `co2_ppm == 0`, and cannot tell "the node said zero" from "the node said nothing". That is why `SENSOR_STATUS_UNSPECIFIED = 0` exists in `node.proto` with a comment saying it must mean "no information" — the zero value of every enum doubles as the value you get from a message that never mentioned it. Give it a meaning that is safe as a default, and never assign 0 to a real state.

**Message fields are different.** A nested message *can* distinguish absence, because presence is encoded by the tag existing at all — even a zero-length one (§3). That is why `Ack.device_info` gets a real presence flag in the generated C:

```c
bool has_device_info;
node_DeviceInfo device_info;
```

and why `command.py` can ask `ack.HasField("device_info")`. Scalars have no `HasField` in proto3 unless explicitly marked `optional` — which is the subject of the next section, because this project reached the point where it needed one.

### Explicit presence, and what it costs

Mark a scalar `optional` and proto3 restores a real presence bit:

```proto
optional uint32 co2_ppm = 4;
```

nanopb generates the same shape it already gave `Ack.device_info` — a `has_` flag beside the value — and the Python runtime grows a `HasField("co2_ppm")` that previously did not exist for scalars. **Nothing about the wire format changes.** The field number is still 4, the wire type is still varint, and old and new readers parse each other's bytes without knowing which side used `optional`. What changes is a *sender's* rule about when to write the field: an unset field is still omitted, but a field explicitly set to zero is now written out in full.

That last clause is the cost, and it is worth being precise about it, because "proto3 does not transmit defaults" quietly becomes false in the way people usually remember it. The rule was never "zeros are free"; it was "*unset* fields are free", and without explicit presence those two were the same sentence. With it they come apart:

| Sender's intent | Without `optional` | With `optional` |
|---|---|---|
| never measured | omitted | omitted |
| measured, value is 0 | omitted — indistinguishable from the above | **written**: tag + value |
| measured, value is nonzero | written | written |

The middle row is the whole trade: five bytes for a `float` that is genuinely 0.0, in exchange for a receiver that can tell "0 °C" from "no thermometer".

**Why this project needed it.** With one node measuring CO₂, temperature and humidity, "everything is zero" only ever meant the SCD-40 was still warming up, and the host could read it that way. A second node measuring pressure and *not* CO₂ breaks that: it would publish a confident `co2_ppm = 0`, which is a plausible-looking concentration and a false one, and nothing in the bytes would let the host object. Presence turns the absence into something the sender states and the receiver can query — `has_co2_ppm == false`, not `co2_ppm == 0`.

So in [`node.proto`](../proto/node.proto) the four **measurements** are `optional` and the **header** fields are not, and that split is the rule rather than a preference: a measurement can be genuinely absent, whereas a node always has a `sequence` and an `uptime_ms`. `sensor_status` stays non-optional too, because §5's other lesson already covers it — `SENSOR_STATUS_UNSPECIFIED = 0` is what an absent enum decodes to, so absence already has a name.

The firmware carries the same distinction internally, one layer earlier: `struct sensor_reading` in `app_channels.h` pairs each measurement with a `has_` flag, and `sensor.cpp` sets them only when a conversion actually completed. A warming-up SCD-40 therefore reports *nothing measured* rather than three zeros, and `protocol.cpp` copies the flags straight across. There is no point in the pipeline where a zero has to stand in for silence.

### Both rules at once: where a 26-byte `Telemetry` goes

The console logs `published telemetry seq=26404 (26 bytes)`. Reconstructing that message field by field puts §2's tags and this section's absent-defaults rule side by side:

```
08 01 10 a4 ce 01 18 9b b9 88 41 20 ac 06 2d ae 47 b3 41 35 33 33 25 42 38 01
```

| Field | # | Wire type | Bytes | Encoding |
|---|---|---|---|---|
| `schema_version = 1` | 1 | varint | 2 | `08 01` |
| `sequence = 26404` | 2 | varint | 4 | `10 a4 ce 01` |
| `uptime_ms = 136453275` | 3 | varint | 5 | `18 9b b9 88 41` |
| `co2_ppm = 812` | 4 | varint | 3 | `20 ac 06` |
| `temperature_c = 22.41` | 5 | 32-bit | 5 | `2d ae 47 b3 41` |
| `humidity_rh = 41.3` | 6 | 32-bit | 5 | `35 33 33 25 42` |
| `sensor_status = OK` | 7 | varint | 2 | `38 01` |
| | | | **26** | |

Every tag is one byte, because every field number is ≤ 15. The message is not fixed-size: an early publish, with `sequence = 1` and about six seconds of uptime, is **22 bytes**, and it grows as those two counters do. nanopb's `node_Telemetry_size` is **42** — the worst case, with every varint at its maximum width and every optional field present (§9). The gap between 26 and 42 is partly the compression varints give you for free on typical values, and partly the `pressure_pa` this node never sends.

Note that these 26 bytes are unchanged by the `optional` markers. All three measurements are present and nonzero, so the same three fields are written either way — presence costs nothing on a message that had something to say.

And a warming-up reading is smaller still. Keep the counters above and drop the three measurements — which is exactly what `sensor.cpp` does before the first conversion completes — and only `08 01`, the 4-byte `sequence`, the 5-byte `uptime_ms` and `38 02` remain: **13 bytes**. A telemetry message is *shortest* precisely when it has least to say. Note *why* it is short, because the previous section is the whole of the difference: without `optional`, those bytes would be absent because the fields hold *zero* and proto3 omits defaults; with it, they are absent because the fields are *unstated*. The bytes on the wire are identical either way; what the host may conclude from them is not.

---

## 6. `oneof` is a tagged union

```proto
oneof payload {
  SetInterval set_interval = 10;
  TriggerMeasurement trigger_measurement = 11;
  GetDeviceInfo get_device_info = 12;
}
```

On the wire a `oneof` is nothing special — just whichever field is set, encoded normally. A decoder with no schema cannot tell it from three independent optional fields; `protoc --decode_raw` showed exactly one field `11` above, with no hint that 10 and 12 were mutually exclusive with it.

The guarantee is at the API level: setting one member clears the others, so exactly one can be present. (If a malformed sender emits two arms anyway, the decoder keeps the last and clears the earlier one — the union has room for one.)

nanopb turns it into an actual C union plus a discriminator:

```c
typedef struct _node_Command {
    uint32_t schema_version;
    uint32_t sequence;
    pb_size_t which_payload;               /* holds node_Command_set_interval_tag, etc. */
    union _node_Command_payload {
        node_SetInterval set_interval;
        node_TriggerMeasurement trigger_measurement;
        node_GetDeviceInfo get_device_info;
    } payload;
} node_Command;
```

which is why the firmware dispatches on `cmd.which_payload` with the generated `node_Command_*_tag` constants, and why its `default:` arm answers `UNSUPPORTED` (§7). `which_payload == 0` means *no* arm was set — the state you get from an empty payload.

The union means `Command` costs the size of its **largest** arm, not the sum. That shows up directly in the generated size constant, worked through in §9.

The numbering starting at 10 is deliberate: it leaves 3–9 free for future fields that apply to *every* command, without disturbing the arms.

---

## 7. Evolution: the actual point of Protobuf

Decoders skip fields they do not recognise. Because the wire type is embedded in every tag (§2, *The tag*), a decoder that meets unknown field 47 still knows how many bytes to step over — so an old reader can parse a new message without being updated.

That single property produces the rules at the top of `node.proto`:

- **Adding a field is always safe.** Old readers ignore it; new readers see the default when old writers omit it.
- **Never change or reuse a field number.** The number *is* the identity. Renaming a field changes nothing on the wire; renumbering changes everything.
- **Deleting means `reserved`-ing the number**, so nobody reuses it later and silently reinterprets old data as a new meaning.

Some further changes that are safe, and worth knowing because they look risky:

- **Adding an enum value.** Old readers see an unknown number; in proto3 it is preserved as the raw integer rather than rejected. They must therefore have a sane `default:` branch — which is exactly what `ACK_STATUS_UNSPECIFIED` and the firmware's `UNSUPPORTED` arm are for.
- **Adding a `oneof` arm.** It is just another field number; old readers skip it and see `which_payload == 0`.
- **Widening `uint32` to `uint64`**, or `int32` to `int64`. Both are wire type 0 and the varint is the same bytes for values that fit.
- **Adding `optional` to an existing scalar.** Field number and wire type are untouched, so old and new readers parse each other's bytes exactly as before. This one comes with an asymmetry worth stating, though, because it is the only place in this list where information is genuinely lost: a new reader can see everything an old writer sent, but an old writer's *zeros were never on the wire*, so they arrive as absent and no amount of new schema can recover them. Presence describes what a sender chose to state; it cannot be applied retroactively to bytes written before anyone was asked. `tests/protocol/` pins both directions — see §12.

And the ones that are not, despite decoding cleanly:

- **Moving a field in or out of a `oneof`.** The wire bytes are identical, but the generated API's clearing behaviour changes underneath both sides.
- **Changing a field's type across wire types** — `uint32` to `float` — makes every old message unparseable at that field.
- **Changing a field's *meaning* under the same number.** `uint32 co2_ppm` reinterpreted as "co2 in tenths of ppm" still decodes cleanly and reports readings ten times too low.

That last one is the important one: **Protobuf enforces structure, not semantics.**

One nanopb-specific caveat. Full Protobuf implementations *retain* unknown fields and re-emit them when a message is re-serialised, so an intermediary can forward a newer message without dropping the parts it did not understand. nanopb has nowhere to store them — it skips unknown fields and forgets them. A nanopb node that decoded and re-encoded a message would silently strip anything newer than its own schema. This node never does that, but it is the kind of assumption that quietly stops being true when a device becomes a gateway.

### Two layers of versioning

The semantics gap is why every message here also carries `schema_version`, and why its comment says "bumped only on a BREAKING change":

- **Additive change** — new field, new enum value, new `oneof` arm. Protobuf handles it. `schema_version` does not move.
- **Breaking change** — a field's meaning, unit, or type changes. Nothing in the encoding can detect this, so the version field is the only signal, and receivers must check it.

Most schemas need the second case rarely. It is worth carrying the field anyway, because retrofitting a version into a format that has none requires the very coordinated update it would have prevented.

`AckStatus` covers the matching case in the other direction: `ACK_STATUS_UNSUPPORTED` is returned when a command **decodes perfectly** but this firmware has no handler — the host is newer than the node. That is not an error in the wire format; it is a version skew the protocol is designed to report rather than crash on.

Note where each layer sits: Protobuf handles skew in *structure* automatically, `schema_version` reports skew in *meaning*, and `UNSUPPORTED` reports skew in *capability*. Three different failures, three different mechanisms; none of them substitutes for another.

---

## 8. What the format cannot catch

The bench's malformed test published the ASCII string `garbage`:

```
67 61 72 62 61 67 65
```

The first byte decides it: `0x67 >> 3 = 12`, `0x67 & 7 = 7` — and wire type 7 is invalid (§2, *The tag*). The decoder rejects the message before reading anything else, and the firmware answers `ACK_STATUS_MALFORMED`.

**But that was luck, not protection.** Two ASCII bytes are enough to make a perfectly valid `Command`:

```sh
printf 'hi' | protoc --decode=node.Command --proto_path=proto proto/node.proto
```

```
13: 105
```

Exit status 0. `0x68` is field 13, wire type 0; `0x69` is the varint 105. Field 13 is not in the schema, so it is skipped as an unknown field (§7), and what the node gets is a `Command` with every field defaulted — `schema_version == 0`, `sequence == 0`, `which_payload == 0`. A zero-byte payload decodes just as cleanly.

This node survives that by construction rather than by detection: `which_payload == 0` hits the `default:` arm and returns `UNSUPPORTED`. Nothing about the *encoding* saved it.

The general lesson: **`MALFORMED` catches corrupt framing, not wrong content.** Protobuf has no magic number and no checksum, so:

| What went wrong | Caught by | How |
|---|---|---|
| Corrupt or truncated bytes | the decoder | invalid wire type, or a length running past the buffer |
| A different message type, structurally legal | nothing | the type is never on the wire (§4); it decodes to defaults and unknown fields |
| Right message, incompatible schema version | `schema_version` | only if the receiver checks it |
| Right message, unimplemented command | `which_payload` dispatch | the `default:` arm → `UNSUPPORTED` |

The second row is the hole, and it is a real one: another application publishing to the same topic, a stale host binary, a replayed payload from an older schema — all can be well-formed Protobuf and still be nonsense. That is precisely what `schema_version` exists to cover, and it is what makes the schema-versioning exercise a real exercise rather than a formality.

---

## 9. nanopb: Protobuf under embedded constraints

Standard Protobuf libraries allocate freely — strings grow, repeated fields are vectors. On a microcontroller with no heap that is unacceptable, so nanopb makes a different trade: **every message becomes a fixed-size C struct whose worst case is known at build time.**

### Bounded fields become plain arrays

nanopb cannot know how long a `string` may get, so by default it emits a pointer field plus callbacks or `malloc`. Giving it a bound in [`proto/node.options`](../proto/node.options) changes that:

```
node.Ack.detail  max_size:48
```

produces

```c
char detail[48];
```

A plain array inside the struct. No allocation anywhere, and the footprint is visible in the map file. `max_size` **counts the NUL terminator**, so 48 stores 47 characters, and oversized input fails to encode rather than truncating silently.

The `.options` file is nanopb-only — a separate file precisely because it is a target-specific decision, not part of the contract. The host generator ignores it entirely and has no limits, so **the node is always the side that constrains the contract**. If a host ever sends a `DeviceInfo` with a 40-character `board`, the node's 32-byte bound is what decides the outcome.

### Generated size constants

nanopb computes the maximum encoded size of each message and emits it as a macro:

```c
#define node_Telemetry_size   42
#define node_Command_size     20
#define node_Ack_size        140
```

These are not magic. Each is the sum of every field's worst case, and working one out makes the whole encoding concrete:

```
node_Telemetry_size = 42
  schema_version   1 (tag) + 5 (uint32 varint, worst case)   =  6
  sequence         1 + 5                                     =  6
  uptime_ms        1 + 5                                     =  6
  co2_ppm          1 + 5                                     =  6
  temperature_c    1 + 4 (float is always 4 bytes)           =  5
  humidity_rh      1 + 4                                     =  5
  sensor_status    1 + 1 (enum, max value 3)                 =  2
  pressure_pa      1 + 5                                     =  6
                                                              -----
                                                                 42
```

Note what the `optional` markers did **not** do to this number: nothing. The worst case already assumed every field was written, so adding presence changed only the *typical* size, never the bound. The whole +6 is `pressure_pa`. That is worth knowing before sizing a buffer around presence — an optional field costs its full worst case in RAM whether or not this particular node ever sets it.

```
node_Command_size = 20
  schema_version   1 (tag) + 5 (uint32 varint, worst case)   =  6
  sequence         1 + 5                                     =  6
  payload          the LARGEST arm, not the sum:
                     SetInterval  = 1 + 5 = 6 bytes of body
                     as a nested message: 1 (tag) + 1 (len) + 6 =  8
                                                              -----
                                                                 20
```

That is §6's union claim in arithmetic: the two empty arms contribute nothing, and adding a fourth arm only grows `Command` if it is bigger than `SetInterval`.

`Ack` is the same exercise one level deeper:

```
node_DeviceInfo_size = 75
  firmware_version  1 + 1 + 15 (max_size 16, minus NUL)  = 17
  board             1 + 1 + 31                           = 33
  client_id         1 + 1 + 23                           = 25

node_Ack_size = 140
  schema_version  1 + 5                                  =  6
  sequence        1 + 5                                  =  6
  status          1 + 1 (enum, max value 5)              =  2
  detail          1 + 1 + 47                             = 49
  device_info     1 + 1 + 75                             = 77
```

Every `max_size` in `node.options` is visible in that total — which is what makes the bounds a real budget rather than a formality.

The constants are what let the firmware declare exactly the right buffer:

```c
uint8_t payload[node_Telemetry_size];
```

If the schema grows, the constant grows with it and the buffer follows automatically at the next build. Hard-coding `uint8_t payload[64]` would work today and overflow after an innocuous schema edit.

### Streams

nanopb reads and writes through stream objects rather than buffers directly:

```c
pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
if (!pb_encode(&stream, node_Telemetry_fields, &msg)) { ... }
size_t written = stream.bytes_written;
```

The stream abstraction is what allows encoding straight to a socket or flash without an intermediate buffer. Here it wraps a plain array — and because it knows the bound, a message that does not fit **fails** rather than overflowing. `PB_GET_ERROR(&stream)` returns a human-readable reason, which is what the firmware logs on a decode failure.

Note that `bytes_written` is the *actual* length (26 in §5), while the buffer was sized for the worst case (42). The difference is what gets published.

Decoding mirrors it with `pb_istream_from_buffer()` and `pb_decode()`.

---

## 10. Reading the firmware

The whole serialisation story lives in one translation unit, `shared/protocol.cpp` — and that it *is* one translation unit is the point. Every byte that crosses between this firmware's own types and the schema crosses here; nothing else in the app includes `pb_encode.h`. Which is also why it can be tested on a laptop with no board attached (see [`testing-guide.md`](testing-guide.md)).

### `encode_telemetry()`

```c
size_t encode_telemetry(const struct sensor_reading &reading, uint8_t *out, size_t out_len)
{
    node_Telemetry msg = node_Telemetry_init_zero;
    msg.schema_version = kSchemaVersion;
    msg.sequence = reading.sequence;
    ...
    pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
    if (!pb_encode(&stream, node_Telemetry_fields, &msg)) { ... }
    return stream.bytes_written;
}
```

Read the signature first, because it is the design. The input is a `struct sensor_reading` — the firmware's own type, off a zbus channel — and the output is wire bytes. **This function is the only place in the firmware where the two representations meet.** The sensor thread has never heard of `node_Telemetry`, and nothing downstream of here has heard of the SCD-40, which is what keeps a schema change from rippling into the acquisition code. (See [`zbus-guide.md`](zbus-guide.md) for the channel it arrives on.)

The mapping is deliberately explicit rather than a `memcpy` or a shared struct. Its cost is a `switch` translating `SENSOR_READING_OK` into `node_SensorStatus_SENSOR_STATUS_OK`; its benefit is that the internal enum and the wire enum can be renumbered independently, and the compiler flags the mapping when either gains a value.

`node_Telemetry_init_zero` is a generated initialiser — always start from it, so fields added later are not left holding stack garbage. With explicit presence it does more than tidy up: it clears every `has_` bit, so a field this function forgets to mention is *absent* rather than silently zero. Forgetting a field is still a bug, but it now produces a message that admits it.

`node_Telemetry_fields` is the generated **field descriptor table**: a compact description of the schema that `pb_encode` walks at runtime. The schema exists as *data*, not as generated code per field, which is a large part of why nanopb is small — one encoder walks every message type rather than each message getting its own emitted serialiser.

The measurements are copied flag-first — `msg.has_co2_ppm = reading.has_co2_ppm;` and only then the value — so the internal presence flags from §5 map one-for-one onto the wire's. That is the entire translation; there is no rule here about *when* a measurement counts as present, because that decision belongs to the code that took the reading.

Note the design choice around failure: a failed sensor read still reaches this function, carrying `SENSOR_READING_ERROR` and no measurements at all, and is published as `sensor_status = ERROR`. Silence would be ambiguous — the host cannot distinguish a broken sensor from a dead node — whereas an explicit status is a fact it can act on. And because every measurement is absent rather than zero, an error report is both one of the smallest messages the node ever sends and one a host cannot misread as a room at 0 °C.

### `decode_command()`

```c
bool decode_command(const uint8_t *buf, size_t len, bool oversized, node_Command *out)
{
    *out = node_Command_init_zero;

    if (oversized) { return false; }

    pb_istream_t stream = pb_istream_from_buffer(buf, len);

    if (!pb_decode(&stream, node_Command_fields, out)) { ... return false; }
    return true;
}
```

**Oversize is rejected outright** rather than decoded from a truncated buffer — a partial message can decode into something plausible (§8), and acting on half a command is worse than refusing it. Whoever calls this then answers `MALFORMED` with `sequence = 0`, because the sequence could not be read: there is nothing to correlate against, and inventing a number would be worse than admitting ignorance.

Note what a `true` return does **not** promise: that the bytes were meant as a `Command`. §4 is the reason — the type is never on the wire, so unrelated bytes decode cleanly into a `Command` with every field defaulted. This function cannot detect that and does not try. The dispatch on `cmd.which_payload` in `commands.cpp` is what catches it, via the `default:` arm returning `UNSUPPORTED` (§6) — which is the same branch that catches a host newer than the node. Two different problems, one branch, and neither of them is the decoder's business.

### `encode_ack()`

Builds an `Ack` into a `node_Ack_size` buffer: sequence, status, an optional bounded `detail` string and an optional `DeviceInfo`. The publishing half is `send_ack()` in `main.cpp`, and it is best-effort by design — a failure there is logged, not propagated, because the command may already have taken effect and there is nothing useful to undo.

That division is the file boundary in miniature: `protocol.cpp` turns a result into bytes and cannot fail for any reason but "it did not fit"; `main.cpp` decides what to do when the network refuses them.

---

## 11. Two toolchains, one contract

The same `.proto` feeds two completely independent generators:

| | Firmware | Host |
|---|---|---|
| Generator | nanopb, via `zephyr_nanopb_sources()` in `gateway/CMakeLists.txt` | `grpc_tools.protoc`, via `host/generate.sh` |
| Output | `node.pb.c` / `node.pb.h` (C) | `node_pb2.py` |
| Runs | Every build, into the build directory | On demand, gitignored |
| Bounds | Applies `node.options` | Ignores it |

Neither output is checked in. Both are regenerated from the same file, which is what makes "single source of truth" a mechanical guarantee rather than a promise: there is no state in which the firmware builds against a stale schema, because the schema is compiled on every build.

### The gencode/runtime trap

Python's generated modules embed the compiler version and validate it at import:

```
gencode 7.35.1  runtime 5.29.6
Runtime version cannot be older than the linked gencode version.
```

A newer runtime can read older generated code, never the reverse. This bench hit exactly that: a system `protoc` a major generation ahead of the available Python runtime.

`host/requirements.txt` therefore pins `grpcio-tools`, which bundles a `protoc` matched to its own runtime, so the pair cannot drift — and deliberately does **not** touch the Zephyr workspace venv, which is shared with another project and drives the nanopb generator.

**The firmware was unaffected by the same mismatch**, which is instructive: nanopb has `protoc` emit a *descriptor set* — a serialised representation of the `.proto`, itself a Protobuf message — and generates C from that with its own Python generator. It never imports `protoc`'s generated Python, so the version check never runs. The incompatibility is specific to Python's generated modules, not to Protobuf itself.

---

## 12. Exercising the encoding

Most of this guide can be re-derived from a terminal, with no board involved. The rest needs the node and the harness on the Pi. Both are worth doing.

### Without hardware: take the format apart

Everything in §2 and §8 is reproducible from any machine with `protoc`:

```sh
# take apart any captured payload, no schema needed
printf '\x08\x01\x10\x2a\x5a\x00' | protoc --decode_raw

# ... or with the schema, to get names and types back
printf '\x08\x01\x10\x2a\x5a\x00' | protoc --decode=node.Command --proto_path=proto proto/node.proto

# and the size constants nanopb derived from it (after a build)
grep _size gateway/build/node.pb.h
```

The last one should print the constants §9 works through by hand — `node_Telemetry_size` 42, `node_Command_size` 20, `node_Ack_size` 140, `node_DeviceInfo_size` 75. If your arithmetic in §9 disagrees with the generator, the generator is right and the interesting question is which field's worst case you mis-counted.

**Exercise A — feel why field numbers are permanent (§7).** Change one field number in `node.proto`, rebuild, and re-run the decode above. The bytes still decode cleanly, exit status 0 — and they mean something entirely different. Nothing anywhere reports an error. Revert afterwards.

**Proves:** a field number *is* the field's identity, and nothing in the format checks that two sides agree on it — the single most important property to internalise, and thirty seconds to see.

**Exercise B — confirm the type is not on the wire** (§4)**.** Decode the same six bytes as each of the three message types:

```sh
for T in Command Telemetry Ack; do
  printf '\x08\x01\x10\x2a\x5a\x00' | protoc --decode=node.$T --proto_path=proto proto/node.proto
done
```

All three succeed.

**Proves:** a serialised message carries no type identity; the topic it arrived on is what asserts the type, which is why the topic hierarchy is part of the contract and not just housekeeping.

**Exercise C — watch explicit presence appear and disappear (§5).** `protoc --decode` prints only the fields a message actually contains, which makes presence directly visible. Encode a `Telemetry` whose measurements are all zero, once as the schema stands and once with the `optional` markers removed:

```sh
# co2_ppm = 0, stated explicitly: field 4 is on the wire
printf '\x08\x01\x20\x00' | protoc --decode=node.Telemetry --proto_path=proto proto/node.proto
```

That prints `schema_version: 1` and `co2_ppm: 0`. Now delete `optional` from `co2_ppm` in `node.proto` and run it again: the same four bytes still decode, and `co2_ppm: 0` **disappears from the output** — because without presence the decoder cannot report the difference between a zero it was sent and a zero it defaulted. Revert afterwards.

**Proves:** presence is a property of the *reader's schema*, not of the bytes. The same payload means "measured zero" to one side and "said nothing" to the other, which is exactly the failure mode §5 exists to remove.

**Exercise D — decode old↔new without a second board (§7).** `./scripts/test.sh` runs `tests/protocol/` on qemu in about eighteen seconds. Two of its cases, `test_old_reader_decodes_new_telemetry` and `test_new_reader_decodes_old_telemetry`, hand-build a descriptor for the *previous* schema with nanopb's `PB_BIND` X-macro and run both directions against the current one. Read those two tests: they are the shortest statement in the repo of what "additive changes are safe" actually promises.

**Proves:** §7's rules hold in both directions against a real previous schema — and that an old writer's zeros are the one thing a new reader cannot recover, because they were never on the wire to recover.

### With the node: the round trip

Start the harness on the Pi (`host/.venv/bin/python host/monitor.py`) and work through:

| Command | Expect | Proves |
|---|---|---|
| *(just watch)* | a decoded `Telemetry` every ~5 s | nanopb encode ↔ Python decode agree on field numbers, wire types and enum values |
| `command.py info` | `ACK_STATUS_OK` + `firmware`, `board`, `clientid` | `max_size`-bounded strings survive the round trip (§9) |
| `command.py trigger` | `ACK_STATUS_OK`, immediate telemetry | the empty-message `oneof` arm — two bytes on the wire — dispatches (§6) |
| `command.py interval 2000` | `ACK_STATUS_OK`, cadence changes | the arm that *carries* a field, and the largest one in the union (§9) |
| `command.py interval 100` | `ACK_STATUS_INVALID_ARGUMENT` + bounds in `detail` | rejected, not clamped — and `detail` is a bounded string |
| `command.py --sequence 42 trigger`, twice | second says `duplicate ignored`, no second measurement | `sequence` is what makes QoS 1 redelivery safe |

Restore with `command.py interval 5000`.

**Exercise E — the malformed case, and its limits (§8).** Publish something that is not a `Command` at all:

```sh
mosquitto_pub -h 192.168.10.1 -t node/1/command -m garbage -q 1
```

The node answers `ACK_STATUS_MALFORMED` and **telemetry keeps flowing** — the payload was drained from the socket rather than desynchronising the MQTT stream.

Now the important half. Publish two bytes that *are* structurally legal:

```sh
printf 'hi' | mosquitto_pub -h 192.168.10.1 -t node/1/command -q 1 -s
```

This one comes back `ACK_STATUS_UNSUPPORTED`, not `MALFORMED`.

**Proves:** `MALFORMED` catches corrupt framing, not wrong content. `garbage` was rejected only because its first byte encodes wire type 7, which is invalid; `hi` decodes into field 13 as an unknown field and yields a `Command` with everything defaulted. The node survives it by construction — `which_payload == 0` hits the `default:` arm — not by detection. That gap is exactly what `schema_version` exists to cover.

---

## 13. The model in one paragraph

A Protobuf message is **fields back to back, each preceded by a tag** packing a field number and a wire type into one varint — no header, no length, no checksum, no type name. That single layout explains the rest. The wire type lets a decoder step over a field it has never heard of, which is the mechanism behind every evolution rule: **adding a field is safe, a field number is permanent, a deleted number must be reserved.** Fields a sender leaves unset are not transmitted at all, so absence and zero are indistinguishable for a plain scalar — and marking one `optional` buys back the distinction, at the price of writing out an explicit zero. What the format does **not** give you is semantics — it enforces structure, so a wrong message type or a redefined unit decodes perfectly and means something else. That gap is why `schema_version` exists, why `UNSUPPORTED` reports capability skew, and why the **MQTT topic is the type discriminator** and therefore part of the contract. nanopb adds one constraint on top: no allocation. Bounds in `.options` turn strings into plain arrays and let the generator emit a worst-case size per message, so buffers come from `node_Telemetry_size` and cannot silently be outgrown.

## 14. Where to go next

- **[`proto/node.proto`](../proto/node.proto)** — the reference half of this guide. The schema decisions and their rationale live inline in its comments.
- **`tests/protocol/`'s two evolution cases** — `test_old_reader_decodes_new_telemetry` and `test_new_reader_decodes_old_telemetry` run the previous schema against the current one in both directions, so §7's promise is an assertion rather than a claim. §12's Exercise D reads them; doing the same thing by hand on the wire — add a field, regenerate both sides, reflash only one end — is how you come to believe it.
- **[`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md)** — the surrounding MQTT client, and where `encode_telemetry()` and `decode_command()` are called from in the event loop.
- **[`testing-guide.md`](testing-guide.md)** — `tests/protocol/` turns much of this guide into assertions: that a warming-up reading is shorter on the wire (§5), and that `hi` decodes into a legal `Command` while `garbage` does not (§4, §8).
- **[`zbus-guide.md`](zbus-guide.md)** — the internal channel a reading crosses *before* it reaches the encoder, and why the wire type deliberately stops there.
- **nanopb's own docs** — `concepts.md` and `reference.md` in the module source, for callbacks, `FT_POINTER` fields, and the options this project did not need.
