# Understanding the Communication Architecture

*A from-first-principles guide to how one sensor reading gets from the SCD-40 to the host PC.*

This is a teaching document, not project documentation. It explains the concepts and the
flow in the order that makes them easiest to learn, using this repository — a CO₂ sensor
node on an ST Nucleo-H753ZI talking to a Raspberry Pi — as the running example. For the
project's actual decisions (topic table, QoS per topic, broker config), see its companion,
[`mqtt-design.md`](../docs/mqtt-design.md). Once these concepts make sense,
[`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md) reads the firmware that
implements them, line by line.

---

## 1. Why this feels like a lot of moving parts

The README names a pile of things: Ethernet, LAN8742, IP, TCP, MQTT, Mosquitto, QoS,
topics, Protobuf, nanopb, zbus, paho-mqtt. Listed flat like that, it reads as a dozen
unrelated technologies to learn.

They are not unrelated, and there are not a dozen. They are a **stack**, and each layer
exists to solve exactly one problem that the layer below it cannot. Once you can say what
each layer gives you — and, just as important, what it refuses to give you — the pile
collapses into a short chain of "…and therefore we also need…".

The chain is:

> Wires move **bits** → but who are they for? → **Ethernet** moves *frames* between two
> adapters → but only across one cable → **IP** moves *packets* between addresses → but
> unreliably and out of order → **TCP** gives a *reliable byte stream* → but with no
> message boundaries and no fan-out → **MQTT** gives *whole messages* to *whoever is
> interested* → but they are just opaque bytes → **Protobuf** gives them *meaning*.

Everything below MQTT, Zephyr implements for you. Everything from MQTT up is your work.

---

## 2. The one big idea: each layer solves one problem

Here is the whole architecture in one table. The right column is the important one — it's
the reason the *next* layer up has to exist.

| Layer | Gives you | Does **not** give you (⇒ why the next layer exists) |
|---|---|---|
| **Ethernet** (MAC + PHY) | A frame delivered to a MAC address on this cable | Any notion of "which machine" beyond this one link |
| **IP** | Addressing: `192.168.10.1` ↔ `.2` | Reliability, ordering, or the concept of a conversation |
| **TCP** | A reliable, ordered **byte stream** to a port | **Message boundaries**, and any fan-out to multiple readers |
| **MQTT** | Whole **messages**, delivered by **topic** to any number of subscribers | Any idea what the bytes *mean* |
| **Protobuf** | **Meaning**: typed fields, and a versionable schema | — |

Two rows deserve emphasis, because they are the ones people skip:

**TCP gives you a byte stream, not messages.** If you write three 100-byte messages, the
receiver may read 300 bytes at once, or 47 then 253. TCP faithfully preserves *order* and
*bytes*; it has no concept whatsoever of where one message ends and the next begins. Over
raw TCP, you must invent that yourself — typically a length prefix, plus buffering and
reassembly logic. **MQTT solves this**, which is why the README says there is "no app-level
framing / stream reassembly": every payload arrives whole or not at all.

**MQTT gives you delivery, not meaning.** An MQTT payload is an opaque byte array. The
broker never looks inside it. Sending `{"co2": 812}`, or `812`, or a Protobuf blob, is all
the same to MQTT. Choosing Protobuf is a decision that lives entirely *above* MQTT.

### The journey of one reading

Each layer wraps the one above it — this is **encapsulation**:

```
                     ┌──────────────────────────────────────┐
Protobuf  Telemetry: │ co2=812  temp=22.5  rh=41.2  seq=7   │   ~20 bytes
                     └──────────────────────────────────────┘
                     ┌────────────┬─────────────────────────┐
MQTT      PUBLISH:   │ hdr+topic  │  <the 20 bytes above>   │
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

The Pi unwraps it in exactly the reverse order and hands your ~20 bytes to the harness.

### What's in that `hdr`?

The `hdr+topic` box above is MQTT's **fixed header** followed by its **variable header**.
For a `PUBLISH` packet:

```
Fixed header
   byte 1      packet type (= PUBLISH) + the DUP / QoS / RETAIN flag bits
   1–4 bytes   Remaining Length — how many bytes follow in this packet
Variable header
   2 bytes     length of the topic string
   N bytes     the topic itself, e.g. "node/1/telemetry"
   2 bytes     packet identifier — present ONLY for QoS 1 and 2
Payload
   N bytes     your Protobuf, carried verbatim and never inspected
```

**`Remaining Length` is how MQTT solves the framing problem** claimed above. It states
exactly how many bytes this message occupies, so the receiver knows precisely where this
message ends and the next begins — it is the length prefix you would otherwise have had to
design, implement, and debug yourself on top of raw TCP.

The rest of the header is machinery you will meet again: the DUP / QoS / RETAIN flag bits
and the packet identifier are literally the `d0, q1, r0, m1` printed in the `-d` traces in
§8, and the packet identifier's absence at QoS 0 is why nothing can be acked at that level.

---

## 3. Why a broker? The shift you have to make

This is the conceptual jump of the whole architecture, and it is worth slowing down for. It
changes *who is responsible for what* — and in doing so, deletes a pile of code the node
would otherwise have to carry.

### 3.1 The problem: client/server puts the node in charge of its consumers

With raw TCP you would write a **client/server** pair. Say the Nucleo is the server: it
holds a socket open, the Pi connects to it, and the two talk directly. Each endpoint knows
about the other.

Now add a second consumer — a dashboard, a logger. The node must:

- **accept** each new connection as it arrives,
- keep a **roster** of every connected consumer's socket,
- **add and remove** entries as consumers connect, disconnect, or crash,
- and **loop over that roster**, writing a copy of every reading to each one.

That is all bookkeeping about *who is currently connected*, it lives in the firmware, it
grows with every consumer added — and none of it has anything to do with measuring CO₂.

### 3.2 The answer: everyone connects to a broker

MQTT replaces the direct client/server relationship with **publish/subscribe through a
broker**: a middleman that every participant connects to.

```
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

**Every arrow points at the broker.** The publisher and the subscribers never connect to
each other; they may not even know the other exists. The broker is the only thing any of
them talks to.

**Exercise 1 in §8 demonstrates this directly:** `mosquitto_pub` publishes happily whether or
not a `mosquitto_sub` is running — it hands its message to the broker and exits, never
knowing whether anyone was there to receive it.

### 3.3 The vocabulary: MQTT is a protocol, Mosquitto is a program

Two words used above name different *kinds* of thing, and keeping them apart prevents a lot
of muddle:

- **MQTT** is the **protocol** — the written rules for what bytes go on the wire (`CONNECT`,
  `PUBLISH`, `PUBACK`, topics, QoS flags). This project uses version **3.1.1**. A protocol
  is a *contract*, not a program.
- **Mosquitto** is one **program that implements** that protocol — specifically the
  **broker** role. The clients are implementations too, just of the other role.

The same split exists on the web:

```
HTTP   : MQTT           the protocol / contract
nginx  : Mosquitto      a server / broker implementation
curl   : mosquitto_pub  a client implementation
```

| Role | Speaks MQTT as… | Programs here |
|---|---|---|
| **Broker** | server | Mosquitto |
| **Client** | client | `mosquitto_pub` / `mosquitto_sub`, the paho-mqtt harness, Zephyr's `CONFIG_MQTT_LIB` (the Nucleo) |

Why this isn't pedantry: **the Nucleo's MQTT code and Mosquitto share zero source code.**
One is a C library inside Zephyr; the other is a standalone C program from an unrelated
project. They interoperate *only* because both conform to the protocol — which is the whole
value of a protocol being a written spec rather than a particular program. Any conforming
client talks to any conforming broker: swap Mosquitto for HiveMQ or EMQX, or paho for a C#
client, and nothing else changes.

So frame the work ahead accordingly: when you write the firmware you are not "programming
Mosquitto" — you are implementing the **client side of MQTT 3.1.1**, and Mosquitto is merely
the conforming broker you happen to test against.

### 3.4 Who listens? Exactly one program does

"Listening" is overloaded in everyday speech, so this guide uses it in one strict sense:

> **Listening** = holding a **listening socket**: a socket bound to a port and sitting in
> `accept()`, **open to new inbound TCP connections**. This is what makes a program a
> *server*.

Crucially, that is **not** the same as *receiving data*. A program can read incoming bytes
all day on a connection **it opened itself** without ever listening. Keeping these two apart
resolves most confusion about who is a server here:

| | **Listening** — open to new inbound TCP connections | **Receives data** |
|---|---|---|
| **Mosquitto** (broker) | ✅ yes — on `192.168.10.1:1883` | ✅ |
| **Nucleo** | ❌ never | ✅ commands |
| `mosquitto_sub` | ❌ never | ✅ published messages |
| `mosquitto_pub` | ❌ never | ✅ `CONNACK`, `PUBACK` |

So in this entire system there is **exactly one listening socket** — Mosquitto's, on port
1883. Every other participant, the Nucleo included, only ever **dials out**. None of them is
a server; all of them receive data.

That single listening port serves all of them simultaneously because a TCP connection is
identified by its **4-tuple** `(client IP, client port, broker IP, 1883)`. Each client's
IP/port pair differs, so the Pi's TCP stack demultiplexes the traffic into separate
connections. This is why one port can hold thousands of clients at once: the *connection*,
not the port, is the unit of identity.

### 3.5 Connection direction vs. data flow

The arrows in the diagram show only **one** of two directions that matter, and it is easy to
assume they show the other.

- **Connection direction** — *who dials whom*, i.e. who opened the TCP connection. This is
  what the arrows mean (each is labelled `connect`), and per §3.4 they all point at the
  broker because the broker is the only listener. `mosquitto_sub` is the case worth pausing
  on: it *receives* messages, so you would naturally expect its arrow to point outward, from
  the broker toward it. But receiving data is not the same as opening a connection — the
  subscriber dials the broker like everyone else.
- **Data-flow direction** — *which way messages travel* once a connection is open. A
  different question, and for one of the two roles it runs **opposite** to the arrow:

| Role | Connection (who dials) | Data (which way messages go) | |
|---|---|---|---|
| Publisher | pub → broker | pub → broker | same direction |
| Subscriber | sub → broker | broker → sub | **opposite!** |

The publisher is intuitive — it dials the broker *and* sends data that way. The surprising
one is the **subscriber**: it dials the broker, yet messages come back *at* it, the other way
down that same connection.

Data can travel opposite to the connection because **a TCP connection is full-duplex**: once
open, either end may send whenever it likes, and which end dialed becomes irrelevant. The
subscriber opens the pipe; the broker pushes messages back through it.

This is precisely how **commands reach the Nucleo even though the Nucleo never listens**.
The node dials out once; commands arrive back down that same open connection, and it reads
them off its socket — *receiving*, not *listening*, in the §3.4 sense.

That distinction is also what makes MQTT work through **NAT** (Network Address Translation,
the router boundary that blocks unsolicited inbound connections). NAT blocks new *inbound
connections*; it does not block return *data* on a connection opened from the inside. Since
the node only ever initiates its one outbound connection, commands ride back down it
untroubled.

### 3.6 What the node gets out of it

Compare against the bookkeeping §3.1 demanded:

- **The node is a client, never a server.** It dials out, holds no listening socket, and
  keeps **no roster of consumers** — no list of connected receivers to loop over and copy
  each reading to. It calls `mqtt_publish(...)` once, hands one copy to the broker, and the
  broker fans out to whoever subscribed. That publish path is the same single line whether
  zero consumers or a hundred are connected.
- **Adding a consumer changes no firmware.** Start a second subscriber and it just works.
- **Consumers can be absent.** The node publishes into the void without error.
- **One connection carries both directions.** Telemetry out and commands in share the single
  TCP connection the node opened — no inbound port, no firewall hole, no listening socket on
  the device.

---

## 4. Topics: MQTT's own addressing scheme, above IP

A **topic** is a concept that exists **purely at the MQTT layer** — it is defined by the
MQTT protocol and by nothing beneath it. Concretely, it is a UTF-8 string with `/` as a
separator, carried as a field inside every MQTT `PUBLISH` packet (the `hdr+topic` box in the
§2 encapsulation diagram). The broker reads that field and routes the message by matching it
against what subscribers asked for.

**Nothing below MQTT knows topics exist.** TCP sees an opaque byte stream; IP sees a packet
addressed to `192.168.10.1`; Ethernet sees a frame addressed to a MAC address. No switch,
no router, no kernel network stack ever parses or acts on a topic. Only MQTT software does —
the broker, and the client library at each end.

That makes it a *second, independent* addressing system stacked on top of the first. IP
addresses say *which machine*; topics say *which stream of information*. A subscriber never
says "give me messages from 192.168.10.2" — it says "give me `node/1/telemetry`", and
neither side cares what IP that came from.

```
node/1/telemetry
└─┬┘ │  └───┬───┘
  │  │      └── what kind of information
  │  └───────── which node
  └──────────── namespace
```

### Wildcards

Subscribers (never publishers) may use two wildcards:

| Wildcard | Meaning | `node/1/telemetry` | `node/2/telemetry` | `node/1/sensor/telemetry` |
|---|---|---|---|---|
| `node/1/telemetry` | exact | ✅ | ❌ | ❌ |
| `node/+/telemetry` | `+` = exactly **one** level | ✅ | ✅ | ❌ |
| `node/#` | `#` = **any remaining** levels | ✅ | ✅ | ✅ |

Two hard rules: **wildcards are subscribe-only** (publishing to `node/+/telemetry` is
illegal — a message must have one concrete topic), and **`#` must be the last character**.

This is why hierarchies are ordered **general → specific**. Because the project uses
`node/<id>/<kind>`, a harness can watch one node (`node/1/telemetry`), the same reading
from every node (`node/+/telemetry`), or everything one node says (`node/1/#`) — without
the firmware knowing or changing. **Your topic layout is your subscription API.** Design it
like one; it is as hard to change later as a REST URL scheme.

---

## 5. Quality of Service: what is actually being promised

QoS is the most misunderstood part of MQTT, largely because the obvious guess is wrong.

### The three levels

| QoS | Name | Packet flow | Guarantee |
|---|---|---|---|
| **0** | at most once | `PUBLISH` → *(nothing)* | fire and forget |
| **1** | at least once | `PUBLISH` → `PUBACK` | arrives, but **maybe twice** |
| **2** | exactly once | `PUBLISH` → `PUBREC` → `PUBREL` → `PUBCOMP` | arrives exactly once |

**Exercise 4 in §8 shows this on the wire** with `-d`: QoS 0 sends `PUBLISH` and stops;
QoS 1 sends `PUBLISH` and waits for a `PUBACK` before moving on. One extra round trip —
that's the entire difference.

### The insight: TCP reliability ≠ MQTT QoS

The natural objection is: *TCP already guarantees delivery — so what is QoS 1 for?*

They promise different things:

- **TCP** guarantees the bytes reached **the broker's TCP stack** (its kernel).
- **QoS 1** guarantees **the broker application** accepted and took ownership of the
  message.

Between those two lies everything that can go wrong above the kernel: the broker being
mid-restart, out of memory, out of disk for a persistent store, or the connection dropping
in the instant between "TCP accepted the bytes" and "Mosquitto processed them."

So the real exposure of QoS 0 is **not** wire corruption (TCP's checksums and
retransmissions handle that). It is the **reconnect gap**: if the link drops mid-publish,
TCP retries with exponential backoff for a while and then gives up, and the message is
simply gone — with nobody, on either end, ever knowing it existed.

### The DUP flag, and why QoS 1 costs you something

In the trace, `PUBLISH (d0, q1, r0, m1, ...)`:

```
d0  DUP flag    — 1 if this is a REDELIVERY of a message already sent
q1  QoS level
r0  RETAIN flag
m1  packet identifier — what the PUBACK correlates back to
```

If a `PUBACK` is lost in flight, the sender cannot tell "the broker never got it" from
"the broker got it, and the ack was lost." So it redelivers with `d1` — and the broker,
which *did* already have it, may deliver it to subscribers **twice**.

This is what "at least once" literally means, and it has a direct design consequence:
**anything you send at QoS 1 must be idempotent, or must carry an identifier you can
deduplicate on.** That is precisely why the README's `Command` message has a `sequence`
field. It isn't decoration — QoS 1 forces it.

The packet identifier only exists on the wire for QoS 1 and 2. A QoS 0 `PUBLISH` carries
none, because nothing will ever be acked and so there is nothing to correlate.

---

## 6. Retained messages and the Last Will

### Retain: last known state

Normal pub/sub is strictly **live**: publish to a topic nobody is subscribed to and the
message is gone forever. The broker is a router, not a database.

The `retain` flag makes one exception. The broker keeps **the most recent retained message
per topic**, and delivers it immediately to any client that subscribes *later* —
**Exercise 3 in §8** shows a subscriber started *after* the publish receiving it instantly.

It is a **last-known-value cache**, not a history — exactly one message per topic, the
newest. Publishing a retained *empty* payload clears it.

Why it matters here: a harness that starts up mid-flight gets the last reading instantly
instead of waiting up to 5 s for the next sample.

### Last Will and Testament: speaking after you die

A node can crash, or its cable can be pulled. It never gets to say "I'm going offline."

So MQTT lets a client register a **will** at `CONNECT` time: a topic, a payload, a QoS, a
retain flag. The broker holds it. If the client ever disconnects **without** a clean
`DISCONNECT` packet — crash, cable pull, keepalive timeout — **the broker publishes the
will on the client's behalf.**

The idiom, which `mqtt-design.md` adopts:

- Will = `node/1/status` → `offline`, retained.
- On connect, the node immediately publishes `node/1/status` → `online`, retained.

Now `node/1/status` is *always* correct, permanently, for anyone who subscribes — and the
node needed no code for the failure case. The broker covers it. This is the cheapest and
most instructive piece of MQTT's lifecycle machinery.

---

## 7. Sessions, keepalive, and why reconnect is real work

### Keepalive: TCP fails slowly and silently

**TCP is slow to notice a dead peer.** Pull the cable and the sender sits in
retransmit-with-backoff for minutes before giving up. Worse, an *idle* TCP connection sends
nothing at all — so a connection whose peer vanished looks byte-for-byte identical to a
connection whose peer simply has nothing to say. Both are silent.

This is a real problem for a node that publishes every 5 s and otherwise idles. It could
sit on a dead connection indefinitely, cheerfully believing it is connected.

So MQTT layers **its own** liveness check on top: at `CONNECT` the client declares a
**keepalive** interval (say 60 s). If it hasn't sent anything for that long, it must send a
`PINGREQ` and get a `PINGRESP`. If the broker hears nothing within 1.5× keepalive, it
declares the client dead and **fires the will**.

**This is the answer to "why does MQTT reinvent something TCP already does?"** It doesn't —
TCP has no application-level liveness signal at all, and its failure detection is too slow
and too silent to be useful for a device.

### Clean vs persistent sessions

At `CONNECT`, a client either asks for a **clean session** (throw away all state; start
fresh) or a **persistent** one (broker, remember my subscriptions and queue my QoS 1
messages while I'm gone).

The tempting choice is "persistent, obviously — don't lose my data." For **telemetry that
is actively wrong**, and it's the sharpest argument in `mqtt-design.md`: with a persistent
session and QoS 1 telemetry, a node that drops for two minutes reconnects and the broker
**floods it with 24 stale readings**. Live telemetry that is two minutes old is not
valuable; it is misleading. For a *command* queue, the same behaviour is exactly what you
want.

Different data, different guarantees. That's why QoS and session are per-purpose decisions,
not one global setting.

---

## 8. Hands-on: prove all of it with the CLI

Everything above is theory until you watch it happen. This section is a **runnable lab**:
set the broker up once, then work through five exercises that each demonstrate one concept
from the preceding sections — with the commands explained, the expected output shown, and a
note on what it proves.

Do this **before** writing any firmware. `mosquitto_pub` and `mosquitto_sub` are ordinary
MQTT clients — nothing privileged about them. Anything they do, the Nucleo will also do, so
ten minutes here makes the Zephyr API obvious instead of mysterious.

Everything runs on the Pi. Several exercises need **two terminals**, so open a second SSH
session (`ssh pi@pi5.local`) alongside the first. They're referred to as **A** (subscriber)
and **B** (publisher).

### 8.1 Setup: install and expose the broker

```sh
sudo apt update
sudo apt install -y mosquitto mosquitto-clients
```

`mosquitto` is the broker (a service that starts automatically); `mosquitto-clients`
provides the `mosquitto_pub` / `mosquitto_sub` CLI tools.

Before changing anything, observe the default:

```sh
mosquitto -h | head -1                  # version (expect 2.0.x)
systemctl status mosquitto --no-pager   # is the broker service running?
ss -tlnp | grep 1883                    # which IP:port is it LISTENING on?
```

`ss -tlnp` is not MQTT at all — it's a Linux tool listing **l**istening **t**CP sockets,
**n**umerically, with the owning **p**rocess. It answers the one question that matters
before any client can connect: *is anything open to connections at the address the Nucleo
will dial?* On a fresh install you get:

```
LISTEN 0  100  127.0.0.1:1883  0.0.0.0:*
LISTEN 0  100      [::1]:1883     [::]:*
```

**Loopback only** — the broker is running perfectly and is completely unreachable from the
Nucleo. That is Mosquitto 2.0's deliberate security default, not a fault. A listening socket
is **IP + port**, not port alone (§3.4), so a client dialing `192.168.10.1:1883` would be
**refused**. Worth seeing with your own eyes before fixing it.

Now bind a listener to the bench link. Create a drop-in so the shipped `mosquitto.conf`
stays pristine (it reads this directory via its `include_dir` setting — confirm with
`grep include_dir /etc/mosquitto/mosquitto.conf`):

```sh
sudo tee /etc/mosquitto/conf.d/bench.conf > /dev/null <<'EOF'
listener 1883 192.168.10.1
allow_anonymous true
EOF

sudo systemctl restart mosquitto
ss -tlnp | grep 1883
```

Expected — note the loopback entries are **gone**:

```
LISTEN 0  100  192.168.10.1:1883  0.0.0.0:*
             └──────┬──────┘└─┬─┘
                    │         └── port
                    └──────────── bound IP  ← the part everyone forgets
```

- **`listener <port> <bind-address>`** — defining any listener **replaces** the implicit
  localhost default rather than adding to it. That's why loopback disappeared, and why even
  on the Pi you must now use `-h 192.168.10.1`, never `-h localhost`.
- Binding to `192.168.10.1` (not `0.0.0.0`) keeps the broker reachable **only on the bench
  cable** — deliberately invisible to the home LAN, where the Pi is `192.168.1.105`.
- **`allow_anonymous true`** — once any listener is defined, Mosquitto 2.0 denies anonymous
  clients unless told otherwise. Without it you get a confusing failure: TCP connects fine,
  then the MQTT `CONNECT` is rejected as "not authorised", which looks like a network problem
  but isn't. Bench-only; a real product uses credentials + TLS.

> ⚠️ **This works now but dies on the next reboot.** Binding to a *specific* IP means
> mosquitto cannot start until that address exists — and on boot it loses the race against
> NetworkManager, failing with `Cannot assign requested address` before `eth0` is configured.
> It is a direct consequence of "a listening socket is IP + port" (§3.4). The fix is a
> systemd drop-in ordering mosquitto after `network-online.target` plus a relaxed restart
> limiter; see **Boot ordering** in [`mqtt-design.md`](../docs/mqtt-design.md) for the exact file.

### 8.2 The two tools, decoded

**Subscribe:**

```sh
mosquitto_sub -h 192.168.10.1 -t 'node/1/telemetry' -v
              └──────┬───────┘ └────────┬─────────┘ └┬┘
                     │                  │            └── verbose: print "topic payload"
                     │                  │                (essential once wildcards mean you can't tell which topic hit)
                     │                  └── topic filter — may contain + and #
                     └── broker address (-p 1883 is the default port)
```

It appears to hang. It hasn't: it is holding an open TCP connection with an MQTT session on
top, waiting for the broker to push messages down it (§3.5). Always **quote the topic** —
in the shell, `#` starts a comment and `+` can glob.

**Publish:**

```sh
mosquitto_pub -h 192.168.10.1 -t 'node/1/telemetry' -m 'hello' -q 1 -r -d
                                                    └───┬────┘ └─┬┘ └┤ └┬┘
                                                        │        │   │  └ debug: print control packets
                                                        │        │   └──── retain
                                                        │        └──────── QoS (default 0)
                                                        └───────────────── payload
```

Useful extras: `-C <n>` on `mosquitto_sub` exits after *n* messages, and `-i <id>` sets an
explicit client id on either tool.

### Exercise 1 — Publish/subscribe decoupling

*Demonstrates §3.2: every arrow points at the broker; the publisher never knows a subscriber
exists.*

**Terminal A:**
```sh
mosquitto_sub -h 192.168.10.1 -t 'node/1/telemetry' -v
```
**Terminal B:**
```sh
mosquitto_pub -h 192.168.10.1 -t 'node/1/telemetry' -m 'hello from the pi'
```

A prints `node/1/telemetry hello from the pi`.

Now the actual lesson — **stop the subscriber in A (Ctrl-C) and publish again from B.** The
publish still succeeds, with no error and no complaint. Restart A and it receives nothing;
the message is long gone. The publisher never had any idea whether anyone was listening.

**Proves:** the two clients never connect to each other. Both dial the broker independently,
and publishing is decoupled from consumption. This is why the Nucleo can publish telemetry
without knowing or caring whether your Python harness is running.

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

Only the **first** arrives. `node/2/telemetry` doesn't match the filter, so the broker
simply drops it for this subscriber.

Now restart A with a single-level wildcard, and rerun both publishes plus a nested one:

```sh
mosquitto_sub -h 192.168.10.1 -t 'node/+/telemetry' -v
```
```sh
mosquitto_pub -h 192.168.10.1 -t 'node/1/telemetry'        -m 'node 1'
mosquitto_pub -h 192.168.10.1 -t 'node/2/telemetry'        -m 'node 2'
mosquitto_pub -h 192.168.10.1 -t 'node/1/sensor/telemetry' -m 'nested'
```

The first two arrive; the third does **not** — `+` matches exactly one level, and
`node/1/sensor/telemetry` has an extra one. Finally restart A with `-t 'node/#'` and repeat:
now **all three** arrive, because `#` matches any remaining depth.

**Proves:** your topic hierarchy *is* your subscription API. Because topics run
general → specific, a consumer can select one node, one kind across all nodes, or everything
from one node — with no firmware change. (Remember: wildcards are **subscribe-only**, and `#`
must be last.)

### Exercise 3 — Retained messages

*Demonstrates §6: the broker caches the last retained message per topic.*

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

This time the subscriber prints `node/1/status online` **immediately on connecting**, despite
subscribing *after* the publish. Subscribe again and it happens again — the broker is holding
it. Clear it with an empty retained payload:

```sh
mosquitto_pub -h 192.168.10.1 -t 'node/1/status' -m '' -r
```

**Proves:** retain is a **last-known-value cache** (exactly one message per topic, the
newest), not a history. It's what lets a harness starting up mid-flight know the node's state
instantly instead of waiting for the next 5 s sample.

### Exercise 4 — QoS on the wire

*Demonstrates §5: the QoS 1 acknowledgement, and the header fields from §2.*

QoS is nearly impossible to *observe* on a healthy local cable — which is itself the lesson.
So inspect the packets instead, with `-d`:

```sh
mosquitto_pub -h 192.168.10.1 -t 'node/1/telemetry' -m 'qos zero' -q 0 -d
mosquitto_pub -h 192.168.10.1 -t 'node/1/telemetry' -m 'qos one'  -q 1 -d
```

QoS 0:
```
Client (null) sending CONNECT              ← open the MQTT session
Client (null) received CONNACK (0)         ← broker accepts; 0 = success
Client (null) sending PUBLISH (d0,q0,r0,m1,'node/1/telemetry',...(8 bytes))
Client (null) sending DISCONNECT           ← clean teardown (so no will fires)
```

QoS 1 — identical except for one extra packet:
```
Client (null) sending PUBLISH (d0,q1,r0,m1,'node/1/telemetry',...(7 bytes))
Client (null) received PUBACK (Mid: 1)     ← the entire difference
Client (null) sending DISCONNECT
```

`Client (null)` just means no client id was set, so libmosquitto generated one. The
`d0,q1,r0,m1` flags are literally the `PUBLISH` header fields from §2, and `PUBACK (Mid: 1)`
correlates back to that `m1`.

**Proves:** QoS 1 costs exactly one extra round trip, in which the **broker application**
confirms it took ownership — a different promise from TCP's guarantee that the bytes reached
the broker's *kernel* (§5).

Note what else this reveals: `mosquitto_pub` performs a **complete connect → publish →
disconnect cycle on every invocation.** That's fine for a CLI and wrong for the Nucleo, which
will connect **once** and hold the session open for its lifetime — which is exactly why
keepalive and reconnect become your problem in firmware and never came up here.

### Exercise 5 — Last Will and Testament

*Demonstrates §6/§7: the broker speaks for a client that dies without a clean disconnect.*

This version uses two CLI clients, so it needs no firmware and proves the mechanism in
isolation. Testing the **node's** will is a harder problem — the obvious methods take the
observer down along with the node — and needs a packet filter instead; see *Testing the
Last Will* in [`mqtt-design.md`](../docs/mqtt-design.md) for that procedure.

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

Terminal B prints `node/1/status offline`, published by **the broker**, not by any client.
Contrast with stopping A via Ctrl-C, which sends a clean `DISCONNECT` and therefore fires
**no** will.

**Proves:** the node gets correct offline reporting for free, including on crash or cable
pull — the failure case it could never have handled itself. Combined with a retained `online`
published at connect, `node/1/status` is then always accurate for any subscriber.

### Exercise 6 — Point the same tools at the real node

*Demonstrates §3.3: the Nucleo is just another conforming MQTT client, and every concept
above applies to it unchanged.*

With the node flashed and running, watch everything it says:

```sh
mosquitto_sub -h 192.168.10.1 -t 'node/#' -v
```

You will see three things at once, and each is a concept from earlier made concrete:

```
node/1/status online                          ← retained (§6), delivered instantly on subscribe
node/1/telemetry <binary>                     ← QoS 0, every ~5 s
node/1/telemetry <binary>
```

The `status` line arrives **immediately**, before any telemetry, because it is retained —
Exercise 3 with a real publisher. The telemetry lines arrive on the sensor's own cadence.

But the telemetry payload is unreadable, and that is the point: **MQTT gives you delivery,
not meaning** (§2). The broker never looked inside those bytes and neither can
`mosquitto_sub`. To read them you need the schema:

```sh
host/.venv/bin/python host/monitor.py
```

```
14:02:11  node/1/status        [retained] online
14:02:16  node/1/telemetry     seq=1042  co2=  812 ppm  temp=22.41 C  rh=41.3 %  up=  5210.4s  SENSOR_STATUS_OK  (schema v1)
```

Same bytes, same broker, same topics — the only thing added is Protobuf, decoding a
payload MQTT carried verbatim and never inspected.

**Proves:** the layering in §2 is real and separable. Everything you learned with
`mosquitto_pub`/`mosquitto_sub` transfers to the firmware without modification, because
both are clients of the same protocol (§3.3) — and the one thing the CLI tools cannot do
is the one thing that lives *above* MQTT.

### 8.3 Quick reference

| Command | What it does |
|---|---|
| `ss -tlnp \| grep 1883` | which **IP:port** the broker is open on (§3.4) |
| `systemctl status mosquitto --no-pager` | is the broker service running |
| `sudo systemctl restart mosquitto` | apply a config change |
| `mosquitto_sub -h HOST -t 'TOPIC' -v` | subscribe; `-v` prints `topic payload` |
| `mosquitto_pub -h HOST -t 'TOPIC' -m 'MSG'` | publish once |
| `-q 0\|1\|2` | QoS level (default 0) |
| `-r` | retain (empty payload `-m ''` clears it) |
| `-d` | print the MQTT control packets |
| `-C n` | (sub) exit after *n* messages |
| `-i ID` | set an explicit client id |

Config lives in `/etc/mosquitto/conf.d/bench.conf`; the broker is bound to
`192.168.10.1:1883` with anonymous access, so always use `-h 192.168.10.1` — **never**
`-h localhost`, which no longer has a listener.

---

## 9. The whole path, end to end

Where every piece finally sits:

```
┌─ NUCLEO ─────────────────────────────────┐      ┌─ RASPBERRY PI ──────────────┐
│                                          │      │                             │
│  SCD-40 ──I²C──▶ sensor thread           │      │   Mosquitto (broker)        │
│                  (sensor.cpp)            │      │   192.168.10.1:1883         │
│                       │                  │      │        │                    │
│                       ▼ zbus_chan_pub()  │      │        │ routes by topic    │
│                  zbus channel            │      │        ▼                    │
│                    │      │              │      │   paho-mqtt harness         │
│        the value   │      │ listener     │      │        │                    │
│        stays here  │      ▼ bumps it     │      │        ▼                    │
│                    │   eventfd           │      │   protobuf decode → log     │
│                    │      │              │      │                             │
│                    ▼      ▼              │      └─────────────────────────────┘
│           MQTT thread (main.cpp)         │                    ▲
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

Note that the eventfd never leaves the chip — both it and the channel are in-process. The
signal and the value travel **separately**: the eventfd says *something happened*, the
channel holds *what it was*. That is why the MQTT thread has two inbound arrows.

Two boundaries in that diagram are the actual learning goals:

- **sensor thread → MQTT thread.** The sensor thread never mentions MQTT; it publishes a
  struct to an in-process channel and moves on. A zbus *listener* — which runs on the
  sensor thread, so it does nothing but bump an eventfd — is what lets the MQTT thread
  wait on the socket and the bus in a single `poll()`. Sensing and transport are decoupled
  *inside* the firmware, exactly as pub/sub decouples the node from the host *outside* it.
  Same idea, two scales; see [`zbus-guide.md`](zbus-guide.md).
- **nanopb → MQTT.** MQTT carries opaque bytes. Protobuf decides what they mean. Both ends
  derive from the same `.proto`, which is why that file — not the firmware — is the
  contract. See [`protobuf-guide.md`](protobuf-guide.md).

---

## 10. The model in one paragraph

Ethernet moves frames across one cable; IP addresses machines; TCP turns that into a
reliable byte stream but deliberately refuses to mark where messages end. MQTT adds message
framing and, crucially, a **broker**: nobody connects to anybody, everyone connects to the
broker, and messages are routed by **topic** rather than address — so the node publishes
without knowing or caring who listens. **QoS** picks what is being promised (0 = fire and
forget, self-healing for a 5 s telemetry stream; 1 = acked but possibly duplicated, so
commands need a sequence number to dedupe). **Retain** gives late subscribers the last known
value; the **will** lets the broker announce your death; **keepalive** exists because TCP
notices death far too slowly and silently. Everything up to and including TCP, Zephyr
implements; MQTT lifecycle and above is your work — and Protobuf, riding inside the payload
as bytes MQTT never inspects, is what makes those bytes mean `co2 = 812 ppm`.
