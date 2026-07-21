# Understanding Protobuf and nanopb

*A from-first-principles guide to how a sensor reading becomes bytes on the wire, and back.*

A teaching document. It explains the concepts in the order that makes them easiest to
learn, using this repository's contract — [`proto/node.proto`](../proto/node.proto) — as
the running example.

Every byte sequence below is verifiable, and was re-checked with `protoc --decode_raw`
while writing this. The `Command` byte strings are captures from packets this bench
actually sent; the `Telemetry` breakdown is reconstructed field by field from a logged
reading, and reproduces the exact length the node reported on the console.

Roughly, the shape of the document:

- **§1–§2** — what a schema buys you, and what the encoding actually looks like. §2 ends
  on the fact that surprises people most: *nothing on the wire says which message this
  is*, which §6 later turns into a real failure mode.
- **§3–§6** — the four things that surprise people next: absent defaults, `oneof`,
  evolution, and what the format deliberately does *not* protect you from.
- **§7–§9** — nanopb, this firmware, and the two generators that share the contract.
- **§10** — exercises: most of it reproducible from a terminal alone, the rest on the node.
- **§11–§12** — the whole model in a paragraph, and where to go next.

For how those bytes get carried, see [`communication-guide.md`](communication-guide.md)
(MQTT concepts) and [`mqtt-design.md`](../docs/mqtt-design.md) (this project's topic and QoS
decisions). For the surrounding firmware, see
[`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md).

---

## 1. Why a schema at all

The cheapest thing a node can publish is text:

```
seq=1 co2=812 temp=22.41 rh=41.3
```

That is readable, debuggable with `mosquitto_sub`, and needs no tooling at all. It is also
a contract that exists only in the heads of the people who wrote both ends. Nothing stops
the host from expecting `co2_ppm=` while the node sends `co2=`. Nothing records that
`temp` is Celsius. Adding a field means every parser must be updated in lockstep, and a
typo becomes a runtime surprise rather than a build error.

A schema moves that contract into a file both sides generate from. The trade is
readability on the wire for **a single definition neither side can silently diverge from**
— which is why `proto/` is described as the source of truth and why generated code is
never hand-edited.

Protobuf adds three things beyond "an agreed format":

1. **Compactness.** A real reading — with a five-digit sequence number, an uptime, a
   schema version and a status, none of which the text line above carried — encodes to
   **26 bytes** (§3). That text line, with the same sequence number substituted in, is 36
   characters, and it says less; the equivalent JSON object is 149 bytes. Useful, but the
   least interesting of the three.
2. **Typed fields.** `float` vs `uint32` is decided once, in one place, rather than
   re-parsed and re-guessed by every reader.
3. **Defined evolution rules.** The part that actually matters, covered in §5 — and the
   reason Protobuf exists at all rather than "a struct we both memcpy".

A useful way to hold the last point: text and JSON let *anything* change and hope both
sides notice; a hand-rolled binary struct lets *nothing* change without breaking. Protobuf
picks a precise middle — a specific list of changes that are guaranteed safe, and a
specific list that are not.

---

## 2. The wire format

### A message is just fields, back to back

A Protobuf message is a flat sequence of **fields**, each preceded by a tag:

```
[tag][value][tag][value][tag][value]...
```

There is no message header, no field count, no terminator, and — importantly — **no magic
number and no checksum**. A message is fields, one after another, until the buffer ends.
Everything the decoder needs in order to walk it comes from the tags themselves.

That has a consequence worth stating early, because §6 turns on it: **the encoding cannot
tell you whether it is looking at the right kind of message.** It can only tell you whether
the bytes are structurally walkable.

Note that the shape is *tag-value*, not tag-length-value: only some field types carry an
explicit length, and the rest are self-delimiting. Which ones, and why, is the next three
subsections.

### Varints: the encoding everything else leans on

Integers — including the tags — are encoded as **varints**: seven bits at a time, lowest
group first, with the top bit of each byte meaning "another byte follows".

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

A `uint32` therefore costs between 1 and 5 bytes depending on its magnitude, and a varint
is **self-delimiting**: a decoder knows where it ends without being told a length.

Two gotchas that explain choices in `node.proto`:

- **Negative `int32` is the worst case in the format.** It is sign-extended to 64 bits
  before encoding, so `-1` takes **ten** bytes. If you need negatives, use `sint32`, which
  applies zigzag encoding (`0, -1, 1, -2, 2 → 0, 1, 2, 3, 4`) so small magnitudes stay
  small either side of zero.
- **This schema sidesteps it entirely.** Every integer here is `uint32`: counters, `co2_ppm`,
  `interval_ms`, `uptime_ms` — all naturally non-negative. Temperature, which genuinely goes
  below zero, is a `float` and so does not use varints at all.

### The tag: field number and wire type in one varint

Each field's tag is a single varint packing two things:

```
tag = (field_number << 3) | wire_type
```

The low three bits are the **wire type**, which tells a decoder *how to read the next
bytes* even if it has never heard of the field number:

| Wire type | Meaning | Used by |
|---|---|---|
| 0 | varint | `uint32`, `int32`, `bool`, enums |
| 1 | 64-bit fixed | `double`, `fixed64` |
| 2 | length-delimited | `string`, `bytes`, nested messages, packed repeated |
| 3, 4 | start/end group | deprecated, never emitted by proto3 |
| 5 | 32-bit fixed | `float`, `fixed32` |
| 6, 7 | — | **invalid** — remember this, it explains §6 |

**This is why field numbers 1–15 matter.** The tag is itself a varint, and a one-byte
varint holds values up to 127. Field 15 produces tags in the range `120`–`127`, whatever
its wire type — still one byte. Field 16 with wire type 0 is already `128`, which needs
two. So every field numbered 1–15 saves a byte on *every single message that carries it*.

`Telemetry` is the high-rate message here, so `node.proto` deliberately keeps all seven of
its fields inside that range. `Command`'s `oneof` arms start at 10 and still fit; the
numbering leaves 3–9 free for future fields common to every command (§4).

Reading the tags in the packet below:

```
0x08 >> 3 = 1   0x08 & 7 = 0     field 1, varint
0x62 >> 3 = 12  0x62 & 7 = 2     field 12, length-delimited
```

### How each wire type says where its value ends

The tag alone is enough to skip a field, because each wire type is self-describing about
its own length:

| Wire type | Value layout | Length known from |
|---|---|---|
| 0 varint | `[varint]` | the continuation bits |
| 1 64-bit | `[8 bytes]` | fixed by the type |
| 2 length-delimited | `[length varint][that many bytes]` | the explicit length prefix |
| 5 32-bit | `[4 bytes]` | fixed by the type |

Fixed-width types are little-endian IEEE-754 for `float`/`double`. `22.41` encodes as
`ae 47 b3 41` — that is `0x41b347ae` read back the other way.

Note the asymmetry: **a `float` never gets smaller.** A `uint32` of 812 costs two bytes,
but a `float` of 22.41 costs four regardless of value. That is the price of not having to
agree on a fixed-point scale factor, and it is why the choice between `uint32 co2_ppm` and
`float co2_ppm` is a real one.

Wire type 2 is the interesting one. **A nested message is encoded exactly like a `bytes`
field**: a length, then that many bytes, which happen to be a complete Protobuf message.
Nothing marks it as a message rather than a string. This is what makes the whole format
recursive with no extra machinery — and what lets a decoder skip an unknown nested message
as easily as an unknown integer.

### Order, duplicates, and unknown fields

Three rules that follow from "just fields, back to back", and that matter when debugging:

- **Field order is not guaranteed.** Encoders normally emit fields in number order (nanopb
  and `protoc` both do), but a decoder must accept any order, and must not depend on it.
- **A repeated non-repeated field is legal.** If the same scalar field appears twice, the
  last occurrence wins. For a nested message, the occurrences are merged.
- **Unknown fields are skipped, not fatal.** The wire type in the tag is exactly enough
  information to step over a field the decoder has never heard of. This is the mechanism
  behind every evolution rule in §5.

### Decoding a real `Command`

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

Identical structure. The sequence collapsed from 5 bytes to 1 because varints are
magnitude-sensitive, and the arm is field 11 (`trigger_measurement`) instead of 12, so
`0x5a` rather than `0x62`.

Note what the empty message costs: **two bytes**, a tag and a zero length. Its *presence*
is the whole instruction — exactly as `node.proto` claims when it says an empty message
"is the instruction". Adding a parameter to it later is an additive change (§5), so the
zero-byte body is not a corner cut, it is room left open.

You can take apart any captured payload the same way:

```sh
printf '\x08\x01\x10\x2a\x5a\x00' | protoc --decode_raw
```

```
1: 1
2: 42
11: ""
```

`--decode_raw` needs no schema — it recovers field numbers and wire types straight from the
bytes, which is exactly what a decoder does before it knows what the fields *mean*. Note
what it cannot recover: names, and the difference between an empty nested message and an
empty string. Both are wire type 2 with length 0.

### Nothing on the wire says *which* message this is

A natural question after that walkthrough: those bytes were a `Command` — but where does
the encoding say so?

It does not. **A serialised message carries no type identity at all** — no name, no type
ID, no header. `Telemetry`, `Command` and `Ack` are all just field lists, and this schema
deliberately numbers `schema_version = 1` and `sequence = 2` the same way in all three, so
their first bytes are literally identical.

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

Exit status 0 both times. As a `Telemetry`, field 11 is simply unknown and gets skipped
(§5) — the very mechanism that makes evolution work also makes a wrong guess about the type
look successful.

**The type is always supplied from outside the bytes.** `--decode=node.Command` is you
asserting it. In the firmware the assertion is the descriptor table you pass in:

```c
pb_decode(&stream, node_Command_fields, &cmd);   /* "read these bytes as a Command" */
```

Nothing in the payload can contradict that argument. So every Protobuf system needs a
type discriminator somewhere, and there are three usual places to put it:

| Where the type lives | Cost | Used by |
|---|---|---|
| Out-of-band context | zero bytes | **this project** — the MQTT topic; also gRPC's method name, or a dedicated socket/port |
| An envelope message with a `oneof` over every type | tag + length per message | most single-channel protocols |
| `google.protobuf.Any` | a type-URL *string* per message | dynamic/plugin systems |

This project uses the first. `node/<id>/command` carries a `Command`,
`node/<id>/telemetry` a `Telemetry`, `node/<id>/ack` an `Ack` — one message type per topic,
never mixed. That is why the topic hierarchy in
[`mqtt-design.md`](../docs/mqtt-design.md) is part of the *contract* and not merely
housekeeping: it is the half of the wire format that the `.proto` file does not contain.

The catch is that this makes routing load-bearing. Anything that publishes to
`node/1/command` is asserting "this is a Command", and the node has no way to check the
claim — it decodes whatever arrives as a `Command` and acts on it. That is the failure §6
comes back to.

---

## 3. What is not on the wire

This is the biggest conceptual trap in proto3.

**Fields equal to their default value are not transmitted at all.** A `uint32` that is 0,
a `float` that is 0.0, a `string` that is empty, an enum that is 0 — all are simply absent
from the encoded bytes. The decoder fills them in from the defaults afterwards.

So the 26-byte telemetry above is not a fixed layout at all. A reading with
`co2_ppm = 0` would be 23 bytes, and a `Telemetry` with every field defaulted encodes to
**zero bytes** — a perfectly valid message with no content whatsoever.

Two consequences worth internalising:

**Absence and zero are indistinguishable for scalars.** A decoder receiving no `co2_ppm`
field yields `co2_ppm == 0`, and cannot tell "the node said zero" from "the node said
nothing". That is why `SENSOR_STATUS_UNSPECIFIED = 0` exists in `node.proto` with a comment
saying it must mean "no information" — the zero value of every enum doubles as the value
you get from a message that never mentioned it. Give it a meaning that is safe as a
default, and never assign 0 to a real state.

**Message fields are different.** A nested message *can* distinguish absence, because
presence is encoded by the tag existing at all — even a zero-length one (§2, *Decoding a
real `Command`*). That is why
`Ack.device_info` gets a real presence flag in the generated C:

```c
bool has_device_info;
node_DeviceInfo device_info;
```

and why `command.py` can ask `ack.HasField("device_info")`. Scalars have no `HasField` in
proto3 unless explicitly marked `optional`, which re-adds a presence bit at the cost of a
generated `has_` field on both sides. Nothing here needs one — but if a future field must
distinguish "0" from "not reported", that is the tool.

### Both rules at once: where a 26-byte `Telemetry` goes

The console logs `published telemetry seq=26404 (26 bytes)`. Reconstructing that message
field by field puts §2's tags and this section's absent-defaults rule side by side:

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

Every tag is one byte, because every field number is ≤ 15. The message is not fixed-size:
an early publish, with `sequence = 1` and about six seconds of uptime, is **22 bytes**, and
it grows as those two counters do. nanopb's `node_Telemetry_size` is **36** — the worst
case, with every varint at its maximum width (§7). The gap between 26 and 36 is the
compression varints give you for free on typical values.

And a warming-up reading is smaller still, because the absent-defaults rule bites hard:
with `co2_ppm`, `temperature_c` and `humidity_rh` all at zero, none of the three is
transmitted at all — a telemetry message can be *shorter* precisely when it has least to
say.

---

## 4. `oneof` is a tagged union

```proto
oneof payload {
  SetInterval set_interval = 10;
  TriggerMeasurement trigger_measurement = 11;
  GetDeviceInfo get_device_info = 12;
}
```

On the wire a `oneof` is nothing special — just whichever field is set, encoded normally.
A decoder with no schema cannot tell it from three independent optional fields; `protoc
--decode_raw` showed exactly one field `11` above, with no hint that 10 and 12 were
mutually exclusive with it.

The guarantee is at the API level: setting one member clears the others, so exactly one can
be present. (If a malformed sender emits two arms anyway, the decoder keeps the last and
clears the earlier one — the union has room for one.)

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

which is why the firmware dispatches on `cmd.which_payload` with the generated
`node_Command_*_tag` constants, and why its `default:` arm answers `UNSUPPORTED` (§5).
`which_payload == 0` means *no* arm was set — the state you get from an empty payload.

The union means `Command` costs the size of its **largest** arm, not the sum. That shows up
directly in the generated size constant, worked through in §7.

The numbering starting at 10 is deliberate: it leaves 3–9 free for future fields that apply
to *every* command, without disturbing the arms.

---

## 5. Evolution: the actual point of Protobuf

Decoders skip fields they do not recognise. Because the wire type is embedded in every tag
(§2, *The tag*), a decoder that meets unknown field 47 still knows how many bytes to step
over — so an
old reader can parse a new message without being updated.

That single property produces the rules at the top of `node.proto`:

- **Adding a field is always safe.** Old readers ignore it; new readers see the default
  when old writers omit it.
- **Never change or reuse a field number.** The number *is* the identity. Renaming a field
  changes nothing on the wire; renumbering changes everything.
- **Deleting means `reserved`-ing the number**, so nobody reuses it later and silently
  reinterprets old data as a new meaning.

Some further changes that are safe, and worth knowing because they look risky:

- **Adding an enum value.** Old readers see an unknown number; in proto3 it is preserved as
  the raw integer rather than rejected. They must therefore have a sane `default:` branch —
  which is exactly what `ACK_STATUS_UNSPECIFIED` and the firmware's `UNSUPPORTED` arm are for.
- **Adding a `oneof` arm.** It is just another field number; old readers skip it and see
  `which_payload == 0`.
- **Widening `uint32` to `uint64`**, or `int32` to `int64`. Both are wire type 0 and the
  varint is the same bytes for values that fit.

And the ones that are not, despite decoding cleanly:

- **Moving a field in or out of a `oneof`.** The wire bytes are identical, but the
  generated API's clearing behaviour changes underneath both sides.
- **Changing a field's type across wire types** — `uint32` to `float` — makes every old
  message unparseable at that field.
- **Changing a field's *meaning* under the same number.** `uint32 co2_ppm` reinterpreted as
  "co2 in tenths of ppm" still decodes cleanly and reports readings ten times too low.

That last one is the important one: **Protobuf enforces structure, not semantics.**

One nanopb-specific caveat. Full Protobuf implementations *retain* unknown fields and
re-emit them when a message is re-serialised, so an intermediary can forward a newer
message without dropping the parts it did not understand. nanopb has nowhere to store them
— it skips unknown fields and forgets them. A nanopb node that decoded and re-encoded a
message would silently strip anything newer than its own schema. This node never does that,
but it is the kind of assumption that quietly stops being true when a device becomes a
gateway.

### Two layers of versioning

The semantics gap is why every message here also carries `schema_version`, and why its
comment says "bumped only on a BREAKING change":

- **Additive change** — new field, new enum value, new `oneof` arm. Protobuf handles it.
  `schema_version` does not move.
- **Breaking change** — a field's meaning, unit, or type changes. Nothing in the encoding
  can detect this, so the version field is the only signal, and receivers must check it.

Most schemas need the second case rarely. It is worth carrying the field anyway, because
retrofitting a version into a format that has none requires the very coordinated update it
would have prevented.

`AckStatus` covers the matching case in the other direction: `ACK_STATUS_UNSUPPORTED` is
returned when a command **decodes perfectly** but this firmware has no handler — the host is
newer than the node. That is not an error in the wire format; it is a version skew the
protocol is designed to report rather than crash on.

Note where each layer sits: Protobuf handles skew in *structure* automatically,
`schema_version` reports skew in *meaning*, and `UNSUPPORTED` reports skew in
*capability*. Three different failures, three different mechanisms; none of them substitutes
for another.

---

## 6. What the format cannot catch

The bench's malformed test published the ASCII string `garbage`:

```
67 61 72 62 61 67 65
```

The first byte decides it: `0x67 >> 3 = 12`, `0x67 & 7 = 7` — and wire type 7 is invalid
(§2, *The tag*). The decoder rejects the message before reading anything else, and the
firmware
answers `ACK_STATUS_MALFORMED`.

**But that was luck, not protection.** Two ASCII bytes are enough to make a perfectly valid
`Command`:

```sh
printf 'hi' | protoc --decode=node.Command --proto_path=proto proto/node.proto
```

```
13: 105
```

Exit status 0. `0x68` is field 13, wire type 0; `0x69` is the varint 105. Field 13 is not
in the schema, so it is skipped as an unknown field (§5), and what the node gets is a
`Command` with every field defaulted — `schema_version == 0`, `sequence == 0`,
`which_payload == 0`. A zero-byte payload decodes just as cleanly.

This node survives that by construction rather than by detection: `which_payload == 0` hits
the `default:` arm and returns `UNSUPPORTED`. Nothing about the *encoding* saved it.

The general lesson: **`MALFORMED` catches corrupt framing, not wrong content.** Protobuf has
no magic number and no checksum, so:

| What went wrong | Caught by | How |
|---|---|---|
| Corrupt or truncated bytes | the decoder | invalid wire type, or a length running past the buffer |
| A different message type, structurally legal | nothing | the type is never on the wire (§2, *Nothing on the wire says which message this is*); it decodes to defaults and unknown fields |
| Right message, incompatible schema version | `schema_version` | only if the receiver checks it |
| Right message, unimplemented command | `which_payload` dispatch | the `default:` arm → `UNSUPPORTED` |

The second row is the hole, and it is a real one: another application publishing to the
same topic, a stale host binary, a replayed payload from an older schema — all can be
well-formed Protobuf and still be nonsense. That is precisely what `schema_version` exists
to cover, and it is what makes the schema-versioning exercise a real exercise rather than a
formality.

---

## 7. nanopb: Protobuf under embedded constraints

Standard Protobuf libraries allocate freely — strings grow, repeated fields are vectors.
On a microcontroller with no heap that is unacceptable, so nanopb makes a different trade:
**every message becomes a fixed-size C struct whose worst case is known at build time.**

### Bounded fields become plain arrays

nanopb cannot know how long a `string` may get, so by default it emits a pointer field plus
callbacks or `malloc`. Giving it a bound in
[`proto/node.options`](../proto/node.options) changes that:

```
node.Ack.detail  max_size:48
```

produces

```c
char detail[48];
```

A plain array inside the struct. No allocation anywhere, and the footprint is visible in
the map file. `max_size` **counts the NUL terminator**, so 48 stores 47 characters, and
oversized input fails to encode rather than truncating silently.

The `.options` file is nanopb-only — a separate file precisely because it is a
target-specific decision, not part of the contract. The host generator ignores it entirely
and has no limits, so **the node is always the side that constrains the contract**. If a
host ever sends a `DeviceInfo` with a 40-character `board`, the node's 32-byte bound is
what decides the outcome.

### Generated size constants

nanopb computes the maximum encoded size of each message and emits it as a macro:

```c
#define node_Telemetry_size   36
#define node_Command_size     20
#define node_Ack_size        140
```

These are not magic. Each is the sum of every field's worst case, and working one out makes
the whole encoding concrete:

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

That is §4's union claim in arithmetic: the two empty arms contribute nothing, and adding a
fourth arm only grows `Command` if it is bigger than `SetInterval`.

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

Every `max_size` in `node.options` is visible in that total — which is what makes the
bounds a real budget rather than a formality.

The constants are what let the firmware declare exactly the right buffer:

```c
uint8_t payload[node_Telemetry_size];
```

If the schema grows, the constant grows with it and the buffer follows automatically at the
next build. Hard-coding `uint8_t payload[64]` would work today and overflow after an
innocuous schema edit.

### Streams

nanopb reads and writes through stream objects rather than buffers directly:

```c
pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
if (!pb_encode(&stream, node_Telemetry_fields, &msg)) { ... }
size_t written = stream.bytes_written;
```

The stream abstraction is what allows encoding straight to a socket or flash without an
intermediate buffer. Here it wraps a plain array — and because it knows the bound, a message
that does not fit **fails** rather than overflowing. `PB_GET_ERROR(&stream)` returns a
human-readable reason, which is what the firmware logs on a decode failure.

Note that `bytes_written` is the *actual* length (26 in §2), while the buffer was sized for
the worst case (36). The difference is what gets published.

Decoding mirrors it with `pb_istream_from_buffer()` and `pb_decode()`.

---

## 8. Reading the firmware

Three functions in `firmware/src/main.cpp` carry the whole serialisation story.

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

Read the signature first, because it is the design. The input is a `struct sensor_reading`
— the firmware's own type, off a zbus channel — and the output is wire bytes. **This
function is the only place in the firmware where the two representations meet.** The sensor
thread has never heard of `node_Telemetry`, and nothing downstream of here has heard of the
SCD-40, which is what keeps a schema change from rippling into the acquisition code. (See
[`zbus-guide.md`](zbus-guide.md) for the channel it arrives on.)

The mapping is deliberately explicit rather than a `memcpy` or a shared struct. Its cost is
a `switch` translating `SENSOR_READING_OK` into `node_SensorStatus_SENSOR_STATUS_OK`; its
benefit is that the internal enum and the wire enum can be renumbered independently, and
the compiler flags the mapping when either gains a value.

`node_Telemetry_init_zero` is a generated initialiser — always start from it, so fields
added later are not left holding stack garbage. Given §3, this also means any field left
untouched costs nothing on the wire.

`node_Telemetry_fields` is the generated **field descriptor table**: a compact description
of the schema that `pb_encode` walks at runtime. The schema exists as *data*, not as
generated code per field, which is a large part of why nanopb is small — one encoder walks
every message type rather than each message getting its own emitted serialiser.

Note the design choice around failure: a failed sensor read still reaches this function,
carrying `SENSOR_READING_ERROR` and zeroed measurements, and is published as
`sensor_status = ERROR`. Silence would be ambiguous — the host cannot distinguish a broken
sensor from a dead node — whereas an explicit status is a fact it can act on. Note also
what §3 does to such a message: the three zeroed measurement fields vanish from the wire
entirely, so an error report is one of the smallest messages the node ever sends.

### `handle_incoming_publish()`

The MQTT half is covered in the walkthrough; the Protobuf half is:

```c
node_Command cmd = node_Command_init_zero;
pb_istream_t stream = pb_istream_from_buffer(payload, kept);

if (oversized || !pb_decode(&stream, node_Command_fields, &cmd)) {
    send_ack(0, node_AckStatus_ACK_STATUS_MALFORMED, ...);
    return;
}
```

Two decisions worth noting. **Oversize is rejected outright** rather than decoded from a
truncated buffer — a partial message can decode into something plausible (§6), and acting on
half a command is worse than refusing it. And the failure Ack carries `sequence = 0`,
because the sequence could not be read: there is nothing to correlate against, and inventing
a number would be worse than admitting ignorance.

What follows the decode is the dispatch on `cmd.which_payload` from §4, with the `default:`
arm returning `UNSUPPORTED` — the branch that catches both a newer host and the
structurally-legal-but-meaningless payloads of §6.

### `send_ack()`

Builds an `Ack`, encodes it into a `node_Ack_size` buffer, and publishes at QoS 1. It is
best-effort by design — a failure here is logged, not propagated, because the command may
already have taken effect and there is nothing useful to undo.

---

## 9. Two toolchains, one contract

The same `.proto` feeds two completely independent generators:

| | Firmware | Host |
|---|---|---|
| Generator | nanopb, via `zephyr_nanopb_sources()` in `firmware/CMakeLists.txt` | `grpc_tools.protoc`, via `host/generate.sh` |
| Output | `node.pb.c` / `node.pb.h` (C) | `node_pb2.py` |
| Runs | Every build, into the build directory | On demand, gitignored |
| Bounds | Applies `node.options` | Ignores it |

Neither output is checked in. Both are regenerated from the same file, which is what makes
"single source of truth" a mechanical guarantee rather than a promise: there is no state in
which the firmware builds against a stale schema, because the schema is compiled on every
build.

### The gencode/runtime trap

Python's generated modules embed the compiler version and validate it at import:

```
gencode 7.35.1  runtime 5.29.6
Runtime version cannot be older than the linked gencode version.
```

A newer runtime can read older generated code, never the reverse. This bench hit exactly
that: a system `protoc` a major generation ahead of the available Python runtime.

`host/requirements.txt` therefore pins `grpcio-tools`, which bundles a `protoc` matched to
its own runtime, so the pair cannot drift — and deliberately does **not** touch the Zephyr
workspace venv, which is shared with another project and drives the nanopb generator.

**The firmware was unaffected by the same mismatch**, which is instructive: nanopb has
`protoc` emit a *descriptor set* — a serialised representation of the `.proto`, itself a
Protobuf message — and generates C from that with its own Python generator. It never
imports `protoc`'s generated Python, so the version check never runs. The incompatibility is
specific to Python's generated modules, not to Protobuf itself.

---

## 10. Exercising the encoding

Most of this guide can be re-derived from a terminal, with no board involved. The rest
needs the node and the harness on the Pi. Both are worth doing.

### Without hardware: take the format apart

Everything in §2 and §6 is reproducible from any machine with `protoc`:

```sh
# take apart any captured payload, no schema needed
printf '\x08\x01\x10\x2a\x5a\x00' | protoc --decode_raw

# ... or with the schema, to get names and types back
printf '\x08\x01\x10\x2a\x5a\x00' | protoc --decode=node.Command --proto_path=proto proto/node.proto

# and the size constants nanopb derived from it (after a build)
grep _size firmware/build/node.pb.h
```

The last one should print the constants §7 works through by hand — `node_Telemetry_size`
36, `node_Command_size` 20, `node_Ack_size` 140, `node_DeviceInfo_size` 75. If your
arithmetic in §7 disagrees with the generator, the generator is right and the interesting
question is which field's worst case you mis-counted.

**Exercise A — feel why field numbers are permanent (§5).** Change one field number in
`node.proto`, rebuild, and re-run the decode above. The bytes still decode cleanly, exit
status 0 — and they mean something entirely different. Nothing anywhere reports an error.
That is the single most important property of the format to internalise, and it takes
about thirty seconds to prove. Revert afterwards.

**Exercise B — confirm the type is not on the wire** (§2, *Nothing on the wire says which
message this is*)**.** Decode the same six bytes as
each of the three message types:

```sh
for T in Command Telemetry Ack; do
  printf '\x08\x01\x10\x2a\x5a\x00' | protoc --decode=node.$T --proto_path=proto proto/node.proto
done
```

All three succeed. **Proves:** a serialised message carries no type identity; the topic it
arrived on is what asserts the type, which is why the topic hierarchy is part of the
contract and not just housekeeping.

### With the node: the round trip

Start the harness on the Pi (`host/.venv/bin/python host/monitor.py`) and work through:

| Command | Expect | Proves |
|---|---|---|
| *(just watch)* | a decoded `Telemetry` every ~5 s | nanopb encode ↔ Python decode agree on field numbers, wire types and enum values |
| `command.py info` | `ACK_STATUS_OK` + `firmware`, `board`, `clientid` | `max_size`-bounded strings survive the round trip (§7) |
| `command.py trigger` | `ACK_STATUS_OK`, immediate telemetry | the empty-message `oneof` arm — two bytes on the wire — dispatches (§4) |
| `command.py interval 2000` | `ACK_STATUS_OK`, cadence changes | the arm that *carries* a field, and the largest one in the union (§7) |
| `command.py interval 100` | `ACK_STATUS_INVALID_ARGUMENT` + bounds in `detail` | rejected, not clamped — and `detail` is a bounded string |
| `command.py --sequence 42 trigger`, twice | second says `duplicate ignored`, no second measurement | `sequence` is what makes QoS 1 redelivery safe |

Restore with `command.py interval 5000`.

**Exercise C — the malformed case, and its limits (§6).** Publish something that is not a
`Command` at all:

```sh
mosquitto_pub -h 192.168.10.1 -t node/1/command -m garbage -q 1
```

The node answers `ACK_STATUS_MALFORMED` and **telemetry keeps flowing** — the payload was
drained from the socket rather than desynchronising the MQTT stream.

Now the important half. Publish two bytes that *are* structurally legal:

```sh
printf 'hi' | mosquitto_pub -h 192.168.10.1 -t node/1/command -q 1 -s
```

This one comes back `ACK_STATUS_UNSUPPORTED`, not `MALFORMED`. **Proves:** `MALFORMED`
catches corrupt framing, not wrong content. `garbage` was rejected only because its first
byte encodes wire type 7, which is invalid; `hi` decodes into field 13 as an unknown field
and yields a `Command` with everything defaulted. The node survives it by construction —
`which_payload == 0` hits the `default:` arm — not by detection. That gap is exactly what
`schema_version` exists to cover.

---

## 11. The model in one paragraph

A Protobuf message is **fields back to back, each preceded by a tag** that packs a field
number and a wire type into one varint — no header, no length, no checksum, and no type
name. That single layout explains almost everything else. The wire type is what lets a
decoder step over a field it has never heard of, which is the mechanism behind every
evolution rule: **adding a field is always safe, a field number is permanent, and a
deleted number must be reserved.** Fields equal to their default are simply not
transmitted, so absence and zero are indistinguishable for scalars — hence
`SENSOR_STATUS_UNSPECIFIED = 0` — while nested messages can express presence and so get a
real `has_` flag. A `oneof` is nothing special on the wire, just whichever arm is set; the
union is an API guarantee, and it costs the size of the largest arm rather than the sum.
What the format does **not** give you is semantics: it enforces structure, so a wrong
message type or a redefined unit decodes perfectly and means something else — which is why
`schema_version` carries breaking changes, `UNSUPPORTED` reports capability skew, and the
**MQTT topic is the type discriminator** and therefore part of the contract. nanopb takes
that format and adds one constraint, no allocation: `.options` bounds turn strings into
plain arrays and let the generator emit a worst-case size per message, so the firmware
sizes its buffers from `node_Telemetry_size` and the schema cannot outgrow them unnoticed.
Both toolchains generate from the same `.proto` on every build, which is what makes
"single source of truth" mechanical rather than aspirational.

## 12. Where to go next

- **[`proto/node.proto`](../proto/node.proto)** — the reference half of this guide. The
  schema decisions and their rationale live inline in its comments.
- **The schema-versioning exercise**, the one thing this repo still has outstanding: add a
  field, regenerate both sides, and prove an old reader still parses a new message and
  vice versa. §5 says it is safe; doing it is how you believe it.
- **[`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md)** — the
  surrounding MQTT client, including where `encode_telemetry()` and
  `handle_incoming_publish()` sit in the event loop.
- **[`zbus-guide.md`](zbus-guide.md)** — the internal channel a reading crosses *before*
  it reaches the encoder, and why the wire type deliberately stops there.
- **nanopb's own docs** — `concepts.md` and `reference.md` in the module source, for
  callbacks, `FT_POINTER` fields, and the options this project did not need.
