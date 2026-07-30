# CAN from first principles

*A from-first-principles guide to a bus with no addresses, no master, and eight bytes per frame — and what a protocol has to do about that.* For how *this* project configures and verifies the controller, see [`can-bringup.md`](../docs/can-bringup.md).

One thing up front, because it is the difference that explains most of the others: **MQTT is a messaging protocol running on a network, and CAN is a wire with a convention about voltage.** There is no broker, no connection, no address, and nothing that knows another node exists. What CAN has instead is a shared electrical medium where every node hears every bit at the same time, and a set of rules — arbitration, in-frame acknowledgement, error counters — that turn that into something reliable. Almost every design decision later in this guide follows from "every node hears every bit".

**The shape of this document:**

- **§1–§2** — what kind of thing CAN is, and the two wires underneath it. §2 is where the transceiver stops being an accessory and becomes mandatory.
- **§3–§4** — the frame, and the trick that makes the ID double as a priority.
- **§5–§6** — acknowledgement and error handling. Together these explain why a node alone on a bus cannot successfully transmit, which is the single most common bench surprise.
- **§7** — filters: how a receiver decides what it hears, when there are no addresses.
- **§8** — the eight-byte problem, and ISO-TP. This is the section that matters most for this project.
- **§9** — what this project wires up, and how each choice mirrors or inverts one from the MQTT side.
- **§10** — exercises on the running node, most of which need no hardware at all.
- **§11–§12** — the whole model in a paragraph, and where to go next.

Assumed: roughly what a bit on a wire is. No CAN knowledge. §8 and §9 compare against MQTT, so [`communication-guide.md`](communication-guide.md) helps there but is not a prerequisite.

---

## 1. A bus is not a network

Start with what you already have in this project. Ethernet plus MQTT gives you: a link where frames are addressed to a MAC, an IP layer that routes, a TCP connection that is established and can drop, and a broker that owns topics and decides who gets what. Five layers of indirection between "the sensor produced a number" and "the host read it".

CAN has none of that. A CAN bus is two wires that every node connects to in parallel, and the rules are:

- **Nobody is in charge.** There is no master, no arbiter, no coordinator. Any node may start transmitting whenever the bus is idle.
- **Nothing is addressed.** A frame carries an identifier, but that identifier names *the message*, not a sender and not a recipient. `0x702` does not mean "node 2"; it means "this is the thing we agreed `0x702` means".
- **Everybody hears everything.** Every node receives every frame. Receivers discard what they are not interested in (§7), but the discarding is a local decision, not a delivery decision.
- **There is no connection.** Nothing is established and nothing drops. A node that stops transmitting is indistinguishable from a node that was never there — which is why liveness has to be built on top (§9).

This is the same shape as **zbus**, oddly enough, and the comparison is worth holding onto because you already understand zbus from [`zbus-guide.md`](zbus-guide.md). A zbus channel is named, not addressed; publishers do not know who observes; observers opt in. CAN is that, with the channel name being the frame ID and the opt-in being a hardware acceptance filter. The difference is that zbus is inside one chip and cannot lose a message to electrical noise.

The consequence to carry forward: **CAN is message-oriented, not node-oriented.** Designing a CAN protocol is designing an ID space, not an address space.

## 2. Two wires, and a convention about which state wins

CAN's physical layer is a **differential pair**: CANH and CANL. A bit is not "is this wire high?" but "how far apart are these two wires?". Both idle at about 2.5 V — that is a **recessive** bit, logical 1. To send a **dominant** bit, logical 0, a transmitter pulls CANH up and CANL down, to roughly 3.5 V and 1.5 V.

Two properties fall out, and both are load-bearing.

**Differential signalling survives noise.** Interference couples into both wires roughly equally, so it shifts both voltages together and leaves the *difference* intact. This is why CAN works in a car engine bay, and it is why the pair is usually twisted. A single-ended signal would need a clean ground reference across the whole bus, which a vehicle does not have.

**Dominant always beats recessive.** This is the important one. The drivers are arranged so that if *any* node on the bus transmits dominant while others transmit recessive, the bus goes dominant. The bus is a **wired-AND**: the electrical state is the logical AND of what every node is driving. One node can pull the whole bus to 0; no node can force it to 1.

That single rule is what makes §4 (arbitration) and §5 (acknowledgement) possible. Both are just nodes deliberately overwriting each other's recessive bits.

### Why the transceiver is not optional

An MCU's CAN peripheral does not do any of this. It gives you two ordinary digital signals: `TX` (a push-pull output) and `RX` (an input), at 3.3 V logic. The **transceiver** — an SN65HVD230, TJA1051, MCP2562 — is the part that converts single-ended logic to the differential pair *and* implements the wired-AND behaviour.

So the tempting shortcut fails, and it fails instructively. Cross-connecting two boards directly (`A.TX → B.RX`, `B.TX → A.RX`) looks like it should work and does not, because of a rule from §4: **a transmitter must read its own bits back on `RX` while sending.** With crossed point-to-point wires, node A sends a dominant bit, samples its `RX`, sees B's idle recessive line, concludes the bus disagreed with it, and raises a bit error. It does not degrade — it fails immediately and retries into the error states of §6.

You *can* emulate the wired-AND without transceivers, and understanding how is a good check on whether §2 has landed: configure both `TX` pins as **open-drain**, tie `TX`, `TX`, `RX`, `RX` all to one node with a pull-up to 3.3 V and a shared ground. Now one wire genuinely is the logical bus — anyone pulling low is dominant, the pull-up supplies recessive — and arbitration, acknowledgement and error signalling all behave correctly. What you lose is the differential half: noise immunity, common-mode range, and any ability to attach a normal CAN tool. Fine for understanding, not a substitute for two €3 parts.

Never simply tie two push-pull `TX` pins together. One driving high while the other drives low is a short through both output stages.

### Termination

The pair is a transmission line, and an unterminated line reflects. CAN wants **120 Ω across CANH and CANL at each of the two physical ends** of the bus — two resistors total, no matter how many nodes, and specifically not one per node. Reflections corrupt bits at the sampling point, which produces the worst class of fault: it works at low bit rates and on short wires, then fails intermittently when you speed up or lengthen the cable. Many transceiver breakout boards include a 120 Ω resistor, which is convenient for a two-node bus and a trap for a three-node one.

## 3. The frame

A classic CAN data frame, in transmission order:

```
 SOF   IDENTIFIER    RTR  control    DATA         CRC      ACK    EOF
  │        │          │      │        │            │        │      │
  1 bit   11 bits    1 bit  6 bits   0–8 bytes   15+1 bits  2 bits 7 bits
  dom.    the ID            incl.    the payload           slot+
                            DLC                            delim
```

- **SOF** — one dominant bit. The bus was idle (recessive); this is what everyone synchronises on.
- **Identifier** — 11 bits in the standard format, giving 2048 IDs. There is an extended format with 29 bits, selected by a flag; this project uses standard IDs, which are also cheaper in hardware filter slots.
- **DLC** — the data length code, saying how many payload bytes follow. **0 to 8.** That is the whole budget, and §8 is about living with it.
- **CRC** — 15 bits over everything preceding, with a fixed polynomial. Catches the bit errors that arbitration and monitoring do not.
- **ACK slot** — the transmitter sends recessive here and *expects someone to overwrite it with dominant*. §5.
- **EOF** — seven recessive bits, which is also why §3's other mechanism exists.

**Bit stuffing.** After five consecutive identical bits, the transmitter inserts one bit of the opposite polarity, and receivers remove it. CAN has no separate clock line — receivers recover timing from edges in the data — so a long run of identical bits would let clocks drift apart. The stuffing guarantees an edge at least every five bits. The cost is that frame length is *not* fixed: the same 8-byte payload can occupy a different number of bit times depending on its bit pattern, which is why CAN bus-load calculations always work in worst cases.

Note what the frame does **not** contain: no source, no destination, no length beyond the payload, no sequence number, no protocol version. Everything about *meaning* lives in the ID and in the agreement between the nodes. This is the same fact [`protobuf-guide.md`](protobuf-guide.md) makes about Protobuf — the type is not on the wire — and the same one [`mqtt-design.md`](../docs/mqtt-design.md) states about topics. On CAN, the ID is what asserts the type.

## 4. Arbitration: the identifier is the priority

Two nodes start transmitting at the same moment. On Ethernet this is a collision: both frames are destroyed and both nodes back off for a random interval. On CAN, nothing is destroyed and no time is wasted. Here is the trick.

Every transmitting node **monitors the bus while it transmits**, bit by bit. The rule is:

- If a node sends recessive and reads back recessive, it is still winning; carry on.
- If a node sends **recessive** and reads back **dominant**, then somebody else is sending dominant — a lower ID — so it has lost. It stops transmitting immediately and becomes a receiver of the frame already in progress.
- If a node sends dominant and reads back recessive, something is wrong with the bus: that is a bit error (§6).

Because the identifier is transmitted most-significant-bit first and dominant is 0, **the frame with the numerically lowest ID wins**, and it wins without losing a single bit time. The loser has not corrupted anything; it simply retries when the bus goes idle. This is why CAN is used for real-time control: bus arbitration is deterministic, and the highest-priority message's worst-case latency can be calculated rather than measured.

Three consequences that matter in practice:

- **ID assignment is priority assignment.** Choosing `0x080` versus `0x700` for a message is choosing what it preempts. This is a protocol design decision, not a naming one.
- **Two nodes must never transmit the same ID.** They would arbitrate identically through the whole identifier field, then disagree in the data phase, and both would raise bit errors. One ID has exactly one transmitter.
- **A transmitter's own `RX` path is part of the protocol**, not a debugging convenience. This is the rule that kills the crossed-wires shortcut in §2.

## 5. Acknowledgement is in-frame, and collective

The transmitter sends the ACK slot as recessive. **Every node that received the frame with a valid CRC drives that bit dominant** — during the same frame, one bit time after the CRC. If the transmitter reads back dominant, at least one node heard it correctly.

This is unlike anything in the MQTT world, and the differences are worth being precise about.

- It is **in-frame**, not a separate response. There is no round trip, no packet, no matching. Acknowledgement costs one bit.
- It is **collective and anonymous**. The transmitter learns that *somebody* received the frame. It does not learn who, or how many. There is no way to ask "did node 2 get it?" — that requires an application-level reply, which is exactly what the `Ack` message in this project's schema is for.
- It is **not delivery to the intended recipient.** A frame can be ACKed by a node that filters it out at the application level.

And the consequence that surprises everyone at the bench: **a node alone on a bus cannot successfully transmit.** With no other node to drive the ACK slot, the transmitter reads back recessive, treats it as an acknowledgement error, increments its error counter, and retries — forever, or until it reaches the states in §6. A brand-new two-node bus where one node is unpowered does not look like "half working". It looks broken at both ends.

This is precisely why **internal loopback mode** (§10, Exercise 1) exists and why it is the right first test: the controller acknowledges its own frames internally, so you can prove timing, filters and the data path with nothing else attached.

## 6. Errors, counters, and the three states

CAN's error handling is unusually aggressive: nodes actively destroy frames they believe are corrupt, so that everyone agrees on what happened.

Five error types are detected: **bit error** (monitored level differs from the transmitted one, outside arbitration), **stuff error** (six identical consecutive bits), **CRC error**, **form error** (a fixed-format field has the wrong value), and **acknowledgement error** (§5). On detecting any of them, a node transmits an **error frame** — six consecutive dominant bits, which deliberately violates the stuffing rule so that every other node also detects an error and discards the frame. The transmitter then retries.

Each node keeps two counters, a **transmit error counter (TEC)** and a **receive error counter (REC)**. Errors increment them by 8 (roughly); successful transmission and reception decrement them by 1. That asymmetry means a node has to be persistently wrong, not occasionally unlucky, to escalate. The counters drive three states:

| State | Condition | Behaviour |
|---|---|---|
| **Error-active** | TEC and REC < 128 | Normal. Signals errors with dominant error frames. |
| **Error-passive** | either ≥ 128 | Still communicates, but signals errors *recessively* — it can no longer disturb the bus for everyone else. |
| **Bus-off** | TEC ≥ 256 | Disconnected from the bus. Transmits nothing, receives nothing. |

Bus-off is the mechanism that stops one broken node from taking down a working bus, and it is the state a lone transmitting node reaches (§5). Recovery requires 128 occurrences of 11 consecutive recessive bits, which Zephyr's driver can either do automatically or leave to the application; this project takes the automatic default, recorded in [`can-bringup.md`](../docs/can-bringup.md).

The practical read: **error counters are the first diagnostic, not the last.** `can show` prints both. Zeroes with nothing arriving means frames are not reaching the controller at all — wiring, termination or a stopped controller. Climbing TEC means your frames are going out and nothing is acknowledging them.

## 7. Filters: the receiver decides what it hears

With no addresses, every node receives every frame, and a 16 KB MCU cannot afford to wake for all of them. **Acceptance filters** are the hardware answer: a controller holds a small table of ID-plus-mask pairs, and only frames matching an entry are delivered to software at all. Non-matching frames are dropped in silicon, costing zero CPU.

A filter is an ID and a mask. The mask selects which bits of the ID must match: `id = 0x700, mask = 0x7FF` matches exactly `0x700`, while `mask = 0x700` matches any ID sharing those top bits — which is how a node subscribes to a *range* of message types. This is the hardware equivalent of MQTT's `node/#` wildcard, and it is doing the same job for the same reason.

Two behaviours matter more than the concept, and both are hardware-specific rather than architectural:

- **Filter slots are finite and small.** The H7's FDCAN offers 28 standard and 8 extended slots; smaller parts offer far fewer, and bxCAN on the F072 counts them in banks whose capacity depends on whether you use standard or extended IDs. Filter budget is a real design constraint.
- **Overlapping filters do not all fire.** Zephyr documents this as hardware-dependent (`include/zephyr/drivers/can.h:1335`), and on M_CAN the list is evaluated in order and **matching stops at the first match**. Install three filters for the same ID and you get three slots, three callbacks, and exactly one of them invoked. This has a sharp practical edge that §10's Exercise 3 demonstrates.

## 8. Eight bytes, and what ISO-TP does about it

Here is the problem this project exists to meet. A `Telemetry` message in [`node.proto`](../proto/node.proto) encodes to a few dozen bytes. A CAN frame carries **eight**. Something has to give, and there are exactly three honest options.

**Option 1: hand-pack the signals.** Define a byte-and-bit layout, scale each value into an integer field, and fit the important ones into 8 bytes. This is what the automotive world does, and a **DBC** file is the machine-readable form of that agreement: signal name, start bit, length, scale, offset, unit. It is compact, it costs nothing to encode, and its worst-case latency is one frame.

The price is everything Protobuf was giving you. There is no presence, so "absent" and "zero" become the same bits. Adding a field means finding spare bits, and old and new nodes disagree about what byte 6 means with no way to detect it. Changing a scale factor silently corrupts every reading. Schema evolution stops being a solved problem and becomes a spreadsheet.

**Option 2: split it yourself.** Invent a header — a message ID and a fragment index — and reassemble. This works and is what many in-house protocols do, but you will re-derive flow control, retransmission and reassembly timeouts, badly, over about two years.

**Option 3: ISO-TP.** ISO 15765-2, the transport layer defined for exactly this and the one **UDS** — automotive diagnostics — runs on. It carries up to 4095 bytes over classic CAN by segmenting, and it is worth knowing its four frame types because they are visible on the wire:

| PCI type | Name | Role |
|---|---|---|
| `0` | Single Frame (SF) | Payload ≤ 7 bytes, sent in one frame with a length nibble |
| `1` | First Frame (FF) | Starts a longer transfer, carrying the total length |
| `2` | Consecutive Frame (CF) | The remaining data, with a 4-bit rolling sequence number |
| `3` | Flow Control (FC) | The *receiver* replies: continue, wait, or abort |

The flow-control frame is the interesting part, and it is why ISO-TP is more than fragmentation. After the First Frame, the sender stops and waits. The receiver replies with a **block size** — how many consecutive frames it will accept before wanting another flow-control frame — and **STmin**, the minimum separation time between them. A 16 KB microcontroller can therefore throttle a sender that would otherwise overrun its buffers, and a busy bus can be given breathing room. This is backpressure, negotiated per transfer, in a protocol from 2004.

The cost is latency and bus load: a 40-byte payload becomes one FF, one FC, and five or six CFs — seven or eight frames and at least one round trip, where a hand-packed frame would have been one. So the choice is not "ISO-TP is better". It is:

- **Periodic, latency-sensitive, small, stable** → hand-pack it. Heartbeats and control signals.
- **Occasional, larger, evolving** → ISO-TP, and put a real schema on top.

That split is exactly what §9 does.

One last comparison, because it sharpens what MQTT was doing for you. TCP gives a **stream** with no message boundaries, so an application must add framing — a length prefix, usually. MQTT adds that framing itself, which is why this project's README can say there is no reassembly problem: each payload arrives whole. CAN gives **fixed-size datagrams with no stream at all**, so the framing problem returns in a different shape: not "where does this message end?" but "how do I say something longer than a frame?". Three transports, three different answers, and only the middle one was free.

## 9. What this project wires up

The H753ZI is a **gateway**: MQTT and Ethernet on one side, CAN on the other. The F072RB is a **peer node** with its own node identity and its own BME280, not a sensor slave. It encodes its own `Telemetry` and the gateway relays those bytes without decoding them.

```
        F072RB  (peer node)                      H753ZI  (gateway)                Pi
        ───────────────────                      ─────────────────                ──
   BME280 ──▶ sensor thread
                   │
                   ▼  encode (nanopb)
             Telemetry bytes
                   │
                   │  ISO-TP, 0x092  ──────────▶ relay thread
                   │                                  │  bytes, never decoded
                   │                                  ▼
                   │                            zbus channel ──▶ MQTT ──▶ node/2/telemetry
                   │
             heartbeat, raw ──────────────────▶ rx filter ──▶ liveness ──▶ node/2/status
             single frame, 0x0A2, 1 Hz                          timeout
                   │
                   ◀── ISO-TP, 0x082 ────────── relayed Command from node/2/command
```

Two payload styles on one bus, chosen by §8's rule:

| | Heartbeat | Telemetry, commands, acks |
|---|---|---|
| Encoding | hand-packed 8 bytes | Protobuf, from `proto/node.proto` |
| Transport | one raw CAN frame | ISO-TP, segmented |
| Rate | 1 Hz, fixed | on the sample period, or on demand |
| Evolution | a version byte, and that is all | field numbers and presence |
| Why | liveness must not depend on ISO-TP state | it does not fit, and it must evolve |

And the property the whole arrangement exists to demonstrate: **the gateway does not decode the payloads it relays.** Add a field to the F072's schema, reflash only the F072, and the gateway carries the new bytes untouched while an old host still decodes them. That is the end-to-end principle made testable rather than asserted.

The mirror-image comparison against the MQTT side is where the two halves of this project meet:

| | MQTT side | CAN side |
|---|---|---|
| What names a message | the topic | the frame ID |
| Who decides what arrives | broker subscription | hardware acceptance filter |
| Framing | free — payloads arrive whole | 8 bytes, so ISO-TP or hand-packing |
| Acknowledgement | PUBACK, per hop, per recipient | one bit, in-frame, anonymous |
| Liveness | keepalive plus a retained Last Will | a heartbeat and a timeout, in firmware |
| Priority | none; QoS is about reliability | the ID, by arbitration |

The liveness row is the one to dwell on. MQTT hands you a broker that notices a dead node and publishes on its behalf. CAN hands you nothing — a silent node is indistinguishable from an absent one (§1) — so the gateway has to build it: a heartbeat at a known ID, a timeout, and an explicit publish. Every mechanism MQTT gave away for free has to be re-earned, and re-earning it is how you learn what it was doing.

The decisions behind all of this — the pin choice, the bitrate, the ID map, the timeouts — are recorded in [`can-bringup.md`](../docs/can-bringup.md). This guide explains the concepts; that page records what was chosen.

## 10. Exercising the CAN link

Most of this needs **no CAN hardware at all** — internal loopback proves the controller against itself. You need the console on the Mac (`./scripts/console.sh`, quit with **Ctrl-A** then **K**), and nothing else. The device is `can@4000a000`; tab completion after `can mode ` will fill it in.

### Exercise 1 — A frame out and back, with no bus

*Demonstrates §3 (the frame), §5 (why loopback is the right first test) and §7 (filters).*

```
uart:~$ can show can@4000a000
uart:~$ can mode can@4000a000 loopback
uart:~$ can start can@4000a000
uart:~$ can filter add can@4000a000 0x702
uart:~$ can send can@4000a000 0x702 01 02 03 04 05 06 07 08
```

`can show` should report `core clock: 80000000 Hz`, `capabilities: normal loopback listen-only`, and `state: stopped`. After the `send`:

```
can@4000a000       702   [8]  01 02 03 04 05 06 07 08
```

The subcommand is `can filter add`, not `can add` — a bare `can add` prints the entire help text rather than a useful error. If `can mode` returns `-EBUSY`, the controller is already running; `can stop` first, since mode changes require the stopped state.

**Proves:** bit timing accepted at 500 kbit/s, the clock tree, driver binding, filter installation, and both data paths — everything except the physical layer. Note what makes this possible at all: in normal mode with no second node, this frame would never be acknowledged, the error counter would climb, and §6 would eventually take the controller bus-off. Loopback has the controller acknowledge itself, which is why it is the first test and not the last.

### Exercise 2 — Three filters, one callback

*Demonstrates §7 (first-match semantics).*

With loopback still running from Exercise 1, add the same filter twice more, then send:

```
uart:~$ can filter add can@4000a000 0x702
uart:~$ can filter add can@4000a000 0x702
uart:~$ can send can@4000a000 0x702 AA BB CC DD
```

Each `filter add` reports a distinct `filter ID` — 0, 1, 2 — so three hardware slots really were allocated and three callbacks really were registered. But the frame prints **once**, not three times.

**Proves:** filter matching stops at the first match on this hardware, and Zephyr does not deduplicate (`can_mcan_add_rx_filter_std()`, `drivers/can/can_mcan.c`). Two of those three slots are dead weight. The alternative design — every matching filter firing — would make overlapping subscriptions composable, and would also mean one frame could wake several handlers, which is exactly what a filter is supposed to prevent.

### Exercise 3 — Make `can dump` lie to you

*Demonstrates §7, and why the previous exercise is not academic.*

Still in loopback, with the `0x702` filters installed:

```
uart:~$ can dump can@4000a000
uart:~$ can send can@4000a000 0x702 11 22 33
```

`can dump` installs a catch-all filter — `id = 0, mask = 0` (`drivers/can/can_shell.c:481`) — which should match everything. Watch which handler actually prints, and note that the catch-all landed in a *higher* slot than the specific filters from Exercise 1, because slots are allocated lowest-free-first.

**Proves:** a specific filter installed earlier shadows a later catch-all completely. Once the relay's heartbeat filter occupies slot 0 at boot, `can dump` can show **nothing at all** while heartbeats arrive perfectly. This is the practical reason to reach for `can filter add <id>` rather than `can dump` when debugging a live node — and the reason an empty dump must never be read as a dead bus.

### Once the transceivers arrive

Three more exercises belong here and are deliberately not written yet, because a procedure nobody has run is not a procedure: the two-node round trip at 500 kbit/s, deliberately mismatching the bitrate to see what a misconfigured bus looks like from both ends, and watching the error counters climb toward bus-off on a node whose peer is powered down (§5, §6). They arrive with the hardware.

## 11. The model in one paragraph

CAN is two wires on which every node hears every bit, plus one convention — dominant beats recessive — from which everything else is built: the lowest identifier wins arbitration without wasting a bit time, any receiver acknowledges by overwriting a single recessive bit in the sender's own frame, and nodes that are persistently wrong escalate through error-passive to bus-off so that one broken device cannot hold the bus. There are no addresses, so an identifier names a message rather than a node and receivers opt in with hardware filters; there is no connection, so a silent node and an absent node are the same thing and liveness must be built on top; and there are eight payload bytes, so anything larger is either hand-packed into a fixed bit layout or segmented with ISO-TP, which negotiates flow control per transfer. Everything MQTT was quietly providing — framing, addressing, liveness, per-recipient acknowledgement — has to be re-earned here, and re-earning it is the point.

## 12. Where to go next

- **[`can-bringup.md`](../docs/can-bringup.md)** — the reference half: which pins, which bitrate and why it must be stated explicitly, what the board's devicetree already provides, and the bring-up checks in procedure form.
- **[`communication-guide.md`](communication-guide.md)** — the MQTT side of every comparison in §9. If §8's framing argument was the interesting part, that guide is where the thing being compared against is built up.
- **[`protobuf-guide.md`](protobuf-guide.md)** — what rides inside the ISO-TP payloads, and why "the type is not on the wire" (§3) is a schema problem rather than a transport one.
- **[`zbus-guide.md`](zbus-guide.md)** — the in-process bus the relayed bytes land on once they are off the wire, and a useful contrast: the same publish/subscribe shape, without the possibility of electrical failure.
