# Learning roadmap — concepts before coding

A study order for the concepts in `README.md`, sorted by *layer* and by *how much
you actually have to implement*. The point: the pile is smaller than it looks once
you separate what Zephyr implements for you from what you write yourself.

**Status (2026-07-20):** Phase 1 ✅ · Phase 2 ✅ · Phase 3 (MQTT) ✅ ·
Phase 4 (Protobuf/nanopb) ✅ · **Phase 5 (zbus) ← next**

## The reframe

The README's concept list isn't flat — it's a layered stack, and Zephyr implements
the bottom of it. Map each concept to its layer and to how deep you personally go:

| Layer | Project concept | Who implements it | How deep *you* go |
|---|---|---|---|
| **App payload** | Protobuf / nanopb | You (schema + generated C) | **Deep** — you write `.proto`, `.options`, encode/decode |
| **App internal** | zbus | You (channels + observers) | **Deep** — but it's *not networking* |
| **App protocol** | MQTT 3.1.1 | Zephyr `CONFIG_MQTT_LIB` (you drive it) | **Deep** — lifecycle is the core exercise |
| **Transport** | TCP | Zephyr net stack | *Light* — refresher only |
| **Network** | IP, static addressing, ARP | Zephyr net stack (you configure) | *Medium* — set IPs, subnet, ping |
| **Link + PHY** | Ethernet: MAC, LAN8742, RMII, Auto-MDIX | Zephyr driver + STM32 ETH peripheral | **Shallow** — mental model only |

**The single most important thing:** the STM32 `eth.yaml`
(`../../shared_refs/stm32/STM32H753xI/RM0433/eth.yaml` — MAC, DMA descriptors, RMII,
MDIO, ~1065 lines of registers) is **debugging reference, not a prerequisite**. The
`nucleo_h753zi` board target already wires the ETH driver + LAN8742 PHY driver. You
will almost never touch a register in it. Knowing *what a PHY vs a MAC is* is worth
an hour; memorizing descriptor rings is a rabbit hole. Reach for the yaml only when
the link misbehaves.

## Dependency structure

Two concepts form a **strict stack** — each needs the one below working:

```
Ethernet link up  →  IP address / ping  →  TCP connects  →  MQTT session
```

Two are **orthogonal** — no dependency on the network; learnable anytime, even
offline on a laptop:

- **Protobuf / nanopb** — how you serialize a message. Just bytes; zero hardware needed.
- **zbus** — an *in-process* pub/sub bus between threads on the MCU. Despite sitting
  next to "MQTT" in the README, it has **nothing to do with the wire** — it's the
  internal decoupling between the sensor thread and the publisher thread. Don't let
  its placement fool you into treating it as a network concept.

## Recommended order

Go **bottom-up to get a working link (shallow), then top-down where the real code
lives (deep)**. Slot the two orthogonal topics in wherever your energy fits.

### Phase 1 — Ethernet + IP, just enough to ping
This is the genuine knowledge gap (the Kurose *Top-Down* notes stop at the network
layer, chapters 1–4; Ethernet lives in the *link* layer, chapter 6, not yet covered).
But you need surprisingly little:
- Concepts: frame, MAC address, PHY vs MAC, what **RMII** is (the 2-bit-wide bus
  between the STM32 MAC and the LAN8742 PHY), what **Auto-MDIX** does (lets a straight
  cable work with no crossover — that's the README line).
- Then the config reality: static IP on both ends, one `/24` subnet, **no gateway**
  (direct cable, no router), and **ARP** as the thing that maps IP→MAC on the link.
- **Checkpoint:** explain why Pi `192.168.10.1` and Nucleo `.2` with mask
  `255.255.255.0` talk with no router, and `ping` succeeds both ways.

### Phase 2 — TCP refresher (light)
You have notes (chapter 3). Re-anchor: connection setup, ports, and that TCP is a
*byte stream* with no message boundaries — because that's what MQTT rescues you from.
- **Checkpoint:** say in one sentence why raw TCP would force length-prefix framing
  and MQTT doesn't.

### Phase 3 — MQTT (deep — the heart of the project)
Broker vs client, pub/sub, topics, QoS 0/1, keepalive/PING, and **reconnect on link
drop** (the README calls this out twice — *the* thing that bites in production). Learn
it hands-on with Mosquitto *before* touching Zephyr: run the broker, use
`mosquitto_pub`/`mosquitto_sub` to feel topics and QoS, then a tiny paho-mqtt script.
- **Checkpoint:** two terminals — one subscribes to `node/1/telemetry`, the other
  publishes; kill/restart and watch reconnect.

### Phase 4 — Protobuf + nanopb (deep, orthogonal — anytime)
`.proto` syntax, field numbers, `optional`, then nanopb specifics: `.options` files,
fixed-size vs callback fields, static/no-malloc allocation. Learn **schema versioning**
here too (add a field, decode old↔new).
- **Checkpoint:** encode a `Telemetry` message in Python, decode the raw bytes in C
  (or vice-versa); then add a field and prove old readers still parse.

### Phase 5 — zbus (deep, orthogonal — anytime)
Channels, observers, publish/subscribe *between threads*. Smallest of the five.
- **Checkpoint:** a producer thread publishes a struct to a channel, an observer
  thread logs it — no networking involved.

Implementation then just *assembles* these:
`sensor → zbus channel → nanopb encode → MQTT publish`.

## Short version

1. **Don't** study the STM32 ETH peripheral first — Zephyr owns it.
2. The real *networking* gap is small: link/IP basics to get `ping` working (Phase 1–2).
3. The real *depth* belongs at the top: **MQTT lifecycle** (Phase 3) is the crown
   jewel, then **nanopb** and **zbus**, both learnable independently of the network.
