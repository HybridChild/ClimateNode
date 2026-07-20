# Understanding Protobuf and nanopb

*A from-first-principles guide to how a sensor reading becomes bytes on the wire, and back.*

A teaching document. It explains the concepts in the order that makes them easiest to
learn, using this repository's contract — [`proto/node.proto`](../proto/node.proto) — as
the running example. Every byte sequence below is real: taken from packets this bench
actually sent, and re-verified with `protoc --decode_raw`.

For how those bytes get carried, see [`communication-guide.md`](communication-guide.md)
(MQTT concepts) and [`mqtt-design.md`](mqtt-design.md) (this project's topic and QoS
decisions). For the surrounding firmware, see
[`firmware-mqtt-walkthrough.md`](firmware-mqtt-walkthrough.md).

---

## 1. Why a schema at all

Before Phase 4 this node published telemetry as text:

```
seq=1 co2=812 temp=22.41 rh=41.3
```

That is readable, debuggable with `mosquitto_sub`, and needs no tooling. It is also a
contract that exists only in the heads of the people who wrote both ends. Nothing stops
the host from expecting `co2_ppm=` while the node sends `co2=`. Nothing records that
`temp` is Celsius. Adding a field means every parser must be updated in lockstep, and a
typo becomes a runtime surprise rather than a build error.

A schema moves that contract into a file both sides generate from. The trade is
readability on the wire for **a single definition neither side can silently diverge from**
— which is why `proto/` is described as the source of truth and why generated code is
never hand-edited.

Protobuf adds three things beyond "an agreed format":

1. **Compactness** — the 36-byte-max `Telemetry` above would be ~40 characters as text,
   and considerably more as JSON.
2. **Typed fields** — `float` vs `uint32` is decided once, not re-parsed per reader.
3. **Defined evolution rules** — the part that actually matters, covered in §5.

## 2. The wire format: tag-length-value

A Protobuf message is a flat sequence of **fields**, each preceded by a tag. There is no
message header, no field count, no terminator, and — importantly — **no magic number and
no checksum**. A message is just fields, one after another, until the buffer ends.

Each field is:

```
[tag varint][value]
```

where the tag packs two things into one number:

```
tag = (field_number << 3) | wire_type
```

The wire type (3 bits) tells a decoder *how to read the next bytes* even if it has never
heard of the field number:

| Wire type | Meaning | Used by |
|---|---|---|
| 0 | varint | `uint32`, `int32`, `bool`, enums |
| 1 | 64-bit fixed | `double`, `fixed64` |
| 2 | length-delimited | `string`, `bytes`, nested messages |
| 5 | 32-bit fixed | `float`, `fixed32` |

Types 3, 4 are deprecated; 6 and 7 are **invalid** — remember that, it explains §10.

### Varints

Integers are encoded seven bits at a time, low group first, with the top bit of each byte
meaning "another byte follows". Small numbers are small:

| Value | Encoding | Bytes |
|---|---|---|
| 1 | `01` | 1 |
| 42 | `2a` | 1 |
| 654128137 | `89 e8 f4 b7 02` | 5 |

This is why field numbers 1–15 matter: their tag fits in one byte, while 16+ needs two.
`Telemetry` is the high-rate message, so `node.proto` deliberately keeps all its fields in
that range.

### Decoding a real packet

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

`0x08 >> 3 = 1`, `0x08 & 7 = 0`. `0x62 >> 3 = 12`, `0x62 & 7 = 2`.

The same command with `--sequence 42` was **6 bytes**:

```
08 01 10 2a 5a 00
```

Identical structure; the sequence collapsed from 5 bytes to 1 because varints are
magnitude-sensitive. And field 11 (`trigger_measurement`) instead of 12, so `0x5a`
rather than `0x62`.

Note what the empty message costs: **two bytes**, a tag and a zero length. Its *presence*
is the whole instruction — exactly as `node.proto` claims when it says an empty message
"is the instruction".

You can do this yourself on any captured payload:

```sh
printf '\x08\x01\x10\x2a\x5a\x00' | protoc --decode_raw
```

`--decode_raw` needs no schema — it recovers field numbers and wire types straight from
the bytes, which is exactly what a decoder does before it knows what the fields *mean*.

## 3. What is not on the wire

This is the biggest conceptual trap in proto3.

**Fields equal to their default value are not transmitted at all.** A `uint32` that is 0,
a `float` that is 0.0, a `string` that is empty, an enum that is 0 — all are simply absent
from the encoded bytes. The decoder fills them in from the defaults afterwards.

Two consequences worth internalising:

**Absence and zero are indistinguishable for scalars.** A decoder receiving no `co2_ppm`
field yields `co2_ppm == 0`, and cannot tell "the node said zero" from "the node said
nothing". That is why `SENSOR_STATUS_UNSPECIFIED = 0` exists in `node.proto` with a comment
saying it must mean "no information" — the zero value of every enum doubles as the value
you get from a message that never mentioned it. Give it a meaning that is safe as a
default.

**Message fields are different.** A nested message *can* distinguish absence, because
presence is encoded by the tag existing at all. That is why `Ack.device_info` gets a real
presence flag in the generated C:

```c
bool has_device_info;
node_DeviceInfo device_info;
```

and why `command.py` can ask `ack.HasField("device_info")`. Scalars have no `HasField`
in proto3 unless explicitly marked `optional`.

## 4. `oneof` is a tagged union

```proto
oneof payload {
  SetInterval set_interval = 10;
  TriggerMeasurement trigger_measurement = 11;
  GetDeviceInfo get_device_info = 12;
}
```

On the wire a `oneof` is nothing special — just whichever field is set, encoded normally.
The guarantee is at the API level: setting one member clears the others, so exactly one
can be present.

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
`node_Command_*_tag` constants. The union means `Command` costs the size of its **largest**
arm, not the sum — relevant when arms grow.

The numbering starting at 10 is deliberate: it leaves 3–9 free for future fields that
apply to *every* command, without disturbing the arms.

## 5. Evolution: the actual point of Protobuf

Decoders skip fields they do not recognise. Because the wire type is embedded in every
tag, a decoder that meets unknown field 47 still knows how many bytes to step over — so an
old reader can parse a new message without being updated.

That single property produces the rules at the top of `node.proto`:

- **Adding a field is always safe.** Old readers ignore it; new readers see the default
  when old writers omit it.
- **Never change or reuse a field number.** The number *is* the identity. Renaming a field
  changes nothing on the wire; renumbering changes everything.
- **Deleting means `reserved`-ing the number**, so nobody reuses it later and silently
  reinterprets old data as a new meaning.

Note what is *not* protected: changing a field's **type** or its **meaning** under the same
number. `uint32 co2_ppm` reinterpreted as "co2 in tenths of ppm" still decodes cleanly and
reports readings ten times too low. Protobuf enforces structure, not semantics.

### Two layers of versioning

That gap is why every message here also carries `schema_version`, and why its comment says
"bumped only on a BREAKING change":

- **Additive change** — new field, new enum value, new `oneof` arm. Protobuf handles it.
  `schema_version` does not move.
- **Breaking change** — a field's meaning, unit, or type changes. Nothing in the encoding
  can detect this, so the version field is the only signal, and receivers must check it.

Most schemas need the second case rarely. It is worth carrying the field anyway, because
retrofitting a version into a format that has none requires the very coordinated update it
would have prevented.

`AckStatus` covers the matching case in the other direction: `ACK_STATUS_UNSUPPORTED`
is returned when a command **decodes perfectly** but this firmware has no handler — the
host is newer than the node. That is not an error in the wire format; it is a version skew
the protocol is designed to report rather than crash on.

## 6. nanopb: Protobuf under embedded constraints

Standard Protobuf libraries allocate freely — strings grow, repeated fields are vectors.
On a microcontroller with no heap that is unacceptable, so nanopb makes a different trade.

### Bounded fields become plain arrays

nanopb cannot know how long a `string` may get, so by default it emits a pointer field
plus callbacks or `malloc`. Giving it a bound in
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

The `.options` file is nanopb-only. The host generator ignores it entirely and has no
limits — so **the node is always the side that constrains the contract**.

### Generated size constants

nanopb computes the maximum encoded size of each message:

```c
#define node_Telemetry_size   36
#define node_Command_size     20
#define node_Ack_size        140
```

which is what lets the firmware declare exactly the right buffer:

```c
uint8_t payload[node_Telemetry_size];
```

If the schema grows, that constant grows with it and the buffer follows automatically.
Hard-coding `uint8_t payload[64]` would work today and overflow after an innocuous schema
edit.

### Streams

nanopb reads and writes through stream objects rather than buffers directly:

```c
pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
if (!pb_encode(&stream, node_Telemetry_fields, &msg)) { ... }
size_t written = stream.bytes_written;
```

The stream abstraction is what allows encoding straight to a socket or flash without an
intermediate buffer. Here it wraps a plain array — and because it knows the bound, a
message that does not fit **fails** rather than overflowing. `PB_GET_ERROR(&stream)`
returns a human-readable reason.

Decoding mirrors it with `pb_istream_from_buffer()` and `pb_decode()`.

## 7. Reading the firmware

Three functions in `firmware/src/main.cpp` carry the whole Phase 4 change.

### `encode_telemetry()`

```c
node_Telemetry msg = node_Telemetry_init_zero;
msg.schema_version = kSchemaVersion;
msg.sequence = sequence;
...
pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
if (!pb_encode(&stream, node_Telemetry_fields, &msg)) { ... }
return stream.bytes_written;
```

`node_Telemetry_init_zero` is a generated initialiser — always start from it, so fields
added later are not left holding stack garbage. `node_Telemetry_fields` is the generated
**field descriptor table**: a compact description of the schema that `pb_encode` walks. The
schema exists at runtime as data, not as generated code per field, which is a large part of
why nanopb is small.

Note the design choice around failure: a failed sensor read still publishes, carrying
`sensor_status = ERROR` with zeroed measurements. Silence would be ambiguous — the host
cannot distinguish a broken sensor from a dead node — whereas an explicit status is a fact
it can act on.

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
truncated buffer — a partial message can decode into something plausible, and acting on
half a command is worse than refusing it. And the failure Ack carries `sequence = 0`,
because the sequence could not be read: there is nothing to correlate against, and
inventing a number would be worse than admitting ignorance.

### `send_ack()`

Builds an `Ack`, encodes it into a `node_Ack_size` buffer, and publishes at QoS 1. It is
best-effort by design — a failure here is logged, not propagated, because the command may
already have taken effect and there is nothing useful to undo.

## 8. Two toolchains, one contract

The same `.proto` feeds two completely independent generators:

| | Firmware | Host |
|---|---|---|
| Generator | nanopb, via `zephyr_nanopb_sources()` in `firmware/CMakeLists.txt` | `grpc_tools.protoc`, via `host/generate.sh` |
| Output | `node.pb.c` / `node.pb.h` (C) | `node_pb2.py` |
| Runs | Every build, into the build directory | On demand, gitignored |
| Bounds | Applies `node.options` | Ignores it |

Neither output is checked in. Both are regenerated from the same file, which is what makes
"single source of truth" a mechanical guarantee rather than a promise.

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
workspace venv, which is shared with another project.

**The firmware was unaffected by the same mismatch**, which is instructive: nanopb has
`protoc` emit a *descriptor set* — a serialised representation of the `.proto` — and
generates C from that. It never imports generated Python, so the version check never runs.
The incompatibility is specific to Python's generated modules, not to Protobuf itself.

## 9. What the bench verified

Running the harness against the node (2026-07-20) exercised the whole path:

- Telemetry encoded by nanopb, decoded by Python, fields and enums agreeing
- All three `oneof` arms dispatched correctly
- `max_size`-bounded strings surviving the round trip in `DeviceInfo`
- An out-of-range `SetInterval` rejected with `INVALID_ARGUMENT` rather than clamped
- A redelivered command acknowledged but not re-executed
- A malformed payload answered with `MALFORMED` without desynchronising MQTT

## 10. What `MALFORMED` cannot catch

The malformed test published the ASCII string `garbage`:

```
67 61 72 62 61 67 65
```

The first byte decides it: `0x67 >> 3 = 12`, `0x67 & 7 = **7**` — and wire type 7 is
invalid (§2). The decoder rejects the message before reading anything else.

But that was luck, not protection. Protobuf has **no magic number and no checksum**, so a
byte sequence that happens to be structurally legal decodes cleanly into a message with
unknown fields and defaulted values — and a node acting on `Command{}` would see
`which_payload == 0` and fall through to `UNSUPPORTED`. Safe here, by construction rather
than by detection.

The general lesson: `MALFORMED` catches corrupt **framing**, not wrong **content**. A
payload from an incompatible schema version, or from an entirely different application
publishing to the same topic, can be perfectly well-formed Protobuf and still be nonsense.
That is precisely the hole `schema_version` exists to cover, and it is what makes the
schema-versioning exercise a real exercise rather than a formality.
