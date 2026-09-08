# Understanding the Communication Architecture

*A from-first-principles guide to how one sensor reading gets from the SCD-40 to the host PC.*

This is a teaching document, not project documentation. It explains the concepts and the flow in the order that makes them easiest to learn, using this repository — a CO₂ sensor node on an ST Nucleo-H753ZI talking to a Raspberry Pi — as the running example. For the project's actual decisions (topic table, QoS per topic, broker config), see its companion, [`mqtt-design.md`](../docs/mqtt-design.md). Once these concepts make sense, [`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md) reads the firmware that implements them, line by line.

**Prerequisites:** that TCP/IP exists. No MQTT knowledge at all — the broker, topics, QoS, sessions and the will are built from nothing — and no devicetree or Kconfig, so this guide pairs with [`protobuf-guide.md`](protobuf-guide.md) as a self-contained route through the comms material, readable without any of the Zephyr-side guides.

**The shape of this document:**

- **§1–§2** — the stack as a chain of "…and therefore we also need…", and what each layer refuses to do for you.
- **§3** — the broker, and the shift in responsibility that is the conceptual jump of the whole architecture. The longest section, and the one worth slowing down for.
- **§4–§7** — the four things MQTT adds on top of "deliver bytes": topics, QoS, sessions and keepalive, then retain and the Last Will.
- **§8** — the whole path, end to end, and the one idea MQTT turns out to share with the two layers either side of it.
- **§9** — a runnable lab: six exercises, five of them needing nothing but two CLI tools.
- **§10–§11** — the whole model in a paragraph, and where to go next.

---

## 1. Why this feels like a lot of moving parts

The README names a pile of things: Ethernet, LAN8742, IP, TCP, MQTT, Mosquitto, QoS, topics, Protobuf, nanopb, zbus, paho-mqtt. Listed flat like that, it reads as a dozen unrelated technologies to learn.

They are not unrelated, and there are not a dozen. They are a **stack**, and each layer exists to solve exactly one problem that the layer below it cannot. Once you can say what each layer gives you — and, just as important, what it refuses to give you — the pile collapses into a short chain of "…and therefore we also need…".

The chain is:

> Wires move **bits** → but who are they for? → **Ethernet** moves *frames* between two adapters → but only across one cable → **IP** moves *packets* between addresses → but unreliably and out of order → **TCP** gives a *reliable byte stream* → but with no message boundaries and no fan-out → **MQTT** gives *whole messages* to *whoever is interested* → but they are just opaque bytes → **Protobuf** gives them *meaning*.

Everything below MQTT, Zephyr implements for you. Everything from MQTT up is your work.

This guide takes that lower half as a given and builds upward. If you want the *other* direction — how those layers actually get wired up, how a handful of `CONFIG_NET_*` lines and one devicetree node become a live socket with no application code, and how much would change if the wire were USB or Wi-Fi — that is [`network-stack-guide.md`](network-stack-guide.md), paired with [`network-bringup.md`](../docs/network-bringup.md).

---

## 2. The same chain, as a reference table

§1 told the chain as a story; here it is as a table to come back to. The right-hand column is the one that does the work — what each layer *refuses* to do is precisely why the next one up has to exist.

| Layer | Gives you | Does **not** give you (⇒ why the next layer exists) |
| --- | --- | --- |
| **Ethernet** (MAC + PHY) | A frame delivered to a MAC address on this cable | Any notion of "which machine" beyond this one link |
| **IP** | Addressing: `192.168.10.1` ↔ `.2` | Reliability, ordering, or the concept of a conversation |
| **TCP** | A reliable, ordered **byte stream** to a port | **Message boundaries**, and any fan-out to multiple readers |
| **MQTT** | Whole **messages**, delivered by **topic** to any number of subscribers | Any idea what the bytes *mean* |
| **Protobuf** | **Meaning**: typed fields, and a versionable schema | — |

Two rows deserve emphasis, because they are the ones people skip:

**TCP gives you a byte stream, not messages.** If you write three 100-byte messages, the receiver may read 300 bytes at once, or 47 then 253. TCP faithfully preserves *order* and *bytes*; it has no concept whatsoever of where one message ends and the next begins. Over raw TCP, you must invent that yourself — typically a length prefix, plus buffering and reassembly logic. **MQTT solves this**, which is why the README can say there is no app-level framing to write: every payload arrives whole or not at all. Worth knowing what that is worth: [`can-guide.md`](can-guide.md) §8 is the same problem on a transport that does *not* solve it, where eight payload bytes per frame force either a hand-packed bit layout or ISO-TP segmentation. Three transports, three different answers, and only this one is free.

**MQTT gives you delivery, not meaning.** An MQTT payload is an opaque byte array. The broker never looks inside it. Sending `{"co2": 812}`, or `812`, or a Protobuf blob, is all the same to MQTT. Choosing Protobuf is a decision that lives entirely *above* MQTT.

### The journey of one reading

Each layer wraps the one above it — this is **encapsulation**:

```text
                     ┌──────────────────────────────────────┐
Protobuf  Telemetry: │ co2=812  temp=22.5  rh=41.2  seq=7 … │   26 bytes
                     └──────────────────────────────────────┘
                     ┌────────────┬─────────────────────────┐
MQTT      PUBLISH:   │ hdr+topic  │  <the 26 bytes above>   │
                     └────────────┴─────────────────────────┘
                     ┌────────────┬─────────────────────────┐
TCP       segment:   │ src/dst    │  <the MQTT packet>      │
                     │ port       │                         │
                     └────────────┴─────────────────────────┘
                     ┌────────────┬─────────────────────────┐
IP        packet:    │ 10.2→10.1  │  <the TCP segment>      │
                     └────────────┴─────────────────────────┘
                     ┌────────────┬─────────────────────┬───┐
Ethernet  frame:     │ dst/src MAC│  <the IP packet>    │CRC│
                     └────────────┴─────────────────────┴───┘
                                    → out the RJ45 →
```

The Pi unwraps it in exactly the reverse order and hands your 26 bytes to the **host harness**: the Python tooling under `host/`, run on the Pi, which is this project's own MQTT client — `monitor.py` receives these messages and prints them decoded, and `command.py` sends commands back to the node and waits for the reply. (The `…` is the rest of Telemetry's fields — uptime, sensor status, schema version — which §9 shows decoded.)

### What's in that `hdr`? The part that matters here

The `hdr+topic` box above is MQTT's **fixed header** followed by its **variable header**. Most of its fields belong to concepts that arrive later — QoS (§5) and retain (§7), picked back up there. One field, though, is the whole reason the table above says MQTT is where framing gets solved:

```text
Fixed header
   byte 1      packet type (= PUBLISH) + four flag bits         → §5, §7
   1–4 bytes   Remaining Length — how many bytes follow         ← the framing answer
Variable header
   2 bytes     length of the topic string
   N bytes     the topic itself, e.g. "node/1/telemetry"        → §4
   2 bytes     packet identifier — QoS 1 and 2 only             → §5
Payload
   N bytes     your Protobuf, carried verbatim and never inspected
```

**`Remaining Length` is how MQTT solves the framing problem.** It states exactly how many bytes this message occupies, so the receiver knows where this message ends and the next begins — the length prefix you would otherwise have had to design, implement, and debug yourself on top of raw TCP. The rest of the header carries concepts you have not met yet; the arrows point to where each is picked up.

---

## 3. Why a broker? The shift you have to make

The table credited MQTT with two jobs TCP won't do: frame whole messages — that was §2's header — and deliver each to *any number of subscribers*. That second job is the conceptual jump of the whole architecture, and it is worth slowing down for. It changes *who is responsible for what* — and in doing so, deletes a pile of code the node would otherwise have to carry.

### 3.1 The problem: client/server puts the node in charge of its consumers

With raw TCP you would write a **client/server** pair. Say the Nucleo is the server: it holds a socket open, the Pi connects to it, and the two talk directly. Each endpoint knows about the other.

Now add a second consumer — a dashboard, a logger. The node must:

- **accept** each new connection as it arrives,
- keep a **roster** of every connected consumer's socket,
- **add and remove** entries as consumers connect, disconnect, or crash,
- and **loop over that roster**, writing a copy of every reading to each one.

That is all bookkeeping about *who is currently connected*, it lives in the firmware, it grows with every consumer added — and none of it has anything to do with measuring CO₂.

### 3.2 The answer: everyone connects to a broker

MQTT replaces the direct client/server relationship with **publish/subscribe through a broker**: a middleman that every participant connects to.

```text
      Nucleo                    Raspberry Pi                   other clients
   ┌──────────┐              ┌───────────────┐              ┌──────────────┐
   │ publisher├──connect────▶│  Mosquitto    │◀───connect───┤ paho harness │
   └──────────┘              │   (broker)    │              └──────────────┘
                             │               │              ┌──────────────┐
                             │ LISTENING on  │◀───connect───┤  dashboard   │
                             │ 192.168.10.1  │              └──────────────┘
                             │ :1883         │
                             └───────────────┘
```

**Every arrow points at the broker.** The publisher and the subscribers never connect to each other; they may not even know the other exists. The broker is the only thing any of them talks to.

**Exercise 1 in §9 demonstrates this directly:** `mosquitto_pub` publishes happily whether or not a `mosquitto_sub` is running — it hands its message to the broker and exits, never knowing whether anyone was there to receive it.

### 3.3 The vocabulary: MQTT is a protocol, Mosquitto is a program

Two words used above name different *kinds* of thing, and keeping them apart prevents a lot of muddle:

- **MQTT** is the **protocol** — the written rules for what bytes go on the wire (`CONNECT`/`CONNACK`, `SUBSCRIBE`/`SUBACK`, `PUBLISH`/`PUBACK`, topics, QoS flags). This project uses version **3.1.1**. A protocol is a *contract*, not a program.
- **Mosquitto** is one **program that implements** that protocol — specifically the **broker** role. The clients are implementations too, just of the other role.

The same split exists on the web:

```text
HTTP   : MQTT           the protocol / contract
nginx  : Mosquitto      a server / broker implementation
curl   : mosquitto_pub  a client implementation
```

| Role | Speaks MQTT as… | Programs here |
| --- | --- | --- |
| **Broker** | server | Mosquitto |
| **Client** | client | `mosquitto_pub` / `mosquitto_sub`, the paho-mqtt harness, Zephyr's `CONFIG_MQTT_LIB` (the Nucleo) |

Why this isn't pedantry: **the Nucleo's MQTT code and Mosquitto share zero source code.** One is a C library inside Zephyr; the other is a standalone C program from an unrelated project. They interoperate *only* because both conform to the protocol — which is the whole value of a protocol being a written spec rather than a particular program. Any conforming client talks to any conforming broker: swap Mosquitto for HiveMQ or EMQX, or paho for a C# client, and nothing else changes.

So frame the work ahead accordingly: when you write the firmware you are not "programming Mosquitto" — you are implementing the **client side of MQTT 3.1.1**, and Mosquitto is merely the conforming broker you happen to test against.

### 3.4 Who listens? Exactly one program does

"Listening" is overloaded in everyday speech, so this guide uses it in one strict sense:

> **Listening** = holding a **listening socket**: a socket bound to a port and sitting in `accept()`, **open to new inbound TCP connections**. This is what makes a program a *server*.

Crucially, that is **not** the same as *receiving data*. A program can read incoming bytes all day on a connection **it opened itself** without ever listening. Keeping these two apart resolves most confusion about who is a server here:

| | **Listening** — open to new inbound TCP connections | **Receives data** |
| --- | --- | --- |
| **Mosquitto** (broker) | ✅ yes — on `192.168.10.1:1883` | ✅ |
| **Nucleo** | ❌ never | ✅ commands |
| `mosquitto_sub` | ❌ never | ✅ `SUBACK`, published messages |
| `mosquitto_pub` | ❌ never | ✅ `CONNACK`, `PUBACK` |

So in this entire system there is **exactly one listening socket** — Mosquitto's, on port
1883. Every other participant, the Nucleo included, only ever **dials out**. None of them is a server; all of them receive data.

That one port serves them all at once because a TCP connection is identified by its whole `(client IP, client port, broker IP, 1883)` tuple, not by the port alone — so each client's connection stays distinct even though they share the broker's port.

### 3.5 Connection direction vs. data flow

The arrows in the diagram in §3.2 show only **one** of two directions that matter, and it is easy to assume they show the other.

- **Connection direction** — *who dials whom*. This is what the arrows mean (each is labelled `connect`), and by §3.4 they all point at the broker, subscribers included.
- **Data-flow direction** — *which way messages travel* once a connection is open. A different question, and for one of the two roles it runs **opposite** to the arrow:

| Role | Connection (who dials) | Data (which way messages go) | |
| --- | --- | --- | --- |
| Publisher | pub → broker | pub → broker | same direction |
| Subscriber | sub → broker | broker → sub | **opposite!** |

The publisher is intuitive — it dials the broker *and* sends data that way. The surprising one is the **subscriber**: it dials the broker, yet messages come back *at* it, the other way down that same connection.

Data can travel opposite to the connection because **a TCP connection is full-duplex**: once open, either end may send whenever it likes, and which end dialed becomes irrelevant. The subscriber opens the pipe; the broker pushes messages back through it.

This is precisely how **commands reach the Nucleo even though the Nucleo never listens**. The node dials out once; commands arrive back down that same open connection, and it reads them off its socket — *receiving*, not *listening*, in the §3.4 sense.

That distinction is also what makes MQTT work through **NAT** (Network Address Translation, the router boundary that blocks unsolicited inbound connections). NAT blocks new *inbound connections*; it does not block return *data* on a connection opened from the inside. Since the node only ever initiates its one outbound connection, commands ride back down it untroubled.

All of that concerns the TCP connection. What the two ends say to each other *once* it is open — the MQTT session's own opening handshake — is the start of §6.

### 3.6 What the node gets out of it

Compare against the bookkeeping §3.1 demanded:

- **The node is a client, never a server.** It dials out, holds no listening socket, and keeps **no roster of consumers** — no list of connected receivers to loop over and copy each reading to. It calls `mqtt_publish(...)` once, hands one copy to the broker, and the broker fans out to whoever subscribed. That publish path is the same single line whether zero consumers or a hundred are connected.
- **Adding a consumer changes no firmware.** Start a second subscriber and it just works.
- **Consumers can be absent.** The node publishes into the void without error.
- **One connection carries both directions.** Telemetry out and commands in share a single TCP connection the node opened — no inbound port, no firewall hole, no listening socket on the device. A client opens one connection per *identity* it speaks for, and this gateway ends up needing two, for a reason that only becomes visible in §7. The second is a full TCP connection in its own right — its own socket and its own source port, carrying its own independent MQTT conversation — which by §3.4's tuple is as distinct to the broker as any other client's. What it does not add is a way *in*: it is dialled outward like the first, so the node still holds no listening socket and is still never a server.

---

## 4. Topics: MQTT's own addressing scheme, above IP

A **topic** is a concept that exists **purely at the MQTT layer** — it is defined by the MQTT protocol and by nothing beneath it. Concretely, it is a UTF-8 string with `/` as a separator, carried as a field inside every MQTT `PUBLISH` packet (the `hdr+topic` box in the §2 encapsulation diagram). The broker reads that field and routes the message by matching it against what subscribers asked for.

**Nothing below MQTT knows topics exist.** TCP sees an opaque byte stream; IP sees a packet addressed to `192.168.10.1`; Ethernet sees a frame addressed to a MAC address. No switch, no router, no kernel network stack ever parses or acts on a topic. Only MQTT software does — the broker, and the client library at each end.

That makes it a *second, independent* addressing system stacked on top of the first. IP addresses say *which machine*; topics say *which stream of information*. A subscriber never says "give me messages from 192.168.10.2" — it says "give me `node/1/telemetry`", and neither side cares what IP that came from.

```text
node/1/telemetry
└─┬┘ │  └───┬───┘
  │  │      └── what kind of information
  │  └───────── which node
  └──────────── namespace
```

### Wildcards

Subscribers (never publishers) may use two wildcards:

| Wildcard | Meaning | `node/1/telemetry` | `node/2/telemetry` | `node/1/sensor/telemetry` |
| --- | --- | --- | --- | --- |
| `node/1/telemetry` | exact | ✅ | ❌ | ❌ |
| `node/+/telemetry` | `+` = exactly **one** level | ✅ | ✅ | ❌ |
| `node/+/+/telemetry` | two `+` = **one** level each | ❌ | ❌ | ✅ |
| `node/#` | `#` = **any remaining** levels | ✅ | ✅ | ✅ |

Two hard rules:
- **wildcards are subscribe-only** (publishing to `node/+/telemetry` is illegal — a message must have one concrete topic)
- **`#` must be the last character**.

This is why hierarchies are ordered **general → specific**. Because the project uses `node/<id>/<kind>`, a harness can watch one node (`node/1/telemetry`), the same reading from every node (`node/+/telemetry`), or everything one node says (`node/1/#`) — without the firmware knowing or changing. **Your topic layout is your subscription API.** Design it like one; it is as hard to change later as a REST URL scheme.

### What a subscription carries besides the filter

A filter is not the whole of a subscription. A client subscribes by sending a `SUBSCRIBE` packet listing one or more **(topic filter, requested QoS)** pairs, and the broker replies with a `SUBACK` carrying a **granted QoS for each filter** in that list — normally what was asked for, possibly lower if broker policy caps it, or a failure code for a filter it declines outright. Two things follow, and §5 leans on both:

- **The level belongs to the filter, not to the client or the connection.** One `SUBSCRIBE` may carry several filters, each with its own requested level, and the broker tracks them independently — `subscribe_to_commands()` in `gateway/src/main.cpp` sends exactly one such packet, listing both of the command topics its session serves. Where a filter is a wildcard, the single level granted for it covers every topic that matches: `host/monitor.py` subscribes once to `node/#` at QoS 1, and that one grant governs telemetry, status and acks alike.
- **The subscriber asks and the broker grants**, and it is the *granted* value that governs what the broker sends. A client that requests 2 and is granted 1 is subscribed at 1, whatever it intended.

What a level actually promises is §5's subject. The point here is only that a subscriber picks one for each filter when it subscribes — a choice entirely independent of whatever the publisher picks for its messages, and made on a different schedule: a subscription's level is set once and stands until the client changes it, while a publisher chooses afresh for every `PUBLISH`. (This project happens to fix one level per topic, which is what [`mqtt-design.md`](../docs/mqtt-design.md)'s table records, but that is its own convention rather than anything the protocol requires.)

---

## 5. Quality of Service: what is actually being promised

QoS is the most misunderstood part of MQTT, because there are two wrong guesses to clear and nothing on the wire contradicts either one. The first is that QoS duplicates what TCP already does. The second survives longer: that a QoS level describes the trip from publisher to subscriber. It describes **one hop** of that trip, and a message that reaches a subscriber has crossed two. So a level answers two questions, and this section takes them in that order — **which hop** it covers, and **what its acknowledgement actually proves**.

### The three levels

| QoS | Name | Packet flow | Guarantee |
| --- | --- | --- | --- |
| **0** | at most once | `PUBLISH` → *(nothing)* | fire and forget |
| **1** | at least once | `PUBLISH` → `PUBACK` | arrives, but **maybe more than once** |
| **2** | exactly once | `PUBLISH` → `PUBREC` → `PUBREL` → `PUBCOMP` | arrives exactly once |

Read the `Packet flow` column carefully: **every row describes one client talking to the broker**, not a journey from one end to the other. That is the next subsection's subject, and holding it in mind from the start saves unlearning later.

**Exercise 4 in §9 shows this on the wire** with `-d` — the flag on the `mosquitto_pub` / `mosquitto_sub` CLI tools that dumps every MQTT control packet as it goes by: QoS 0 sends `PUBLISH` and stops; QoS 1 sends `PUBLISH` and waits for a `PUBACK` before moving on. One extra round trip — that's the entire difference.

### Which hop? One delivery is two transactions

A publisher never sends to a subscriber. It sends to the broker, and the broker sends to each subscriber. Those are **two independent transactions**, negotiated separately, each with its own QoS, its own acknowledgements and its own packet identifiers:

```text
publisher ──PUBLISH (QoS 1, id 7)──▶  broker  ──PUBLISH (QoS 1, id 43)──▶ subscriber
          ◀──── PUBACK (id 7) ──────          ◀──── PUBACK (id 43) ──────
                    hop 1                                hop 2
```

So subscribers do acknowledge — **to the broker, never to the publisher.** The broker is the receiver on hop 1 and the sender on hop 2, and the two hops share nothing: not their timing, and not the packet identifier (the `id` above — §2 spotted it in the header, and it is drawn from a separate pool per connection, which is why the same message is numbered 7 on its way in and 43 on its way out). The publisher retires its message the moment its own `PUBACK` arrives, whether or not hop 2 has happened, has failed, or exists at all — there may be no subscribers, or a hundred, and hop 1 looks identical either way.

**The subscriber, not the publisher, decides what hop 2 promises.** Its level is `min(published QoS, granted QoS)`, the granted half being what §4's `SUBACK` returned for the filter that matched:

| Published at | Subscribed at | Hop 2 runs at | Subscriber acks? |
| --- | --- | --- | --- |
| 1 | 1 | **1** | yes — `PUBACK` to the broker |
| 1 | 0 | **0** | no — downgraded to the weaker of the two |
| 0 | 1 | **0** | no — a guarantee the sender never made cannot be upgraded |
| 2 | 2 | **2** | yes — the four-packet flow from the table above, with the broker as sender |
| 2 | 1 | **1** | a `PUBACK` only; the exactly-once promise stopped at the broker |

The last two rows are the ones that catch people: **QoS 2 is not a publisher-side setting that a subscriber inherits.** Hop 2 can genuinely run at 2 — the broker performs the sender's half of the handshake and the subscriber holds the identifier — but only if that subscriber asked for 2. Because the level is negotiated per subscriber, one message published at QoS 2 can be going out at 2 to one subscriber, 1 to another and 0 to a third, at the same time. Whether anybody receives it exactly once is their decision, not the publisher's.

`host/monitor.py` is row three of that table, live: it subscribes once to `node/#` and asks for QoS 1, but telemetry is published at 0, so telemetry still reaches it at 0 — while acks and status, published at 1, reach it at 1. One subscription, one granted level, two different delivery guarantees, decided message by message by whichever side asked for less.

A publisher therefore cannot buy delivery *to* anyone by raising its own QoS; it buys delivery to the broker and nothing further. That is the entire content of the phrase **QoS is hop-by-hop**, and it is the sentence to carry through the rest of this section: it is why the acknowledgement below means less than it looks like it means, why QoS 2 turns out not to escape it either, and why this project's real guarantees live in `Command.sequence` and `Ack` rather than in a QoS level at all. Concretely, the node meets hop 2 from the receiving side for every command it is sent, and must answer with a `PUBACK` of its own or be redelivered forever; [`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md) has the handful of lines that do it, and the separate counter the node keeps for the identifiers it originates.

### What the acknowledgement proves: TCP reliability ≠ MQTT QoS

Scope settled, the second question: on that first hop, what has actually been promised once the `PUBACK` arrives? The natural objection is: *TCP already guarantees delivery — so what is QoS 1 for?*

They promise different things:

- **TCP** guarantees the bytes reached **the broker's TCP stack** (its kernel).
- **QoS 1** guarantees **the broker application** accepted and took ownership of the message.

Between those two lies everything that can go wrong above the kernel: the broker being mid-restart, out of memory, out of disk for a persistent store, or the connection dropping in the instant between "TCP accepted the bytes" and "Mosquitto processed them."

So what actually loses a QoS 0 message? Not a corrupted bit on the wire — TCP catches those and resends. What loses it is the connection itself failing. Walk through it slowly:

1. The node calls publish and hands the bytes to TCP.
2. The link goes down — someone pulls the cable, or the Pi reboots.
3. TCP resends those bytes, waiting a bit longer before each attempt. Eventually it stops trying and reports the connection dead.
4. The node sees the dead connection, dials the broker again, and carries on publishing new readings.

The reading from step 1 never arrived, and now nobody can point at it. Mosquitto cannot report it missing, because Mosquitto never knew it existed. The node knows its connection dropped, but not which reading went down with it. Messages lost this way — the ones in flight between step 2 and step 4 — are the **reconnect gap**, and that window is what QoS 0 exposes you to.

The only trace is at the host: `Telemetry.sequence` jumps, say from 41 straight to 43. Even that is not proof of a QoS 0 drop by itself, because `proto/node.proto` names two other things that can leave the same gap.

It helps to line up three separate things you might want to be true after publishing, because only two of them are ever promised to you:

| What you might want to be true | What promises it |
| --- | --- |
| the bytes arrived at the broker's machine | TCP |
| the broker *program* has the message and has taken responsibility for it | the `PUBACK` — this is what QoS 1 buys |
| a subscriber has the message | **nothing does** |

The middle row is what a `PUBACK` means. The bottom row is what people assume it means. No QoS level delivers that bottom row, and the previous subsection is why: the `PUBACK` settles hop 1, and a subscriber sits on the far side of hop 2.

### Back to the header: the flag bits, now that they mean something

§2 deferred four fields of the `PUBLISH` header. Three of them are this section's — and this is also where the terse notation comes from, because the `-d` traces print those fields as `PUBLISH (d0, q1, r0, m1, ...)`, the same QoS and identifier the diagram above spells out in full:

```text
d0  DUP flag    — 1 if this is a REDELIVERY of a message already sent
q1  QoS level   — the three levels above
r0  RETAIN flag — 1 asks the broker to KEEP this as the topic's last
                  known value, for whoever subscribes later (§7)
m1  packet identifier — what the PUBACK correlates back to
```

The packet identifier is the one worth pausing on: **it exists on the wire only for QoS 1 and 2.** A QoS 0 `PUBLISH` carries none, because nothing will ever be acked and so there is nothing to correlate — the absence of two bytes in the header is the entire difference in guarantee, made physical. It is also scoped to one connection, which is the mechanical reason the two hops above numbered the same message 7 and 43: the identifier is a slot in a conversation, not a name the message carries around.

### The DUP flag, and the duplicate QoS 1 creates

If a `PUBACK` is lost in flight, the sender cannot tell "the broker never got the message" from "the broker got it, and the ack was lost." So it redelivers with `d1` — and the broker, which *did* already have it, has no way to tell either. Nothing caps the number of retransmissions: every lost `PUBACK` buys another redelivery, so the number of duplicates is bounded only by how long the acks keep going missing.

**The duplicate does not stop at the broker.** Having already forwarded the first copy, the broker forwards the redelivery as well — so it arrives at every subscriber, and *no subscriber can filter it*, because by then it is a separate message with its own identifier on its own hop. A subscriber cannot buy its way out with a higher QoS of its own either: hop 2 is working perfectly, delivering two messages exactly as it was asked to. This is hop-by-hop biting in the direction people expect least — a failure on someone else's hop, arriving as correct behaviour on yours.

This is what "at least once" literally means, and it has a direct design consequence: **anything you send at QoS 1 must be idempotent, or must carry an identifier you can deduplicate on.** That is precisely why the `Command` message has a `sequence` field. It isn't decoration — QoS 1 forces it.

### QoS 2, and why this project stops at 1

MQTT has its own answer to that duplicate — QoS 2 — and the machinery is worth understanding before deciding, as this project does, not to use it.

```text
sender                                    broker
  │  PUBLISH  (d0, q2, m7)  ─────────────▶│  records identifier 7
  │◀──────────────────────  PUBREC  (m7)  │  "got it, and I am holding 7"
  │  PUBREL   (m7)  ─────────────────────▶│  forwards to subscribers, releases 7
  │◀──────────────────────  PUBCOMP (m7)  │  "done — 7 is retired"
```

**The guarantee comes from the state, not from the packet count.** Under QoS 1 the broker remembers nothing once it has sent its `PUBACK`, so a redelivered `PUBLISH` is indistinguishable from a new one. Under QoS 2 it holds the packet identifier from the moment it answers `PUBREC` until it sends `PUBCOMP`, so a redelivery carrying an identifier it is already holding is acknowledged but **not forwarded a second time**. That held identifier is the whole mechanism; the two extra packets exist to put it there and take it away again, which is also why the flow needs two round trips rather than one longer one — the sender must know the broker is holding the identifier before it can safely tell it to release.

**What it fixes, and what it still cannot reach.** It fixes the previous subsection's duplicate, and it fixes it *at the source*: the broker never forwards a second copy, so no subscriber sees one, whatever their own level. That is a real gain and a downstream-visible one — which is exactly what makes QoS 2 look like an end-to-end guarantee. It isn't, because it does not **compose**. Hop 2 still negotiates separately, so exactly-once *out* of the broker also requires the subscriber to have asked for QoS 2; and even with QoS 2 on both hops the pair is not one guarantee, because the broker is a boundary rather than a link inside a single handshake — nothing binds the outcome of hop 1 to hop 2. A broker that dies between sending `PUBCOMP` and forwarding leaves the publisher holding a completed exactly-once transaction for a message nobody will ever receive, and whether it survives that is a question about the broker's persistence settings rather than about QoS. Bridged or clustered brokers add further hops, each with the same seam.

**What the state costs.** Both ends carry it. The sender can no longer retire a message when the first acknowledgement arrives — it must keep the payload until `PUBCOMP`, and retransmit `PUBREL` if that never comes, which is a second retry path with its own timer and its own edge cases on reconnect. The broker keeps an identifier per in-flight message, across a reconnect if the session is persistent. None of that is prohibitive on a target the size of the gateway, but it is a second state machine in the client for every session — and the gateway runs two sessions, the second speaking for the CAN-attached peer node because it cannot share the first (§7 gives the reason), so any per-session machinery is built and debugged once and paid for twice.

**Two limits decide it here.** The first is general: exactly-once is a promise about *delivery*, not about *processing*. It says the broker forwards the message once; it says nothing about whether the program that received it finished acting on it, so a consumer that crashes between receiving and completing its work is outside what any QoS level can speak to. The second is structural, and it is non-composition taken one hop further: **QoS covers only the hops MQTT can see.** The moment a message travels further than the broker — as commands do here, where the gateway subscribes on a node's behalf and relays them onward over a link MQTT knows nothing about — exactly-once *to the broker* is not an end-to-end statement about anything.

So the cheaper answer is also the stronger one. `Command.sequence` is an application-level identifier, which means it survives every hop rather than just the first: whatever finally acts on the command dedupes on the same field, and the `Ack` echoing it back is what makes the guarantee end to end. Having paid for that field once — and QoS 1 already forced it — QoS 2 would add two packets, a retransmit timer and per-session state on top, in exchange for a property the schema already provides more completely. See *QoS rationale* in [`mqtt-design.md`](../docs/mqtt-design.md), which also has the hop-by-hop argument in the relay's concrete terms.

---

## 6. Sessions, keepalive, and why reconnect is real work

### Opening the session: `CONNECT` and `CONNACK`

Everything so far has quietly assumed a working MQTT session. Dialling the TCP connection (§3.5) is only half of getting one: the MQTT session riding on top has its own opening handshake, and it is exactly two packets.

```text
client                                      broker
  │  CONNECT ────────────────────────────▶ │  checks protocol version,
  │    client id, clean-session flag,      │  client id, credentials
  │    keepalive, will, credentials        │
  │◀──────────────────── CONNACK (rc, sp)  │  rc = 0 accepted, non-zero refused
  │                                        │  sp = "I already had a session
  │                                        │       stored under this id"
```

Three things about it are worth knowing:

- **`CONNECT` must be the very first packet on a new TCP connection.** A conforming broker that receives anything else simply closes the socket. So every MQTT session starts the same way, whatever the client means to do next.
- **`CONNACK`'s return code is the broker's verdict, and a non-zero one is a refusal rather than a transport failure.** The TCP connection worked and the broker replied — it just said no: unacceptable protocol version, rejected client id, bad credentials, or not authorised. That last one is precisely the confusing failure §9.1 warns about, and knowing the code exists is what makes it legible as a *decision* rather than a broken network.
- **A client is allowed to keep talking before `CONNACK` arrives** — MQTT 3.1.1 explicitly permits it — but this firmware does not. `gateway/src/main.cpp` publishes its `online` status and sends its `SUBSCRIBE` only once the CONNACK event fires, because anything sent optimistically is discarded if the verdict turns out to be a refusal.

The second `CONNACK` field, **session present**, is the broker answering the clean-versus-persistent question below from its own side: it tells the client whether state was actually found and resumed under that client id, rather than leaving it to assume.

That one `CONNECT` is also where most of this guide's machinery gets declared — the client id and clean-session flag below, the keepalive interval that follows them, and the Last Will of §7. None of it can be renegotiated on a live session, which is why §7's one-will-per-connection limit forces a whole second connection rather than a second setting.

### Keepalive: TCP fails slowly and silently

**TCP is slow to notice a dead peer.** Pull the cable and the sender sits in retransmit-with-backoff for minutes before giving up. Worse, an *idle* TCP connection sends nothing at all — so a connection whose peer vanished looks byte-for-byte identical to a connection whose peer simply has nothing to say. Both are silent.

This is a real problem for a node that publishes every 5 s and otherwise idles: it can sit on a dead connection indefinitely, cheerfully believing it is connected.

So MQTT layers **its own** liveness check on top: at `CONNECT` the client declares a **keepalive** interval (say 60 s). If it hasn't sent anything for that long, it must send a `PINGREQ` and get a `PINGRESP`. If the broker hears nothing within 1.5× keepalive, it declares the client dead — and acts on that, which is what §7's Last Will is for.

**This is the answer to "why does MQTT reinvent something TCP already does?"** It doesn't — TCP has no application-level liveness signal at all, and its failure detection is too slow and too silent to be useful for a device.

### Clean vs persistent sessions

At `CONNECT`, a client either asks for:

- a **clean session** (broker, throw away all state; start fresh)
- a **persistent session** (broker, remember my subscriptions and queue my QoS 1 messages between visits)

What ties the two visits in a persistent session together is the **client id** — a string every client puts in its `CONNECT`, and the name the broker files the session under. A returning client gets its own state back only by presenting the same id, which is what makes "remember my subscriptions" a well-defined request at all. The flip side is a constraint: two live connections presenting the *same* id are a collision rather than a pair, and the broker resolves it by disconnecting the older one. That is why the gateway's two sessions (§7) must carry different ids — `gateway/src/main.cpp` asserts it at startup, because the failure mode is each session kicking the other off forever and looking exactly like a network fault.

The tempting choice is "persistent, obviously — don't lose my data." For **telemetry that is actively wrong**, and it's the sharpest argument in `mqtt-design.md`: with a persistent session and QoS 1 telemetry, a node that drops for two minutes reconnects and the broker **floods it with 24 stale readings**. Live telemetry that is two minutes old is not valuable; it is misleading. For a *command* queue, the same behaviour is exactly what you want.

Different data, different guarantees. That's why QoS and session are per-purpose decisions, not one global setting.

### Reconnecting: a state machine, not a retry

The heading of this section claimed reconnect is real work. Here is why. Everything §6 has described is **per connection**: the handshake, the client id, the keepalive, the session state. When a connection dies — the keepalive deadline passes, the socket errors, the broker restarts — none of it survives. The client cannot resume; it has to open a new TCP connection, send a fresh `CONNECT`, and wait for a fresh `CONNACK`. And because this project asks for a **clean session**, the broker deliberately kept nothing, so every subscription is gone and has to be sent again too.

That is why `gateway/src/main.cpp` announces and subscribes inside its CONNACK handler rather than at startup. Publishing the retained `online` and sending the `SUBSCRIBE` are not initialisation that runs once — they are what must happen *every time* a session comes up. (Ask for a persistent session instead and the subscriptions survive, which is exactly the trade the previous subsection priced.)

The other half of the problem is *when* to try again. Reconnecting instantly hammers a broker that may be down for minutes, so the delay grows: 1 s, then 2, 4, 8, capped at 30 s (`kBackoffMinMs` and `kBackoffMaxMs` in `main.cpp`). It drops back to 1 s only after a session that actually reached `CONNACK`, because a connection that died before that is no evidence the broker is healthy.

```text
   IDLE ──delay elapsed──▶ CONNECTING ──CONNACK──▶ SERVING
     ▲                          │                      │
     │      refused, or         │                      │  keepalive expires,
     │      never answered      │                      │  or the socket dies
     └──────────────────────────┴──────────────────────┘
          back to IDLE with the delay doubled — and only
          a session that reached CONNACK resets it to 1 s
```

One consequence is worth carrying to the bench: **a broker that comes back early still waits out the delay already running.** Restarting Mosquitto takes seconds, but the node may not return for 30. That is the intended trade — patience over hammering a dead endpoint — though it means "broker downtime" and "node downtime" are different numbers. The sensor keeps sampling the whole time, so what the host loses is a stretch of `Telemetry.sequence`, not a stretch of time. [`mqtt-design.md`](../docs/mqtt-design.md) records the numbers as a decision.

---

## 7. Retained messages and the Last Will

### Retain: last known state

Normal pub/sub is strictly **live**: publish to a topic nobody is subscribed to and the message is gone forever. The broker is a router, not a database.

The `retain` flag makes one exception. The broker keeps **the most recent retained message per topic**, and delivers it immediately to any client that subscribes *later* — **Exercise 3 in §9** shows a subscriber started *after* the publish receiving it instantly.

It is a **last-known-value cache**, not a history — exactly one message per topic, the newest. Publishing a retained *empty* payload clears it.

Why it matters here: `status` is retained, so a harness that starts up mid-flight learns the node's last known state without waiting for it to say anything. Retain on its own would make that a stale promise — a node that dies unexpectedly never publishes `offline`, and the cached `online` would outlive it indefinitely — which is precisely the job of the Last Will below: the broker overwrites the retained value once §6's keepalive deadline passes. Telemetry deliberately is **not** retained — a stored reading describes air that no longer exists, and hands a late subscriber no way to tell how stale it is. See *Telemetry is not retained* in [`mqtt-design.md`](../docs/mqtt-design.md).

### Last Will and Testament: speaking after you die

A node can crash, or its cable can be pulled. It never gets to say "I'm going offline."

So MQTT lets a client register a **will** at `CONNECT` time: a topic, a payload, a QoS, a retain flag. The broker holds it. If the client ever disconnects **without** a clean `DISCONNECT` packet — crash, cable pull, or the keepalive deadline of §6 expiring — **the broker publishes the will on the client's behalf.**

Note how those two sections fit together: §6 gives the broker a *deadline* by which it can conclude a silent client is dead, and this section gives it something to *do* about it. Neither is much use alone.

The idiom, which `mqtt-design.md` adopts:

- Will = `node/1/status` → `offline`, retained.
- On connect, the node immediately publishes `node/1/status` → `online`, retained.

**Why a will carries retain and QoS flags at all.** Retain is purely broker-side, so setting it on a *client's* will can look like a category error. It isn't: **the dying client is not the one that publishes this message.** The four will fields are stored parameters for a `PUBLISH` the *broker* emits on the client's behalf, so it honours the retain bit exactly as it would any other publisher's. The QoS field reads the same way — with no client-to-broker hop to cover, it governs only hop 2, the delivery outward to each subscriber, still capped by what that subscriber asked for (§5).

**What retain buys: the will overwrites the `online`.** The cache holds one message per topic. `online` fills that slot at connect; a *retained* will replaces it with `offline`. Drop the retain bit and the will still fires live to whoever is subscribed at that instant, but the slot goes on reading `online` for everyone who arrives later. The two retain flags are a matched pair, and neither half of the idiom works alone:

| `online` at connect | the will | What a subscriber joining later sees |
| --- | --- | --- |
| retained | retained | **the idiom** — current truth, stale by at most the detection deadline |
| retained | *not* retained | `online` **forever** after a crash; only clients already subscribed at the moment of death ever learn otherwise |
| *not* retained | retained | `offline` for a node that is alive and publishing — the last death is cached and nothing overwrites it |
| neither | neither | nothing at all; the state is knowable only by catching a transition in the instant it happens |

Now `node/1/status` answers for anyone who subscribes, whenever they subscribe — with one bound worth stating precisely: after an unclean death the retained value still reads `online` until the broker's 1.5× keepalive deadline passes, 90 s at the gateway's `kKeepaliveSec` of 60. None of which costs the node a line of code for its own death: the will goes in `CONNECT`, and the broker does the rest.

**One will per connection.** The will lives in the `CONNECT` packet, so a client registers exactly one death notice no matter how many nodes it speaks for. The gateway cannot cover the peer from its own firmware either — the moment `node/2/status` matters most is the case where the gateway itself dies, and a dead gateway publishes nothing. So it opens a second MQTT session for the peer node, with its own client id, keepalive and packet-id space, for no other reason than to give the broker a second will to hold; [`mqtt-design.md`](../docs/mqtt-design.md) has both client ids and the failure table that prices it.

---

## 8. The whole path, end to end

§1–§7 built MQTT from the outside in. This section puts it back where it lives: **one layer of a chain that starts in a sensor's registers and ends at a decoded line on the Pi.** No new MQTT is introduced. What is new is the two seams either side of it — §2's table drew them as neat stacked rows, and they are the parts you actually have to design.

```text
┌─ NUCLEO ─────────────────────────────────┐      ┌─ RASPBERRY PI ──────────────┐
│                                          │      │                             │
│  SCD-40 ──I²C──▶ sensor thread           │      │                             │
│                  (sensor.cpp)            │      │   Mosquitto (broker)        │
│                       │ (1)              │      │   192.168.10.1:1883         │
│                       ▼                  │      │        │                    │
│                 chan_telemetry ────┐     │      │        │ routes by topic    │
│                       │            │     │      │        ▼                    │
│                       │ (2)        │     │      │   paho-mqtt harness         │
│                       ▼            │     │      │        │                    │
│                    eventfd         │     │      │        ▼                    │
│                       │            │     │      │   protobuf decode → log     │
│                       │ (3)        │ (4) │      │                             │
│                       ▼            │     │      └─────────────────────────────┘
│           MQTT thread (main.cpp) ◀─┘     │                    ▲
│           poll(socket, eventfd)          │                    │
│                       │                  │                    │
│                       ▼ nanopb encode    │                    │
│                 [26 opaque bytes]        │                    │
│                       │                  │                    │
│                       ▼ mqtt_publish()   │                    │
│              TCP ▸ IP ▸ Ethernet         │                    │
└───────────────────────┼──────────────────┘                    │
                        └─── RJ45, 100 Mbit full-duplex ────────┘
                              192.168.10.2 → 192.168.10.1
```

### From sample to socket, four steps

The Nucleo half is where readers usually stall, because two of the four steps carry no data at all:

1. **`zbus_chan_pub()`.** The sensor thread copies its reading into `chan_telemetry` and goes straight back to sampling. That is the whole of its involvement: it names no socket, waits for no reader, and does not care whether one exists.
2. **The listener fires.** A zbus *listener* is a callback the bus runs **synchronously in the publisher's context** — still on the sensor thread, still inside `zbus_chan_pub()`, with the channel locked. So it must not block, and `on_telemetry()` in `main.cpp` does exactly one thing: `zvfs_eventfd_write(fd, 1)`. It forwards no reading. It is a doorbell.
3. **`poll()` returns.** The MQTT thread has been blocked in one `zsock_poll()` over its sockets *and* that eventfd. The write makes the descriptor readable, and the thread wakes.
4. **`zbus_chan_read()` fetches the value.** Only now, and by the MQTT thread's own hand, does the reading move — which is why arrow (4) points *backwards* up the diagram. It was never in flight; it sat in the channel until a reader came for it.

### Why the signal and the value travel separately

Step 2 looks like an indirection for nothing — the listener already had the reading, so why not hand it over? Two constraints make the split the cheap answer.

**A zbus channel is not a file descriptor.** `poll()` waits on descriptors, so a thread cannot wait on a socket and a channel in one call. Without a descriptor standing in for the bus, the firmware would need a second thread blocked on the bus, and then a way to hand what it received to the thread that owns the socket — which is the problem you started with, moved one thread to the left. The eventfd is what makes **one thread, one `poll()`, blocked on everything at once** possible, and that shape is the whole architecture of `main.cpp`.

**A doorbell can be rung more often than it is answered.** The eventfd is a counter: every publish adds one, and reading it returns the total and resets it. The channel is a single slot: every publish overwrites it. So if the MQTT thread is busy reconnecting while three readings land, it wakes to a count of 3 and a channel holding only the newest — and `publish_pending_telemetry()` logs exactly that (`readings coalesced into one publish`). For telemetry that is the *right* loss: a two-minute-old reading is misleading, which is the same argument §6 made against a persistent session. It is also why the host can detect it at all — the gap shows up as a jump in `Telemetry.sequence`, the application-level counter §5 ended on.

### The same shape at three scales

Now the seams. Every boundary in that diagram is the same trick: two things that must cooperate are forbidden from knowing about each other, and something in the middle carries the contract instead.

| Boundary | What each side is spared | Where the contract lives |
| --- | --- | --- |
| sensor thread ↔ MQTT thread | `sensor.cpp` names no socket; `main.cpp` names no I²C | `shared/app_channels.h` — the channel and its struct |
| node ↔ host | the node keeps no roster of consumers (§3.6); the host needs no address for the node | the topic tree (§4) |
| encoder ↔ decoder | MQTT carries opaque bytes and never inspects them | `proto/node.proto` |

Read down that middle column and MQTT stops looking special: **pub/sub through a broker is the same decoupling as pub/sub through a zbus channel, done across a network instead of across a thread boundary.** Same idea, two scales — [`zbus-guide.md`](zbus-guide.md) is the inner one in full, and [`protobuf-guide.md`](protobuf-guide.md) the third row.

The trade is identical every time, and it is worth naming because it is the reason this project is shaped the way it is: you give up a direct call — with its obvious control flow and its compiler-checked signature — and you gain the ability to change one side without touching the other. The price is that the thing in the middle is now a real artefact you have to design, version and debug. That is why each row's right-hand column is a file this repo takes seriously.

**One simplification, so the picture stays readable.** It follows a single reading on a single MQTT session; the gateway runs **two** through that same `poll()`, the second speaking for the CAN-attached peer node and publishing `node/2/…` on its behalf (§7 is why the count is two, and [`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md) has the loop that serves both). Nothing else changes — both sessions are outbound, both share this one loop — only where the payloads come from: the SCD-40 on this side, the CAN relay on the other.

---

## 9. Hands-on: prove all of it with the CLI

Everything above is theory until you watch it happen. This section is a **runnable lab**: set the broker up once, then work through six exercises that each demonstrate one concept from the preceding sections — with the commands explained, the expected output shown, and a note on what it proves.

Five of the six use nothing but the CLI tools, and that is deliberate rather than a limitation. `mosquitto_pub` and `mosquitto_sub` are ordinary MQTT clients — nothing privileged about them, and nothing the Nucleo does that they cannot (§3.3). Isolating each concept in two commands is far easier than isolating it inside a running firmware, so the concepts come first and the node arrives last: **Exercise 6 points the same tools at the real node** and shows that nothing changes.

Everything runs on the Pi. Several exercises need **two terminals**, so open a second SSH session (`ssh pi@pi5.local`) alongside the first. They're referred to as **A** (subscriber) and **B** (publisher).

### 9.1 Setup: install and expose the broker

```sh
sudo apt update
sudo apt install -y mosquitto mosquitto-clients
```

`mosquitto` is the broker (a service that starts automatically); `mosquitto-clients` provides the `mosquitto_pub` / `mosquitto_sub` CLI tools.

Before changing anything, observe the default:

```sh
mosquitto -h | head -1                  # version (expect 2.0.x)
systemctl status mosquitto --no-pager   # is the broker service running?
ss -tlnp | grep 1883                    # which IP:port is it LISTENING on?
```

`ss -tlnp` is not MQTT at all — it's a Linux tool listing **l**istening **t**CP sockets, **n**umerically, with the owning **p**rocess. It answers the one question that matters before any client can connect: *is anything open to connections at the address the Nucleo will dial?* On a fresh install you get:

```text
LISTEN 0  100  127.0.0.1:1883  0.0.0.0:*
LISTEN 0  100      [::1]:1883     [::]:*
```

**Loopback only** — the broker is running perfectly and is completely unreachable from the Nucleo. That is Mosquitto 2.0's deliberate security default, not a fault. A listening socket is **IP + port**, not port alone (§3.4), so a client dialing `192.168.10.1:1883` would be **refused**. Worth seeing with your own eyes before fixing it.

Now bind a listener to the bench link. Create a drop-in so the shipped `mosquitto.conf` stays pristine (it reads this directory via its `include_dir` setting — confirm with `grep include_dir /etc/mosquitto/mosquitto.conf`):

```sh
sudo tee /etc/mosquitto/conf.d/bench.conf > /dev/null <<'EOF'
listener 1883 192.168.10.1
allow_anonymous true
EOF

sudo systemctl restart mosquitto
ss -tlnp | grep 1883
```

Expected — note the loopback entries are **gone**:

```text
LISTEN 0  100  192.168.10.1:1883  0.0.0.0:*
             └──────┬──────┘└─┬─┘
                    │         └── port
                    └──────────── bound IP  ← the part everyone forgets
```

- **`listener <port> <bind-address>`** — defining any listener **replaces** the implicit localhost default rather than adding to it. That's why loopback disappeared, and why even on the Pi you must now use `-h 192.168.10.1`, never `-h localhost`.
- Binding to `192.168.10.1` (not `0.0.0.0`) keeps the broker reachable **only on the bench cable** — deliberately invisible to the home LAN, where the Pi is `192.168.1.105`.
- **`allow_anonymous true`** — once any listener is defined, Mosquitto 2.0 denies anonymous clients unless told otherwise. Without it you get a confusing failure: TCP connects fine, then the MQTT `CONNECT` is rejected as "not authorised", which looks like a network problem but isn't. Bench-only; a real product uses credentials + TLS.

> ⚠️ **This works now but dies on the next reboot.** Binding to a *specific* IP means mosquitto cannot start until that address exists — and on boot it loses the race against NetworkManager, failing with `Cannot assign requested address` before `eth0` is configured. It is a direct consequence of "a listening socket is IP + port" (§3.4). The fix is a systemd drop-in ordering mosquitto after `network-online.target` plus a relaxed restart limiter; see **Boot ordering** in [`mqtt-design.md`](../docs/mqtt-design.md) for the exact file.

### 9.2 The two tools, decoded

**Subscribe:**

```sh
mosquitto_sub -h 192.168.10.1 -t 'node/1/telemetry' -q 1 -v
              └──────┬───────┘ └────────┬─────────┘ └┬┘ └┬┘
                     │                  │            │   └── verbose: print "topic payload"
                     │                  │            │       (essential once wildcards mean you can't tell which topic hit)
                     │                  │            └────── QoS to REQUEST for this filter (§4) — default 0
                     │                  └── topic filter — may contain + and #
                     └── broker address (-p 1883 is the default port)
```

It appears to hang. It hasn't: it is holding an open TCP connection with an MQTT session on top, waiting for the broker to push messages down it (§3.5). Always **quote the topic** — in the shell, `#` starts a comment and `+` can glob.

**Publish:**

```sh
mosquitto_pub -h 192.168.10.1 -t 'node/1/telemetry' -m 'hello' -q 1 -r -d
                                                    └───┬────┘ └─┬┘ └┤ └┬┘
                                                        │        │   │  └ debug: print control packets
                                                        │        │   └──── retain
                                                        │        └──────── QoS (default 0)
                                                        └───────────────── payload
```

Useful extras: `-C <n>` on `mosquitto_sub` exits after *n* messages, and `-i <id>` sets an explicit client id on either tool.

### Exercise 1 — Publish/subscribe decoupling

*Demonstrates §3.2: every arrow points at the broker; the publisher never knows a subscriber exists.*

**Terminal A:**
```sh
mosquitto_sub -h 192.168.10.1 -t 'node/1/telemetry' -v
```
**Terminal B:**
```sh
mosquitto_pub -h 192.168.10.1 -t 'node/1/telemetry' -m 'hello from the pi'
```

A prints `node/1/telemetry hello from the pi`.

Now the actual lesson — **stop the subscriber in A (Ctrl-C) and publish again from B.** The publish still succeeds, with no error and no complaint. Restart A and it receives nothing; the message is long gone. The publisher never had any idea whether anyone was listening.

**Proves:** the two clients never connect to each other. Both dial the broker independently, and publishing is decoupled from consumption. This is why the Nucleo can publish telemetry without knowing or caring whether your Python harness is running.

### Exercise 2 — Topic filtering and wildcards

*Demonstrates §4: the broker routes by topic, and wildcards define subscription scope.*

**Terminal A** — exact topic:
```sh
mosquitto_sub -h 192.168.10.1 -t 'node/1/telemetry' -v
```
**Terminal B:**
```sh
mosquitto_pub -h 192.168.10.1 -t 'node/1/telemetry' -m 'node 1 data'
mosquitto_pub -h 192.168.10.1 -t 'node/2/telemetry' -m 'node 2 data'
```

Only the **first** arrives. `node/2/telemetry` doesn't match the filter, so the broker simply drops it for this subscriber.

Now restart A with a single-level wildcard, and rerun both publishes plus a nested one:

```sh
mosquitto_sub -h 192.168.10.1 -t 'node/+/telemetry' -v
```
```sh
mosquitto_pub -h 192.168.10.1 -t 'node/1/telemetry'        -m 'node 1'
mosquitto_pub -h 192.168.10.1 -t 'node/2/telemetry'        -m 'node 2'
mosquitto_pub -h 192.168.10.1 -t 'node/1/sensor/telemetry' -m 'nested'
```

The first two arrive; the third does **not** — `+` matches exactly one level, and `node/1/sensor/telemetry` has an extra one. Finally restart A with `-t 'node/#'` and repeat: now **all three** arrive, because `#` matches any remaining depth.

**Proves:** your topic hierarchy *is* your subscription API. Because topics run general → specific, a consumer can select one node, one kind across all nodes, or everything from one node — with no firmware change. (Remember: wildcards are **subscribe-only**, and `#` must be last.)

### Exercise 3 — Retained messages

*Demonstrates §7: the broker caches the last retained message per topic.*

First show the normal case. With **no subscriber running**, publish without `-r`:
```sh
mosquitto_pub -h 192.168.10.1 -t 'node/1/status' -m 'ephemeral'
```
Now subscribe — you get **nothing**. Plain pub/sub is strictly live.

```sh
mosquitto_sub -h 192.168.10.1 -t 'node/1/status' -v
```

Ctrl-C that, and repeat **with** the retain flag:
```sh
mosquitto_pub -h 192.168.10.1 -t 'node/1/status' -m 'online' -r
```
```sh
mosquitto_sub -h 192.168.10.1 -t 'node/1/status' -v
```

This time the subscriber prints `node/1/status online` **immediately on connecting**, despite subscribing *after* the publish. Subscribe again and it happens again — the broker is holding it. Clear it with an empty retained payload:

```sh
mosquitto_pub -h 192.168.10.1 -t 'node/1/status' -m '' -r
```

**Proves:** retain is a **last-known-value cache** (exactly one message per topic, the newest), not a history. It's what lets a harness starting up mid-flight learn the node's state instantly — which is why `status` carries the flag here and telemetry does not.

### Exercise 4 — QoS on the wire

*Demonstrates §5: the QoS 1 acknowledgement, the header flag bits it explains, and — by what it leaves out — that a trace of one client is a trace of one hop.*

QoS is nearly impossible to *observe* on a healthy local cable — which is itself the lesson. So inspect the packets instead, with `-d`:

```sh
mosquitto_pub -h 192.168.10.1 -t 'node/1/telemetry' -m 'qos zero' -q 0 -d
mosquitto_pub -h 192.168.10.1 -t 'node/1/telemetry' -m 'qos one'  -q 1 -d
```

QoS 0:
```text
Client (null) sending CONNECT              ← open the MQTT session
Client (null) received CONNACK (0)         ← broker accepts; 0 = success
Client (null) sending PUBLISH (d0,q0,r0,m1,'node/1/telemetry',...(8 bytes))
Client (null) sending DISCONNECT           ← clean teardown (so no will fires)
```

QoS 1 — identical except for one extra packet:
```text
Client (null) sending PUBLISH (d0,q1,r0,m1,'node/1/telemetry',...(7 bytes))
Client (null) received PUBACK (Mid: 1)     ← the entire difference
Client (null) sending DISCONNECT
```

`Client (null)` just means no client id was set, so libmosquitto generated one. The `d0,q1,r0,m1` flags are literally the `PUBLISH` header fields decoded in §5, and `PUBACK (Mid: 1)` correlates back to that `m1`.

**Proves:** QoS 1 costs exactly one extra round trip, in which the **broker application** confirms it took ownership — a different promise from TCP's guarantee that the bytes reached the broker's *kernel* (§5).

**And note what is missing from the trace.** It ends at `DISCONNECT`, one packet after the `PUBACK`. Whether a subscriber existed, whether the broker forwarded anything, and what QoS that second hop ran at are all absent — not omitted by the tool, but genuinely invisible from where `mosquitto_pub` stands. This is §5's hop-by-hop rule as an observation rather than a claim: the publisher's complete record of a successful QoS 1 delivery contains no evidence about anyone else. Put `-d` on a `mosquitto_sub` in another terminal and you get the other half of the picture, as a separate trace with its own packet identifiers.

Note what else this reveals: `mosquitto_pub` performs a **complete connect → publish → disconnect cycle on every invocation.** That's fine for a CLI and wrong for the Nucleo, which will connect **once** and hold the session open for its lifetime — which is exactly why keepalive and reconnect become your problem in firmware and never came up here.

### Exercise 5 — Last Will and Testament

*Demonstrates §7, with §6's keepalive as the other way a client gets declared dead: the broker speaks for a client that dies without a clean disconnect.*

This version uses two CLI clients, so it needs no firmware and proves the mechanism in isolation. Testing the **node's** will is a harder problem — the obvious methods take the observer down along with the node — and needs a packet filter instead; see *Testing the Last Will* in [`mqtt-design.md`](../docs/mqtt-design.md) for that procedure.

**Terminal A** — a client that registers a will, then just sits there:
```sh
mosquitto_sub -h 192.168.10.1 -t 'node/1/command' \
  --will-topic 'node/1/status' --will-payload 'offline' --will-retain
```

**Terminal B** — watch the status topic:
```sh
mosquitto_sub -h 192.168.10.1 -t 'node/1/status' -v
```

Now **kill A ungracefully** so it never sends `DISCONNECT` — in a third terminal:
```sh
pkill -9 -f "will-topic"
```

Terminal B prints `node/1/status offline`, published by **the broker**, not by any client. Contrast with stopping A via Ctrl-C, which sends a clean `DISCONNECT` and therefore fires **no** will.

**Proves:** the node gets correct offline reporting for free, including on crash or cable pull — the failure case it could never have handled itself. Combined with a retained `online` published at connect, `node/1/status` is then always accurate for any subscriber.

### Exercise 6 — Point the same tools at the real node

*Demonstrates §3.3: the Nucleo is just another conforming MQTT client, and every concept above applies to it unchanged.*

With the node flashed and running, watch everything it says:

```sh
mosquitto_sub -h 192.168.10.1 -t 'node/#' -v
```

You will see three things at once, and each is a concept from earlier made concrete:

```text
node/1/status online                          ← retained (§7), delivered instantly on subscribe
node/1/telemetry <binary>                     ← QoS 0, every ~5 s
node/1/telemetry <binary>
```

The `status` line arrives **immediately**, before any telemetry, because it is retained — Exercise 3 with a real publisher. The telemetry lines arrive on the sensor's own cadence.

But the telemetry payload is unreadable, and that is the point: **MQTT gives you delivery, not meaning** (§2). The broker never looked inside those bytes and neither can `mosquitto_sub`. To read them you need the schema:

```sh
host/.venv/bin/python host/monitor.py
```

```text
14:02:11  node/1/status        [retained] online
14:02:16  node/1/telemetry     seq=1042  co2=  812 ppm  temp=22.41 C  rh=41.3 %  p=    -- Pa  up=  5210.4s  SENSOR_STATUS_OK  (schema v1)
```

Same bytes, same broker, same topics — the only thing added is Protobuf, decoding a payload MQTT carried verbatim and never inspected.

**Proves:** the layering in §2 is real and separable. Everything you learned with `mosquitto_pub`/`mosquitto_sub` transfers to the firmware without modification, because both are clients of the same protocol (§3.3) — and the one thing the CLI tools cannot do is the one thing that lives *above* MQTT.

### 9.3 Quick reference

| Command | What it does |
| --- | --- |
| `ss -tlnp \| grep 1883` | which **IP:port** the broker is open on (§3.4) |
| `systemctl status mosquitto --no-pager` | is the broker service running |
| `sudo systemctl restart mosquitto` | apply a config change |
| `mosquitto_sub -h HOST -t 'TOPIC' -v` | subscribe; `-v` prints `topic payload` |
| `mosquitto_pub -h HOST -t 'TOPIC' -m 'MSG'` | publish once |
| `-q 0\|1\|2` | (pub) QoS to publish at; (sub) QoS to *request* for the filter — default 0 either way (§4, §5) |
| `-r` | retain (empty payload `-m ''` clears it) |
| `-d` | print the MQTT control packets |
| `-C n` | (sub) exit after *n* messages |
| `-i ID` | set an explicit client id |
| `--will-topic T --will-payload P --will-retain` | register a Last Will on this client (§7, Exercise 5) |

Config lives in `/etc/mosquitto/conf.d/bench.conf`; the broker is bound to `192.168.10.1:1883` with anonymous access, so always use `-h 192.168.10.1` — **never** `-h localhost`, which no longer has a listener.

---

## 10. The model in one paragraph

Ethernet moves frames across one cable; IP addresses machines; TCP turns that into a reliable byte stream but deliberately refuses to mark where messages end. MQTT adds message framing and, crucially, a **broker**: nobody connects to anybody, everyone connects to the broker, and messages are routed by **topic** rather than address — so the node publishes without knowing or caring who listens. **QoS** picks what is being promised (0 = fire and forget, self-healing for a 5 s telemetry stream; 1 = acked but possibly duplicated, so commands need a sequence number to dedupe; 2 = exactly once, unused here because that sequence number already does the job, and does it across the hops MQTT's per-hop handshake cannot reach). **Retain** gives late subscribers the last known value; the **will** lets the broker announce your death; **keepalive** exists because TCP notices death far too slowly and silently. Everything up to and including TCP, Zephyr implements; MQTT lifecycle and above is your work — and Protobuf, riding inside the payload as bytes MQTT never inspects, is what makes those bytes mean `co2 = 812 ppm`.

## 11. Where to go next

- **[`mqtt-design.md`](../docs/mqtt-design.md)** — the reference half of this guide: the topic table, the QoS decision per topic and its rationale, the broker config, and the packet-filter procedure for testing the *node's* will rather than a CLI client's.
- **[`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md)** — the same concepts as running code: the connect/keepalive/reconnect state machine, the single `poll()` that waits on both sockets and the whole bus at once, and where each MQTT event is handled.
- **[`protobuf-guide.md`](protobuf-guide.md)** — the layer above, and the one thing `mosquitto_sub` could not show you in Exercise 6.
- **[`zbus-guide.md`](zbus-guide.md)** — the same publish/subscribe idea applied *inside* the chip, which is the other decoupling boundary in §8's diagram.
- **The MQTT 3.1.1 specification** — short, readable, and the authority for anything this guide simplified. Sections 3.1 (`CONNECT`), 3.3 (`PUBLISH`) and 3.8–3.9 (`SUBSCRIBE`/`SUBACK`) cover most of what the firmware touches.
